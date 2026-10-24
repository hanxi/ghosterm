/* Shared non-UI service owner. No socket, libssh2, engine or platform UI calls. */
#ifndef GT_IPC_SERVER_H
#define GT_IPC_SERVER_H
#include "handshake.h"
#include "control.h"
typedef enum {GT_SERVER_RUNNING,GT_SERVER_CLOCK_FAILED,GT_SERVER_CLOSED} gt_server_state_t;
typedef enum {GT_SERVER_ADMITTED,GT_SERVER_ADMIT_INVALID,GT_SERVER_ADMIT_FULL,
  GT_SERVER_ADMIT_CLOSED,GT_SERVER_ADMIT_IDS_EXHAUSTED,GT_SERVER_ADMIT_NO_MEMORY,
  GT_SERVER_ADMIT_CLOCK,GT_SERVER_ADMIT_INTERNAL} gt_server_admit_t;
typedef enum {GT_SERVER_DATA_IDLE,GT_SERVER_DATA_PROGRESS,GT_SERVER_DATA_CLOSE} gt_server_data_rc_t;
typedef struct {
  gt_session_manager_t *manager; /* borrowed; no implicit stop/join on close */
  gt_creation_t *creation; /* optional borrowed ledger; same manager/instance */
  /* Optional shared runtime reservation owner, exclusively claimed until close.
   * Manager/creation/instance must match. Handshake tokens/binding fields must
   * be unset; server derives them from this owner (one source of truth).
   * ATTACH is explicitly unsupported without both owner and data dispatcher. */
  gt_attachment_owner_t *attachments;
  gt_handshake_config_t handshake; /* shared identity, clock, tokens and callbacks */
  size_t connections; /* 1..64, fixed slot allocation */
  gt_connection_limits_t limits; /* RX >= 80, TX >= CONTROL_REPLY_BYTES */
  uint32_t idle_timeout_ms; /* 1..60000; complete valid peer messages only */
  void *data_context;
  /* Optional data/restore dispatcher, one bounded attempt, no reentry or joins.
   * Server already stepped connection and checked current generation. Borrows
   * connection/scope only during call; cannot close/destroy/retain either.
   * PROGRESS means a valid peer message was accepted, never mere output traffic.
   * NULL explicitly disables DATA at HELLO, even if a token store is supplied.
   * Callback must still enforce S/L and input barriers; BIND is not LIVE. */
  gt_server_data_rc_t (*data_step)(void *,gt_connection_t *,const gt_token_scope_t *);
  /* Optional retirement hook for stateful data dispatchers. Runs exactly once
   * after binding revoked, before connection destroyed; no joins/reentry.
   * Callback context survives server_close until all async work is reaped. */
  void (*data_drop)(void *,const gt_token_scope_t *);
  /* Optional bounded error drain after generation revoked. Only IDLE keeps
   * stream alive; no peer progress/lease renewal or session commands allowed.
   * Existing idle/frame deadlines still apply. Requires step+drop. */
  gt_server_data_rc_t (*data_revoked)(void *,gt_connection_t *,const gt_token_scope_t *);
} gt_server_config_t;
typedef struct gt_server gt_server_t;
/* Single non-UI owner. One fixed server allocation plus charged connection/
 * handshake allocations, no I/O threads. All borrowed contexts/manager/tokens/
 * creation ledger outlive close.
 * CREATE commits a new worker through the optional creation ledger. Does not
 * listen, directly operate backends/engines, reap workers, or implement data
 * restoration. Worker factories perform actual process/state launch. */
gt_status_t gt__server_open(const gt_server_config_t *,const gt_allocator_t *,gt_server_t **);
/* Always consumes valid ops (all callbacks supplied), including rejection:
 * closes refused streams exactly once. Invalid ops are untouched. Clears id.
 * Success assigns a never-reused server-local issuer ID. Stream context must
 * remain alive until connected(id) becomes false or server_close returns. */
gt_server_admit_t gt__server_admit(gt_server_t *,const gt_stream_ops_t *,uint64_t trusted_principal,uint64_t *id);
/* One bounded round over fixed slots, rotating the start. Partial output does
 * not block reading other peers. Handshake handoff can perform a second bounded
 * IO step; never poll/wait/retry, join or send terminal bytes to a UI queue.
 * Global clock/token failure closes peers only; never terminates manager. */
gt_server_state_t gt__server_step(gt_server_t *);
bool gt__server_connected(const gt_server_t *,uint64_t id);
bool gt__server_wants_write(const gt_server_t *,uint64_t id);
size_t gt__server_connections(const gt_server_t *);
/* Revoke pending tokens by issuer and detach consumed data generations once.
 * Never close manager/store or stop backend. Caller quiesces all operations. */
void gt__server_close(gt_server_t *);
#endif
