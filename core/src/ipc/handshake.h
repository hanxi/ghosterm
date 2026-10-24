/* Shared local/remote server bootstrap. No backend, engine or UI calls. */
#ifndef GT_IPC_HANDSHAKE_H
#define GT_IPC_HANDSHAKE_H
#include "connection.h"
#include "tokens.h"
#include "bootstrap-wire.h"
typedef enum {GT_HANDSHAKE_HELLO,GT_HANDSHAKE_HELLO_ACK,GT_HANDSHAKE_BIND,
  GT_HANDSHAKE_BIND_ACK,GT_HANDSHAKE_READY,GT_HANDSHAKE_TAKEN,GT_HANDSHAKE_FAILED} gt_handshake_state_t;
typedef enum {GT_HANDSHAKE_NO_ERROR,GT_HANDSHAKE_PROTOCOL,GT_HANDSHAKE_COMPATIBILITY,
  GT_HANDSHAKE_FEATURES,GT_HANDSHAKE_INSTANCE,GT_HANDSHAKE_TOKEN,
  GT_HANDSHAKE_TIMEOUT,GT_HANDSHAKE_CLOCK,GT_HANDSHAKE_IO,GT_HANDSHAKE_ENTROPY} gt_handshake_error_t;
typedef struct {
  gt_protocol_identity_t identity;
  uint64_t principal; /* trusted peer identity; never a wire field */
  uint32_t timeout_ms; /* 1..60000, entire HELLO/ACK/BIND/ACK transaction */
  gt_token_store_t *tokens;
  void *clock_context;bool (*clock)(void *,uint64_t *);
  gt_token_random_t random; /* fill NULL selects OS CSPRNG */
  void *binding_context;
  /* Required when tokens supplied. Must be bounded/nonblocking, no reentry.
   * current checks authority generation still exists; drop detaches ONLY that
   * generation, never ends session or revokes a newer generation. */
  bool (*binding_current)(void *,const gt_token_scope_t *);
  void (*binding_drop)(void *,const gt_token_scope_t *);
} gt_handshake_config_t;
typedef struct gt_handshake gt_handshake_t;
/* Borrows a fresh, unstepped connection/store/contexts. No ownership on
 * constructor failure. Handshakes and tokens share one I/O owner and one
 * monotonic clock domain; this module does not create an event loop.
 * Connection limits must admit the fixed schemas (at least 80 bytes each).
 * Peer identity must already be verified by endpoint. One charged allocation. */
gt_status_t gt__handshake_open(gt_connection_t *,const gt_handshake_config_t *,const gt_allocator_t *,gt_handshake_t **);
gt_handshake_state_t gt__handshake_step(gt_handshake_t *);
gt_handshake_error_t gt__handshake_error(const gt_handshake_t *);
/* Drives one final step; success transfers live connection and, for DATA,
 * generation responsibility to caller. Management returns zero binding.
 * READY alone does NOT enable terminal input: restore S/L barriers still apply.
 * Further frames remain queued for the real command/restore dispatcher. */
bool gt__handshake_take(gt_handshake_t *,gt_protocol_role_t *,gt_token_scope_t *);
/* Before take: cancel connection and drop any consumed reservation once.
 * After take: only free handshake; caller owns connection/binding lifecycle.
 * Rejection replies wait for challenge ACK or bounded timeout before closing,
 * so HUP does not discard the unread rejection. Nothing stops a session. */
void gt__handshake_close(gt_handshake_t *);
#endif
