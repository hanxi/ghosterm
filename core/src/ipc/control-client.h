/* Shared client management owner after successful management bootstrap. */
#ifndef GT_IPC_CONTROL_CLIENT_H
#define GT_IPC_CONTROL_CLIENT_H
#include "control.h"
#include "bootstrap-wire.h"
#include "connection.h"
typedef struct gt_control_client gt_control_client_t;
typedef struct {uint16_t type;uint64_t session;const gt_create_spec_t *create;uint8_t nonce[16];} gt_control_request_t;
typedef enum {GT_CONTROL_CLIENT_REPLY,GT_CONTROL_CLIENT_UNKNOWN} gt_control_delivery_t;
typedef struct {
  gt_control_delivery_t delivery;gt_status_t error;
  uint16_t request_type,reply_type;uint64_t request_id,session;uint8_t nonce[16];
  gt_control_status_t status;size_t count;gt_managed_info_t rows[GT_CONTROL_MAX_ROWS];
  gt_creation_result_t creation;gt_attach_result_t attachment;
} gt_control_result_t;
typedef struct {
  uint8_t instance[GT_INSTANCE_BYTES];uint32_t heartbeat_ms,request_timeout_ms;
  void *context;bool (*clock)(void *,uint64_t *);
  /* No reentry. Borrowed POD (including token) during call, erased afterward.
   * UNKNOWN never means rollback. CREATE must be queried by the original nonce
   * after reconnect; this owner never retries or auto-creates a replacement. */
  void (*result)(void *,const gt_control_result_t *);
} gt_control_client_config_t;
typedef enum {GT_CONTROL_CLIENT_READY,GT_CONTROL_CLIENT_BUSY,GT_CONTROL_CLIENT_FAILED} gt_control_client_state_t;
typedef enum {GT_CONTROL_CLIENT_QUEUED,GT_CONTROL_CLIENT_BACKPRESSURE,GT_CONTROL_CLIENT_INVALID,GT_CONTROL_CLIENT_UNSUPPORTED,GT_CONTROL_CLIENT_UNAVAILABLE} gt_control_submit_t;
/* Adopts TX-idle authenticated MANAGEMENT connection on success. RX>=4104,
 * TX>=8192. One fixed admitted object, one outstanding request, no IO thread.
 * Config instance is the identity returned by client bootstrap, not a Host alias. */
gt_status_t gt__control_client_open(gt_connection_t *,const gt_control_client_config_t *,const gt_allocator_t *,gt_control_client_t **);
gt_control_client_state_t gt__control_client_step(gt_control_client_t *);
gt_control_submit_t gt__control_client_submit(gt_control_client_t *,const gt_control_request_t *,uint64_t *id,gt_status_t *error);
gt_status_t gt__control_client_error(const gt_control_client_t *);
void gt__control_client_close(gt_control_client_t *);
#endif
