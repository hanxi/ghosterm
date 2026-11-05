/* Private session: one selected instance on one admitted duplex stream.
 * Optional bounded requests/resources; no gateway, auto-reconnect
 * or public ABI. Caller supplies metadata from its verified package and
 * peer owners, not remote self-reported trust. One serial non-UI IO owner.
 * Only stop is cross-thread; all stop callers finish before destroy. */
#ifndef GT_BRIDGE_SESSION_H
#define GT_BRIDGE_SESSION_H
#include "extension/wire.h"
#include "extension/rpc.h"
#define GT_BRIDGE_SESSION_CAPS_MAX 16u
#define GT_BRIDGE_SESSION_CONTROL_MAX 4096u
typedef struct gt_bridge_session gt_bridge_session_t;
typedef struct gt_bridge_resource_store gt_bridge_resource_store_t;
typedef enum {GT_BRIDGE_HOST,GT_BRIDGE_EXECUTOR} gt_bridge_role_t;
typedef enum {GT_BRIDGE_PEER_LOCAL,GT_BRIDGE_PEER_SSH} gt_bridge_peer_kind_t;
typedef struct {gt_bridge_peer_kind_t kind;char identity[128];} gt_bridge_peer_evidence_t;
typedef struct {uint32_t fragment_bytes,message_bytes,resource_bytes;} gt_bridge_budgets_t;
typedef struct {
  char plugin_id[129],plugin_version[65],client_digest[65],executor_digest[65];
  char host_version[65],sdk_version[65];
  size_t required_count,available_count;
  char required[GT_BRIDGE_SESSION_CAPS_MAX][129],available[GT_BRIDGE_SESSION_CAPS_MAX][129];
  gt_bridge_budgets_t budgets;
  uint32_t handshake_ms,consent_ms;
  void *clock_context;bool (*clock)(void *,uint64_t *);
  gt_bridge_peer_evidence_t peer;
  bool rpc_enabled;
  size_t rpc_method_count;
  gt_bridge_rpc_method_t rpc_methods[16];
  /* Borrowed connection Store outlives Session and all handed-off snapshots. */
  gt_bridge_resource_store_t *resource_store;
  uint64_t resource_scope_bytes;
} gt_bridge_session_config_t;
typedef enum {
  GT_BRIDGE_SESSION_SENDING,GT_BRIDGE_SESSION_WAIT_HELLO,GT_BRIDGE_SESSION_WAIT_OFFER,
  GT_BRIDGE_SESSION_CONSENT,GT_BRIDGE_SESSION_WAIT_DECISION,GT_BRIDGE_SESSION_WAIT_ACCEPT,
  GT_BRIDGE_SESSION_WAIT_READY,GT_BRIDGE_SESSION_WAIT_ACK,GT_BRIDGE_SESSION_ACTIVE,
  GT_BRIDGE_SESSION_ENDING,GT_BRIDGE_SESSION_ENDED,GT_BRIDGE_SESSION_CLOSED
} gt_bridge_session_phase_t;
typedef enum {GT_BRIDGE_DECISION_UNKNOWN,GT_BRIDGE_DECISION_ALLOW,GT_BRIDGE_DECISION_DENY} gt_bridge_decision_t;
typedef enum {
  GT_BRIDGE_END_NONE,GT_BRIDGE_END_DENIED,GT_BRIDGE_END_REVOKED,GT_BRIDGE_END_EXPIRED,
  GT_BRIDGE_END_INCOMPATIBLE,GT_BRIDGE_END_PROTOCOL,GT_BRIDGE_END_LOST,GT_BRIDGE_END_PEER,
  GT_BRIDGE_END_LOCAL,GT_BRIDGE_END_INTERNAL
} gt_bridge_session_end_t;
typedef enum {GT_BRIDGE_ACCESS_UNKNOWN,GT_BRIDGE_ACCESS_ALLOWED,GT_BRIDGE_ACCESS_DENIED,GT_BRIDGE_ACCESS_REVOKED,GT_BRIDGE_ACCESS_UNSUPPORTED} gt_bridge_access_t;
typedef struct {char connection[33],instance[33],token[33];uint64_t epoch;} gt_bridge_lease_t;
typedef struct {
  gt_bridge_session_phase_t phase;
  gt_bridge_session_end_t end;
  gt_status_t error;
  gt_bridge_decision_t decision;
  char connection[33],instance[33],consent_token[33],peer_host_version[65];
  uint64_t epoch,consent_deadline,received,sent;
  gt_bridge_budgets_t budgets;
  gt_bridge_peer_evidence_t peer;
} gt_bridge_session_info_t;
/* All config strings/POD are copied; allocator/clock context outlive destroy.
 * Takes admitted stream only on success. No network IO during construction.
 * Session metadata equality is not package installation/hash verification.
 * Fresh connection/instance/nonces come from OS entropy, never a weak fallback. */
