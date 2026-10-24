/* Shared local/remote USER data-path owner, independent of engine and socket. */
#ifndef GT_IPC_USER_DISPATCHER_H
#define GT_IPC_USER_DISPATCHER_H
#include "server.h"
#include "user-wire.h"
typedef struct gt_user_dispatcher gt_user_dispatcher_t;
typedef struct {
  gt_session_runtime_t *runtime;gt_attachment_owner_t *attachments;
  uint8_t instance[16];size_t connections,commands; /* 1..64 / 1..16 */
} gt_user_dispatcher_config_t;
/* One fixed allocation of peer/command mailboxes, one mutex, no IO thread.
 * Single non-UI IO owner calls all APIs; worker callbacks copy result POD only.
 * Domain/runtime/attachment owner outlive successful close. Owns a runtime
 * reference per occupied peer, including disconnected peers with pending work.
 * Host configures connection RX >=65560 / TX >=40 for the full USER schema. */
gt_status_t gt__user_dispatcher_open(const gt_user_dispatcher_config_t *,const gt_allocator_t *,gt_user_dispatcher_t **);
/* Server-compatible USER-only dispatcher. Service steps connection first and
 * must install drop below. Does not implement snapshots/READY/output; a full
 * data dispatcher must route restoration types separately on this same owner.
 * One request and one response at most; no joins/IO waits/UI queue. Malformed,
 * stale or exhausted peer/mailbox capacity closes this data connection only.
 * Worker queue backpressure gets explicit REJECT. Accepted input is copied by
 * worker before RX release. No queued ACK, automatic replay or input backlog. */
gt_server_data_rc_t gt__user_dispatcher_step(void *,gt_connection_t *,const gt_token_scope_t *);
/* Server retirement hook: after generation revoked, before connection freed.
 * Marks only the exact scope disconnected; no worker stop, join or token call.
 * Completion contexts survive disconnect and cannot write to any connection. */
void gt__user_dispatcher_drop(void *,const gt_token_scope_t *);
/* Bounded sweep, even with no peers/traffic. Releases disconnected leases only
 * after every callback's last mailbox access; active replies remain retained. */
void gt__user_dispatcher_reap(gt_user_dispatcher_t *);
/* Requires server/data callers quiesced. BUSY while active or callback pending,
 * preserving object; call after server close and reap, never from a callback. */
gt_runtime_rc_t gt__user_dispatcher_close(gt_user_dispatcher_t *);
#endif
