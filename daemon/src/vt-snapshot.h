/* Daemon composition only; no second restore state machine or wire channel. */
#ifndef GT_DAEMON_VT_SNAPSHOT_H
#define GT_DAEMON_VT_SNAPSHOT_H
#include "vt-state.h"
#include "ipc/data-router.h"
/* Factory is borrowed through router close and real task reaping. The returned
 * encoder owns no allocation until invoked; blobs use the router's allocator.
 * Uses GT_VT_SNAPSHOT_FORMAT. NULL factory returns an empty invalid encoder. */
gt_snapshot_encoder_t gt__vt_snapshot_encoder(gt_vt_factory_t *);
#endif
