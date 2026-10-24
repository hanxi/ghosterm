#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "local-session-provider.h"
#include "proxy-status.h"
#include "socket-stream.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#define GT_LOCAL_SPAWN 1
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
#define GT_LOCAL_SPAWN 1
#endif
#endif
extern char **environ;
typedef struct {
  gt_socket_stream_t socket;
  pid_t pid;
  int diagnostic;
  bool borrowed, stopping, killed;
  uint64_t stopped;
} child;
struct gt_local_session_provider {
  gt_allocator_t allocator;
  gt_local_session_provider_config_t config;
  char proxy[1025], daemon[1025], runtime[1025];
  child children[2];
  uint64_t now, stderr_bytes;
  gt_status_t error;
  bool active, clock_fault;
};
static void *allocate(void *c, size_t n) {
  (void)c;
  return malloc(n);
}
static void release(void *c, void *p, size_t n) {
  (void)c;
  (void)n;
  free(p);
}
static bool tick(gt_local_session_provider_t *p) {
  uint64_t now;
  if (p->clock_fault || !p->config.clock(p->config.clock_context, &now) ||
      now < p->now) {
    p->clock_fault = true;
    p->error = GT_ERR_INTERNAL;
    return false;
  }
  p->now = now;
  return true;
}
static void close_fd(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}
static void stop(gt_local_session_provider_t *p, child *c) {
  gt__socket_stream_close(&c->socket);
  close_fd(&c->diagnostic);
  if (c->pid > 0 && !c->stopping) {
    c->stopping = true;
    c->stopped = p->now;
    (void)kill(c->pid, SIGTERM);
  }
}
static void pump(gt_local_session_provider_t *p) {
  bool clock = tick(p);
  for (unsigned i = 0; i < 2; i++) {
    child *c = &p->children[i];
    if (c->diagnostic >= 0) {
      char bytes[4096];
      ssize_t n = read(c->diagnostic, bytes, sizeof(bytes));
      if (n > 0)
        p->stderr_bytes += (uint64_t)n;
      else if (!n ||
               (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        close_fd(&c->diagnostic);
    }
    if (c->pid <= 0)
      continue;
    int status = 0;
    pid_t result = waitpid(c->pid, &status, WNOHANG);
    if (result == c->pid) {
      c->pid = 0;
      close_fd(&c->diagnostic);
      if (!c->stopping) {
        gt_status_t error;
        if (WIFEXITED(status) &&
            gt__proxy_exit_status(WEXITSTATUS(status), &error))
          p->error = error;
        else
          p->error = GT_ERR_IO;
      }
      continue;
    }
    if (result < 0 &&
        errno != EINTR) { /* Lost exclusive wait ownership is a caller error.
                             Never signal a reused PID. */
      c->pid = 0;
      p->error = GT_ERR_INTERNAL;
      close_fd(&c->diagnostic);
      continue;
    }
    if (c->stopping && !c->killed && (!clock || p->now - c->stopped >= 250)) {
      (void)kill(c->pid, SIGKILL);
      c->killed = true;
    }
  }
}
static bool paths(const char *s) {
  if (!s || s[0] != '/' || strlen(s) > 1024)
    return false;
  for (size_t i = 0; s[i]; i++)
    if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127)
      return false;
  return true;
}
#ifdef GT_LOCAL_SPAWN
static int isolated_fd(int fd) {
  if (fd >= 3)
    return fd;
  int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  int saved = errno;
  close(fd);
  errno = saved;
  return copy;
}
#endif
static gt_status_t spawn_proxy(gt_local_session_provider_t *p, unsigned slot,
                               gt_stream_ops_t *out) {
#ifndef GT_LOCAL_SPAWN
  (void)p;
  (void)slot;
  (void)out;
  return GT_ERR_UNSUPPORTED;
#else
  child *c = &p->children[slot];
  int pair[2] = {-1, -1}, diagnostic[2] = {-1, -1};
  gt_status_t error = GT_ERR_IO;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) || pipe(diagnostic))
    goto finish;
  for (unsigned i = 0; i < 2; i++) {
    pair[i] = isolated_fd(pair[i]);
    diagnostic[i] = isolated_fd(diagnostic[i]);
    if (pair[i] < 0 || diagnostic[i] < 0)
      goto finish;
    if (fcntl(pair[i], F_SETFD, FD_CLOEXEC) ||
        fcntl(diagnostic[i], F_SETFD, FD_CLOEXEC))
      goto finish;
  }
  if (fcntl(diagnostic[0], F_SETFL, O_NONBLOCK))
    goto finish;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  if (posix_spawn_file_actions_init(&actions))
    goto finish;
  if (posix_spawnattr_init(&attributes)) {
    posix_spawn_file_actions_destroy(&actions);
    goto finish;
  }
  int rc = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
  if (!rc)
    rc = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
  if (!rc)
    rc = posix_spawn_file_actions_adddup2(&actions, diagnostic[1],
                                          STDERR_FILENO);
