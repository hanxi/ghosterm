/* Private parent/child readiness channel, never stdin/stdout or GTSD traffic. */
#ifndef GT_DAEMON_STARTUP_WIRE_H
#define GT_DAEMON_STARTUP_WIRE_H
#include "ipc/bootstrap-wire.h"
#include "ghosterm/types.h"
enum {GT_STARTUP_READY_BYTES=72,GT_STARTUP_ACK_BYTES=24};
bool gt__startup_ready_encode(gt_status_t,const gt_protocol_identity_t *,uint8_t out[GT_STARTUP_READY_BYTES]);
bool gt__startup_ready_decode(const uint8_t in[GT_STARTUP_READY_BYTES],gt_status_t *,gt_protocol_identity_t *);
void gt__startup_ack_encode(const uint8_t instance[16],uint8_t out[GT_STARTUP_ACK_BYTES]);
bool gt__startup_ack_valid(const uint8_t in[GT_STARTUP_ACK_BYTES],const uint8_t instance[16]);
void gt__startup_commit_encode(const uint8_t instance[16],uint8_t out[GT_STARTUP_ACK_BYTES]);
bool gt__startup_commit_valid(const uint8_t in[GT_STARTUP_ACK_BYTES],const uint8_t instance[16]);
/* Fresh detached helper only. FD3 is a connected private Unix stream from its
 * parent. No service IO/CREATE is allowed before a complete ACK. On failure the
 * caller shuts down the still-unpublished host. Monotonic whole transaction. */
bool gt__startup_serve(int fd,gt_status_t,const gt_protocol_identity_t *,uint32_t timeout_ms,
  void *context,bool (*cancelled)(void *));
#endif
