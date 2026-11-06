#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/stream.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  gt_bridge_stream_t base;
  gt_allocator_t allocator;
  int input, output, wake[2];
} fd_stream_t;
static void *allocate(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
static gt_bridge_io_t result(gt_bridge_io_state_t state, size_t n, gt_status_t status) {
  return (gt_bridge_io_t){state, n, status};
}
static int64_t milliseconds(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
/* Block SIGPIPE on this owner only, and consume only a newly generated signal.
 * Do not change the application's disposition or steal an already pending one. */
static ssize_t pipe_write(int fd, const void *p, size_t n) {
  sigset_t set, old, pending;
  sigemptyset(&set); sigaddset(&set, SIGPIPE);
  int e = pthread_sigmask(SIG_BLOCK, &set, &old);
  if (e) { errno = e; return -1; }
  if (sigpending(&pending)) {
    e = errno; (void)pthread_sigmask(SIG_SETMASK, &old, NULL); errno = e; return -1;
  }
  bool had_pipe = sigismember(&pending, SIGPIPE) == 1;
  ssize_t count = write(fd, p, n);
  e = errno;
  if (count < 0 && e == EPIPE && !had_pipe && !sigpending(&pending) &&
      sigismember(&pending, SIGPIPE) == 1) {
    int received;
    (void)sigwait(&set, &received);
  }
  (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
  errno = e;
  return count;
}
static gt_bridge_io_t transfer(gt_bridge_stream_t *base, void *p, size_t n, int ms, bool writing) {
  fd_stream_t *s = (fd_stream_t *)base;
  int fd = writing ? s->output : s->input;
  int64_t now = milliseconds();
  if (now < 0) return result(GT_BRIDGE_ERROR, 0, GT_ERR_IO);
  int64_t end = now + ms;
  bool first = true;
  for (;;) {
    if (atomic_load_explicit(&base->stopping, memory_order_acquire))
      return result(GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED);
    now = milliseconds();
    if (now < 0) return result(GT_BRIDGE_ERROR, 0, GT_ERR_IO);
    if (!first && now >= end) return result(GT_BRIDGE_WAIT, 0, GT_OK);
    first = false;
    int remaining = now < end ? (int)(end - now) : 0;
    struct pollfd fds[2] = {{fd, writing ? POLLOUT : POLLIN, 0}, {s->wake[0], POLLIN, 0}};
    int ready = poll(fds, 2, remaining);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) return result(GT_BRIDGE_ERROR, 0, GT_ERR_IO);
    if (atomic_load_explicit(&base->stopping, memory_order_acquire))
      return result(GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED);
    if (!ready) return result(GT_BRIDGE_WAIT, 0, GT_OK);
    if (fds[0].revents & POLLNVAL) return result(GT_BRIDGE_ERROR, 0, GT_ERR_IO);
    /* HUP/ERR still require a read: buffered tail precedes EOF or real error. */
    if (!(fds[0].revents & (writing ? (POLLOUT | POLLERR | POLLHUP) : (POLLIN | POLLERR | POLLHUP))))
      continue;
    ssize_t count = writing ? pipe_write(fd, p, n) : read(fd, p, n);
    if (count > 0) return result(GT_BRIDGE_DATA, (size_t)count, GT_OK);
    if (!writing && count == 0) return result(GT_BRIDGE_EOF, 0, GT_OK);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    return result(GT_BRIDGE_ERROR, 0, GT_ERR_IO);
  }
}
static gt_bridge_io_t fd_read(gt_bridge_stream_t *s, void *p, size_t n, int ms) {
  return transfer(s, p, n, ms, false);
}
static gt_bridge_io_t fd_write(gt_bridge_stream_t *s, const void *p, size_t n, int ms) {
  return transfer(s, (void *)p, n, ms, true);
}
static void wake(gt_bridge_stream_t *base) {
  fd_stream_t *s = (fd_stream_t *)base;
  uint8_t byte = 1;
  /* Wake fds live until destroy, including after close. Only one stop writes. */
  while (write(s->wake[1], &byte, 1) < 0 && errno == EINTR) {}
}
static gt_bridge_close_t close_step(gt_bridge_stream_t *base, int ms) {
  (void)ms;
  fd_stream_t *s = (fd_stream_t *)base;
  /* Never retry close(EINTR): the descriptor may already have been recycled. */
  int a = close(s->input);
  int b = s->input == s->output ? 0 : close(s->output);
  s->input = s->output = -1;
  /* Local descriptors are relinquished even if close reports a peer IO error.
   * No outstanding provider work or remote flush is implied by RELEASED. */
  (void)a; (void)b;
  return GT_BRIDGE_RELEASED;
}
static void destroy(gt_bridge_stream_t *base) {
  fd_stream_t *s = (fd_stream_t *)base;
  close(s->wake[0]); close(s->wake[1]);
  gt_allocator_t a = s->allocator;
  gt__plugin_memory_free(&a,s, sizeof(*s));
}
static const gt_bridge_stream_ops_t ops = {fd_read, fd_write, wake, close_step, destroy};

gt_status_t gt__bridge_fd_open(int input, int output, const gt_allocator_t *allocator,
                               gt_bridge_stream_t **out) {
  if (!out) return GT_ERR_INVALID_ARG;
  *out = NULL;
  if (input < 0 || output < 0 || (allocator && (!allocator->allocate || !allocator->release)))
    return GT_ERR_INVALID_ARG;
  int fds[2] = {input, output}, flags[2], fdflags[2];
  size_t count = input == output ? 1 : 2;
  for (size_t i = 0; i < count; i++) {
    struct stat st;
    if (fstat(fds[i], &st)) return GT_ERR_IO;
    if (!S_ISFIFO(st.st_mode) && !S_ISSOCK(st.st_mode)) return GT_ERR_INVALID_ARG;
    if (S_ISSOCK(st.st_mode)) {
      int type;
      struct linger linger;
      socklen_t size = sizeof(type);
      if (getsockopt(fds[i], SOL_SOCKET, SO_TYPE, &type, &size)) return GT_ERR_IO;
      size = sizeof(linger);
      if (getsockopt(fds[i], SOL_SOCKET, SO_LINGER, &linger, &size)) return GT_ERR_IO;
      if (type != SOCK_STREAM || linger.l_onoff) return GT_ERR_INVALID_ARG;
    }
    flags[i] = fcntl(fds[i], F_GETFL);
    fdflags[i] = fcntl(fds[i], F_GETFD);
    if (flags[i] < 0 || fdflags[i] < 0) return GT_ERR_IO;
    int mode = flags[i] & O_ACCMODE;
    if ((i == 0 && mode == O_WRONLY) ||
        ((i == 1 || count == 1) && mode == O_RDONLY)) return GT_ERR_INVALID_ARG;
  }
  gt_allocator_t a = allocator ? *allocator : (gt_allocator_t){NULL, allocate, release};
  fd_stream_t *s = gt__plugin_memory_alloc(&a,sizeof(*s));
  if (!s) return GT_ERR_IO;
  *s = (fd_stream_t){.allocator = a, .input = input, .output = output, .wake = {-1, -1}};
  if (pipe(s->wake)) goto fail;
  for (size_t i = 0; i < 2; i++) {
    if (fcntl(s->wake[i], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(s->wake[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
  }
  for (size_t i = 0; i < count; i++) {
    if (fcntl(fds[i], F_SETFL, flags[i] | O_NONBLOCK) < 0 ||
        fcntl(fds[i], F_SETFD, fdflags[i] | FD_CLOEXEC) < 0) {
      for (size_t j = 0; j <= i; j++) {
        (void)fcntl(fds[j], F_SETFL, flags[j]);
        (void)fcntl(fds[j], F_SETFD, fdflags[j]);
      }
      goto fail;
    }
  }
  (void)gt__bridge_stream_init(&s->base, &ops);
  *out = &s->base;
  return GT_OK;
fail:
  if (s->wake[0] >= 0) close(s->wake[0]);
  if (s->wake[1] >= 0) close(s->wake[1]);
  gt__plugin_memory_free(&a,s, sizeof(*s));
  return GT_ERR_IO;
}
