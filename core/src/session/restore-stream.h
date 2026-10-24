/* Private bounded data stream; caller serializes all access. No UI queue. */
#ifndef GT_RESTORE_STREAM_H
#define GT_RESTORE_STREAM_H
#include "ghosterm/types.h"
#include "base/allocator.h"
typedef enum {GT_RESTORE_DETACHED, GT_RESTORE_CAPTURING, GT_RESTORE_SNAPSHOT,
              GT_RESTORE_DRAINING, GT_RESTORE_WAIT_L, GT_RESTORE_LIVE, GT_RESTORE_ENDED, GT_RESTORE_FAILED} gt_restore_phase_t;
typedef enum {GT_RESTORE_OK, GT_RESTORE_IDLE, GT_RESTORE_STALE, GT_RESTORE_WAIT,
              GT_RESTORE_SPACE, GT_RESTORE_LIMIT, GT_RESTORE_PROTOCOL} gt_restore_rc_t;
typedef struct {uint64_t generation,sequence;uint32_t kind;size_t length;} gt_restore_event_t;
typedef struct {uint64_t sequence;uint32_t kind;size_t length,offset;} gt_restore_entry_t;
typedef struct {
  gt_allocator_t allocator;
  uint8_t *bytes;
  gt_restore_entry_t *entries;
  size_t byte_limit,event_limit,used,count,head,read_offset;
  uint64_t generation,snapshot,last,dequeued,cut;
  gt_restore_phase_t phase;
  gt_restore_rc_t failure;
  bool offered;
} gt_restore_stream_t;
bool gt__restore_init(gt_restore_stream_t *,size_t byte_limit,size_t event_limit);
/* Optional domain for both fixed arrays. NULL selects libc. Context outlives
 * destroy; callbacks required, serialized by caller. Init requires an empty
 * object (never re-init a live ring). Allocation failure leaves a zero object.
 * Detach/reset retain arrays and their charge for bounded reuse. */
bool gt__restore_init_with_allocator(gt_restore_stream_t *,size_t,size_t,const gt_allocator_t *);
void gt__restore_destroy(gt_restore_stream_t *);
void gt__restore_reset(gt_restore_stream_t *,uint64_t generation);
void gt__restore_detach(gt_restore_stream_t *);
/* Begin must execute on the state owner at capture boundary S, before S+1.
 * Every applied state event (including resize) then goes through push. */
gt_restore_rc_t gt__restore_begin(gt_restore_stream_t *,uint64_t generation,uint64_t sequence);
gt_restore_rc_t gt__restore_offer(gt_restore_stream_t *,uint64_t generation,uint64_t sequence);
gt_restore_rc_t gt__restore_push(gt_restore_stream_t *,uint64_t sequence,uint32_t kind,const void *,size_t);
/* Copy into bounded transport-thread buffer. SPACE reports required length
 * without consuming. Dequeued is not a GUI-applied acknowledgment. Consumer
 * must preserve this order and detach if it drops a copied event. */
gt_restore_rc_t gt__restore_next(gt_restore_stream_t *,uint64_t generation,
                                gt_restore_event_t *,void *,size_t);
gt_restore_rc_t gt__restore_ready_snapshot(gt_restore_stream_t *,uint64_t generation,uint64_t sequence);
/* Only after S..L delta is dequeued; ordered writer emits LIVE_BEGIN(L) before
 * calling next again. WAIT_L continues sending L+1 output, avoiding deadlock. */
gt_restore_rc_t gt__restore_live_begin(gt_restore_stream_t *,uint64_t generation,uint64_t *sequence);
gt_restore_rc_t gt__restore_ready_live(gt_restore_stream_t *,uint64_t generation,uint64_t sequence);
/* Phase fence only: LIVE with all final events queued becomes ENDED; keep
 * draining accepted bytes. Earlier phase/missing final events returns WAIT.
 * Wrong final/stale generation never mutates prior data. ENDED repeats are
 * idempotent, handshake/push attempts cannot resurrect or discard its tail.
 * Controller separately latches immediate session-wide input revocation. */
gt_restore_rc_t gt__restore_end(gt_restore_stream_t *,uint64_t generation,uint64_t final_sequence);
bool gt__restore_input_allowed(const gt_restore_stream_t *,uint64_t generation,uint64_t applied_sequence);
#endif
