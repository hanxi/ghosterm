/* Private runtime building block. Not a platform binding or daemon protocol. */
#ifndef GT_SESSION_WORKER_H
#define GT_SESSION_WORKER_H
#include "state-owner.h"
#include "ghosterm/session.h"
#include "io/backend.h"
#include "base/allocator.h"
#include "base/thread-options.h"

typedef struct gt_session_worker gt_session_worker_t;
enum { GT_STATE_OUTPUT = 1, GT_STATE_RESIZE = 2, GT_STATE_COLOR_SCHEME = 4, GT_STATE_STYLE = 8 };
typedef struct { uint16_t cols, rows; uint32_t px_width, px_height; } gt_state_size_t;
typedef struct {uint32_t dark;} gt_state_color_scheme_t; /* dark: 0 light, 1 dark. */
typedef enum { GT_WORKER_INPUT, GT_WORKER_RESIZE, GT_WORKER_CAPTURE, GT_WORKER_COLOR_SCHEME, GT_WORKER_STYLE } gt_worker_command_t;
typedef struct {uint64_t generation,command_id,applied_sequence;} gt_worker_stamp_t;
typedef enum { GT_WORKER_ACCEPTED, GT_WORKER_BACKPRESSURE,
               GT_WORKER_STOPPED, GT_WORKER_INVALID, GT_WORKER_NO_MEMORY, GT_WORKER_UNSUPPORTED } gt_worker_submit_t;
/* Private low-frequency terminal result. Never contains terminal/user text.
 * Sequence is last successful authority apply, not a GUI-applied ack. */
typedef enum {GT_WORKER_END_UNKNOWN,GT_WORKER_END_STOPPED,GT_WORKER_END_EOF,GT_WORKER_END_ERROR} gt_worker_end_t;
typedef enum {GT_WORKER_STAGE_NONE,GT_WORKER_STAGE_STATE_OPEN,GT_WORKER_STAGE_BACKEND_OPEN,
  GT_WORKER_STAGE_READ,GT_WORKER_STAGE_INPUT,GT_WORKER_STAGE_RESIZE,
  GT_WORKER_STAGE_APPLY,GT_WORKER_STAGE_REPLY,GT_WORKER_STAGE_CAPTURE} gt_worker_stage_t;
typedef struct {
  gt_worker_end_t kind;
  gt_worker_stage_t stage;
  gt_status_t status;
  bool has_sequence,has_exit_code;
  uint64_t sequence;
  int32_t exit_code;
  gt_status_t termination_status; /* NOT_FOUND unknown, UNSUPPORTED absent observer. */
  gt_io_exit_t termination;
} gt_worker_result_t;
typedef enum {GT_WORKER_RESULT_INVALID,GT_WORKER_RESULT_PENDING,GT_WORKER_RESULT_READY} gt_worker_result_rc_t;
/* Completion runs on the worker without its queue lock. capture ownership
 * transfers on success; no callback may join/free the worker or block on GUI.
 * On input, OK means backend accepted bytes, not peer execution/acknowledgment. */
typedef void (*gt_worker_completion_t)(void *, gt_status_t, gt_state_capture_t);
typedef enum {GT_USER_NOT_EXECUTED,GT_USER_OPERATION_OK,GT_USER_OUTCOME_UNKNOWN} gt_user_delivery_t;
typedef struct {gt_worker_stamp_t stamp;gt_status_t status;gt_user_delivery_t delivery;} gt_worker_user_result_t;
/* Borrowed POD during callback. OPERATION_OK means backend/state accepted the
 * operation, never that a shell command ran. An attempted backend/state error
 * is UNKNOWN (a prefix/side effect may exist), even when status equals a policy
 * rejection's status. NOT_EXECUTED proves this operation never reached either. */
typedef void (*gt_worker_user_completion_t)(void *,const gt_worker_user_result_t *);
/* Only state apply callbacks on the owner may emit replies. The sink copies
 * bytes into a separate 64 KiB staging buffer, never performs backend I/O.
 * Exceeding the bound is a sticky worker fault even if a parser ignores it. */
