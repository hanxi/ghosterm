/* Single-owner, instance-scoped one-use capability store. No session/UI API. */
#ifndef GT_IPC_TOKENS_H
#define GT_IPC_TOKENS_H
#include "base/allocator.h"
#include "ghosterm/types.h"
enum {GT_INSTANCE_BYTES=16,GT_TOKEN_BYTES=40};
typedef struct {uint8_t bytes[GT_TOKEN_BYTES];} gt_attachment_token_t;
typedef struct {uint64_t principal,issuer,session,generation;} gt_token_scope_t;
typedef struct {
  void *context;
  /* Pending token owns its reserved generation after successful issue. On
   * expiry/revoke/clock failure/close this callback releases that reservation;
   * consume transfers responsibility instead. Never reenter the store. */
  void (*revoke)(void *,const gt_token_scope_t *);
} gt_token_owner_t;
typedef struct {
  void *context;
  /* Optional injection, must be a bounded CSPRNG in production. NULL options
   * select OS secure_random; an explicitly missing fill callback is invalid. */
  gt_status_t (*fill)(void *,uint8_t *,size_t);
} gt_token_random_t;
typedef enum {GT_TOKEN_OK,GT_TOKEN_INVALID,GT_TOKEN_FULL,GT_TOKEN_BUSY,GT_TOKEN_DENIED,
  GT_TOKEN_ENTROPY,GT_TOKEN_UNSUPPORTED,GT_TOKEN_CLOCK,GT_TOKEN_NO_MEMORY,
  GT_TOKEN_EXHAUSTED} gt_token_rc_t;
typedef struct gt_token_store gt_token_store_t;
/* Capacity 1..256; fixed charged allocation. Instance must be nonzero. Caller
 * supplies one monotonic clock domain, beginning at now_ms (zero is valid).
 * Contexts outlive close; callbacks never reenter store. */
gt_token_rc_t gt__tokens_open(const uint8_t instance[GT_INSTANCE_BYTES],size_t capacity,
  uint64_t now_ms,const gt_token_random_t *,gt_token_owner_t,const gt_allocator_t *,gt_token_store_t **);
/* Scope IDs nonzero, principal/issuer are TRUSTED server-local IDs derived from
 * authenticated identity/management connection, never client wire claims.
 * Token = unique u64 issuance counter + 32 OS-random bytes. Counter never wraps;
 * secret is 256 bits; no used-token history or indefinite memory growth.
 * Issue only for a newly reserved generation; duplicate pending scope is BUSY.
 * Rejection owns no reservation; success owns it until consume/revocation. */
gt_token_rc_t gt__tokens_issue(gt_token_store_t *,gt_token_scope_t,uint64_t now_ms,
  uint32_t ttl_ms,gt_attachment_token_t *);
/* TTL 1..60000; exact-boundary expiry. Success consumes/wipes record once and
 * returns original issuer too. Mismatched scope does not burn valid token.
 * Unknown/replayed/expired are all denial of THIS request, not an assertion
 * that session exited. Clock/entropy/unsupported errors remain distinct.
 * Token comparisons examine every byte; do not log/export stored secrets. */
gt_token_rc_t gt__tokens_consume(gt_token_store_t *,const gt_attachment_token_t *,
  const uint8_t instance[GT_INSTANCE_BYTES],uint64_t principal,uint64_t session,
  uint64_t generation,uint64_t now_ms,gt_token_scope_t *);
/* Immediate cancellation; exact generation prevents old owner revoking new.
 * Issuer disconnect revokes its unconsumed tokens, not existing sessions. */
void gt__tokens_revoke_issuer(gt_token_store_t *,uint64_t issuer);
void gt__tokens_revoke_generation(gt_token_store_t *,uint64_t session,uint64_t generation);
/* Service timer MUST call even with no incoming requests, so abandoned
 * reservations expire/release without waiting for the next issue/consume. */
gt_token_rc_t gt__tokens_expire(gt_token_store_t *,uint64_t now_ms);
/* Wipes all records before refunding exact allocation. Clock rollback also
 * wipes/revokes all tokens and permanently fails this store. */
void gt__tokens_close(gt_token_store_t *);
#endif
