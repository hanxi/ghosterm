/* Private owner-thread OSC 7501 store. Decoded input only; no VT/UI dependency. */
#ifndef GT_PROGRAM_STATUS_H
#define GT_PROGRAM_STATUS_H
#include "ghosterm/types.h"
#include "base/allocator.h"
typedef struct gt_program_status_store gt_program_status_store_t;
typedef struct {
  const char *id,*app,*title,*message;
  unsigned state,kind; /* idle/working/done/blocked/error/clear = 0..5; kind 0..3 */
  int progress; /* -1 or 0..100; only working/blocked */
} gt_program_status_record_t;
typedef enum {GT_STATUS_OK,GT_STATUS_INVALID,GT_STATUS_LIMIT,GT_STATUS_NO_MEMORY} gt_status_store_result_t;
/* Fixed record capacity 1..1024 and total NUL-terminated string bytes 1..1MiB.
 * Caller normalizes engine reports and supplies NUL strings with no embedded
 * NUL; text is retained, NOT interpreted/sanitized for UI. Each field <=4096
 * bytes. No parent/app inference: decoded app inheritance belongs to engine.
 * All operations (including views/free) belong to one non-UI session owner.
 * No internal lock; callers serialize capture against apply. */
gt_program_status_store_t *gt__program_status_open(size_t records,size_t bytes);
/* Optional owner-supplied allocation domain. Every object, entry array and
 * string allocation uses these callbacks; release receives the exact original
 * byte count. allocate returns suitably aligned uninitialized memory or NULL.
 * Both callbacks required; allocator is copied, context outlives this store AND
 * all copies. Callbacks are serialized by caller just like other store access.
 * Replacement temporarily charges old+new strings; failure preserves old state.
 * Copies inherit this domain. NULL allocator selects the libc default. */
typedef gt_allocator_t gt_status_allocator_t;
gt_program_status_store_t *gt__program_status_open_with_allocator(size_t,size_t,const gt_status_allocator_t *);
gt_status_store_result_t gt__program_status_apply(gt_program_status_store_t *,const gt_program_status_record_t *);
/* Borrowed views valid until mutation/free. Order = first insertion order;
 * update retains position. clear(id) removes id and slash-delimited descendants;
 * empty clear id removes all records. Unknown clear is a successful no-op.
 * Rejection leaves all prior records and accounting unchanged. */
size_t gt__program_status_count(const gt_program_status_store_t *);
size_t gt__program_status_bytes(const gt_program_status_store_t *);
bool gt__program_status_at(const gt_program_status_store_t *,size_t,gt_program_status_record_t *);
/* Semantic prompt kind=1 removes only working/blocked records, retaining
 * done/error/idle descendants independently. Other kinds are no-ops. Returns
 * number removed so owner can publish a summary change. NULL returns zero. */
size_t gt__program_status_semantic(gt_program_status_store_t *,unsigned kind);
/* Deep immutable-by-convention copy; failure leaves *out=NULL. Snapshot can
 * move to another thread only after publication; no concurrent mutation/free. */
gt_status_store_result_t gt__program_status_copy(const gt_program_status_store_t *,gt_program_status_store_t **out);
/* Explicit destination domain (NULL = libc). Source remains unchanged on
 * failure; *out=NULL and all partial destination allocations are released.
 * Destination context lifetime is independent of the source store/context. */
gt_status_store_result_t gt__program_status_copy_with_allocator(const gt_program_status_store_t *,const gt_status_allocator_t *,gt_program_status_store_t **out);
void gt__program_status_close(gt_program_status_store_t *);
#endif
