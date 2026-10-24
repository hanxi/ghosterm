#include "state-owner.h"
#include <string.h>

static gt_status_t enter(gt_state_owner_t *owner) {
  if (!owner || !pthread_equal(owner->thread, pthread_self()))
    return GT_ERR_INVALID_ARG;
  if (owner->failure != GT_OK) return owner->failure;
  /* Query replies must be staged for the worker, never recursively applied. */
  if (owner->busy) return GT_ERR_INVALID_ARG;
  return GT_OK;
}

gt_status_t gt__state_owner_init(gt_state_owner_t *owner, void *context,
                                gt_state_ops_t ops) {
  if (!owner || !context || !ops.apply || !ops.freeze)
    return GT_ERR_INVALID_ARG;
  memset(owner, 0, sizeof(*owner));
  owner->context = context;
  owner->ops = ops;
  owner->thread = pthread_self();
  return GT_OK;
}

gt_status_t gt__state_owner_apply(gt_state_owner_t *owner,
                                 const gt_state_event_t *event) {
  gt_status_t result = enter(owner);
  if (result != GT_OK) return result;
  if (!event || (event->length && !event->payload)) return GT_ERR_INVALID_ARG;
  if (owner->sequence == UINT64_MAX) {
    owner->failure = GT_ERR_PROTOCOL;
    return owner->failure;
  }
  owner->busy = true;
  result = owner->ops.apply(owner->context, event);
  owner->busy = false;
  if (result != GT_OK) {
    owner->failure = result;
    return result;
  }
  owner->sequence++; /* STATE_OWNER_PUBLISH */
  return GT_OK;
}

gt_status_t gt__state_owner_capture(gt_state_owner_t *owner,
                                   gt_state_capture_t *capture) {
  if (!capture) return GT_ERR_INVALID_ARG;
  memset(capture, 0, sizeof(*capture));
  gt_status_t result = enter(owner);
  if (result != GT_OK) return result;
  owner->busy = true;
  void *frozen = NULL;
  result = owner->ops.freeze(owner->context, &frozen);
  owner->busy = false;
  if (result != GT_OK) return result;
  if (!frozen) {
    owner->failure = GT_ERR_INTERNAL;
    return owner->failure;
  }
  capture->frozen = frozen;
  capture->sequence = owner->sequence;
  return GT_OK;
}
