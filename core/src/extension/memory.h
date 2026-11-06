/* Process-wide budget for plugin-owned requested heap bytes. Not RSS, stack,
 * mmap, libc/SSH internal allocations or disk ResourceStore quotas. Reservations
 * include in-flight allocations and remain charged through the actual release.
 * No callbacks run under the accounting lock. Allocator contexts must outlive
 * their allocations, as required by base/allocator.h. No public ABI. */
#ifndef GT_PRIVATE_PLUGIN_MEMORY_H
#define GT_PRIVATE_PLUGIN_MEMORY_H
#include "base/allocator.h"
#include <stdbool.h>
#define GT_PLUGIN_MEMORY_DEFAULT (128u * 1024u * 1024u)
typedef struct {size_t limit,used,peak;} gt_plugin_memory_info_t;
void *gt__plugin_memory_alloc(const gt_allocator_t *,size_t);
void *gt__plugin_memory_calloc(size_t,size_t);
/* Wipes n bytes before invoking the upstream release. NULL is a no-op. */
void gt__plugin_memory_free(const gt_allocator_t *,void *,size_t);
gt_plugin_memory_info_t gt__plugin_memory_info(void);
/* Native policy/test entry; succeeds only when no reservations exist. A failed
 * attempt changes nothing. Zero is invalid; there is no unbounded mode. */
bool gt__plugin_memory_limit(size_t);
#endif
