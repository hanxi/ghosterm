/* Recoverable local/SSH session Transport. All calls and callbacks use one
 * serial non-UI IO owner. Native UI consumes only low-frequency metadata.
 * Snapshot/event bytes go straight to the engine sink, never the UI queue. */
#ifndef GHOSTERM_SESSION_H
#define GHOSTERM_SESSION_H
#include "ghosterm/ssh.h"
#include "ghosterm/transport.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gt_session gt_session_t;
typedef enum {
  GT_SESSION_STOP_NONE,
  GT_SESSION_STOP_PENDING,
  GT_SESSION_STOP_KNOWN,
  GT_SESSION_STOP_UNKNOWN
} gt_session_stop_state_t;
typedef enum {
  GT_SESSION_CONTROL_OK,
  GT_SESSION_CONTROL_NOT_FOUND,
  GT_SESSION_CONTROL_BUSY,
  GT_SESSION_CONTROL_UNSUPPORTED,
  GT_SESSION_CONTROL_INTERNAL
} gt_session_control_status_t;
typedef struct {
  gt_session_stop_state_t state;
  uint64_t session, request;
  gt_session_control_status_t status; /* valid only when state=KNOWN */
  gt_status_t error;
} gt_session_stop_t;
typedef struct {
  uint64_t session, sequence;
  uint32_t phase; /* 0 active (includes startup), 1 reaping, 2 retained */
  bool terminating, has_result, has_sequence, has_exit_code;
  int32_t exit_code;
  uint32_t terminal_kind;
  gt_status_t status, termination_status;
} gt_session_row_t;
typedef enum { GT_SESSION_LOCAL, GT_SESSION_SSH } gt_session_kind_t;
typedef enum {
  GT_SESSION_STATE_STARTING,
  GT_SESSION_STATE_RECOVERING,
  GT_SESSION_STATE_RESTORING,
  GT_SESSION_STATE_LIVE,
  GT_SESSION_STATE_READONLY,
  GT_SESSION_STATE_ENDED,
  GT_SESSION_STATE_FAILED,
  GT_SESSION_STATE_CLOSED,
  GT_SESSION_STATE_CATALOG
} gt_session_state_t;
typedef enum {
  GT_SESSION_SINK_ACCEPTED,
  GT_SESSION_SINK_BUSY,
  GT_SESSION_SINK_FAILED
} gt_session_sink_result_t;
typedef enum {
  GT_SESSION_SUBMIT_QUEUED,
  GT_SESSION_SUBMIT_BACKPRESSURE,
  GT_SESSION_SUBMIT_LOCKED,
  GT_SESSION_SUBMIT_INVALID,
  GT_SESSION_SUBMIT_UNAVAILABLE
} gt_session_submit_t;
typedef enum {
  GT_SESSION_COMMAND_INPUT,
  GT_SESSION_COMMAND_RESIZE,
  GT_SESSION_COMMAND_COLOR,
  GT_SESSION_COMMAND_STYLE
} gt_session_command_t;
typedef enum {
  GT_SESSION_NOT_EXECUTED,
  GT_SESSION_OPERATION_OK,
  GT_SESSION_OUTCOME_UNKNOWN
} gt_session_delivery_t;
typedef enum {
  GT_SESSION_OUTPUT = 1,
  GT_SESSION_RESIZE = 2,
  GT_SESSION_COLOR = 4,
  GT_SESSION_STYLE = 8
} gt_session_event_kind_t;
typedef struct {
  uint16_t cols, rows;
  uint32_t px_width, px_height;
} gt_session_size_t;
typedef struct {
  bool known;
  uint64_t generation, sequence;
} gt_session_applied_t;
typedef struct {
  uint64_t generation, command_id, sequence;
} gt_session_stamp_t;
typedef struct {
  gt_session_command_t kind;
  gt_session_stamp_t stamp;
  gt_status_t status;
  gt_session_delivery_t delivery;
  bool rejected;
  uint32_t rejection;
} gt_session_receipt_t;
/* Current resolved defaults; OSC overrides remain terminal state. No native
 * struct layout is sent on wire. dark must be 0/1, palette is 256 RGB entries. */
