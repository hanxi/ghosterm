/* Private common USER protocol. No native layout, IO or engine dependency. */
#ifndef GT_IPC_USER_WIRE_H
#define GT_IPC_USER_WIRE_H
#include "wire.h"
#include "session/worker.h"
enum {GT_MSG_USER_INPUT=64,GT_MSG_USER_RESIZE=65,GT_MSG_USER_COLOR=66,
  GT_MSG_USER_REJECT=67,GT_MSG_USER_RESULT=68,GT_MSG_USER_STYLE=69,
  GT_USER_PREFIX_BYTES=24,GT_USER_MAX_INPUT=65536,GT_USER_REJECT_BYTES=32,GT_USER_RESULT_BYTES=40};
typedef struct {
  gt_worker_stamp_t stamp;gt_worker_command_t kind;
  const uint8_t *input;size_t length; /* borrow of frame, through release only */
  gt_state_size_t size;gt_state_color_scheme_t color;gt_session_style_t style;
} gt_user_request_t;
typedef enum {GT_USER_REJECT_BACKPRESSURE=1,GT_USER_REJECT_STOPPED,
  GT_USER_REJECT_INVALID,GT_USER_REJECT_NO_MEMORY,GT_USER_REJECT_UNSUPPORTED,
  GT_USER_REJECT_UNAVAILABLE} gt_user_reject_t;
typedef struct {
  gt_worker_command_t kind;gt_worker_stamp_t stamp;bool rejected;
  gt_user_reject_t rejection;gt_status_t status;gt_user_delivery_t delivery;
} gt_user_reply_t;
/* Exact type/direction/flags/zero request_id and fixed field lengths. Nonzero
 * generation/command ID; sequence 0 is valid. Cells >0, color 0/1. Pixel u32
 * values are preserved for host capability admission, not silently truncated.
 * Binary INPUT is 1..64KiB. Failure clears output. Decode borrows INPUT only. */
bool gt__user_request_decode(const gt_wire_frame_t *,gt_user_request_t *);
bool gt__user_request_encode(const gt_user_request_t *,void *,size_t,gt_wire_header_t *);
/* RESULT is an actual worker completion, never a queue ACK. REJECT is proven
 * pre-queue and consumes no command ID. Reply stamp/type echo this request.
 * OPERATION_OK iff status OK; attempted errors remain OUTCOME_UNKNOWN. */
bool gt__user_reply_encode(const gt_user_reply_t *,void *,size_t,gt_wire_header_t *);
bool gt__user_reply_decode(const gt_wire_frame_t *,gt_user_reply_t *);
#endif
