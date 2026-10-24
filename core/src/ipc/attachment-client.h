/* Private shared attachment client, after authenticated HELLO/BIND bootstrap.
 * No engine, backend, SSH, OS or UI dependency. One non-UI IO owner. */
#ifndef GT_IPC_ATTACHMENT_CLIENT_H
#define GT_IPC_ATTACHMENT_CLIENT_H
#include "data-wire.h"
#include "user-wire.h"
#include "connection.h"
typedef struct gt_attachment_client gt_attachment_client_t;
typedef enum {GT_CLIENT_SINK_ACCEPTED,GT_CLIENT_SINK_BUSY,GT_CLIENT_SINK_FAILED} gt_client_sink_rc_t;
typedef struct {bool has_sequence;uint64_t generation,sequence;} gt_client_applied_t;
typedef struct {
  void *context;
  /* ACCEPTED copies/consumes all borrowed bytes before returning; asynchronous
   * application owns its own admitted copy. BUSY accepts nothing and borrows
   * nothing. FAILED may have changed state; owner is invalidated. Both accepted
   * and busy require *error=OK; failure requires a concrete non-OK error. */
  gt_client_sink_rc_t (*snapshot)(void *,uint64_t generation,uint64_t sequence,
    uint32_t format,const uint8_t *,size_t,bool readonly,gt_status_t *error);
  gt_client_sink_rc_t (*event)(void *,uint64_t generation,uint64_t sequence,
    const gt_state_event_t *,gt_status_t *error);
  /* Poll actual contiguous engine application, never mailbox admission. Known
   * sequence 0 is valid. Old-generation acknowledgments are ignored without
   * renewing deadlines; future generation/regression/ahead-of-submission fail. */
  gt_status_t (*applied)(void *,gt_client_applied_t *);
  /* Exactly once on failure/cancel/close, no calls back into this client. Sink
   * has no borrowed client memory; async engine work retains its own context.
   * Keep replicas readonly and revoke generation-specific host/input routes. */
  void (*invalidated)(void *,uint64_t generation,gt_status_t);
  /* Low frequency, owned POD during call. On disconnect every unconfirmed
   * command receives OUTCOME_UNKNOWN once, including queued-but-unsent bytes.
   * Never infer that a shell command executed; never automatically replay. */
  void (*result)(void *,const gt_user_reply_t *);
} gt_client_sink_t;
typedef struct {
  uint64_t session,generation;uint32_t format;size_t snapshot_limit,commands;
  uint32_t restore_timeout_ms,apply_timeout_ms,heartbeat_ms,peer_timeout_ms,
    command_timeout_ms,error_timeout_ms;
  void *clock_context;bool (*clock)(void *,uint64_t *ms);
  gt_client_sink_t sink;
} gt_client_config_t;
typedef enum {GT_CLIENT_RESTORING,GT_CLIENT_LIVE,GT_CLIENT_READONLY,GT_CLIENT_ENDED,GT_CLIENT_CLOSING,GT_CLIENT_FAILED} gt_client_phase_t;
typedef enum {GT_CLIENT_REASON_NONE,GT_CLIENT_REASON_PROTOCOL,GT_CLIENT_REASON_IO,
  GT_CLIENT_REASON_CLOCK,GT_CLIENT_REASON_RESTORE_DEADLINE,GT_CLIENT_REASON_APPLY_DEADLINE,
  GT_CLIENT_REASON_PEER_DEADLINE,GT_CLIENT_REASON_COMMAND_DEADLINE,
  GT_CLIENT_REASON_SINK,GT_CLIENT_REASON_SERVER,GT_CLIENT_REASON_CANCELLED} gt_client_reason_t;
typedef struct {
  gt_client_phase_t phase;gt_client_reason_t reason;gt_status_t error;
  bool has_sequence,has_terminal,readonly;uint64_t sequence;
  gt_managed_info_t terminal;gt_status_t state_error;
  bool has_process;gt_session_process_t process;
} gt_client_info_t;
typedef enum {GT_CLIENT_QUEUED,GT_CLIENT_BACKPRESSURE,GT_CLIENT_LOCKED,
  GT_CLIENT_INVALID,GT_CLIENT_UNAVAILABLE} gt_client_submit_t;
/* Adopts open, TX-idle connection on success only. Caller has verified the
 * instance/session/generation and flushed BIND_ACK. RX may already hold DATA.
 * RX>=65552, TX>=65560. Fixed charged object/command table/TX scratch + one validated snapshot buffer.
 * Commands 1..16, snapshot <=64MiB; all timeouts bounded and nonzero. Allocator
 * and sink/clock contexts outlive close. NULL allocator selects libc. */
gt_status_t gt__attachment_client_open(gt_connection_t *,const gt_client_config_t *,
  const gt_allocator_t *,gt_attachment_client_t **);
/* At most one connection step, one applied poll, one import or event dispatch
 * and one outgoing control frame. Sink BUSY holds one RX frame, bounds memory
 * and is subject to the application deadline; no UI event-queue byte bridge. */
gt_client_phase_t gt__attachment_client_step(gt_attachment_client_t *);
bool gt__attachment_client_info(const gt_attachment_client_t *,gt_client_info_t *);
/* LIVE only. Client stamps generation/ID/actual applied sequence; ignores the
 * caller's stamp. Copies into connection before QUEUED, no ID burned on local
 * backpressure. Server REJECT does not trigger replay or reuse a command ID. */
gt_client_submit_t gt__attachment_client_submit(gt_attachment_client_t *,const gt_user_request_t *,gt_worker_stamp_t *);
/* Engine-encoded input keeps its original applied sequence, not a later
 * mailbox/engine state observed when the native queue is drained. LIVE cut <=
 * sequence <= actual applied; generation must still match. */
gt_client_submit_t gt__attachment_client_submit_at(gt_attachment_client_t *,uint64_t generation,uint64_t sequence,const gt_user_request_t *,gt_worker_stamp_t *);
void gt__attachment_client_cancel(gt_attachment_client_t *);
/* Immediate transport detach, never session terminate. No pending borrowed
 * engine buffers or async callbacks target this handle. Erases owned storage. */
void gt__attachment_client_close(gt_attachment_client_t *);
#endif
