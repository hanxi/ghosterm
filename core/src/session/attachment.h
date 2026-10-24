/* Private per-session attachment ownership. No backend, engine, or UI calls. */
#ifndef GT_SESSION_ATTACHMENT_H
#define GT_SESSION_ATTACHMENT_H
#include "checkpoint-task.h"
#include "restore-stream.h"
typedef struct gt_session_attachment gt_session_attachment_t;
typedef struct {
  uint64_t task, generation, sequence;
  void *result;
} gt_attachment_checkpoint_t;
typedef struct {
  uint64_t task, generation;
  gt_task_state_t state;
  gt_status_t error;
  void *context;
} gt_attachment_retired_t;
/* Pool outlives controller. Only this controller may take/release/forget its
 * task IDs. Callbacks/context follow the executor's lifetime rules. */
gt_session_attachment_t *gt__attachment_open(gt_checkpoint_tasks_t *,size_t queue_bytes,size_t queue_events);
/* Domain covers controller + fixed restore arrays, retained across detach.
 * NULL selects libc; callbacks must support concurrent session/writer access
 * if shared elsewhere. Context outlives successful close. Admission failure
 * releases every partial allocation; BUSY close retains all allocations. */
gt_session_attachment_t *gt__attachment_open_with_allocator(gt_checkpoint_tasks_t *,size_t,size_t,const gt_allocator_t *);
/* One active attachment per session. New generations never reuse old values. */
gt_task_rc_t gt__attachment_attach(gt_session_attachment_t *, uint64_t *generation);
/* Membership only; input_allowed additionally requires both READY phases. */
bool gt__attachment_current(gt_session_attachment_t *, uint64_t generation);
/* One checkpoint submission per attachment. Rejection keeps input ownership
 * with the caller; generation is set by the controller, not trusted from spec. */
gt_task_rc_t gt__attachment_submit(gt_session_attachment_t *, uint64_t generation,
                                  const gt_task_spec_t *, uint64_t *task);
gt_task_rc_t gt__attachment_take(gt_session_attachment_t *, uint64_t generation,
                                gt_attachment_checkpoint_t *);
/* Owner calls begin at S inside capture, before any S+1 event is published.
 * All subsequent authority events, including resize, go through push. Failure
 * revokes only this attachment and cancels its restore task, never the session. */
gt_restore_rc_t gt__attachment_begin(gt_session_attachment_t *,uint64_t generation,uint64_t sequence);
gt_restore_rc_t gt__attachment_push(gt_session_attachment_t *,uint64_t sequence,uint32_t kind,const void *,size_t);
gt_restore_rc_t gt__attachment_next(gt_session_attachment_t *,uint64_t generation,gt_restore_event_t *,void *,size_t);
gt_restore_rc_t gt__attachment_ready_snapshot(gt_session_attachment_t *,uint64_t generation,uint64_t sequence);
gt_restore_rc_t gt__attachment_live_begin(gt_session_attachment_t *,uint64_t generation,uint64_t *sequence);
gt_restore_rc_t gt__attachment_ready_live(gt_session_attachment_t *,uint64_t generation,uint64_t sequence);
/* Current generation, known final sequence only (never substitute 0 for unknown).
 * Latch session-wide read-only state immediately,
 * retained across detach/reattach. Duplicate final sequence is idempotent;
 * mismatches/stale callers leave prior state intact. WAIT means restoration or
 * final data is incomplete: retry the phase fence later, input stays revoked.
 * Membership remains active for queued data/control delivery. Saved-state
 * readonly reattach is permitted; its snapshot/events cannot exceed final S. */
gt_restore_rc_t gt__attachment_end(gt_session_attachment_t *,uint64_t generation,uint64_t final_sequence);
/* Startup failed before an authority sequence exists. Caller must establish
 * this from worker result.has_sequence=false, never from its numeric value.
 * Only before capture/submission; preserve peer membership for error delivery.
 * No data stream is invented: phase stays CAPTURING and no READY is permitted.
 * Unknown remains unknown across detach/reattach, and cannot become known 0. */
gt_restore_rc_t gt__attachment_end_unknown(gt_session_attachment_t *,uint64_t generation);
bool gt__attachment_input_allowed(gt_session_attachment_t *,uint64_t generation,uint64_t applied_sequence);
/* Worker-only USER commit point, atomic with membership/restore state. No I/O.
 * For a current generation every nonzero increasing command ID is consumed,
 * including a rejection while restoring/readonly or for an invalid sequence.
 * Replay => PROTOCOL; stale/not-LIVE/ended/bad sequence => AUTH. Zero IDs are
 * INVALID. A new generation resets command IDs; old generations never reset or
 * consume its IDs. OK cannot recall later in-flight backend I/O on detach. */
gt_status_t gt__attachment_authorize(gt_session_attachment_t *,uint64_t generation,uint64_t command_id,uint64_t applied_sequence);
typedef struct {gt_restore_phase_t phase;gt_restore_rc_t failure;uint64_t snapshot,last,dequeued,cut;size_t bytes,events;bool session_ended;uint64_t terminal_sequence;bool has_terminal_sequence;} gt_attachment_restore_info_t;
gt_restore_rc_t gt__attachment_restore_info(gt_session_attachment_t *,uint64_t generation,gt_attachment_restore_info_t *);
/* Immediately revokes generation and cancels unclaimed checkpoint work. Never
 * stops/joins the session/backend. A result already borrowed by a consumer is
 * retained until release. IPC owner must quiesce its sends on detach; a bytes
 * frame already sent cannot be recalled by this in-process ownership layer. */
gt_task_rc_t gt__attachment_detach(gt_session_attachment_t *, uint64_t generation);
/* May release an old generation's lease after detach; consumer quiesces reads
 * first. Does not restore that generation's authorization. */
gt_task_rc_t gt__attachment_release(gt_session_attachment_t *, gt_attachment_checkpoint_t *);
/* Reap one terminal job, forget its executor slot, return context for caller
 * disposal outside locks. BUSY means cleanup still pending/no terminal job.
 * A failed checkpoint revokes its generation if still current; a newer
 * generation remains untouched. Its original error is returned in retired.
 * Reap regularly, including while waiting for take and after detach, before
 * destroying controller; task completion alone does not run an owner callback. */
gt_task_rc_t gt__attachment_reap(gt_session_attachment_t *, gt_attachment_retired_t *);
/* Exact task ownership, leaving terminal jobs of other generations/owners
 * untouched. Same cleanup/forget semantics as reap; task must be nonzero. */
gt_task_rc_t gt__attachment_reap_task(gt_session_attachment_t *,uint64_t task,gt_attachment_retired_t *);
/* Observational only: no active generation/jobs. Caller separately quiesces API
 * users before close; this is not an atomic ownership transfer or input fence. */
bool gt__attachment_quiescent(gt_session_attachment_t *);
/* Nonblocking. Requires detached, all jobs reaped, and API users quiesced.
 * Pool lifetime is separate; closing this controller never closes the pool. */
gt_task_rc_t gt__attachment_close(gt_session_attachment_t *);
#endif
