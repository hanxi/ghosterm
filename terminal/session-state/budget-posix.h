/* Private P0 adapter. Not part of the public ghosterm C ABI. */
#ifndef GHOSTERM_P0_BUDGET_POSIX_H
#define GHOSTERM_P0_BUDGET_POSIX_H
#ifdef GHOSTERM_P0_ALLOCATOR_ONLY
#include <ghostty/vt/allocator.h>
#else
#include <ghostty/vt.h>
#endif
#include <pthread.h>

typedef enum { P0_BUDGET_GENERAL, P0_BUDGET_PAGES, P0_BUDGET_SCRATCH, P0_BUDGET_KINDS } p0_budget_kind;
typedef enum { P0_BUDGET_HEALTHY, P0_BUDGET_GLOBAL_LIMIT, P0_BUDGET_SCOPE_LIMIT,
               P0_BUDGET_OVERFLOW, P0_BUDGET_OS_ERROR } p0_budget_fault;
/* committed = admitted limits + actual live usage of on-demand scopes.
 * This is budget capacity promised, not preallocated or locked OS memory. */
typedef struct {
    size_t live, peak, committed, peak_committed, mapped, unmapped, attempts, denied;
    size_t by_kind[P0_BUDGET_KINDS];
} p0_budget_usage;
typedef struct {
    pthread_mutex_t mutex;
    size_t limit, page_size, scopes;
    p0_budget_usage usage;
} p0_budget;
struct p0_budget_scope;
typedef struct {
    struct p0_budget_scope *scope;
    p0_budget_kind kind;
} p0_budget_endpoint;
typedef struct p0_budget_scope {
    p0_budget *budget;
    size_t limit;
    p0_budget_usage usage;
    p0_budget_fault fault;
    bool closed, reserved;
    p0_budget_endpoint endpoints[P0_BUDGET_KINDS];
    GhosttyAllocator allocators[P0_BUDGET_KINDS];
} p0_budget_scope;

/* Objects never move after init. Scopes/descriptors outlive every borrower.
 * This provider maps every allocation, including small general allocations.
 * Charges include OS page rounding and temporary over-alignment mappings.
 * resize/remap are optional optimizations; false/NULL does not set a fault. */
bool p0_budget_init(p0_budget *, size_t limit);
bool p0_budget_destroy(p0_budget *); /* false while any scope remains open */
void p0_budget_scope_init(p0_budget_scope *, p0_budget *, size_t limit);
/* Admit a new scope with its entire limit reserved, including unused growth
 * headroom. Failed admission leaves an empty, faulted scope to close. */
bool p0_budget_scope_admit(p0_budget_scope *, p0_budget *, size_t limit);
bool p0_budget_scope_close(p0_budget_scope *); /* false while allocations live */
const GhosttyAllocator *p0_budget_allocator(p0_budget_scope *, p0_budget_kind);
bool p0_budget_set_limit(p0_budget *, size_t);
bool p0_budget_scope_set_limit(p0_budget_scope *, size_t);
p0_budget_usage p0_budget_read(p0_budget *);
p0_budget_usage p0_budget_scope_read(p0_budget_scope *);
p0_budget_fault p0_budget_scope_fault(p0_budget_scope *);
/* First fault is sticky and scoped; another session/scratch scope stays usable.
 * Always inspect after an engine operation, even if the engine returned success. */
#ifndef GHOSTERM_P0_ALLOCATOR_ONLY
GhosttyResult p0_budget_result(p0_budget_scope *, GhosttyResult);
GhosttyResult p0_budget_feed(p0_budget_scope *, GhosttyTerminal, const uint8_t *, size_t);
GhosttyResult p0_budget_resize(p0_budget_scope *, GhosttyTerminal, uint16_t, uint16_t);
/* Explicit authority cell metrics; width/height are pixels per cell, not the
 * whole PTY/window extent. Same owner health and allocator accounting. */
GhosttyResult p0_budget_resize_cells(p0_budget_scope *, GhosttyTerminal, uint16_t, uint16_t, uint32_t, uint32_t);
#endif /* engine terminal helpers */
#endif
