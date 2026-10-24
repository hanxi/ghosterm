#include "session/state-owner.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  gt_state_owner_t *owner;
  unsigned value, calls;
  bool fail, recurse, fail_capture;
} state_t;
static gt_status_t apply(void *ctx, const gt_state_event_t *event) {
  state_t *s = ctx;
  s->calls++;
  assert(s->owner->sequence == s->value);
  if (s->recurse) {
    gt_state_capture_t c;
    assert(gt__state_owner_capture(s->owner, &c) == GT_ERR_INVALID_ARG);
    assert(!c.frozen);
    assert(gt__state_owner_apply(s->owner, event) == GT_ERR_INVALID_ARG);
  }
  s->value++;
  return s->fail ? GT_ERR_IO : GT_OK;
}
static gt_status_t freeze(void *ctx, void **out) {
  state_t *s = ctx;
  if (s->fail_capture) return GT_ERR_IO;
  unsigned *copy = malloc(sizeof(*copy));
  assert(copy);
  *copy = s->value;
  *out = copy;
  return GT_OK;
}
static void *wrong_thread(void *ctx) {
  gt_state_owner_t *o = ctx;
  gt_state_event_t e = {0};
  gt_state_capture_t c;
  assert(gt__state_owner_apply(o, &e) == GT_ERR_INVALID_ARG);
  assert(gt__state_owner_capture(o, &c) == GT_ERR_INVALID_ARG);
  assert(!c.frozen);
  return NULL;
}
int main(int argc, char **argv) {
  gt_state_owner_t o;
  state_t s = {.owner = &o};
  gt_state_ops_t ops = {.apply = apply, .freeze = freeze};
  assert(gt__state_owner_init(&o, &s, ops) == GT_OK);
  gt_state_event_t e = {0};
  gt_state_capture_t c;
  if (argc > 1 && !strcmp(argv[1], "control")) {
    s.fail_capture = true;
    assert(gt__state_owner_capture(&o, &c) == GT_ERR_IO);
    assert(!c.frozen && !c.sequence && !o.sequence && !o.failure);
    s.fail_capture = false;
    assert(gt__state_owner_capture(&o, &c) == GT_OK);
    assert(*(unsigned *)c.frozen == 0);
    free(c.frozen);
    return 0;
  }
  s.recurse = true;
  assert(gt__state_owner_apply(&o, &e) == GT_OK);
  assert(o.sequence == 1);
  assert(gt__state_owner_capture(&o, &c) == GT_OK);
  assert(c.sequence == 1 && *(unsigned *)c.frozen == 1);
  pthread_t t;
  assert(!pthread_create(&t, NULL, wrong_thread, &o));
  assert(!pthread_join(t, NULL));
  assert(s.calls == 1);
  assert(gt__state_owner_apply(&o, &e) == GT_OK);
  assert(c.sequence == 1 && *(unsigned *)c.frozen == 1);
  free(c.frozen);
  s.fail = true;
  assert(gt__state_owner_apply(&o, &e) == GT_ERR_IO);
  assert(o.sequence == 2);
  assert(gt__state_owner_capture(&o, &c) == GT_ERR_IO);
  assert(!c.frozen && !c.sequence);
  assert(gt__state_owner_apply(&o, &e) == GT_ERR_IO);
  assert(s.calls == 3);
  s.fail = false;
  assert(gt__state_owner_init(&o, &s, ops) == GT_OK);
  o.sequence = UINT64_MAX;
  assert(gt__state_owner_apply(&o, &e) == GT_ERR_PROTOCOL);
  assert(s.calls == 3);
  return 0;
}
