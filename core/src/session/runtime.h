/* Private shared local/remote host runtime. No engine headers or UI dependency. */
#ifndef GT_SESSION_RUNTIME_H
#define GT_SESSION_RUNTIME_H
#include "creation.h"
#include "attachment.h"
typedef struct gt_session_runtime gt_session_runtime_t;
typedef struct gt_runtime_session gt_runtime_session_t;
typedef enum {GT_RUNTIME_OK,GT_RUNTIME_INVALID,GT_RUNTIME_NOT_FOUND,
  GT_RUNTIME_BUSY,GT_RUNTIME_CLOSED,GT_RUNTIME_INTERNAL} gt_runtime_rc_t;
typedef struct {
  void *context;
  /* Worker-only; ID/principal are bound before entry. Spec is immutable and
   * borrowed until worker join. Failed opens clean their own partial state.
   * Returns an independent state context for ops/close/invalidated. */
  gt_status_t (*open)(void *,uint64_t principal,uint64_t session,
    const gt_create_spec_t *,gt_worker_reply_sink_t,void **state);
  gt_state_ops_t ops;
  void (*close)(void *state);
  void (*invalidated)(void *state,gt_status_t,uint64_t);
  bool color_scheme_supported;
  bool style_supported;
  /* Releases independent freeze output off UI (capture worker/encoder or final
   * lifecycle owner). Thread-safe, no affinity to the already closed state.
   * Final object is released only after every runtime lease/job is gone.
   * Factory context and engine domain outlive runtime close and task cleanup. */
  void (*release_frozen)(void *factory_context,void *frozen);
} gt_runtime_state_factory_t;
typedef struct {
  uint8_t instance[16];
  size_t sessions,creation_records,queue_bytes,queue_events;
  uint32_t creation_retention_ms;
  gt_checkpoint_tasks_t *tasks; /* borrowed, outlives successful runtime close */
  gt_runtime_state_factory_t state;
  gt_thread_options_t worker_threads,backend_threads; /* all-zero = OS defaults */
} gt_runtime_config_t;
/* One control/lifecycle owner creates, reaps, forgets and closes. Worker and
 * lease access may run concurrently. Domain covers runtime/table, manager,
 * ledger/jobs, session objects, attachments/queues, workers and POSIX PTYs;
 * engine/task-pool allocations and OS thread/descriptor capacity are separate.
 * Thread-safe allocator and state factory context outlive successful close.
 * Allocator callbacks must not reenter runtime/manager/attachment operations. */
gt_status_t gt__runtime_open(const gt_runtime_config_t *,uint64_t now_ms,
  const gt_allocator_t *,gt_session_runtime_t **);
/* Borrowed for the common service; never create/reap/forget/close manager or
 * close ledger directly. Service and token owners must quiesce before close. */
gt_session_manager_t *gt__runtime_manager(gt_session_runtime_t *);
gt_creation_t *gt__runtime_creation(gt_session_runtime_t *);
/* Thread-safe owned reference, max 256 per session. Reserved/unpublished IDs
 * are BUSY; wrong trusted principal/unknown ID are both NOT_FOUND. Never copy
 * ownership or use a pointer after release; release clears caller's pointer.
 * Keep a reference through each active connection and until each checkpoint
 * task has been reaped and its context disposed, including detached old jobs. */
gt_runtime_rc_t gt__runtime_acquire(gt_session_runtime_t *,uint64_t trusted_principal,
  uint64_t session,gt_runtime_session_t **);
void gt__runtime_release(gt_runtime_session_t **);
/* Borrowed for the lifetime of the reference; no backend/state pointer escapes.
 * Do not close it. Attachment APIs retain their S/L/input-generation contracts.
 * This does not make BIND equivalent to LIVE or implement wire input delivery. */
bool gt__runtime_process(gt_runtime_session_t *,gt_session_process_t *);
gt_session_attachment_t *gt__runtime_attachment(gt_runtime_session_t *);
typedef struct {
  gt_worker_result_t result;
  gt_status_t capture_status;
  const void *frozen;uint64_t sequence;
} gt_runtime_final_t;
/* Terminal-only, no worker/engine mutation. BUSY until worker result published,
 * even if capture sink already ran. OK may report unavailable/error snapshot:
 * inspect capture_status/frozen separately from process result/has_sequence.
 * Independent immutable frozen is borrowed through this session lease, survives
 * join and state destruction. Never free/mutate it; async encoders hold a lease.
 * Failure clears out. Query never consumes/frees retained state or invents 0. */
gt_runtime_rc_t gt__runtime_final(gt_runtime_session_t *,gt_runtime_final_t *);
/* Trusted restoration host operation. Completion runs on worker at S, before
 * any S+1 event. Keep lease/context through callback/task cleanup. Success owns
 * capture.frozen; release_capture returns it to this factory, clearing POD.
 * Never pass the borrowed final snapshot to release_capture. */
gt_manager_rc_t gt__runtime_capture(gt_runtime_session_t *,gt_worker_completion_t,void *,gt_worker_submit_t *);
void gt__runtime_release_capture(gt_runtime_session_t *,gt_state_capture_t *);
/* Client INPUT/RESIZE/COLOR_SCHEME only through USER submission. Holds no extra
 * callback context/reference: caller retains its session lease and completion
 * context through callback, even after data disconnect. Admission is queued,
 * not executed. Rejections at worker policy are task-local AUTH/PROTOCOL; do
 * not replay ambiguous commands. Raw manager submit_internal is host-only. */
/* POSIX resize host limits are checked before queueing: zero cells INVALID,
 * pixels >65535 UNSUPPORTED, no callback/ID consumption or worker termination. */
gt_manager_rc_t gt__runtime_submit_user(gt_runtime_session_t *,gt_worker_command_t,
  const gt_worker_stamp_t *,const void *,size_t,gt_worker_user_completion_t,void *,gt_worker_submit_t *);
/* Lifecycle owner only, never callbacks. Join preserves session/attachment;
 * forget needs a retained result, zero references and an idle controller.
 * Neither operation implicitly detaches a client or forgets checkpoint jobs. */
gt_manager_rc_t gt__runtime_reap(gt_session_runtime_t *,uint64_t session);
gt_runtime_rc_t gt__runtime_forget(gt_session_runtime_t *,uint64_t session);
/* Explicit host shutdown, API/control callers quiesced. BUSY leaves the runtime
 * usable and does not stop workers while references/active jobs remain. Once
 * admissible, stops/joins workers before closing controllers, ledger and table.
 * Never called merely because a local/SSH connection disappears. */
gt_runtime_rc_t gt__runtime_close(gt_session_runtime_t *);
#endif
