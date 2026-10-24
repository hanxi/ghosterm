/* Private common envelope for local IPC and SSH streams. No OS/engine/UI API. */
#ifndef GT_IPC_WIRE_H
#define GT_IPC_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GT_WIRE_HEADER_BYTES = 24, GT_WIRE_VERSION = 1,
       GT_WIRE_MAX_PAYLOAD = 1024 * 1024 };
typedef struct {
  uint16_t type;
  uint32_t flags, payload_length;
  uint64_t request_id;
} gt_wire_header_t;
/* GTSD, u16 version/type, u32 flags/length, u64 request_id; all big endian.
 * Type is an opaque nonzero ID here: the protocol dispatcher MUST reject
 * unknown messages, illegal directions/states and invalid request IDs before
 * any operation. This envelope alone never authenticates/adopts a session.
 * Flags are reserved and must be zero. Failure clears a supplied output. */
bool gt__wire_header_encode(const gt_wire_header_t *,uint8_t out[GT_WIRE_HEADER_BYTES]);
bool gt__wire_header_decode(const uint8_t in[GT_WIRE_HEADER_BYTES],uint32_t limit,gt_wire_header_t *);

typedef enum {GT_WIRE_MORE,GT_WIRE_FRAME,GT_WIRE_END,GT_WIRE_ERROR} gt_wire_result_t;
typedef struct {
  gt_wire_header_t header;
  const uint8_t *payload;
} gt_wire_frame_t;
typedef struct {
  uint8_t envelope[GT_WIRE_HEADER_BYTES];
  size_t envelope_used,payload_used;
  uint8_t *storage;
  size_t capacity;
  uint32_t limit;
  gt_wire_header_t header;
  bool ready,failed,ended;
} gt_wire_decoder_t;
/* Single I/O owner. Caller supplies storage with capacity >= limit; limit may
 * be zero for header-only streams, never exceeds MAX_PAYLOAD. No allocations.
 * Decoder and storage outlive frame borrows. Reinitialization is for a NEW
 * stream, never recovery from malformed/partial input on the same stream. */
bool gt__wire_decoder_init(gt_wire_decoder_t *,void *storage,size_t capacity,uint32_t limit);
/* Consumes at most one frame; reports exact consumed prefix even on error.
 * Unconsumed bytes remain caller-owned, including coalesced following frames.
 * FRAME persists until next(), with consumed=0 on repeated feed. Copies input;
 * the caller may immediately reuse its read buffer. Errors/end are sticky. */
gt_wire_result_t gt__wire_decoder_feed(gt_wire_decoder_t *,const void *,size_t,size_t *consumed);
bool gt__wire_decoder_frame(const gt_wire_decoder_t *,gt_wire_frame_t *);
/* Releases the borrowed frame and prepares the next envelope. Never discards
 * a partial frame or clears a sticky error/end. */
bool gt__wire_decoder_next(gt_wire_decoder_t *);
/* Call when the underlying stream reaches EOF. Partial header/body is ERROR,
 * clean boundary is END, an unconsumed complete frame remains FRAME. The owner
 * supplies monotonic deadlines/cancellation; this parser never waits or does I/O. */
gt_wire_result_t gt__wire_decoder_finish(gt_wire_decoder_t *);
#endif
