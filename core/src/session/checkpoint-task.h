/* Private bounded checkpoint executor. No engine/UI dependency. */
#ifndef GT_CHECKPOINT_TASK_H
#define GT_CHECKPOINT_TASK_H
#include "ghosterm/types.h"
#include "base/allocator.h"
#include "base/thread-options.h"
typedef struct gt_checkpoint_tasks gt_checkpoint_tasks_t;
typedef enum {
  GT_TASK_QUEUED, GT_TASK_RUNNING, GT_TASK_READY, GT_TASK_CANCELLING,
  GT_TASK_CANCELLED, GT_TASK_FAILED, GT_TASK_TAKEN, GT_TASK_RELEASING, GT_TASK_FINISHED
} gt_task_state_t;
typedef enum {GT_TASK_OK, GT_TASK_INVALID, GT_TASK_FULL, GT_TASK_BUSY, GT_TASK_STOPPED} gt_task_rc_t;
typedef struct {
  void *context;
  gt_status_t (*encode)(void *, void **);
  void (*release_input)(void *);
  void (*release_result)(void *, void *);
  uint64_t generation, sequence;
} gt_task_spec_t;
typedef struct {
  gt_task_state_t state;
  gt_status_t error;
  uint64_t generation, sequence;
} gt_task_info_t;
/* One encoder thread; 1..16 slots include terminal records until forget.
 * Context/scopes never move and outlive forget/close. After successful submit,
 * executor owns input, calling release_input exactly once on its thread.
 * Rejected submission leaves all input ownership with the caller. Callbacks
 * are bounded, run without the registry lock, and must not close this pool. */
gt_checkpoint_tasks_t *gt__tasks_open(size_t capacity);
/* Optional allocation domain for pool and all inline slots. NULL = libc.
 * Does not cover OS thread stack or encode callback allocations. Context and
 * callbacks outlive close; BUSY close retains charge until actual join/free. */
gt_checkpoint_tasks_t *gt__tasks_open_with_allocator(size_t,const gt_allocator_t *);
/* Explicit stack/guard attributes; invalid settings reject, no fallback.
 * Caller admits OS stack capacity separately and retains it until close/join. */
gt_checkpoint_tasks_t *gt__tasks_open_with_options(size_t,const gt_allocator_t *,const gt_thread_options_t *);
gt_task_rc_t gt__tasks_submit(gt_checkpoint_tasks_t *, const gt_task_spec_t *, uint64_t *id);
gt_task_rc_t gt__tasks_info(gt_checkpoint_tasks_t *, uint64_t id, gt_task_info_t *);
/* Cancellation is nonblocking, does not free an in-flight encoder's input,
 * and defeats take once accepted. TAKEN is too late: returns BUSY. */
gt_task_rc_t gt__tasks_cancel(gt_checkpoint_tasks_t *, uint64_t id);
/* READY -> TAKEN atomically, validating attachment generation. Borrowed result
 * remains valid until release; generation validation and detach must also be
 * serialized by the attachment owner. A second take is BUSY, never a copy. */
gt_task_rc_t gt__tasks_take(gt_checkpoint_tasks_t *, uint64_t id, uint64_t generation,
                           void **result, uint64_t *sequence);
/* Consumer must quiesce reads first. Async: executor releases result before
 * FINISHED. No caller callback or blocking engine free on the UI thread. */
gt_task_rc_t gt__tasks_release(gt_checkpoint_tasks_t *, uint64_t id);
gt_task_rc_t gt__tasks_forget(gt_checkpoint_tasks_t *, uint64_t id);
/* Non-UI lifecycle owner only, with API callers quiesced. BUSY if any result
 * is TAKEN, leaving the pool usable. Otherwise cancels, joins and destroys.
 * A running bounded encode finishes before cancellation cleanup; no pthread
 * cancellation or premature allocator/scope destruction. */
gt_task_rc_t gt__tasks_close(gt_checkpoint_tasks_t *);
#endif
