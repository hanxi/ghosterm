/* Common bounded single-frame writer cursor. No OS/SSH/engine dependency. */
#ifndef GT_IPC_WRITER_H
#define GT_IPC_WRITER_H
#include "wire.h"
typedef enum {GT_WRITER_IDLE,GT_WRITER_PENDING,GT_WRITER_DONE,GT_WRITER_FAILED} gt_wire_writer_state_t;
typedef struct {
  uint8_t envelope[GT_WIRE_HEADER_BYTES];
  const uint8_t *payload;
  size_t payload_length,offset;
  gt_wire_writer_state_t state;
} gt_wire_writer_t;
/* One non-UI I/O owner; initialize only for a new stream. No allocation/queue.
 * Begin copies the envelope and borrows immutable payload until DONE/cancel.
 * A rejected begin leaves the incumbent frame and its borrow untouched. */
void gt__wire_writer_init(gt_wire_writer_t *);
bool gt__wire_writer_begin(gt_wire_writer_t *,const gt_wire_header_t *,const void *payload);
/* One contiguous unsent span. View is valid until advance/cancel; it makes no
 * progress. Caller performs exactly one bounded socket/channel write. WAIT or
 * EAGAIN preserves this cursor; advance only by the ACTUAL positive write count.
 * Output is cleared when no pending bytes. */
bool gt__wire_writer_view(const gt_wire_writer_t *,const uint8_t **,size_t *);
/* Zero preserves pending state. A count beyond the offered span poisons stream;
 * no future frame may be started. DONE means bytes handed to the underlying
 * stream, never peer receipt, acceptance by session, or application execution. */
bool gt__wire_writer_advance(gt_wire_writer_t *,size_t written);
/* Releases borrow, permanently fails this stream. Cancellation/timeout after
 * partial output MUST close underlying stream, never start a replacement frame
 * on it. I/O owner maintains one monotonic deadline across header+body and must
 * also service receive/lease/cancel while output is backed up. */
void gt__wire_writer_cancel(gt_wire_writer_t *);
#endif
