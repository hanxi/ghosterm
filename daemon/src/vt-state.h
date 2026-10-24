/* Private daemon composition: the common runtime owns a headless VT instance.
 * This candidate needs the pinned experimental engine APIs, never a UI host. */
#ifndef GT_DAEMON_VT_STATE_H
#define GT_DAEMON_VT_STATE_H
#include "session/runtime.h"
#include "checkpoint-wire.h"

typedef struct gt_vt_factory gt_vt_factory_t;
typedef struct gt_vt_encoded gt_vt_encoded_t;
typedef struct {
  size_t budget_bytes,state_bytes,frozen_bytes,encoder_bytes,checkpoint_bytes;
  size_t continuation_bytes,history_bytes,history_lines;
  uint64_t image_bytes;
  uint32_t cell_width,cell_height; /* pixels per cell, never PTY window pixels */
  gt_vt_colors_t light,dark;
  bool initial_dark;
  /* Not yet implementable by the common ordered DATA event stream. Explicit
   * request is UNSUPPORTED; false configures all three media off in the engine.
   * This restriction is not a claim that the final recovery profile is ready. */
  bool external_resources;
} gt_vt_config_t;

/* Lifecycle owner; allocator is thread-safe, borrowed until successful close.
 * Engine general/page allocations use stable, separately admitted scopes.
 * Factory/record allocations use allocator. OS threads/stacks remain external.
 * Process-global PNG decoder is installed once before opening any state. */
gt_status_t gt__vt_factory_open(const gt_vt_config_t *,const gt_allocator_t *,gt_vt_factory_t **);
gt_runtime_state_factory_t gt__vt_factory_state(gt_vt_factory_t *);
/* Immutable captures from that state factory only. Encoder worker, no source
 * terminal needed. Output owns its admitted scope until encoded_release.
 * canonical/resources and metadata feed the common checkpoint container.
 * Limit is capped by factory policy; 1..64MiB accepted, never raised silently.
 * Caller retains capture and factory throughout encode; no concurrent release. */
gt_status_t gt__vt_checkpoint_encode(gt_vt_factory_t *,const void *frozen,
  size_t limit,gt_vt_encoded_t **);
gt_vt_snapshot_view_t gt__vt_encoded_view(const gt_vt_encoded_t *);
void gt__vt_encoded_release(gt_vt_encoded_t *);
/* All API callers quiesced. BUSY while any state, frozen or encoded object
 * exists; BUSY neither invalidates the factory nor stops a session. */
gt_runtime_rc_t gt__vt_factory_close(gt_vt_factory_t *);
#endif
