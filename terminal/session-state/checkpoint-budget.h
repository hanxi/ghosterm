/* Private P0 checkpoint ownership. Caller excludes source mutations throughout
 * capture; this is not an atomic/COW capture or a production restore protocol. */
#ifndef GHOSTERM_P0_CHECKPOINT_BUDGET_H
#define GHOSTERM_P0_CHECKPOINT_BUDGET_H
#include "budget-posix.h"
typedef enum { P0_CHECKPOINT_OK, P0_CHECKPOINT_INVALID, P0_CHECKPOINT_SOURCE_FAILED,
               P0_CHECKPOINT_LIMIT, P0_CHECKPOINT_NO_MEMORY, P0_CHECKPOINT_UNSUPPORTED,
               P0_CHECKPOINT_ENGINE_ERROR } p0_checkpoint_status;
typedef enum { P0_CHECKPOINT_PREPARE, P0_CHECKPOINT_CANONICAL, P0_CHECKPOINT_RESOURCES,
               P0_CHECKPOINT_COMPLETE } p0_checkpoint_stage;
typedef struct {p0_checkpoint_stage stage;int engine_code;} p0_checkpoint_error;
typedef struct {
    uint8_t *canonical, *resources;
    size_t canonical_len, resources_len;
    const GhosttyAllocator *allocator;
} p0_checkpoint;
/* Distinct scopes must share a ledger, be admitted with reserved limits,
 * and remain alive until release. The source scope backs the terminal. No
 * partially encoded prefix is published. Valid output storage starts zeroed. */
p0_checkpoint_status p0_checkpoint_capture(p0_budget_scope *source,p0_budget_scope *scratch,
    GhosttyTerminal,size_t max_wire_bytes,p0_checkpoint *,p0_checkpoint_error *);
void p0_checkpoint_release(p0_checkpoint *);
#endif
