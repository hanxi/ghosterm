/* POSIX host composition shared by local IPC and an SSH connection proxy. */
#ifndef GT_DAEMON_HOST_H
#define GT_DAEMON_HOST_H
#include "ipc/unix-endpoint.h"
#include "ipc/data-router.h"
typedef struct gt_session_host gt_session_host_t;
typedef struct {
  const char *directory; /* consumed during open only */
  gt_protocol_identity_t identity;
  gt_runtime_state_factory_t state;gt_snapshot_encoder_t encoder;
  size_t sessions,connections,snapshot_limit;
  uint32_t snapshot_format;
  gt_thread_options_t threads; /* worker/backend/reaper, explicit admission */
} gt_session_host_config_t;
/* One non-UI IO owner. Mandatory thread-safe allocator and borrowed factory /
 * encoder contexts survive successful close. Endpoint authenticates kernel UID
 * on every accept. Listener is published only after service/reaper preparation.
 * No engine, SSH, UI, launch or daemonization policy in this composition. */
gt_status_t gt__session_host_open(const gt_session_host_config_t *,const gt_allocator_t *,gt_session_host_t **);
/* One bounded service round, one accept attempt and poll (0..1000ms). wake_fd
 * is borrowed, -1 or caller's readable cancellation fd; never consumed/closed.
 * Timers and async DATA work progress even with no socket activity. Returns
 * explicitly on service/reaper/poll failure. Never joins workers on this path. */
gt_status_t gt__session_host_poll(gt_session_host_t *,int wake_fd,int timeout_ms);
/* IO callers quiesced. First call removes listener, detaches peers, stops and
 * joins independent reaper. BUSY retains host while async router work drains;
 * repeat off UI. Completed shutdown clears caller pointer, including an
 * INTERNAL endpoint-cleanup error; other failures retain ownership.
 * Completed shutdown stops/joins original workers, frees all owned state.
 * This is explicit host shutdown, never a client disconnect action. */
gt_runtime_rc_t gt__session_host_close(gt_session_host_t **);
#endif
