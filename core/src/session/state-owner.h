/* Private session-thread state boundary; no engine or UI dependency. */
#ifndef GT_SESSION_STATE_OWNER_H
#define GT_SESSION_STATE_OWNER_H
#include <pthread.h>
#include "ghosterm/types.h"

typedef struct {
  uint32_t kind;
  const void *payload;
  size_t length;
} gt_state_event_t;

typedef struct {
  /* A failing apply may have changed state: the owner must become unusable. */
  gt_status_t (*apply)(void *, const gt_state_event_t *);
  /* Return an independent immutable object; failure must leave state intact.
   * On failure no object may escape. Encoding/releasing belongs to the adapter,
   * outside this owner, after successful capture returns. */
  gt_status_t (*freeze)(void *, void **);
} gt_state_ops_t;

typedef struct {
  void *context;
  gt_state_ops_t ops;
  pthread_t thread;
  uint64_t sequence;
  gt_status_t failure;
  bool busy;
} gt_state_owner_t;

typedef struct {
  void *frozen;
  uint64_t sequence;
} gt_state_capture_t;

/* Initialize on the worker that will perform all subsequent operations.
 * Context/ops outlive the owner. No cross-thread reads, including sequence.
 * Commands, generation checks and backend lifetime belong to session runtime. */
gt_status_t gt__state_owner_init(gt_state_owner_t *, void *, gt_state_ops_t);
gt_status_t gt__state_owner_apply(gt_state_owner_t *, const gt_state_event_t *);
gt_status_t gt__state_owner_capture(gt_state_owner_t *, gt_state_capture_t *);
#endif
