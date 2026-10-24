/* Shared management reservation -> data binding ownership; single IO owner. */
#ifndef GT_IPC_ATTACHMENT_OWNER_H
#define GT_IPC_ATTACHMENT_OWNER_H
#include "tokens.h"
#include "session/runtime.h"
typedef struct gt_attachment_owner gt_attachment_owner_t;
typedef enum {GT_ATTACH_OK,GT_ATTACH_NOT_FOUND,GT_ATTACH_BUSY,GT_ATTACH_UNSUPPORTED,
  GT_ATTACH_ENTROPY,GT_ATTACH_CLOCK,GT_ATTACH_INTERNAL} gt_attach_status_t;
typedef struct {
  gt_attach_status_t status;gt_status_t detail;
  uint8_t instance[16];uint64_t session,generation;uint32_t ttl_ms;
  gt_attachment_token_t token;
} gt_attach_result_t;
typedef struct {
  gt_session_runtime_t *runtime; /* borrowed, outlives owner and all consumers */
  uint8_t instance[16];size_t capacity;uint32_t token_ttl_ms;
  gt_token_random_t random; /* fill NULL selects OS random; context then NULL */
} gt_attachment_owner_config_t;
/* Fixed 1..256 reservation slots and a same-sized token store. No engine/PTY
 * operations. Holds one runtime reference per pending/consumed reservation.
 * All calls and callbacks share the service's one IO owner/clock domain. */
gt_status_t gt__attachment_owner_open(const gt_attachment_owner_config_t *,uint64_t now_ms,
  const gt_allocator_t *,gt_attachment_owner_t **);
bool gt__attachment_owner_matches(const gt_attachment_owner_t *,const gt_session_manager_t *,
  const gt_creation_t *,const uint8_t instance[16]);
gt_token_store_t *gt__attachment_owner_tokens(gt_attachment_owner_t *);
/* Exactly one live server can claim an owner: issuer IDs belong to that server.
 * Claim/release do not create a second namespace or reset live capabilities. */
bool gt__attachment_owner_claim(gt_attachment_owner_t *,const void *server);
void gt__attachment_owner_unclaim(gt_attachment_owner_t *,const void *server);
/* Trusted principal/issuer from authenticated management peer, nonzero session.
 * Expire first so exact-boundary retries can reserve the next generation.
 * Failure clears all capability fields, echoes session, retains no reference.
 * Success is CAPTURING membership, never LIVE/input authorization. */
gt_attach_status_t gt__attachment_owner_issue(gt_attachment_owner_t *,uint64_t principal,
  uint64_t issuer,uint64_t session,uint64_t now_ms,gt_attach_result_t *);
/* Handshake and token callbacks. Drop matches all scope fields and detaches only
 * that generation; safe repeatedly and after a newer generation is reserved.
 * Drop never reenters the token store (it also runs from store revocation). */
bool gt__attachment_owner_current(void *,const gt_token_scope_t *);
void gt__attachment_owner_drop(void *,const gt_token_scope_t *);
/* Borrow only for one bounded IO-owner call. Async capture/encoder contexts must
 * acquire their own runtime reference; never retain this borrowed controller. */
gt_session_attachment_t *gt__attachment_owner_attachment(gt_attachment_owner_t *,const gt_token_scope_t *);
/* Call after server close and API users quiesce. BUSY while claimed. Revokes
 * pending and drops remaining consumed scopes; never stops/closes runtime. */
gt_runtime_rc_t gt__attachment_owner_close(gt_attachment_owner_t *);
#endif
