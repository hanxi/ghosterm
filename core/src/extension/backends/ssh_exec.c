#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/backends/ssh_exec.h"
#include "internal.h"
#include "ssh/wire.h"
#include <string.h>
#ifdef GT_HAVE_LIBSSH2
#include <errno.h>
#include <fcntl.h>
#include <libssh2.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* All libssh2 calls, buffers and cleanup belong to this one owner. The only
 * cross-thread operation is a flag + nonblocking wake; no raw session escapes. */
typedef enum { PUMP_NONE, PUMP_WRITE, PUMP_STDOUT, PUMP_STDERR } pump_call_t;
typedef struct {
  gt_bridge_stream_t base;
  gt_allocator_t allocator;
  gt_ssh_t *ssh;
  LIBSSH2_SESSION *session;
  LIBSSH2_CHANNEL *channel;
  gt_bridge_ssh_info_t info;
  int fd, wake[2];
  uint32_t stderr_limit;
  int64_t admission_end, operation_end;
  bool guard_deadline, disposing;
  bool wire_closed;
  int admission_pending; /* 1=open, 2=exec; survives FAILED/STOPPED snapshots */
  bool forwarding;
  unsigned depth;
  uint8_t direct[GT_BRIDGE_SSH_COMMAND_MAX+32];
  size_t direct_size;
  const atomic_bool *owner_stop;
  int owner_wake;
  size_t wire_left;
  pump_call_t blocked;
  uint8_t tx[GT_BRIDGE_IO_QUANTUM], rx[GT_BRIDGE_IO_QUANTUM], err[1024];
  size_t tx_size, tx_sent, rx_size, rx_used;
  char command[GT_BRIDGE_SSH_COMMAND_MAX + 1];
} ssh_stream_t;
static const gt_bridge_stream_ops_t ops;
static void *allocate(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
static int64_t milliseconds(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static gt_bridge_io_t io(gt_bridge_io_state_t state, size_t n, gt_status_t error) {
  return (gt_bridge_io_t){state, n, error};
}
static bool is_stopping(ssh_stream_t *s) {
  return atomic_load_explicit(&s->base.stopping, memory_order_acquire) ||
    (s->owner_stop && atomic_load_explicit(s->owner_stop,memory_order_acquire));
}
static void fail(ssh_stream_t *s, gt_status_t error) {
  if (s->info.phase == GT_BRIDGE_SSH_FAILED) return;
  s->info.failure_phase = s->info.phase;
  s->info.phase = GT_BRIDGE_SSH_FAILED; s->info.error = error;
}
/* Return negative errno, as required by libssh2's IO callbacks. Bound the
 * library's internal packet-drain loops too, not just our outer poll loop. */
static bool permitted(ssh_stream_t *s) {
  if (s->disposing) return true;
  if (is_stopping(s) || !s->wire_left) return false;
  if (s->guard_deadline) {
    int64_t now = milliseconds();
    if (now < 0) { fail(s, GT_ERR_IO); return false; }
    if (now >= s->operation_end) return false;
  }
  return true;
}
static LIBSSH2_RECV_FUNC(receive_callback) {
  (void)socket;
  ssh_stream_t *s = *abstract;
  if (!permitted(s)) return -EAGAIN;
  if (!s->disposing && length > s->wire_left) length = s->wire_left;
  ssize_t n = gt__ssh_wire_read(s->ssh, buffer, length, flags);
  if (!s->disposing && (n == 0 || (n < 0 && n != -EAGAIN && n != -EINTR))) {
    /* Let the library extract any already authenticated, buffered packets
     * before reporting the underlying socket end. TCP end alone is not EOF. */
    s->wire_closed = true;
    return -EAGAIN;
  }
  if (n > 0 && !s->disposing) s->wire_left -= (size_t)n;
  return n;
}
static LIBSSH2_SEND_FUNC(send_callback) {
  (void)socket;
  ssh_stream_t *s = *abstract;
  if (!permitted(s)) return -EAGAIN;
  if (!s->disposing && length > s->wire_left) length = s->wire_left;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  ssize_t n = gt__ssh_wire_write(s->ssh, buffer, length, flags);
  if (n > 0 && !s->disposing) s->wire_left -= (size_t)n;
  return n;
}
static void begin(ssh_stream_t *s, int ms, bool admission) {
  int64_t now = milliseconds();
  if (now < 0) { fail(s, GT_ERR_IO); return; }
  s->operation_end = now + ms;
  s->guard_deadline = ms != 0;
  if (admission && (ms == 0 || s->operation_end > s->admission_end)) {
    s->operation_end = s->admission_end; s->guard_deadline = true;
  }
  s->wire_left = 4 * GT_BRIDGE_IO_QUANTUM;
}
/* -1 error, 0 no budget left, 1 ready. HUP is fed through libssh2 so authenticated
 * channel EOF stays distinct from an unclean TCP reset/disconnect. */
static int wait_ready(ssh_stream_t *s, int ms) {
  if (ms == 0 || !s->wire_left) return 0;
  int64_t now = milliseconds();
  if (now < 0) { fail(s, GT_ERR_IO); return -1; }
  int remaining = now < s->operation_end ? (int)(s->operation_end - now) : 0;
  if (!remaining || is_stopping(s)) return 0;
  int dirs = libssh2_session_block_directions(s->session);
  short events = 0;
  if (dirs & LIBSSH2_SESSION_BLOCK_INBOUND) events |= POLLIN;
  if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND) events |= POLLOUT;
  if (!events) events = POLLIN;
  int wakes[2]={s->wake[0],s->owner_wake};
  int rc = gt__ssh_wire_wait(s->ssh,events,remaining,wakes,s->owner_wake>=0?2:1);
  if (rc < 0) { fail(s, GT_ERR_IO); return -1; }
  return rc > 0;
}
static LIBSSH2_CHANNEL *open_channel(ssh_stream_t *s) {
  if(s->forwarding)return libssh2_channel_open_ex(s->session,"direct-tcpip",12,
    131072,GT_BRIDGE_IO_QUANTUM,(const char *)s->direct,(unsigned)s->direct_size);
  return libssh2_channel_open_ex(s->session, "session", 7, 131072, GT_BRIDGE_IO_QUANTUM, NULL, 0);
}
static gt_status_t mapped_error(int rc, bool admission) {
  if (admission && rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED)
    return GT_ERR_UNSUPPORTED;
  /* The library collapses channel-open reason codes (including resource
   * shortage) into CHANNEL_FAILURE. Do not invent a capability/policy denial. */
  if (rc == LIBSSH2_ERROR_CHANNEL_FAILURE) return GT_ERR_IO;
  if (rc == LIBSSH2_ERROR_SOCKET_SEND || rc == LIBSSH2_ERROR_SOCKET_RECV ||
      rc == LIBSSH2_ERROR_SOCKET_DISCONNECT || rc == LIBSSH2_ERROR_TIMEOUT) return GT_ERR_IO;
  return GT_ERR_PROTOCOL;
}
gt_bridge_ssh_phase_t gt__bridge_ssh_step(gt_bridge_stream_t *base, int ms) {
  if (!base || base->ops != &ops || ms < 0) return GT_BRIDGE_SSH_FAILED;
  ssh_stream_t *s = (ssh_stream_t *)base;
  if (base->closed) return GT_BRIDGE_SSH_CLOSED;
  if (is_stopping(s)) { s->info.phase = GT_BRIDGE_SSH_STOPPED; s->info.error = GT_ERR_CANCELLED; return s->info.phase; }
  if (s->info.phase >= GT_BRIDGE_SSH_READY) return s->info.phase;
  begin(s, ms, true);
  for (;;) {
    int64_t now = milliseconds();
    if (now < 0) { fail(s, GT_ERR_IO); break; }
    if (now >= s->admission_end) { s->info.admission_expired = true; fail(s, GT_ERR_IO); break; }
    if (is_stopping(s)) { s->info.phase = GT_BRIDGE_SSH_STOPPED; s->info.error = GT_ERR_CANCELLED; break; }
    if (s->info.phase == GT_BRIDGE_SSH_FAILED) break;
    int rc;
    if (s->info.phase == GT_BRIDGE_SSH_CHANNEL) {
      s->admission_pending = 1;
      s->channel = open_channel(s);
      rc = s->channel ? 0 : libssh2_session_last_errno(s->session);
      if (rc != LIBSSH2_ERROR_EAGAIN) s->admission_pending = 0;
      if (!rc && s->channel) {
        s->info.phase = s->forwarding ? GT_BRIDGE_SSH_READY : GT_BRIDGE_SSH_EXEC;
        if(s->forwarding)break;
        continue;
      }
    } else {
      /* Intentionally no request_pty, shell or extended-data merge. */
      s->admission_pending = 2;
      rc = libssh2_channel_exec(s->channel, s->command);
      if (rc != LIBSSH2_ERROR_EAGAIN) s->admission_pending = 0;
      if (!rc) { s->info.phase = GT_BRIDGE_SSH_READY; break; }
    }
    if (rc != LIBSSH2_ERROR_EAGAIN) { fail(s, mapped_error(rc, true)); break; }
    if (s->wire_closed) { fail(s, GT_ERR_IO); break; }
    if (wait_ready(s, ms) <= 0) break;
  }
  return s->info.phase;
}
/* Drain one library call. Only an outbound EAGAIN pins this exact call: a
 * different API cannot finish libssh2's partially transmitted packet. All
 * buffers are internal, so read-side progress can safely flush queued writes. */
static int pump_call(ssh_stream_t *s, pump_call_t call) {
  ssize_t n;
  if (call == PUMP_WRITE) {
    n = libssh2_channel_write_ex(s->channel, 0, (const char *)s->tx + s->tx_sent, s->tx_size - s->tx_sent);
    if (n > 0) {
      if ((size_t)n > s->tx_size - s->tx_sent) { fail(s, GT_ERR_INTERNAL); return -1; }
      s->tx_sent += (size_t)n;
    }
  } else if (call == PUMP_STDOUT) {
    n = libssh2_channel_read_ex(s->channel, 0, (char *)s->rx, sizeof(s->rx));
    if (n > 0) { s->rx_size = (size_t)n; s->rx_used = 0; }
  } else {
    n = libssh2_channel_read_ex(s->channel, SSH_EXTENDED_DATA_STDERR, (char *)s->err, sizeof(s->err));
    if (n > 0) {
      if(s->forwarding){fail(s,GT_ERR_PROTOCOL);return -1;}
      s->info.stderr_bytes += (size_t)n;
      size_t keep = sizeof(s->info.diagnostic) - s->info.diagnostic_size;
      if (keep > (size_t)n) keep = (size_t)n;
      memcpy(s->info.diagnostic + s->info.diagnostic_size, s->err, keep);
      s->info.diagnostic_size += keep;
      s->info.diagnostic_truncated = s->info.stderr_bytes > s->info.diagnostic_size;
      if (s->info.stderr_bytes > s->stderr_limit) { fail(s, GT_ERR_PROTOCOL); return -1; }
    }
  }
  if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
    int dirs = libssh2_session_block_directions(s->session);
    s->blocked = (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? call : PUMP_NONE;
    return 0;
  }
  s->blocked = PUMP_NONE;
  if (n < 0) {
    /* Confirmed channel EOF may arrive with a subsequent TCP close. Never
     * convert an arbitrary reset into EOF; queued stdout must be delivered. */
    if (call != PUMP_WRITE && libssh2_channel_eof(s->channel)) return 0;
    fail(s, mapped_error((int)n, false)); return -1;
  }
  return 1;
}
static int pump(ssh_stream_t *s) {
  if (s->blocked != PUMP_NONE) return pump_call(s, s->blocked);
  if (s->tx_sent < s->tx_size) {
    int rc = pump_call(s, PUMP_WRITE);
    if (rc < 0 || s->blocked != PUMP_NONE) return rc;
  }
  int err = pump_call(s, PUMP_STDERR);
  if (err < 0 || s->blocked != PUMP_NONE) return err;
  if (s->rx_used < s->rx_size) return 1;
  int out = pump_call(s, PUMP_STDOUT);
  if (out < 0) return out;
  /* n==0 without channel_eof is idle, not an end-of-stream fact. */
  if (s->rx_used == s->rx_size && libssh2_channel_eof(s->channel)) s->info.remote_eof = true;
  return out > 0 || err > 0;
}
static gt_bridge_io_t transfer(ssh_stream_t *s, void *p, size_t n, int ms, bool writing) {
  if (s->info.phase == GT_BRIDGE_SSH_FAILED) return io(GT_BRIDGE_ERROR, 0, s->info.error);
  if (s->info.phase != GT_BRIDGE_SSH_READY) return io(GT_BRIDGE_ERROR, 0, GT_ERR_INVALID_ARG);
  begin(s, ms, false);
  for (;;) {
    if (is_stopping(s)) {
      s->info.phase = GT_BRIDGE_SSH_STOPPED; s->info.error = GT_ERR_CANCELLED;
      return io(GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED);
    }
    if (s->info.phase == GT_BRIDGE_SSH_FAILED) return io(GT_BRIDGE_ERROR, 0, s->info.error);
    if (writing && s->info.remote_eof) return io(GT_BRIDGE_CLOSED, 0, GT_ERR_IO);
    if (!writing && s->rx_used < s->rx_size) {
      size_t count = s->rx_size - s->rx_used;
      if (count > n) count = n;
      memcpy(p, s->rx + s->rx_used, count); s->rx_used += count;
      return io(GT_BRIDGE_DATA, count, GT_OK);
    }
    if (writing && s->tx_sent == s->tx_size) {
      if (s->info.remote_eof) return io(GT_BRIDGE_CLOSED, 0, GT_ERR_IO);
      memcpy(s->tx, p, n); s->tx_size = n; s->tx_sent = 0;
      /* Accepted into this bounded owned queue; not remote delivery. */
      (void)pump(s);
      if (s->info.phase == GT_BRIDGE_SSH_FAILED) return io(GT_BRIDGE_ERROR, 0, s->info.error);
      return io(GT_BRIDGE_DATA, n, GT_OK);
    }
    if (!writing && s->info.remote_eof) return io(GT_BRIDGE_EOF, 0, GT_OK);
    int progress = pump(s);
    if (progress < 0) return io(GT_BRIDGE_ERROR, 0, s->info.error);
    if ((!writing && s->rx_used < s->rx_size) ||
        (writing && s->tx_sent == s->tx_size) || s->info.remote_eof) continue;
    if (s->wire_closed) { fail(s, GT_ERR_IO); return io(GT_BRIDGE_ERROR, 0, GT_ERR_IO); }
    int64_t now = milliseconds();
    if (now < 0) { fail(s, GT_ERR_IO); return io(GT_BRIDGE_ERROR, 0, GT_ERR_IO); }
    if (!ms || now >= s->operation_end || !s->wire_left) return io(GT_BRIDGE_WAIT, 0, GT_OK);
    if (!progress && wait_ready(s, ms) <= 0) {
      if (s->info.phase == GT_BRIDGE_SSH_FAILED) return io(GT_BRIDGE_ERROR, 0, s->info.error);
      if (is_stopping(s)) continue;
      return io(GT_BRIDGE_WAIT, 0, GT_OK);
    }
  }
}
static gt_bridge_io_t stream_read(gt_bridge_stream_t *base, void *p, size_t n, int ms) {
  return transfer((ssh_stream_t *)base, p, n, ms, false);
}
static gt_bridge_io_t stream_write(gt_bridge_stream_t *base, const void *p, size_t n, int ms) {
  return transfer((ssh_stream_t *)base, (void *)p, n, ms, true);
}
static void wake(gt_bridge_stream_t *base) {
  ssh_stream_t *s = (ssh_stream_t *)base;
  uint8_t byte = 1;
  while (write(s->wake[1], &byte, 1) < 0 && errno == EINTR) {}
}
static gt_bridge_close_t close_step(gt_bridge_stream_t *base, int ms) {
  (void)ms;
  ssh_stream_t *s = (ssh_stream_t *)base;
  s->disposing = true;
  gt__ssh_abort(s->ssh); /* dedicated socket only, before library cleanup */
  /* Resume a partial admission with its original arguments so libssh2 drops
   * its pending open/exec allocation, including an unconfirmed open channel. */
  if (s->admission_pending == 1) s->channel = open_channel(s);
  else if (s->admission_pending == 2) (void)libssh2_channel_exec(s->channel, s->command);
  s->admission_pending = 0;
  if (s->channel) {
    if (s->blocked != PUMP_NONE) (void)pump_call(s, s->blocked);
    /* Make the shut-down socket visible to the library before session_free. */
    (void)libssh2_channel_read_ex(s->channel, 0, (char *)s->rx, sizeof(s->rx));
  }
  bool released = false;
  gt_status_t status = gt__ssh_dispose_step(&s->ssh, &released);
  if (status != GT_OK) return GT_BRIDGE_CLOSE_ERROR;
  if (!released) return GT_BRIDGE_DRAINING;
  s->session = NULL; s->channel = NULL;
  s->info.phase = GT_BRIDGE_SSH_CLOSED;
  return GT_BRIDGE_RELEASED;
}
static void destroy(gt_bridge_stream_t *base) {
  ssh_stream_t *s = (ssh_stream_t *)base;
  close(s->wake[0]); close(s->wake[1]);
  gt_allocator_t a = s->allocator;
  gt__plugin_memory_free(&a,s, sizeof(*s));
}
static const gt_bridge_stream_ops_t ops = {stream_read, stream_write, wake, close_step, destroy};

static gt_status_t stream_open(gt_ssh_t **owned, const char *command,uint16_t target_port,
  const gt_bridge_ssh_options_t *options, const gt_allocator_t *allocator, gt_bridge_stream_t **out) {
  if (out) *out = NULL;
  if (!owned || !*owned || !out || !command) return GT_ERR_INVALID_ARG;
  size_t length = strnlen(command, GT_BRIDGE_SSH_COMMAND_MAX + 1);
  if (!length || length > GT_BRIDGE_SSH_COMMAND_MAX) return GT_ERR_INVALID_ARG;
  gt_bridge_ssh_options_t o = options ? *options : (gt_bridge_ssh_options_t){5000, 262144};
  gt_allocator_t a = allocator ? *allocator : (gt_allocator_t){NULL, allocate, release};
  if (!o.admission_ms || o.admission_ms > 60000 || !o.stderr_limit ||
      o.stderr_limit > GT_BRIDGE_SSH_STDERR_MAX || !a.allocate || !a.release) return GT_ERR_INVALID_ARG;
  void *session = NULL; int fd = -1;
  if (gt__ssh_stream_claim(*owned, &session, &fd, false) != GT_OK || !session || fd < 0) return GT_ERR_INVALID_ARG;
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || !(flags & O_NONBLOCK)) return GT_ERR_INVALID_ARG;
  int64_t now = milliseconds();
  if (now < 0) return GT_ERR_IO;
  ssh_stream_t *s = gt__plugin_memory_alloc(&a,sizeof(*s));
  if (!s) return GT_ERR_IO;
  memset(s, 0, sizeof(*s)); s->wake[0] = s->wake[1] = -1;
  s->allocator = a; s->stderr_limit = o.stderr_limit; s->admission_end = now + o.admission_ms;
  s->owner_wake=-1;s->depth=gt__ssh_wire_depth(*owned);s->forwarding=target_port!=0;
  if(target_port) {
    uint32_t values[4]={(uint32_t)length,target_port,9,0};
    size_t at=0;
    for(size_t i=0;i<4;i++) {
      uint32_t v=values[i];s->direct[at++]=(uint8_t)(v>>24);s->direct[at++]=(uint8_t)(v>>16);
      s->direct[at++]=(uint8_t)(v>>8);s->direct[at++]=(uint8_t)v;
      if(i==0){memcpy(s->direct+at,command,length);at+=length;}
      if(i==2){memcpy(s->direct+at,"127.0.0.1",9);at+=9;}
    }
    s->direct_size=at;
  }
  s->info.phase = GT_BRIDGE_SSH_CHANNEL; s->fd = fd;
  memcpy(s->command, command, length + 1);
  if (pipe(s->wake)) goto failed;
  for (int i = 0; i < 2; i++) {
    if (fcntl(s->wake[i], F_SETFL, O_NONBLOCK) < 0 || fcntl(s->wake[i], F_SETFD, FD_CLOEXEC) < 0) goto failed;
  }
#ifdef SO_NOSIGPIPE
  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one))) goto failed;