typedef struct {
  uint32_t dark;
  uint8_t background[3], foreground[3], cursor[3], palette[256][3];
} gt_session_style_t;
typedef struct {
  gt_session_event_kind_t kind;
  const uint8_t *bytes;
  size_t length; /* OUTPUT only, borrowed during callback */
  gt_session_size_t size;
  bool dark;
  gt_session_style_t style;
} gt_session_event_t;
typedef struct {
  void *context;
  /* ACCEPTED consumes/copies all bytes; BUSY borrows nothing. Both set
   * error=OK. FAILED requires non-OK and invalidates the replica. No callback
   * reentry. */
  gt_session_sink_result_t (*snapshot)(void *, uint64_t generation,
                                       uint64_t sequence, uint32_t format,
                                       const uint8_t *, size_t, bool readonly,
                                       gt_status_t *error);
  gt_session_sink_result_t (*event)(void *, uint64_t generation,
                                    uint64_t sequence,
                                    const gt_session_event_t *,
                                    gt_status_t *error);
  gt_status_t (*applied)(void *, gt_session_applied_t *);
  void (*invalidated)(void *, uint64_t generation, gt_status_t);
  /* Low frequency. OPERATION_OK means backend accepted bytes/operation, never
   * that a shell command executed. Unconfirmed commands become UNKNOWN once. */
  void (*receipt)(void *, const gt_session_receipt_t *);
} gt_session_sink_t;
typedef struct {
  uint8_t nonce[16]; /* nonzero stable creation identity, never replay CREATE */
  gt_pty_opts_t
      process; /* explicit absolute executable/cwd; argv/env bounded */
} gt_session_create_t;
typedef struct {
  void *context;
  /* Async native DNS + platform credential unlock. Return immediately; supply
   * a fresh SSH object on this IO owner after callback returns. IDs never
   * repeat. No completions may target the session after close has succeeded. */
  void (*requested)(void *, uint64_t request, uint32_t attempt);
  void (*cancelled)(void *, uint64_t request);
  void (*hostkey)(void *, uint64_t request, const char *type,
                  const char *fingerprint);
} gt_session_auth_t;
typedef struct {
  gt_session_kind_t kind;
  const char *proxy, *daemon, *runtime;
  uint8_t compatibility[32], instance[16];
  uint64_t session, required_features;
  const gt_session_create_t *create;
  /* Exactly one mode: create, session, pending_nonce, catalog_only.
   * Resume and pending creation lookup require the original nonzero instance.
   * Pending nonce performs QUERY_CREATE only, even after ledger expiry. */
  uint8_t pending_nonce[16];
  bool catalog_only;
  uint32_t
      snapshot_format; /* exact engine format, no legacy backend negotiation */
  size_t snapshot_limit; /* 0 -> 16MiB, max 64MiB */
  uint32_t recovery_timeout_ms, authentication_timeout_ms; /* 0 -> 60000 */
  void *clock_context;
  bool (*clock)(void *, uint64_t *monotonic_ms);
  gt_session_sink_t sink;
  gt_session_auth_t authentication;
} gt_session_options_t;
/* Low-frequency Source PTY observation, independent of terminal state sequence.
 * Unknown child and unavailable cwd are explicit. UTF-8 cwd is never truncated. */