typedef struct {
  void *context;
  gt_status_t (*write)(void *, const uint8_t *, size_t);
} gt_worker_reply_sink_t;
typedef struct {
  void *context; /* Borrowed until join returns. Factories run on worker. */
  gt_io_backend_t *(*open_backend)(void *);
  gt_status_t (*open_state)(void *, gt_worker_reply_sink_t, void **);
  void (*close_state)(void *);
  gt_state_ops_t state_ops;
  /* Direct stream consumer, after successful state apply; borrowed bytes.
   * Must be bounded/nonblocking. No UI queue or synchronous GUI call. */
  void (*output)(void *, const uint8_t *, size_t, uint64_t);
  /* Every successful authority mutation, after reply flush attempt, including
   * failed reply delivery. Failed apply never publishes. Direct data path,
   * never UI. The observer may revoke a slow attachment without stopping state. */
  void (*event)(void *, const gt_state_event_t *, uint64_t);
  /* Sticky owner failure only, after backend close and before close_state.
   * Borrowed state context, exact owner error, last successful apply sequence.
   * State may already be partly changed: adapter must discard it, never export
   * a final snapshot. Last sequence does not certify current state validity.
   * Optional; close_state remains mandatory and runs exactly once. This owner
   * callback must be bounded, must not join, and result is not READY yet. */
  void (*state_invalidated)(void *state, gt_status_t, uint64_t last_sequence);
  /* Default false: reject COLOR_SCHEME before allocation/apply, preserve state.
   * True promises state_ops.apply handles GT_STATE_COLOR_SCHEME on the owner. */
  bool color_scheme_supported;
  bool style_supported;
  /* Optional metadata only, <=4 observations/s and unchanged values coalesced.
   * Runs on Source owner, never advances authority sequence or carries bytes. */
  void (*process)(void *,const gt_session_process_t *);
  void (*finished)(void *, gt_status_t);
  /* Optional USER command policy, on worker immediately before backend/state
   * execution, without queue lock. No I/O/waits/reentry. Non-OK rejects only
   * this command; no write/resize/apply, no sticky session failure. Context is
   * config.context, not state. Returning OK is the authorization commit point:
   * later detach cannot recall an already executing write or backend backlog. */
  gt_status_t (*authorize)(void *,gt_worker_command_t,const gt_worker_stamp_t *);
  /* Optional terminal snapshot sink, exactly once on worker before close_state
   * and result publication, after backend close. Healthy EOF/STOP freezes an
   * independent immutable object; error/start failure never exports state.
   * Transfers capture.frozen on OK; sink owns its adapter release. Ordinary
   * freeze failure is separate from session outcome; a NULL success violates
   * owner contract and becomes a CAPTURE error. No UI/joins/reentry. */
  void (*final_capture)(void *,gt_status_t,gt_state_capture_t);
} gt_worker_config_t;
/* Start is asynchronous. Factory failure is reported through finished. */
gt_session_worker_t *gt__worker_start(const gt_worker_config_t *);
/* Domain covers worker (inline queue/reply buffer) and copied request payloads.
 * NULL = libc. Concurrent submit/owner completion may call allocator on different
 * threads; caller supplies thread-safe callbacks and keeps context until join.
 * Does not cover thread stack or backend/state factory allocations. Failed
 * submit owns no input and schedules no callback; stop/join releases queued data. */
gt_session_worker_t *gt__worker_start_with_allocator(const gt_worker_config_t *,const gt_allocator_t *);
/* Explicit stack/guard attributes; invalid settings reject, no fallback.
 * Caller admits OS stack capacity separately and retains it until close/join. */
gt_session_worker_t *gt__worker_start_with_options(const gt_worker_config_t *,const gt_allocator_t *,const gt_thread_options_t *);
/* Fixed queue: <=16 outstanding requests and <=64 KiB copied input, including
 * the in-flight command. Each submission is accepted in full or not at all.
 * Resize payload must be exactly gt_state_size_t; capture has no payload.
 * COLOR_SCHEME copies a validated gt_state_color_scheme_t and applies it through
 * the same owner/sequence/capture boundary; it never writes user bytes or resizes
 * the backend. Unsupported adapters return UNSUPPORTED without mutating state. */
gt_worker_submit_t gt__worker_submit_internal(gt_session_worker_t *, gt_worker_command_t,
                                    const void *, size_t,
                                    gt_worker_completion_t, void *);
/* Trusted host operation above bypasses USER authorization deliberately; never
 * route client input through it. User submission copies stamp and bytes into
 * the same bounded queue; callback is mandatory and borrowed until completion.
 * Missing policy => UNSUPPORTED, malformed/zero generation or command ID =>
 * INVALID; neither accepts data or invokes callback. CAPTURE is internal-only.
 * ACCEPTED means queued, never executed; authorization may fail later. */
gt_worker_submit_t gt__worker_submit_user(gt_session_worker_t *,gt_worker_command_t,
  const gt_worker_stamp_t *,const void *,size_t,gt_worker_user_completion_t,void *);
/* Stop has reserved capacity and cannot be starved by a full queue. Already
 * executing callbacks/backend calls finish; queued commands get CANCELLED.
 * Read is bounded to 10 ms. Wake-capable backends (PTY) interrupt read on
 * submit/stop; other backends retain the bounded wait. Pending commands use
 * a nonblocking read between executions. Existing write/close bounds apply. */
void gt__worker_stop(gt_session_worker_t *);
/* Immutable result published after backend/state/queued command cleanup, before
 * finished callback. Query is thread-safe until join, which requires queries
 * quiesced like submitters. PENDING/INVALID zero output; inspect rc before fields.
 * finished may run before start returns: callback queries need a safely published
 * handle. Caller can query after start returns and copy before join frees it.
 * EOF records only an exit code already known before close; absence stays unknown.
 * Explicit observe_exit supplies CODE/SIGNAL; legacy exited() is never used.
 * Missing observer is preserved as termination_status=UNSUPPORTED. Other
 * unavailable observations stay NOT_FOUND, invalid observation reports PROTOCOL. */
gt_worker_result_rc_t gt__worker_result(gt_session_worker_t *,gt_worker_result_t *);
/* Non-UI lifecycle owner only. Caller quiesces submitters before freeing;
 * exactly one join, never from a worker callback. Does not implicitly stop. */
void gt__worker_join(gt_session_worker_t *);
#endif
