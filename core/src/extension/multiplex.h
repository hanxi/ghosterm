/* Private framed connection owner. Native code explicitly registers each
 * instance; peer frames never install, create or authorize an instance.
 * One serial non-UI owner pumps the mux and every Session. Only stop is
 * cross-thread; all stop callers finish before destruction. No reconnect. */
#ifndef GT_BRIDGE_MULTIPLEX_H
#define GT_BRIDGE_MULTIPLEX_H
#include "extension/stream.h"

#define GT_BRIDGE_MUX_INSTANCES_MAX 8u
#define GT_BRIDGE_MUX_QUEUE_MAX 64u
typedef struct gt_bridge_mux gt_bridge_mux_t;

typedef struct {
    char connection[33];
    uint32_t frame_bytes, receive_frames, instances;
} gt_bridge_mux_config_t;

typedef struct {
    char connection[33], instance[33];
    uint64_t epoch;
} gt_bridge_mux_identity_t;

typedef struct {
    gt_status_t error;
    size_t instances;
    uint64_t received, sent, rejected;
    uint64_t accepted_bytes, sent_bytes, received_bytes;
    uint32_t buffered;
    bool stopping, released;
} gt_bridge_mux_info_t;

typedef struct {
    gt_bridge_mux_identity_t identity;
    gt_status_t error, close_error;
    size_t queued_frames;
    uint64_t received, sent, rejected;
    bool closing, close_sent, released;
} gt_bridge_mux_branch_info_t;

/* Adopts a pristine admitted stream on success only. Fixed finite frame and
 * receive-queue capacities are charged to the shared plugin heap. */
gt_status_t gt__bridge_mux_open(gt_bridge_stream_t **,
                                const gt_bridge_mux_config_t *,
                                const gt_allocator_t *,
                                gt_bridge_mux_t **);
/* Epochs are native-issued connection-wide invocation generations. A bounded
 * 64-generation admission window permits out-of-order registration, rejects
 * every reused/retired generation and never wraps. Active old generations stay
 * valid. Failure consumes neither the epoch nor the caller's stream owner.
 * Reusing an instance ID requires a fresh epoch and retirement of its old
 * branch. At most eight branches are live, including draining branches. */
gt_status_t gt__bridge_mux_branch_open(gt_bridge_mux_t *,
                                       const char *instance,
                                       uint64_t epoch,
                                       gt_bridge_stream_t **);
bool gt__bridge_mux_branch_identity(const gt_bridge_stream_t *, gt_bridge_mux_identity_t *);
bool gt__bridge_mux_branch_info(const gt_bridge_stream_t *, gt_bridge_mux_branch_info_t *);
gt_status_t gt__bridge_mux_branch_status(const gt_bridge_stream_t *);
gt_status_t gt__bridge_mux_scope_info(const gt_bridge_mux_t *,
                                      const char *,
                                      uint64_t,
                                      gt_bridge_mux_branch_info_t *);

typedef struct {
    uint64_t next_send, next_receive;
    uint32_t outgoing_bytes, incoming_bytes;
    bool enabled, host, awaiting, send_exhausted, receive_exhausted;
} gt_bridge_mux_native_info_t;

gt_status_t gt__bridge_mux_native_enable(gt_bridge_mux_t *, bool host);
gt_status_t gt__bridge_mux_native_send(gt_bridge_mux_t *, const void *, size_t);
gt_status_t gt__bridge_mux_native_take(gt_bridge_mux_t *, void *, size_t, size_t *);
bool gt__bridge_mux_native_info(const gt_bridge_mux_t *, gt_bridge_mux_native_info_t *);
/* At most one physical write and one read (wait capped at 10ms). Session IO on
 * child streams is nonblocking and never pumps other instances recursively.
 * FIFO within a branch; round-robin between branch heads, with at most eight
 * non-bulk selections before a waiting resource chunk. One complete pending
 * outbound frame per branch bounds cancellation latency and backpressure.
 * Receive overflow fails only that instance explicitly; unknown/retired frames
 * are counted/rejected, never routed to a new owner. Malformed framing/JSON or
 * a foreign connection ID fails the physical connection. */
gt_status_t gt__bridge_mux_step(gt_bridge_mux_t *, int ms);
bool gt__bridge_mux_info(const gt_bridge_mux_t *, gt_bridge_mux_info_t *);
void gt__bridge_mux_stop(gt_bridge_mux_t *);
/* Pump while closing child Sessions. Child close drains accepted frames and a
 * scope-only unknown-outcome CLOSE; it does not stop the physical connection.
 * Parent close stops physical IO and waits for all child owners to be destroyed.
 * A retained physical write buffer lives through completed physical close. */
gt_bridge_close_t gt__bridge_mux_close(gt_bridge_mux_t *, int ms);
gt_status_t gt__bridge_mux_destroy(gt_bridge_mux_t **);
#endif