typedef struct {
  gt_transport_child_t child;
  bool has_cwd;
  char cwd[1024];
} gt_session_process_t;
typedef struct {
  gt_session_state_t state;
  gt_status_t error;
  uint32_t reason, attempts, busy_replies;
  uint8_t instance[16];
  uint64_t session, generation, sequence;
  bool has_sequence, readonly, has_terminal, has_exit_code;
  int32_t exit_code;
  uint32_t terminal_kind;
  gt_status_t terminal_status, termination_status, state_error;
  bool has_admitted_create;
  gt_session_stop_t stop;
  bool has_process;
  gt_session_process_t process;
} gt_session_info_t;
/* Construction copies paths/create metadata, does no connect/spawn until step.
 * After step first reports has_admitted_create, persist instance + original
 * nonce BEFORE the next step may flush CREATE. On process restart use only
 * pending_nonce lookup, never create a replacement automatically.
 * All context pointers outlive successful close. NULL clock uses monotonic OS
 * clock. */
gt_status_t gt_session_open(const gt_session_options_t *, gt_session_t **);
gt_session_state_t gt_session_step(gt_session_t *);
bool gt_session_info(const gt_session_t *, gt_session_info_t *);
/* Catalog mode needs no engine sink or snapshot_format and opens no DATA
 * stream. Only metadata crosses to UI. NOT_FOUND means not loaded; OK with
 * count=0 is a known empty catalog. Capacity 0..64, required count on small
 * capacity, all-or-nothing rows. No names, commands or secrets are persisted.
 */
gt_status_t gt_session_catalog(gt_session_t *, gt_session_row_t *,
                               size_t capacity, size_t *count);
gt_session_submit_t gt_session_catalog_refresh(gt_session_t *);
/* Explicit termination, separate from detach/cancel/close. QUEUED is not exit
 * confirmation. Info.stop distinguishes pending, known admission, and UNKNOWN;
 * the coordinator never replays this request after ambiguous disconnect. */
gt_session_submit_t gt_session_terminate(gt_session_t *, uint64_t session);
/* Encoded input carries its source generation/actual engine applied sequence.
 * Old generation, ahead-of-engine or pre-barrier sequence is rejected. No
 * replay. QUEUED copies bytes but is not an execution acknowledgment. */
gt_session_submit_t gt_session_input(gt_session_t *, uint64_t generation,
                                     uint64_t sequence, const void *, size_t,
                                     gt_session_stamp_t *);
gt_session_submit_t gt_session_resize(gt_session_t *, uint64_t generation,
                                      uint64_t sequence, gt_session_size_t,
                                      gt_session_stamp_t *);
gt_session_submit_t gt_session_color(gt_session_t *, uint64_t generation,
                                     uint64_t sequence, bool dark,
                                     gt_session_stamp_t *);
/* Ordered style replacement uses the same generation/LIVE/input authorization
 * and result semantics. Updates defaults only, preserving program OSC overrides. */
gt_session_submit_t gt_session_style(gt_session_t *, uint64_t generation,
                                     uint64_t sequence, const gt_session_style_t *,
                                     gt_session_stamp_t *);
/* Supply takes unlocked fresh SSH on success only, clears *ssh; address family
 * 4/6 is a resolved binary address separate from original SSH hostkey identity.
 */
gt_status_t gt_session_auth_supply(gt_session_t *, uint64_t request,
                                   gt_ssh_t **, uint8_t family,
                                   const uint8_t address[16], uint32_t scope);
/* Adopt a dedicated authenticated connection from the setup authenticator.
 * Same IO owner, no shell/SFTP aliases; takes/clears only on success. Original
 * request/deadline still apply. No second TCP connection or handshake. */
gt_status_t gt_session_auth_adopt(gt_session_t *, uint64_t request, gt_ssh_t **);
gt_status_t gt_session_auth_reject(gt_session_t *, uint64_t request,
                                   gt_status_t);
gt_status_t gt_session_auth_decide(gt_session_t *, uint64_t request,
                                   gt_hostkey_decision_t);
void gt_session_cancel(gt_session_t *); /* detach, never TERMINATE */
/* Requests detach then polls owned local-proxy reaping without waiting. false:
 * retain handle/context and call again on the same owner; true: handle freed.
 * Reentrant close returns false. No callbacks after true. */
bool gt_session_close(gt_session_t *);
#ifdef __cplusplus
}
#endif
#endif
