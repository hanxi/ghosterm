/* Private bounded process-local registry. No engine/UI/protocol dependency. */
#ifndef GT_SESSION_MANAGER_H
#define GT_SESSION_MANAGER_H
#include "worker.h"
typedef struct gt_session_manager gt_session_manager_t;
typedef enum {GT_MANAGER_OK,GT_MANAGER_INVALID,GT_MANAGER_FULL,GT_MANAGER_NOT_FOUND,
  GT_MANAGER_BUSY,GT_MANAGER_CLOSED,GT_MANAGER_START_FAILED,GT_MANAGER_IDS_EXHAUSTED,
  GT_MANAGER_REGISTER_FAILED} gt_manager_rc_t;
typedef enum {GT_MANAGED_WORKER,GT_MANAGED_REAPING,GT_MANAGED_RETAINED} gt_managed_phase_t;
typedef struct {
  uint64_t id;
  gt_managed_phase_t phase;
  bool termination_requested,has_result;
  gt_worker_result_t result; /* valid only when has_result; unknown stays unknown */
} gt_managed_info_t;
/* Capacity 1..64 includes starting workers, joins and retained terminal records.
 * One fixed allocation through the supplied thread-safe allocator (NULL=libc).
 * Allocator context outlives close. IDs are unique until this manager closes;
 * they are process-local, not authenticated or globally unique session IDs. */
gt_session_manager_t *gt__manager_open(size_t,const gt_allocator_t *);
/* Success transfers config.context to the manager, disposed exactly once after
 * worker join. Rejection owns nothing, invokes no disposer. Copies config;
 * its other borrowed objects/allocator/thread capacity must outlive disposal.
 * Memory allocator callbacks must not reenter manager or worker operations.
 * Worker startup is asynchronous: factory failure produces a retained error,
 * not a fabricated synchronous success/exit code. Failed admission clears id.
 * Optional register_id runs after reserving a unique ID, outside the manager
 * mutex, BEFORE any worker factory/callback. It is provisional metadata binding,
 * not registry publication. Must be bounded, nonblocking, no engine/backend I/O.
 * Failure clears the reserved slot, burns its ID, starts no worker, and transfers
 * no context ownership. Caller cleans any partial binding on rejection.
 * Factories and callbacks execute without the manager mutex. */
gt_manager_rc_t gt__manager_create(gt_session_manager_t *,const gt_worker_config_t *,
  const gt_allocator_t *worker_allocator,const gt_thread_options_t *,
  void (*dispose)(void *),gt_status_t (*register_id)(void *,uint64_t),uint64_t *id);
/* Thread-safe registry operations; no worker/backend pointer escapes. User
 * completion callbacks run under the worker's existing owner contract. */
gt_manager_rc_t gt__manager_submit_internal(gt_session_manager_t *,uint64_t,gt_worker_command_t,
  const void *,size_t,gt_worker_completion_t,void *,gt_worker_submit_t *);
/* Same lifetime/admission, USER metadata preserved; never bypass policy by
 * falling back to submit_internal when admission/authorization is rejected. */
gt_manager_rc_t gt__manager_submit_user(gt_session_manager_t *,uint64_t,gt_worker_command_t,
  const gt_worker_stamp_t *,const void *,size_t,gt_worker_user_completion_t,void *,gt_worker_submit_t *);
gt_manager_rc_t gt__manager_info(gt_session_manager_t *,uint64_t,gt_managed_info_t *);
/* Snapshot is all-or-nothing, ordered by fixed slot, including retained records.
 * FULL sets required count without writing rows. No allocation or pagination. */
gt_manager_rc_t gt__manager_list(gt_session_manager_t *,gt_managed_info_t *,size_t,size_t *);
/* Idempotent explicit stop; never called merely because a client detaches.
 * Terminal records keep their observed result. This is not an exit-code promise. */
gt_manager_rc_t gt__manager_terminate(gt_session_manager_t *,uint64_t);
/* Non-UI lifecycle owner, never a worker callback. BUSY until result published;
 * joins outside the registry mutex, then disposes context. Concurrent reapers
 * cannot double-join. Retained record still consumes capacity until forget. */
gt_manager_rc_t gt__manager_reap(gt_session_manager_t *,uint64_t);
gt_manager_rc_t gt__manager_forget(gt_session_manager_t *,uint64_t);
/* API callers quiesced, non-UI owner, never a worker callback. BUSY on a create
 * or reap already in progress, leaves manager usable. Otherwise stops/joins all
 * workers, disposes contexts and frees registry; no implicit detached shutdown. */
gt_manager_rc_t gt__manager_close(gt_session_manager_t *);
#endif
