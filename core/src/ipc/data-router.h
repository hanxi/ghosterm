/* Shared local/SSH restoration owner. No terminal engine, socket or UI calls. */
#ifndef GT_IPC_DATA_ROUTER_H
#define GT_IPC_DATA_ROUTER_H
#include "data-wire.h"
#include "user-dispatcher.h"
typedef struct {const uint8_t *bytes;size_t length;} gt_snapshot_blob_t;
typedef struct {
  void *context;
  /* Checkpoint executor only, bounded, immutable source; all returned storage
   * is admitted through domain and the supplied limit, never a borrowed view
   * of a mutable terminal. Partial result (even on error) is owned by release.
   * Format/compatibility belongs to engine adapter, includes its validation. */
  gt_status_t (*encode)(void *,const void *frozen,size_t limit,const gt_allocator_t *,gt_snapshot_blob_t **);
  void (*release)(void *,gt_snapshot_blob_t *);
} gt_snapshot_encoder_t;
typedef struct {
  gt_session_runtime_t *runtime;gt_attachment_owner_t *attachments;uint8_t instance[16];
  size_t connections,user_commands,snapshot_limit,chunk_bytes;uint32_t format;
  uint32_t restore_timeout_ms,error_timeout_ms;
  void *clock_context;bool (*clock)(void *,uint64_t *ms);
  gt_snapshot_encoder_t encoder;
} gt_data_router_config_t;
typedef struct gt_data_router gt_data_router_t;
/* Fixed peer/capture contexts + shared native event and wire scratch + USER
 * mailboxes. Invalid limits reject before domain allocation. Limits 1..64
 * peers, 1..16 commands, 1..64MiB snapshot, chunks
 * 1..65536; restore deadline <=10min, error drain <=60s. Clock same monotonic
 * domain as server. Connection RX>=65560, TX>=max(80,16408,chunk_bytes+16).
 * Domain is thread-safe and outlives successful close, as do runtime/owner and
 * encoder/engine scopes. No production format is fabricated by this owner. */
gt_status_t gt__data_router_open(const gt_data_router_config_t *,const gt_allocator_t *,gt_data_router_t **);
/* Install all three server hooks with context=router. Single non-UI IO owner.
 * One inbound frame and at most one outbound frame per call; peer progress
 * only for accepted inbound messages. One snapshot/encoder job per generation.
 * S capture is on worker; already-terminal immutable state may begin after
 * published worker result, when S+1 is impossible. BIND never grants input. */
gt_server_data_rc_t gt__data_router_step(void *,gt_connection_t *,const gt_token_scope_t *);
gt_server_data_rc_t gt__data_router_revoked(void *,gt_connection_t *,const gt_token_scope_t *);
void gt__data_router_drop(void *,const gt_token_scope_t *);
/* Host timer calls even without peers: exact task reap and lease cleanup,
 * without joins/stopping sessions. Old jobs retain contexts until true reap. */
void gt__data_router_reap(gt_data_router_t *);
gt_runtime_rc_t gt__data_router_close(gt_data_router_t *);
#endif
