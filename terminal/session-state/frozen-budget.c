#include "frozen-budget.h"
#include <string.h>

static bool ready(p0_budget_scope *scope) {
    return scope && scope->reserved &&
        p0_budget_result(scope, GHOSTTY_SUCCESS) == GHOSTTY_SUCCESS;
}
static bool source_ready(p0_budget_scope *scope, GhosttyTerminal terminal) {
    bool failed = true;
    return ready(scope) && terminal && ghostty_terminal_get(terminal,
        GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR, &failed) == GHOSTTY_SUCCESS && !failed;
}
void p0_frozen_release(p0_frozen_checkpoint *frozen) {
    if (!frozen) return;
    if (frozen->handle) ghostty_snapshot_p0_frozen_free(frozen->handle);
    memset(frozen, 0, sizeof(*frozen));
}
GhosttyResult p0_frozen_capture(p0_budget_scope *source, p0_budget_scope *task,
    GhosttyTerminal terminal, size_t limit, p0_frozen_checkpoint *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !source || !task || source == task ||
        source->budget != task->budget || !source->reserved || !task->reserved || !terminal)
        return GHOSTTY_INVALID_VALUE;
    if (!source_ready(source, terminal)) return GHOSTTY_REJECTED;
    GhosttyResult rc = p0_budget_result(task, GHOSTTY_SUCCESS);
    if (rc != GHOSTTY_SUCCESS) return rc;
    p0_frozen_checkpoint staged = {.scope = task};
    rc = ghostty_snapshot_p0_freeze(terminal,
        p0_budget_allocator(task, P0_BUDGET_SCRATCH), limit, &staged.handle);
    rc = p0_budget_result(task, rc);
    if (!source_ready(source, terminal)) rc = GHOSTTY_REJECTED;
    if (rc != GHOSTTY_SUCCESS) {
        p0_frozen_release(&staged);
        return rc;
    }
    *out = staged;
    return GHOSTTY_SUCCESS;
}
GhosttyResult p0_frozen_encode(const p0_frozen_checkpoint *frozen,
    p0_budget_scope *encoder, size_t limit, p0_checkpoint *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !frozen || !frozen->handle || !frozen->scope || !encoder ||
        frozen->scope == encoder || frozen->scope->budget != encoder->budget ||
        !frozen->scope->reserved || !encoder->reserved) return GHOSTTY_INVALID_VALUE;
    if (!ready(frozen->scope)) return GHOSTTY_REJECTED;
    GhosttyResult rc = p0_budget_result(encoder, GHOSTTY_SUCCESS);
    if (rc != GHOSTTY_SUCCESS) return rc;
    p0_checkpoint staged = {.allocator = p0_budget_allocator(encoder, P0_BUDGET_SCRATCH)};
    rc = ghostty_snapshot_p0_frozen_encode(frozen->handle, staged.allocator, limit,
        &staged.canonical, &staged.canonical_len, &staged.resources, &staged.resources_len);
    rc = p0_budget_result(encoder, rc);
    if (rc != GHOSTTY_SUCCESS) {
        p0_checkpoint_release(&staged);
        return rc;
    }
    *out = staged;
    return GHOSTTY_SUCCESS;
}
