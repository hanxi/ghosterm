/* Private request class. One non-UI owner; validation callbacks are trusted,
 * bounded, pure and non-reentrant. JSON views are borrowed until the next
 * mutating owner call/step, forget or destroy; copy before asynchronous use.
 * No worker or side effect here. */
#ifndef GT_BRIDGE_RPC_H
#define GT_BRIDGE_RPC_H
#include "extension/wire.h"
#define GT_BRIDGE_RPC_SLOTS 64u
#define GT_BRIDGE_RPC_BODY_MAX 3072u
#define GT_BRIDGE_RPC_FRAME_MAX 4096u
#define GT_BRIDGE_RPC_WINDOW 8192u
typedef bool (*gt_bridge_rpc_validate_t)(void *,const uint8_t *,size_t);
typedef struct {
  char method[129],capability[129];
  uint8_t receiver; /* 0 Host, 1 Executor; validated by the Session owner */
  gt_bridge_rpc_validate_t input,output;
  void *context; /* borrowed through session destroy */
} gt_bridge_rpc_method_t;
typedef enum {GT_BRIDGE_RPC_OK,GT_BRIDGE_RPC_WAIT,GT_BRIDGE_RPC_INVALID,GT_BRIDGE_RPC_DENIED,GT_BRIDGE_RPC_UNSUPPORTED,GT_BRIDGE_RPC_CLOSED} gt_bridge_rpc_status_t;
typedef enum {GT_BRIDGE_RPC_QUEUED,GT_BRIDGE_RPC_PENDING,GT_BRIDGE_RPC_SUCCEEDED,GT_BRIDGE_RPC_FAILED,GT_BRIDGE_RPC_CANCELLED,GT_BRIDGE_RPC_UNKNOWN} gt_bridge_rpc_state_t;
typedef struct {
  uint64_t handle;char request_id[33];gt_bridge_rpc_state_t state;
  bool cancel_requested,deadline_elapsed,confirmed;
  const uint8_t *outcome;size_t outcome_size;
} gt_bridge_rpc_result_t;
typedef struct {uint64_t ticket;char request_id[33],method[129];const uint8_t *input;size_t input_size;} gt_bridge_rpc_request_t;
typedef struct gt_bridge_rpc gt_bridge_rpc_t;
typedef struct gt_bridge_session gt_bridge_session_t;
/* Internal integration, not callable from platform bindings. */
gt_status_t gt__bridge_rpc_new(gt_bridge_session_t *,gt_bridge_wire_t *,const gt_allocator_t *,gt_bridge_rpc_t **);
void gt__bridge_rpc_free(gt_bridge_rpc_t *);
void gt__bridge_rpc_end(gt_bridge_rpc_t *);
bool gt__bridge_rpc_has_owners(const gt_bridge_rpc_t *);
gt_status_t gt__bridge_rpc_receive(gt_bridge_rpc_t *,const gt_bridge_message_t *,size_t frame_size,uint64_t now);
gt_status_t gt__bridge_rpc_prepare(gt_bridge_rpc_t *,uint64_t control_seq,uint64_t now,char *,size_t,size_t *,bool *control);
void gt__bridge_rpc_commit(gt_bridge_rpc_t *);
bool gt__bridge_rpc_current(gt_bridge_rpc_t *,uint64_t now);
void gt__bridge_rpc_sent(gt_bridge_rpc_t *);
gt_bridge_rpc_status_t gt__bridge_rpc_start(gt_bridge_rpc_t *,const void *,const char *,const void *,size_t,uint32_t,uint64_t,uint64_t *);
gt_bridge_rpc_status_t gt__bridge_rpc_cancel(gt_bridge_rpc_t *,const void *,uint64_t);
bool gt__bridge_rpc_result(gt_bridge_rpc_t *,const void *,uint64_t,uint64_t,gt_bridge_rpc_result_t *);
gt_bridge_rpc_status_t gt__bridge_rpc_forget(gt_bridge_rpc_t *,const void *,uint64_t);
gt_bridge_rpc_status_t gt__bridge_rpc_take(gt_bridge_rpc_t *,const void *,uint64_t,gt_bridge_rpc_request_t *);
gt_bridge_rpc_status_t gt__bridge_rpc_check(gt_bridge_rpc_t *,const void *,uint64_t,uint64_t);
gt_bridge_rpc_status_t gt__bridge_rpc_respond(gt_bridge_rpc_t *,const void *,uint64_t,const void *,size_t,uint64_t);
#endif
