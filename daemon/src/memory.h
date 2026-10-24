#ifndef GT_DAEMON_MEMORY_H
#define GT_DAEMON_MEMORY_H
#include "base/allocator.h"
#include <stdatomic.h>
typedef struct {size_t limit;atomic_size_t used;} gt_daemon_memory_t;
/* Initialize once at a stable address, before any borrowers; exact C allocation
 * domain only. VT mapping budgets, OS descriptors and native stacks separate. */
void gt__daemon_memory_init(gt_daemon_memory_t *,size_t);
gt_allocator_t gt__daemon_memory_allocator(gt_daemon_memory_t *);
#endif