#endif
  (void)gt__bridge_stream_init(&s->base, &ops);
  if (gt__ssh_stream_claim(*owned, &session, &fd, true) != GT_OK) goto failed;
  s->ssh = *owned; *owned = NULL; s->session = session;
  libssh2_session_set_blocking(s->session, 0);
  *libssh2_session_abstract(s->session) = s;
  libssh2_session_callback_set2(s->session, LIBSSH2_CALLBACK_RECV, (libssh2_cb_generic *)receive_callback);
  libssh2_session_callback_set2(s->session, LIBSSH2_CALLBACK_SEND, (libssh2_cb_generic *)send_callback);
  *out = &s->base;
  return GT_OK;
failed:
  if (s->wake[0] >= 0) close(s->wake[0]);
  if (s->wake[1] >= 0) close(s->wake[1]);
  gt__plugin_memory_free(&a,s, sizeof(*s)); return GT_ERR_IO;
}
gt_status_t gt__bridge_ssh_open(gt_ssh_t **owned,const char *command,
 const gt_bridge_ssh_options_t *options,const gt_allocator_t *allocator,gt_bridge_stream_t **out) {
  return stream_open(owned,command,0,options,allocator,out);
}
gt_status_t gt__bridge_ssh_forward_open(gt_ssh_t **owned,const char *host,uint16_t port,
 const gt_bridge_ssh_options_t *options,const gt_allocator_t *allocator,gt_bridge_stream_t **out) {
  if(out)*out=NULL;
  if(!port || !host || strnlen(host,256)>255)return GT_ERR_INVALID_ARG;
  for(const unsigned char *p=(const unsigned char *)host;*p;p++)if(*p<33 || *p==127)return GT_ERR_INVALID_ARG;
  return stream_open(owned,host,port,options,allocator,out);
}
gt_status_t gt__bridge_ssh_attach_stop(gt_bridge_stream_t *base,const atomic_bool *stop,int wake) {
  if(!base || base->ops!=&ops || base->closed || !stop || wake<0)return GT_ERR_INVALID_ARG;
  ssh_stream_t *s=(ssh_stream_t *)base;s->owner_stop=stop;s->owner_wake=wake;return GT_OK;
}
int gt__bridge_ssh_descriptor(const gt_bridge_stream_t *base) {
  return !base || base->ops!=&ops || base->closed ? -1 : ((const ssh_stream_t *)base)->fd;
}
unsigned gt__bridge_ssh_depth(const gt_bridge_stream_t *base) {
  return !base || base->ops!=&ops ? 0 : ((const ssh_stream_t *)base)->depth;
}
bool gt__bridge_ssh_forward_ready(const gt_bridge_stream_t *base) {
  if(!base || base->ops!=&ops || base->closed)return false;
  const ssh_stream_t *s=(const ssh_stream_t *)base;
  return s->forwarding && s->info.phase==GT_BRIDGE_SSH_READY &&
    !atomic_load_explicit(&base->stopping,memory_order_acquire);
}
int gt__bridge_ssh_wait(gt_bridge_stream_t *base,short requested,int ms,const int *wakes,size_t count) {
  if(!gt__bridge_ssh_forward_ready(base))return -1;
  ssh_stream_t *s=(ssh_stream_t *)base;
  if(is_stopping(s))return 0;
  if((requested&POLLIN) && s->rx_used<s->rx_size)return 1;
  if((requested&POLLOUT) && s->tx_sent==s->tx_size)return 1;
  int dirs=libssh2_session_block_directions(s->session);short events=0;
  if(dirs&LIBSSH2_SESSION_BLOCK_INBOUND)events|=POLLIN;
  if(dirs&LIBSSH2_SESSION_BLOCK_OUTBOUND)events|=POLLOUT;
  if(!events)events=POLLIN;
  return gt__ssh_wire_wait(s->ssh,events,ms,wakes,count);
}
bool gt__bridge_ssh_info(const gt_bridge_stream_t *base, gt_bridge_ssh_info_t *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!base || base->ops != &ops || !out) return false;
  *out = ((const ssh_stream_t *)base)->info;
  return true;
}
#else
gt_status_t gt__bridge_ssh_forward_open(gt_ssh_t **s,const char *h,uint16_t p,const gt_bridge_ssh_options_t *o,const gt_allocator_t *a,gt_bridge_stream_t **r){(void)s;(void)h;(void)p;(void)o;(void)a;if(r)*r=NULL;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__bridge_ssh_attach_stop(gt_bridge_stream_t *s,const atomic_bool *a,int f){(void)s;(void)a;(void)f;return GT_ERR_UNSUPPORTED;}
int gt__bridge_ssh_descriptor(const gt_bridge_stream_t *s){(void)s;return -1;}
unsigned gt__bridge_ssh_depth(const gt_bridge_stream_t *s){(void)s;return 0;}
bool gt__bridge_ssh_forward_ready(const gt_bridge_stream_t *s){(void)s;return false;}
int gt__bridge_ssh_wait(gt_bridge_stream_t *s,short e,int t,const int *f,size_t n){(void)s;(void)e;(void)t;(void)f;(void)n;return -1;}
gt_status_t gt__bridge_ssh_open(gt_ssh_t **owned, const char *command,
  const gt_bridge_ssh_options_t *options, const gt_allocator_t *allocator, gt_bridge_stream_t **out) {
  (void)owned; (void)command; (void)options; (void)allocator;
  if (out) *out = NULL;
  return GT_ERR_UNSUPPORTED;
}
gt_bridge_ssh_phase_t gt__bridge_ssh_step(gt_bridge_stream_t *base, int ms) {
  (void)base; (void)ms; return GT_BRIDGE_SSH_FAILED;
}
bool gt__bridge_ssh_info(const gt_bridge_stream_t *base, gt_bridge_ssh_info_t *out) {
  (void)base; if (out) memset(out, 0, sizeof(*out)); return false;
}
#endif
