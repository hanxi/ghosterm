/* Private shared local/SSH session coordinator. One serial non-UI owner. */
#ifndef GT_IPC_SESSION_CLIENT_H
#define GT_IPC_SESSION_CLIENT_H
#include "attachment-client.h"
#include "client-bootstrap.h"
#include "control-client.h"
typedef struct gt_session_client gt_session_client_t;
typedef enum {GT_SESSION_LINK_WAIT,GT_SESSION_LINK_READY,GT_SESSION_LINK_RETRY,GT_SESSION_LINK_FATAL} gt_session_link_state_t;
typedef struct {
  void *context;
  /* Bounded/nonblocking, every step, including LIVE. start exactly once per
   * attempt. Authentication may run in a separately owned cancellable worker;
   * WAIT never blocks this owner. READY continues pumping local/SSH IO.
   * WAIT/READY require error=OK; RETRY accepts IO/CANCELLED only; FATAL needs a
   * concrete error. Authentication/hostkey/version rejection is always FATAL. */
  gt_session_link_state_t (*step)(void *,bool start,uint32_t attempt,gt_status_t *error);
  /* READY only; immediate fresh stream ops, no wait/authentication. Slot 0 is
   * management, slot 1 DATA. Failure owns no stream; success transfers close. */
  gt_status_t (*stream)(void *,unsigned slot,gt_stream_ops_t *);
  /* Exactly once per started attempt, AFTER protocol streams are destroyed.
   * Nonblocking, cancels current connect/auth/IO. Never terminates a session.
   * Context remains alive until coordinator close and provider work quiesces. */
  void (*reset)(void *);
} gt_session_link_t;
typedef enum {GT_SESSION_STARTING,GT_SESSION_RECOVERING,GT_SESSION_RESTORING,
  GT_SESSION_LIVE,GT_SESSION_READONLY,GT_SESSION_ENDED,GT_SESSION_FAILED,GT_SESSION_CLOSED,GT_SESSION_CATALOG} gt_session_phase_t;
typedef enum {GT_SESSION_REASON_NONE,GT_SESSION_REASON_LINK,GT_SESSION_REASON_BOOTSTRAP,
  GT_SESSION_REASON_CONTROL,GT_SESSION_REASON_CREATE_UNKNOWN,GT_SESSION_REASON_NOT_FOUND,
  GT_SESSION_REASON_ATTACHMENT,GT_SESSION_REASON_DEADLINE,GT_SESSION_REASON_CLOCK,
  GT_SESSION_REASON_CANCELLED,GT_SESSION_REASON_LOCAL} gt_session_reason_t;
typedef enum {GT_CLIENT_STOP_NONE,GT_CLIENT_STOP_PENDING,GT_CLIENT_STOP_KNOWN,GT_CLIENT_STOP_UNKNOWN} gt_client_stop_state_t;
typedef struct {gt_client_stop_state_t state;uint64_t session,request;gt_control_status_t status;gt_status_t error;} gt_client_stop_t;
typedef struct {
  gt_session_phase_t phase;gt_session_reason_t reason;gt_status_t error;
  uint8_t instance[16];uint64_t session,generation;uint32_t attempts,busy_replies;
  bool has_attachment,has_admitted_create,input_locked;gt_client_info_t attachment;gt_client_stop_t stop;
} gt_session_client_info_t;
typedef struct {
  gt_session_link_t link;
  uint8_t compatibility[32],instance[16];uint64_t required_features,session;
  /* Select exactly one mode below. Resuming requires nonzero instance. Create
   * is validated and deep-copied at open. Its nonce remains stable across all
   * attempts; after first admission only QUERY_CREATE is allowed on reconnect. */
  const gt_create_spec_t *create;
  /* One mode: create, session, pending_nonce, or catalog_only. A pending nonce
   * requires the original instance and only permits QUERY_CREATE, never CREATE.
   * Catalog mode opens no DATA stream and needs no engine sink. */
  uint8_t pending_nonce[16];bool catalog_only;
  uint32_t recovery_timeout_ms,backoff_ms,bootstrap_timeout_ms,
    management_heartbeat_ms,management_timeout_ms;
  void *clock_context;bool (*clock)(void *,uint64_t *);
  /* session/generation and clock are set by coordinator; all other limits and
   * sink callbacks follow attachment-client. Sink contexts outlive close. */
  gt_client_config_t attachment;
} gt_session_client_config_t;
/* Fixed admitted owner + deep copy; child protocol allocations charged to the
 * same allocator. Provider is untouched if construction fails. Strict identity,
 * one overall initial/recovery deadline; retry does not renew it. */
gt_status_t gt__session_client_open(const gt_session_client_config_t *,const gt_allocator_t *,gt_session_client_t **);
gt_session_phase_t gt__session_client_step(gt_session_client_t *);
bool gt__session_client_info(const gt_session_client_t *,gt_session_client_info_t *);
/* LIVE only. No queued input while disconnected/restoring, no command replay. */
gt_client_submit_t gt__session_client_submit(gt_session_client_t *,const gt_user_request_t *,gt_worker_stamp_t *);
gt_client_submit_t gt__session_client_submit_at(gt_session_client_t *,uint64_t generation,uint64_t sequence,const gt_user_request_t *,gt_worker_stamp_t *);
/* Catalog is an all-or-nothing bounded snapshot; NOT_FOUND means not loaded,
 * while OK/count=0 is an observed empty list. Small capacity reports required
 * count and INVALID_ARG without writing rows. Refresh is read-only. */
gt_status_t gt__session_client_catalog(const gt_session_client_t *,gt_managed_info_t *,size_t,size_t *);
gt_client_submit_t gt__session_client_catalog_refresh(gt_session_client_t *);
/* Explicit management command, never retried automatically. A known OK is
 * admission of termination, not observation of process exit. */
gt_client_submit_t gt__session_client_terminate(gt_session_client_t *,uint64_t session);
/* Immediate detach, never TERMINATE. Callback results for unconfirmed commands
 * remain UNKNOWN. Non-reentrant callbacks, including sink and provider. */
void gt__session_client_cancel(gt_session_client_t *);
void gt__session_client_close(gt_session_client_t *);
#endif