gt_status_t gt__bridge_session_open(gt_bridge_stream_t **,gt_bridge_role_t,
 const gt_bridge_session_config_t *,const gt_allocator_t *,gt_bridge_session_t **);
gt_bridge_session_phase_t gt__bridge_session_step(gt_bridge_session_t *,int ms);
bool gt__bridge_session_info(const gt_bridge_session_t *,gt_bridge_session_info_t *);
bool gt__bridge_session_runtime_admissible(const gt_bridge_session_t *);
/* Owner-only command from the native authorization path; token binds the
 * displayed decision to this offer. UNKNOWN waits, never grants or rejects. */
gt_status_t gt__bridge_session_decide(gt_bridge_session_t *,const char *,gt_bridge_decision_t);
bool gt__bridge_session_lease(const gt_bridge_session_t *,gt_bridge_lease_t *);
gt_bridge_access_t gt__bridge_session_access(const gt_bridge_session_t *,const gt_bridge_lease_t *,const char *capability);
void gt__bridge_session_revoke(gt_bridge_session_t *);
void gt__bridge_session_stop(gt_bridge_session_t *);
gt_bridge_close_t gt__bridge_session_close(gt_bridge_session_t *,int ms);
gt_status_t gt__bridge_session_destroy(gt_bridge_session_t *);
/* Opt-in request feature, negotiated before any application traffic. Native
 * handles are local; wire IDs are assigned only when a frame can be queued.
 * QUEUED cancellation has no remote effect; sent cancellation is a request,
 * never a cancellation receipt. Timeout may later gain a confirmed response
 * while its handle is retained. No automatic replay on a new session. */
gt_bridge_rpc_status_t gt__bridge_session_request(gt_bridge_session_t *,const gt_bridge_lease_t *,const char *,const void *,size_t,uint32_t timeout_ms,uint64_t *handle);
gt_bridge_rpc_status_t gt__bridge_session_cancel_request(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t handle);
bool gt__bridge_session_request_result(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t,gt_bridge_rpc_result_t *);
gt_bridge_rpc_status_t gt__bridge_session_forget_request(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t);
gt_bridge_rpc_status_t gt__bridge_session_take_request(gt_bridge_session_t *,const gt_bridge_lease_t *,gt_bridge_rpc_request_t *);
gt_bridge_rpc_status_t gt__bridge_session_check_request(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t ticket);
gt_bridge_rpc_status_t gt__bridge_session_respond(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t ticket,const void *outcome,size_t);
bool gt__bridge_session_scope(const gt_bridge_session_t *,const gt_bridge_lease_t *);
bool gt__bridge_session_has_capability(const gt_bridge_session_t *,const char *);
/* Borrowed immutable RPC registry and role, for the internal class owner. */
const gt_bridge_rpc_method_t *gt__bridge_session_methods(const gt_bridge_session_t *,size_t *,gt_bridge_role_t *);
#endif
