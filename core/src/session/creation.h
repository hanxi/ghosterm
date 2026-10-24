/* Private creation ledger and owned worker launch, independent of IPC/UI. */
#ifndef GT_SESSION_CREATION_H
#define GT_SESSION_CREATION_H
#include "create-spec.h"
#include "manager.h"
typedef enum {GT_CREATION_ACCEPTED,GT_CREATION_UNKNOWN,GT_CREATION_INVALID,
  GT_CREATION_CONFLICT,GT_CREATION_BUSY,GT_CREATION_UNSUPPORTED,GT_CREATION_NO_MEMORY,
  GT_CREATION_PREPARE_FAILED,GT_CREATION_START_FAILED,GT_CREATION_CLOCK,GT_CREATION_INTERNAL} gt_creation_status_t;
typedef struct {gt_creation_status_t status;gt_status_t detail;uint64_t session;} gt_creation_result_t;
typedef struct {
  gt_session_manager_t *manager;
  uint8_t instance[16];size_t capacity;uint32_t retention_ms;
  void *factory_context;
  /* One bounded nonblocking preparation on control owner: no engine/backend
   * creation, I/O, waiting or reentry. Spec is immutable, owned until AFTER
   * dispose. Its strings may be borrowed by the returned worker context.
   * Core zeroes config first; dispose is called exactly once after each invoked
   * prepare, including errors/invalid config (context may be NULL/partial).
   * Factories in worker config execute only on the session worker. */
  gt_status_t (*prepare)(void *,uint64_t trusted_principal,const gt_create_spec_t *,gt_worker_config_t *);
  /* Mandatory provisional runtime binding. Runs before any worker callback,
   * outside manager mutex; ID is reserved but not yet published. No I/O/waits.
   * Return failure to reject without launching. dispose cleans a partial binding
   * on rejection, or releases it after join on success. Never use result-cache
   * lifetime as session-object lifetime. Principal is the same trusted identity
   * passed to prepare; it never comes from the request payload. */
  gt_status_t (*bind_id)(void *,void *worker_context,uint64_t trusted_principal,uint64_t session);
  void (*dispose)(void *,void *worker_context);
  /* Both-zero selects defaults; otherwise supply complete allocator/options.
   * Allocator contexts and factory_context outlive ALL worker disposals, even
   * if the ledger is closed earlier. Allocators must be thread-safe. */
  gt_allocator_t worker_allocator;gt_thread_options_t worker_threads;
} gt_creation_config_t;
typedef struct gt_creation gt_creation_t;
/* Single control owner; 1..64 fixed records, TTL 1..86400000 ms. Instance scoped.
 * One fixed charged ledger allocation; each new launch additionally owns an
 * immutable request/job allocation until manager join/dispose. Does not own
 * manager, factory context, worker allocator or its external thread budgets. */
gt_status_t gt__creation_open(const gt_creation_config_t *,uint64_t now_ms,
  const gt_allocator_t *,gt_creation_t **);
bool gt__creation_matches(const gt_creation_t *,const gt_session_manager_t *,const uint8_t instance[16]);
/* Trusted principal+client nonce scope. Same bytes replay original result;
 * conflicting bytes return CONFLICT without invoking prepare. Manager registry
 * publication is the commit point, not factory completion/shell readiness.
 * Expiry yields UNKNOWN; clients MUST query after ambiguous CREATE and MUST NOT
 * resend CREATE after UNKNOWN/expiry. No exactly-once promise beyond retention.
 * Malformed input returns INVALID/PROTOCOL without consuming a record. */
gt_creation_result_t gt__creation_submit(gt_creation_t *,uint64_t principal,
  const void *,size_t,uint64_t now_ms);
gt_creation_result_t gt__creation_query(gt_creation_t *,uint64_t principal,
  const uint8_t nonce[GT_CREATE_NONCE_BYTES],uint64_t now_ms);
/* Call on idle ticks too. Clock rollback permanently fails ledger, never stops
 * a committed worker. Closing clears ledger only; manager still owns jobs. */
gt_status_t gt__creation_advance(gt_creation_t *,uint64_t now_ms);
void gt__creation_close(gt_creation_t *);
#endif
