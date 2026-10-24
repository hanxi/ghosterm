/* Shared DATA restoration schema. Opaque snapshot bytes use HELLO compatibility. */
#ifndef GT_IPC_DATA_WIRE_H
#define GT_IPC_DATA_WIRE_H
#include "control.h"
enum {GT_MSG_SNAPSHOT_BEGIN=40,GT_MSG_SNAPSHOT_CHUNK=41,GT_MSG_SNAPSHOT_END=42,
  GT_MSG_READY_S=43,GT_MSG_STATE_EVENT=44,GT_MSG_LIVE_BEGIN=45,GT_MSG_READY_L=46,
  GT_MSG_SESSION_END=47,GT_MSG_DATA_PING=49,GT_MSG_DATA_PONG=50,
  GT_MSG_RESTORE_ERROR=51,GT_MSG_RESTORE_ERROR_ACK=52,GT_MSG_PROCESS_META=53,
  GT_DATA_MAX_CHUNK=65536,GT_DATA_MAX_OUTPUT=16384,GT_DATA_MAX_SNAPSHOT=64*1024*1024,
  GT_DATA_READONLY=1,GT_DATA_NO_SNAPSHOT=1};
typedef enum {GT_DATA_ERROR_CAPTURE=1,GT_DATA_ERROR_ENCODE,GT_DATA_ERROR_LIMIT,
  GT_DATA_ERROR_DEADLINE,GT_DATA_ERROR_REVOKED,GT_DATA_ERROR_PROTOCOL} gt_data_error_stage_t;
typedef struct {
  uint16_t type;uint64_t generation,sequence,total,offset,nonce;
  uint32_t format,flags,kind;const uint8_t *bytes;size_t length;
  gt_state_size_t size;gt_state_color_scheme_t color;gt_session_style_t style;
  gt_session_process_t process;
  gt_managed_info_t terminal;gt_status_t error;gt_data_error_stage_t stage;
} gt_data_message_t;
/* Flags/request_id zero; exact lengths/directions are also checked by router.
 * Decode clears POD on failure and borrows bytes through RX release only.
 * Encode clears header on failure, never sends native structs/pointers. */
bool gt__data_encode(const gt_data_message_t *,void *,size_t,gt_wire_header_t *);
bool gt__data_decode(const gt_wire_frame_t *,gt_data_message_t *);
#endif
