/* Private P0 GUI-process admission. Engine-free C boundary, not public ABI. */
#ifndef GT_P0_GUI_BUDGET_H
#define GT_P0_GUI_BUDGET_H
#include <stdbool.h>
#include <stddef.h>
typedef struct p0_gui_pool p0_gui_pool;
typedef struct p0_gui_budget p0_gui_budget;
typedef struct p0_gui_reservation p0_gui_reservation;
typedef enum {P0_GUI_OK,P0_GUI_INVALID,P0_GUI_LIMIT,P0_GUI_NO_MEMORY,P0_GUI_BUSY,P0_GUI_ERROR} p0_gui_status;
typedef struct {size_t total,general,pages,wire,surfaces,metadata,threads;} p0_gui_limits;
typedef struct {size_t surfaces,committed,live,peak,wire_live,reservations;} p0_gui_usage;
/* One pool per GUI process; application supplies its common policy. Each lease
 * reserves general+pages+wire atomically, even while idle. Rejects preserve all
 * prior leases and promises. Zero total disables admission; per-lease amounts
 * and surface count must be positive, sum must not overflow.
 * threads>0 adds per-surface capacity for engine threads, reserved before
 * construction and released after surface_free joins them. Zero excludes it.
 * metadata>0 reserves that entire allocator bound in the same total. It requires
 * a custom domain that enforces this bound INCLUDING allocation overhead; zero
 * explicitly excludes metadata. Pool creation can return LIMIT, transactionally. */
/* Optional domain for pool and both lease descriptors. No engine allocation
 * uses this domain. Callbacks are concurrent-safe when shared; exact sizes are
 * returned and context outlives pool close. NULL keeps libc. */
typedef struct {void *context;void *(*allocate)(void *,size_t);void (*release)(void *,void *,size_t);} p0_gui_metadata_allocator;
p0_gui_status p0_gui_pool_create_with_allocator(p0_gui_limits,const p0_gui_metadata_allocator *,p0_gui_pool **);
p0_gui_status p0_gui_pool_create(p0_gui_limits,p0_gui_pool **);
/* Runtime lease in the SAME process pool, independent of surfaces.
 * Use as capacity for explicit thread stack/guard policy, or opt into the
 * allocator below. A promise alone is not live OS memory.
 * Pool cannot close while held. Caller quiesces/joins all borrowers BEFORE
 * release; descriptor allocation uses the pool metadata domain. */
p0_gui_status p0_gui_reserve(p0_gui_pool *,size_t,p0_gui_reservation **);
p0_gui_status p0_gui_unreserve(p0_gui_reservation *);
/* Optional allocator on a runtime reservation, sharing the same process bound.
 * Descriptor/context live until unreserve. All borrowers must be destroyed first;
 * unreserve returns BUSY while allocations remain. Faults never fall back. */
const void *p0_gui_reserved_allocator(p0_gui_reservation *);
bool p0_gui_reserved_healthy(p0_gui_reservation *);
bool p0_gui_reserved_idle(p0_gui_reservation *);
p0_gui_status p0_gui_admit(p0_gui_pool *,p0_gui_budget **);
p0_gui_usage p0_gui_pool_read(p0_gui_pool *);
/* Pool outlives all leases/calls. BUSY preserves it. Successful destruction
 * requires API users quiesced, as with other opaque handle destruction. */
p0_gui_status p0_gui_pool_close(p0_gui_pool *);
const void *p0_gui_general(p0_gui_budget *);
const void *p0_gui_pages(p0_gui_budget *);
size_t p0_gui_thread_capacity(p0_gui_budget *);
bool p0_gui_healthy(void *);
bool p0_gui_idle(p0_gui_budget *);
/* Snapshot receive/expected buffers use the admitted wire scope, not malloc.
 * Caller returns exact original length and quiesces all borrowers before free. */
void *p0_gui_buffer_new(p0_gui_budget *,size_t);
void p0_gui_buffer_free(p0_gui_budget *,void *,size_t);
/* Quiesce renderer/IO first and tick deferred engine frees. BUSY while ANY
 * allocation lives, including receive buffers. Faulted leases can be released
 * once empty: a resource failure must not permanently retain admission. */
p0_gui_status p0_gui_release(p0_gui_budget *);
#endif
