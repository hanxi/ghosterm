/* Private allocation domain: no engine, UI or public ABI dependency. */
#ifndef GT_PRIVATE_ALLOCATOR_H
#define GT_PRIVATE_ALLOCATOR_H
#include <stddef.h>
typedef struct {
  void *context;
  /* Aligned for any C object; uninitialized memory or NULL. */
  void *(*allocate)(void *,size_t);
  /* Exact original requested size; context outlives every allocation. */
  void (*release)(void *,void *,size_t);
} gt_allocator_t;
#endif
