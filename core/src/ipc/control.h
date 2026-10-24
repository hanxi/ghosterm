/* Private management schema. The same dispatcher serves local and SSH peers. */
#ifndef GT_IPC_CONTROL_H
#define GT_IPC_CONTROL_H
#include "wire.h"
#include "session/manager.h"
#include "session/creation.h"
#include "attachment-owner.h"
enum {
  GT_MSG_LIST=16,GT_MSG_LIST_REPLY=17,GT_MSG_INFO=18,GT_MSG_INFO_REPLY=19,
  GT_MSG_TERMINATE=20,GT_MSG_TERMINATE_REPLY=21,GT_MSG_PING=22,
  GT_MSG_PING_REPLY=23,GT_MSG_CONTROL_ERROR=24,
  GT_MSG_CREATE=32,GT_MSG_CREATE_REPLY=33,GT_MSG_QUERY_CREATE=34,GT_MSG_QUERY_CREATE_REPLY=35,
  GT_MSG_ATTACH=36,GT_MSG_ATTACH_REPLY=37,GT_MSG_DETACH=48,
  GT_CREATE_REPLY_BYTES=32,GT_ATTACH_REPLY_BYTES=88,
  GT_CONTROL_INFO_BYTES=64,GT_CONTROL_MAX_ROWS=64,
  GT_CONTROL_REPLY_BYTES=8+GT_CONTROL_MAX_ROWS*GT_CONTROL_INFO_BYTES
};
typedef enum {GT_CONTROL_OK,GT_CONTROL_NOT_FOUND,GT_CONTROL_BUSY,
  GT_CONTROL_UNSUPPORTED,GT_CONTROL_INTERNAL} gt_control_status_t;
typedef enum {GT_CONTROL_REPLY,GT_CONTROL_MALFORMED,GT_CONTROL_NO_SPACE} gt_control_rc_t;
typedef struct {
  gt_session_manager_t *manager;gt_creation_t *creation;gt_attachment_owner_t *attachments;
  uint64_t principal,issuer,now_ms;
} gt_control_context_t;
/* Exact 64-byte big-endian row, no C layout/pointers. Unknown result/sequence/
 * exit remains unavailable. Outputs clear on error. */
bool gt__control_info_encode(const gt_managed_info_t *,uint8_t out[GT_CONTROL_INFO_BYTES]);
bool gt__control_info_decode(const void *,size_t,gt_managed_info_t *);
/* One serial management owner; full scratch capacity required before doing
 * anything, including CREATE/TERMINATE. No joins or backend/engine factories.
 * CREATE performs bounded metadata preparation and manager admission here.
 * Request IDs must be nonzero; connection owner enforces monotonically rising
 * IDs. Unsupported requests get explicit ERROR; malformed known requests or
 * server-direction frames must close the connection without executing them.
 * Success copies one bounded reply, independent of request's borrowed payload.
 * CREATE/QUERY_CREATE require a creation ledger; NULL returns UNSUPPORTED.
 * principal is trusted stream identity, never a field in the request. */
gt_control_rc_t gt__control_process(const gt_control_context_t *,const gt_wire_frame_t *,
  uint8_t *scratch,size_t capacity,gt_wire_header_t *reply);
bool gt__control_creation_decode(const void *,size_t,uint8_t nonce[16],gt_creation_result_t *);
bool gt__control_attach_encode(const gt_attach_result_t *,uint8_t out[GT_ATTACH_REPLY_BYTES]);
bool gt__control_attach_decode(const void *,size_t,gt_attach_result_t *);
#endif
