/* Private Unix attachment socket owner; no framing, authentication or engine. */
#ifndef GT_ATTACHMENT_PEER_H
#define GT_ATTACHMENT_PEER_H
#include "session/attachment.h"
typedef struct gt_attachment_peer gt_attachment_peer_t;
typedef enum {
  GT_PEER_DATA, GT_PEER_WAIT, GT_PEER_CLOSED, GT_PEER_ERROR,
  GT_PEER_STALE, GT_PEER_INVALID
} gt_peer_kind_t;
typedef struct {gt_peer_kind_t kind;size_t length;int system_error;} gt_peer_result_t;
/* Adopts a connected Unix stream socket for an already authorized generation.
 * Authentication/token validation belongs to the future listener, BEFORE this
 * call. No aliases/readers/writers of fd may remain. Success transfers fd
 * ownership, enables NONBLOCK/CLOEXEC and (on Apple) NOSIGPIPE. Failure leaves
 * ownership with caller. Attachment outlives peer and all calls.
 * All peer calls belong to one non-UI I/O owner, including close. No thread or
 * unbounded queue is created. The owner MUST receive regularly even when idle
 * or output-backed-up, so a dead client cannot retain its attachment slot. */
gt_status_t gt__peer_adopt(gt_session_attachment_t *,uint64_t generation,int fd,gt_attachment_peer_t **);
/* Optional domain covers the peer object; NULL = libc. Context outlives close.
 * Allocation/validation failure leaves fd flags/ownership and generation as-is.
 * Terminal I/O closes fd/revokes generation, retaining the object charge until
 * explicit close. All allocation callbacks run on the same I/O owner. */
gt_status_t gt__peer_adopt_with_allocator(gt_session_attachment_t *,uint64_t,int,const gt_allocator_t *,gt_attachment_peer_t **);
/* One socket operation, timeout 0..1000 ms, monotonic across EINTR. DATA length
 * means bytes read/sent, NOT a frame accepted by session. Short writes are
 * explicit: caller owns the unsent suffix, never silently queues/replays it.
 * WAIT leaves attachment live. HUP/EOF/error close fd and revoke this generation
 * before returning; unread bytes on HUP are discarded. A stale owner closes
 * only its socket, never detaches a newer generation. Terminal results stick.
 * Owner also destroys/cancels its framing buffers and restore leases on terminal
 * result, quiesces sends, and releases leases via attachment_release. */
gt_peer_result_t gt__peer_receive(gt_attachment_peer_t *,void *,size_t,int timeout_ms);
gt_peer_result_t gt__peer_send(gt_attachment_peer_t *,const void *,size_t,int timeout_ms);
void gt__peer_close(gt_attachment_peer_t *);
#endif
