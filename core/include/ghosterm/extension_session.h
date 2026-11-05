/* Trusted native Session boundary, one serial non-UI owner. All metadata is
 * copied from a locally verified package/endpoint snapshot, never inferred from
 * peer output. This gate does not install packages or authorize OS effects.
 * Only stop may race owner calls; no stop caller may race adoption or destroy. */
#ifndef GHOSTERM_EXTENSION_SESSION_H
#define GHOSTERM_EXTENSION_SESSION_H
#include "ghosterm/extension.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_CAPS_MAX 16u
typedef struct gt_ext_session gt_ext_session_t;
typedef enum {GT_EXT_HOST,GT_EXT_EXECUTOR} gt_ext_role_t;
typedef enum {GT_EXT_PEER_LOCAL,GT_EXT_PEER_SSH} gt_ext_peer_kind_t;
typedef enum {
 GT_EXT_SESSION_SENDING,GT_EXT_SESSION_WAIT_HELLO,GT_EXT_SESSION_WAIT_OFFER,
 GT_EXT_SESSION_CONSENT,GT_EXT_SESSION_WAIT_DECISION,GT_EXT_SESSION_WAIT_ACCEPT,
 GT_EXT_SESSION_WAIT_READY,GT_EXT_SESSION_WAIT_ACK,GT_EXT_SESSION_ACTIVE,
 GT_EXT_SESSION_ENDING,GT_EXT_SESSION_ENDED,GT_EXT_SESSION_CLOSED
} gt_ext_session_phase_t;
typedef enum {GT_EXT_DECISION_UNKNOWN,GT_EXT_DECISION_ALLOW,GT_EXT_DECISION_DENY} gt_ext_decision_t;
typedef enum {
 GT_EXT_END_NONE,GT_EXT_END_DENIED,GT_EXT_END_REVOKED,GT_EXT_END_EXPIRED,
 GT_EXT_END_INCOMPATIBLE,GT_EXT_END_PROTOCOL,GT_EXT_END_LOST,GT_EXT_END_PEER,
 GT_EXT_END_LOCAL,GT_EXT_END_INTERNAL
} gt_ext_session_end_t;
typedef enum {GT_EXT_ACCESS_UNKNOWN,GT_EXT_ACCESS_ALLOWED,GT_EXT_ACCESS_DENIED,GT_EXT_ACCESS_REVOKED,GT_EXT_ACCESS_UNSUPPORTED} gt_ext_access_t;
typedef struct {uint32_t fragment_bytes,message_bytes,resource_bytes;} gt_ext_budgets_t;
typedef struct {gt_ext_peer_kind_t kind;char identity[128];} gt_ext_peer_t;
typedef struct {
 char plugin_id[129],plugin_version[65],client_digest[65],executor_digest[65];
 char host_version[65],sdk_version[65];
 size_t required_count,available_count;
 char required[GT_EXT_CAPS_MAX][129],available[GT_EXT_CAPS_MAX][129];
 gt_ext_budgets_t budgets;uint32_t handshake_ms,consent_ms;
 gt_ext_peer_t peer;
 /* session_open supports only 0 (handshake). The dedicated open_rpc constructor
  * accepts GT_EXT_FEATURE_REQUESTS with a native method registry. The dedicated
  * open_resources accepts RESOURCES, optionally REQUESTS, and a native Store.
  * Unknown bits explicitly return UNSUPPORTED. */
 uint32_t features;
 /* NULL selects Core monotonic time. Custom clock/context must outlive the
  * Session and be bounded/non-reentrant; never call a UI/platform loop. */
 void *clock_context;bool (*clock)(void *,uint64_t *);
} gt_ext_session_config_t;
typedef struct {char connection[33],instance[33],token[33];uint64_t epoch;} gt_ext_lease_t;
typedef struct {
 gt_ext_session_phase_t phase;gt_ext_session_end_t end;
 gt_status_t error,cleanup_error;gt_ext_decision_t decision;
 char connection[33],instance[33],consent_token[33],peer_host_version[65];
 uint64_t epoch,consent_deadline,received,sent;
 gt_ext_budgets_t budgets;gt_ext_peer_t peer;
} gt_ext_session_info_t;
/* Requires READY, pristine protocol IO: no bytes accepted/read, no queued raw
 * write, no stop/close. Takes/clears *channel only on success, with no IO during
 * construction. Failure leaves the channel usable. Its capacity reservation
 * remains charged until Session destroy, including failed/draining Sessions.
 * peer.kind must match fd/local or SSH provider. Directory/IPC peer trust and
 * package digest verification remain the trusted native caller's responsibility.
 * No automatic approval, reconnect, replay, PTY/relay or gateway fallback. */
gt_status_t gt_ext_session_open(gt_ext_channel_t **,gt_ext_role_t,const gt_ext_session_config_t *,gt_ext_session_t **);
gt_ext_session_phase_t gt_ext_session_step(gt_ext_session_t *,int ms);
bool gt_ext_session_info(const gt_ext_session_t *,gt_ext_session_info_t *);
/* Ticket is copied native consent_token from this offer; stale/duplicate/wrong
 * instance or late ALLOW never grants. UNKNOWN keeps waiting. Only HOST decides. */
gt_status_t gt_ext_session_decide(gt_ext_session_t *,const char *ticket,gt_ext_decision_t);
/* Native scoped lease is issued only after both READY messages. Copy before
 * async use; never expose token to peer/plugin. scope/decision/access are facts
 * about this Session, not proof a business operation completed. */
bool gt_ext_session_lease(const gt_ext_session_t *,gt_ext_lease_t *);
gt_ext_access_t gt_ext_session_access(const gt_ext_session_t *,const gt_ext_lease_t *,const char *capability);
void gt_ext_session_revoke(gt_ext_session_t *);
void gt_ext_session_stop(gt_ext_session_t *);
/* Live sources keep close DRAINING and forbid destroy even after transport has
 * closed; close/destroy sources first. Retain handle on DRAINING/error.
 * Destroy only after close RELEASED; success
 * clears *owner. Historical end/error survive stop/close; cleanup_error separate. */
gt_ext_close_t gt_ext_session_close(gt_ext_session_t *,int ms);
gt_status_t gt_ext_session_destroy(gt_ext_session_t **);
#ifdef __cplusplus
}
#endif
#endif
