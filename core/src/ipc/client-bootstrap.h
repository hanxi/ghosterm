/* One shared client bootstrap for private IPC or an SSH exec stream. */
#ifndef GT_IPC_CLIENT_BOOTSTRAP_H
#define GT_IPC_CLIENT_BOOTSTRAP_H
#include "bootstrap-wire.h"
#include "connection.h"
typedef struct gt_client_bootstrap gt_client_bootstrap_t;
typedef enum {GT_BOOTSTRAP_RUNNING,GT_BOOTSTRAP_DRAINING,GT_BOOTSTRAP_READY,GT_BOOTSTRAP_TAKEN,GT_BOOTSTRAP_FAILED} gt_bootstrap_state_t;
typedef enum {GT_BOOTSTRAP_NO_ERROR,GT_BOOTSTRAP_PROTOCOL,GT_BOOTSTRAP_COMPATIBILITY,
  GT_BOOTSTRAP_FEATURES,GT_BOOTSTRAP_INSTANCE,GT_BOOTSTRAP_DENIED,GT_BOOTSTRAP_CLOCK,
  GT_BOOTSTRAP_TIMEOUT,GT_BOOTSTRAP_IO,GT_BOOTSTRAP_CANCELLED} gt_bootstrap_error_t;
typedef struct {
  gt_protocol_hello_t hello;
  gt_protocol_bind_t binding; /* DATA only, instance equals expected_instance */
  uint32_t timeout_ms;void *clock_context;bool (*clock)(void *,uint64_t *);
} gt_client_bootstrap_config_t;
typedef struct {gt_bootstrap_state_t state;gt_bootstrap_error_t error;bool has_status;gt_hello_status_t status;} gt_bootstrap_info_t;
typedef struct {gt_protocol_identity_t identity;gt_protocol_role_t role;uint64_t session,generation;} gt_bootstrap_result_t;
/* One non-UI owner. Adopts fresh open/TX-idle connection on success only. RX/TX
 * >=80. One fixed charged object; copies token, erases it as soon as BIND is
 * queued, never retries. Caller retains its own token/instance scope separately.
 * Management binding must be entirely zero; DATA must match HELLO instance. */
gt_status_t gt__client_bootstrap_open(gt_connection_t *,const gt_client_bootstrap_config_t *,const gt_allocator_t *,gt_client_bootstrap_t **);
gt_bootstrap_state_t gt__client_bootstrap_step(gt_client_bootstrap_t *);
bool gt__client_bootstrap_info(const gt_client_bootstrap_t *,gt_bootstrap_info_t *);
/* Final ACK must be completely written before transfer; preserves the first
 * application frame. Single transfer, failure clears outputs. No session API. */
bool gt__client_bootstrap_take(gt_client_bootstrap_t *,gt_connection_t **,gt_bootstrap_result_t *);
void gt__client_bootstrap_cancel(gt_client_bootstrap_t *);
void gt__client_bootstrap_close(gt_client_bootstrap_t *);
#endif
