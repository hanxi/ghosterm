/* Candidate wire: uint32 big-endian length + 1..65536 opaque bytes.
 * Framing only: JSON/UTF-8/schema, identity and authorization belong above it.
 * Bad prefix is terminal: never scan for a plausible later frame. */
#ifndef GT_BRIDGE_FRAME_H
#define GT_BRIDGE_FRAME_H
#include "extension/stream.h"
#define GT_BRIDGE_FRAME_MAX 65536u
#define GT_BRIDGE_FRAME_PREFIX 4u

typedef struct gt_bridge_decoder gt_bridge_decoder_t;
typedef struct gt_bridge_writer gt_bridge_writer_t;
/* One fixed payload allocation per decoder/writer, no allocation on feed/step.
 * Caller allocator (if any) must outlive free; limit is 1..FRAME_MAX. */
gt_status_t gt__bridge_decoder_new(size_t limit, const gt_allocator_t *, gt_bridge_decoder_t **);
/* Stops at one complete frame. Consumed bytes may be less than input length;
 * caller retains and feeds the tail only after consume. A ready view blocks feed.
 * View is borrowed until consume/free. These functions are owner-thread only. */
gt_status_t gt__bridge_decoder_feed(gt_bridge_decoder_t *, const void *, size_t, size_t *consumed);
bool gt__bridge_decoder_view(const gt_bridge_decoder_t *, const uint8_t **, size_t *);
gt_status_t gt__bridge_decoder_consume(gt_bridge_decoder_t *);
/* EOF seals the decoder; partial header/body is terminal PROTOCOL. A ready
 * complete last frame remains available. finish is idempotent; feed after EOF
 * is rejected. No silent success on a truncated frame. */
gt_status_t gt__bridge_decoder_finish(gt_bridge_decoder_t *);
void gt__bridge_decoder_free(gt_bridge_decoder_t *);

gt_status_t gt__bridge_writer_new(size_t limit, const gt_allocator_t *, gt_bridge_writer_t **);
/* Copies one complete payload; busy/oversize rejection preserves prior bytes.
 * Only one queued frame: a control message cannot interleave a partial frame. */
gt_status_t gt__bridge_writer_queue(gt_bridge_writer_t *, const void *, size_t);
size_t gt__bridge_writer_pending(const gt_bridge_writer_t *);
/* Exactly one bounded write_some; advances only by actual local acceptance.
 * ERROR/CLOSED are latched; reopening/retrying on a new stream is not allowed.
 * A pending frame is bound to its first stream, including after WAIT. */
gt_bridge_io_t gt__bridge_writer_step(gt_bridge_writer_t *, gt_bridge_stream_t *, int);
void gt__bridge_writer_free(gt_bridge_writer_t *);
#endif