#ifdef __APPLE__
  if (!rc)
    rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
  if (!rc)
    rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
  char *argv[] = {p->proxy,
                  "--runtime-dir",
                  p->runtime,
                  "--role",
                  slot ? "data" : "management",
                  "--daemon",
                  p->daemon,
                  NULL};
  if (!rc)
    rc = posix_spawn(&c->pid, p->proxy, &actions, &attributes, argv, environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (rc) {
    c->pid = 0;
    error = rc == ENOENT   ? GT_ERR_NOT_FOUND
            : rc == EACCES ? GT_ERR_AUTH
                           : GT_ERR_IO;
    goto finish;
  }
  c->stopping = false;
  c->killed = false;
  c->diagnostic = diagnostic[0];
  diagnostic[0] = -1;
  error = gt__socket_stream_adopt(pair[0], &c->socket, out);
  if (error == GT_OK) {
    pair[0] = -1;
    c->borrowed = true;
  } else
    stop(p, c);
finish:
  for (unsigned i = 0; i < 2; i++) {
    close_fd(&pair[i]);
    close_fd(&diagnostic[i]);
  }
  return error;
#endif
}
static gt_session_link_state_t step(void *context, bool start, uint32_t attempt,
                                    gt_status_t *error) {
  (void)attempt;
  gt_local_session_provider_t *p = context;
  *error = GT_OK;
  if (start) {
    if (p->active) {
      *error = GT_ERR_INTERNAL;
      return GT_SESSION_LINK_FATAL;
    }
    p->active = true;
    p->error = GT_OK;
  }
  pump(p);
  if (p->error != GT_OK) {
    *error = p->error;
    return *error == GT_ERR_IO ? GT_SESSION_LINK_RETRY : GT_SESSION_LINK_FATAL;
  }
  if (!p->active) {
    *error = GT_ERR_INTERNAL;
    return GT_SESSION_LINK_FATAL;
  }
  for (unsigned i = 0; i < 2; i++)
    if (p->children[i].stopping && p->children[i].pid > 0)
      return GT_SESSION_LINK_WAIT;
  return GT_SESSION_LINK_READY;
}
static gt_status_t stream(void *context, unsigned slot, gt_stream_ops_t *out) {
  gt_local_session_provider_t *p = context;
  if (!out || slot > 1 || !p->active || p->error != GT_OK)
    return GT_ERR_INVALID_ARG;
  child *c = &p->children[slot];
  if (c->borrowed || c->pid > 0)
    return GT_ERR_INVALID_ARG;
  return spawn_proxy(p, slot, out);
}
static void reset(void *context) {
  gt_local_session_provider_t *p = context;
  (void)tick(p);
  p->active = false;
  for (unsigned i = 0; i < 2; i++) {
    stop(p, &p->children[i]);
    p->children[i].borrowed = false;
  }
}
gt_status_t gt__local_session_provider_open(
    const gt_local_session_provider_config_t *config,
    const gt_allocator_t *allocator, gt_local_session_provider_t **out) {
  if (out)
    *out = NULL;
  if (!out || !config || !config->clock || !paths(config->proxy) ||
      !paths(config->daemon) || !paths(config->runtime))
    return GT_ERR_INVALID_ARG;
  gt_allocator_t a =
      allocator ? *allocator : (gt_allocator_t){NULL, allocate, release};
  if (!a.allocate || !a.release)
    return GT_ERR_INVALID_ARG;
  uint64_t now;
  if (!config->clock(config->clock_context, &now))
    return GT_ERR_IO;
  gt_local_session_provider_t *p = a.allocate(a.context, sizeof(*p));
  if (!p)
    return GT_ERR_IO;
  memset(p, 0, sizeof(*p));
  p->allocator = a;
  p->config = *config;
  p->now = now;
  strcpy(p->proxy, config->proxy);
  strcpy(p->daemon, config->daemon);
  strcpy(p->runtime, config->runtime);
  p->config.proxy = p->proxy;
  p->config.daemon = p->daemon;
  p->config.runtime = p->runtime;
  for (unsigned i = 0; i < 2; i++) {
    p->children[i].socket.fd = -1;
    p->children[i].diagnostic = -1;
  }
  *out = p;
  return GT_OK;
}
bool gt__local_session_provider_link(gt_local_session_provider_t *p,
                                     gt_session_link_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!p || !out)
    return false;
  *out = (gt_session_link_t){p, step, stream, reset};
  return true;
}
bool gt__local_session_provider_info(const gt_local_session_provider_t *p,
                                     gt_local_provider_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!p || !out)
    return false;
  *out = (gt_local_provider_info_t){
      .stderr_bytes = p->stderr_bytes, .error = p->error, .active = p->active};
  for (unsigned i = 0; i < 2; i++)
    if (p->children[i].pid > 0)
      out->children++;
  return true;
}
bool gt__local_session_provider_quiesce(gt_local_session_provider_t *p) {
  if (!p || p->active)
    return false;
  pump(p);
  for (unsigned i = 0; i < 2; i++)
    if (p->children[i].pid > 0 || p->children[i].borrowed)
      return false;
  return true;
}
bool gt__local_session_provider_destroy(gt_local_session_provider_t *p) {
  if (!p)
    return true;
  if (!gt__local_session_provider_quiesce(p))
    return false;
  gt_allocator_t a = p->allocator;
  memset(p, 0, sizeof(*p));
  a.release(a.context, p, sizeof(*p));
  return true;
}
