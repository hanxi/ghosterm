/* Shared local/SSH framed connection owner. Pure C, no OS or engine calls. */
#ifndef GT_IPC_CONNECTION_H
#define GT_IPC_CONNECTION_H
#include "wire.h"
#include "base/allocator.h"
typedef enum {GT_STREAM_DATA,GT_STREAM_WAIT,GT_STREAM_END,GT_STREAM_ERROR} gt_stream_kind_t;
typedef struct {gt_stream_kind_t kind;size_t length;int system_error;} gt_stream_result_t;
typedef struct {
  void *context;
  /* Exactly one nonblocking attempt; WAIT includes EAGAIN/EINTR. DATA count
   * must be 1..requested; other kinds require zero. Never reenter connection. */
  gt_stream_result_t (*read)(void *,void *,size_t);
  gt_stream_result_t (*write)(void *,const void *,size_t);
  bool (*clock)(void *,uint64_t *monotonic_ms);
  void (*close)(void *);
} gt_stream_ops_t;
typedef struct {uint32_t receive_limit,send_limit,frame_timeout_ms;} gt_connection_limits_t;
typedef enum {GT_CONNECTION_OPEN,GT_CONNECTION_END,GT_CONNECTION_IO_ERROR,
  GT_CONNECTION_PROTOCOL_ERROR,GT_CONNECTION_TIMEOUT,GT_CONNECTION_CANCELLED,
  GT_CONNECTION_CLOCK_ERROR} gt_connection_state_t;
typedef enum {GT_CONNECTION_CREATED,GT_CONNECTION_INVALID,GT_CONNECTION_NO_MEMORY,
  GT_CONNECTION_NO_CLOCK} gt_connection_open_t;
typedef enum {GT_SEND_ACCEPTED,GT_SEND_BUSY,GT_SEND_REJECTED,GT_SEND_CLOSED} gt_connection_send_t;
typedef struct gt_connection gt_connection_t;
/* Single non-UI owner. One charged allocation includes both bounded buffers.
 * Limits 0..MAX_PAYLOAD, timeout 1..60000ms. Success adopts ops context's stream,
 * closes it exactly once on any terminal result; context outlives destroy.
 * Failed construction never calls close; stream remains caller-owned. */
gt_connection_open_t gt__connection_open(const gt_stream_ops_t *,gt_connection_limits_t,
  const gt_allocator_t *,gt_connection_t **);
/* At most one read and one write (<=4096 bytes each). Read precedes write even
 * under output backpressure. Never waits, loops on IO, or dispatches callbacks
 * to UI. Caller must poll/schedule and continue stepping while output is pending.
 * Clock checked before/after IO, rollback/failure is terminal. A partial frame
 * has ONE deadline across header/body/progress; idle RX has no frame deadline.
 * Handshake/idle/lease deadlines belong to protocol owner, not this envelope. */
gt_connection_state_t gt__connection_step(gt_connection_t *);
gt_connection_state_t gt__connection_state(const gt_connection_t *);
int gt__connection_system_error(const gt_connection_t *);
/* Immutable construction limits, same serial owner. Failure clears output. */
bool gt__connection_limits(const gt_connection_t *,gt_connection_limits_t *);
/* Copies header/payload before ACCEPTED; caller may reuse input immediately.
 * One pending frame; BUSY accepts zero and preserves incumbent. Accepted means
 * queued here, not delivered to peer/session/application. Do not replay after
 * ambiguous disconnect. Invalid local requests do not poison an open stream. */
gt_connection_send_t gt__connection_send(gt_connection_t *,const gt_wire_header_t *,const void *);
bool gt__connection_wants_write(const gt_connection_t *);
/* Borrow until release/terminal/destroy. Step before dispatch; no further read
 * while a frame is held, but pending output/deadlines still advance. Dispatcher
 * must validate type/direction/state/identity and release promptly, not wait for
 * a response to drain first. Payload is independent from the copied send buffer. */
bool gt__connection_frame(const gt_connection_t *,gt_wire_frame_t *);
bool gt__connection_release_frame(gt_connection_t *);
/* Consumed RX, completed TX and read scratch are erased after use. Terminal
 * results erase buffered payloads; destroy erases the whole charged allocation.
 * This covers these owned copies, not caller/OS/SSH/engine storage. */
void gt__connection_cancel(gt_connection_t *);
void gt__connection_destroy(gt_connection_t *);
#endif
