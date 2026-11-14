/* Private candidate protocol for trusted Node/Exec child processes. This is NOT
 * the frozen SSH wire envelope, an OS sandbox or runtime execution permission. */
#ifndef GT_PRIVATE_PROCESS_RPC_H
#define GT_PRIVATE_PROCESS_RPC_H
#include "ghosterm/extension_rpc.h"
#include "base/allocator.h"
#include "extension/runtime-resource.h"
#define GT_PROCESS_RPC_MESSAGE_MAX 8192u
typedef struct gt_process_rpc gt_process_rpc_t;

typedef enum {
    GT_PROCESS_RPC_OK,
    GT_PROCESS_RPC_WAIT,
    GT_PROCESS_RPC_INVALID,
    GT_PROCESS_RPC_PROTOCOL
} gt_process_rpc_status_t;

/* Original serial non-UI owner. Borrows an exclusive ACTIVE Executor Session
 * with requests enabled, no prior RPC/resource/source owner; holds a source
 * reservation so the native caller cannot destroy Session first. The native
 * launcher separately verifies package/runtime identity and authorizes exec.
 * No process is started, no business permission granted, no lease is exposed.
 * Allocator context outlives destroy; failure clears out without adoption. */
gt_status_t gt__process_rpc_open(gt_ext_session_t *, const gt_allocator_t *, gt_process_rpc_t **);
/* Same admission, optionally borrowing an unused resource owner bound to this
 * Session. It must outlive the broker; native caller steps/stops/destroys it. */
gt_status_t gt__process_rpc_open_resources(gt_ext_session_t *,
                                           gt_runtime_resources_t *,
                                           const gt_allocator_t *,
                                           gt_process_rpc_t **);
/* One complete child JSON payload, 1..8192 bytes, carried in uint32 BE frames by
 * the pipe owner. Strict UTF-8/duplicate keys/depth/node/work budgets. v=1, id is
 * a canonical decimal string exactly one greater than the last accepted id.
 * Unknown fields/op/version or replay are terminal and revoke the Session.
 * One pending reply: WAIT consumes nothing and performs no operation. Accepted
 * operations never replay; the reply's status is the native RPC status, not a
 * delivery receipt. Input is borrowed only for this call, no allocation here. */
gt_process_rpc_status_t gt__process_rpc_call(gt_process_rpc_t *, const void *, size_t);
/* Borrowed ready reply until consume/destroy. Cleared outputs when unavailable.
 * Copy or finish the entire pipe frame before consume. No data/logs on stderr. */
bool gt__process_rpc_reply(const gt_process_rpc_t *, const uint8_t **, size_t *);
gt_process_rpc_status_t gt__process_rpc_consume(gt_process_rpc_t *);
/* Owner-thread revocation. Existing result/forget observations remain available
 * under the original lease; new business effects are rejected by native Core. */
void gt__process_rpc_revoke(gt_process_rpc_t *);
/* One-shot child activation receipt, not a business operation receipt. */
bool gt__process_rpc_activated(const gt_process_rpc_t *);
/* Revoke first, forget owned outgoing observations, release reservation and
 * wipe local refs/JSON. Does not close/destroy Session or stop/reap a process;
 * caller must stop the child before releasing its pipe owner. Consumes owner. */
gt_status_t gt__process_rpc_destroy(gt_process_rpc_t **);
#endif
