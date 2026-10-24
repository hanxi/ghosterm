/* Private experimental immutable checkpoint adapter; no Core/UI dependency. */
#ifndef GHOSTERM_P0_FROZEN_BUDGET_H
#define GHOSTERM_P0_FROZEN_BUDGET_H
#include "checkpoint-budget.h"
typedef struct {
    GhosttyP0FrozenCheckpoint handle;
    p0_budget_scope *scope;
} p0_frozen_checkpoint;
/* Owner-thread operation, excluding source mutation. All scopes are admitted.
 * Source/task differ, share a ledger, and source backs terminal. The returned
 * object borrows task's stable descriptors until release; source may then die.
 * Caller supplies empty output storage; no partially captured handle escapes. */
GhosttyResult p0_frozen_capture(p0_budget_scope *source, p0_budget_scope *task,
    GhosttyTerminal, size_t max_copy_bytes, p0_frozen_checkpoint *);
/* May run off owner. Encoder is a distinct admitted scope on the same ledger.
 * Frozen object must not be released concurrently. Output borrows encoder's
 * allocator until p0_checkpoint_release. No source terminal/scope is consulted. */
GhosttyResult p0_frozen_encode(const p0_frozen_checkpoint *, p0_budget_scope *encoder,
    size_t max_wire_bytes, p0_checkpoint *);
void p0_frozen_release(p0_frozen_checkpoint *);
#endif
