/* Native framed multiplexing over an admitted physical channel. All calls
 * except stop belong to the same serial non-UI owner as every child Session.
 * Only explicit native registration creates an instance; never expose these
 * handles or transport bytes to plugin code or the UI event queue. */
#ifndef GHOSTERM_EXTENSION_MULTIPLEX_H
#define GHOSTERM_EXTENSION_MULTIPLEX_H
#include "ghosterm/extension.h"
#ifdef __cplusplus
extern "C" {
#endif

#define GT_EXT_MUX_INSTANCES_MAX 8u
#define GT_EXT_MUX_QUEUE_MAX 64u
typedef struct gt_ext_mux gt_ext_mux_t;

typedef struct {
    char connection[33]; /* exact nonzero lower-case 128-bit native identity */
    uint32_t frame_bytes, receive_frames, instances;
} gt_ext_mux_config_t;

typedef struct {
    gt_status_t error, cleanup_error;
    uint32_t instances;
    uint64_t received, sent, rejected;
    bool stopping, released;
} gt_ext_mux_info_t;

typedef struct {
    char connection[33], instance[33];
    uint64_t epoch;
    gt_status_t error, close_error;
    uint32_t queued_frames;
    uint64_t received, sent, rejected;
    bool closing, close_sent, released;
} gt_ext_mux_scope_info_t;

/* READY, pristine physical channel only. Success clears *channel; failure
 * retains it. Keeps its one global physical-channel reservation until actual
 * mux destruction. Logical children do not consume additional physical slots.
 * No nested mux, implicit authentication, package grant, reconnect or replay.
 * frame_bytes: 512..65536, receive_frames: 1..64, instances: 1..8. */
gt_status_t gt_ext_mux_open(gt_ext_channel_t **, const gt_ext_mux_config_t *, gt_ext_mux_t **);
/* Explicit native-issued invocation generation; nonzero, never reused within
 * this connection. A bounded 64-generation registration window permits
 * out-of-order admission; active old generations remain valid. Same ID reuse
 * requires retiring the old owner and a fresh epoch. Failure consumes no epoch.
 * Returned READY logical channel has the parent's actual LOCAL/SSH kind and
 * may be adopted by the ordinary Session/RPC/resources constructors. */
gt_status_t gt_ext_mux_open_channel(gt_ext_mux_t *, const char *, uint64_t, gt_ext_channel_t **);
/* One bounded physical IO step. Caller also drives each child Session/Runtime;
 * no recursive driving, automatic execution or unlimited background drain. */
gt_status_t gt_ext_mux_step(gt_ext_mux_t *, int ms);
bool gt_ext_mux_info(const gt_ext_mux_t *, gt_ext_mux_info_t *);
/* Current scope, or bounded final snapshot until its native slot is reused.
 * NOT_FOUND / wrong scope never falls back to another instance. Released and
 * close_sent are transport facts, not a business completion or process receipt. */
gt_status_t
gt_ext_mux_scope_info(const gt_ext_mux_t *, const char *, uint64_t, gt_ext_mux_scope_info_t *);

/* Explicit Native-only connection management, disabled by default. It uses
 * ordinary wire-v1 request/response envelopes at instanceId=connectionId,
 * epoch=1, streamId=rpc.request/rpc.response, method native.multiplex.*.
 * This reserved owner is not a business instance or a plugin-visible Session.
 * No peer frame creates a channel: the Native receiver must validate the
 * requested operation and explicitly call open_channel. Eight business slots
 * remain available. Enable both endpoints before sending management frames.
 * The local HOST sends one request at a time; EXECUTOR sends its matching
 * response. A reply is admission/cleanup metadata, never a business outcome. */
typedef struct {
    uint64_t next_send, next_receive;
    uint32_t outgoing_bytes, incoming_bytes;
    bool enabled, host, awaiting, send_exhausted, receive_exhausted;
} gt_ext_mux_native_info_t;

gt_status_t gt_ext_mux_native_enable(gt_ext_mux_t *, bool host);
/* Copies a validated complete JSON envelope, without the length prefix. One
 * bounded outbound and inbound frame, charged to the shared heap. Busy returns
 * GT_ERR_IO without consuming bytes/sequence. Wrong identity/direction/request
 * ID, malformed JSON and replay are rejected. Send never performs IO. */
gt_status_t gt_ext_mux_native_send(gt_ext_mux_t *, const void *, size_t);
/* NOT_FOUND means no complete message. Short buffers return INVALID_ARG and
 * report the required size without consuming the message. No NUL is added. */
gt_status_t gt_ext_mux_native_take(gt_ext_mux_t *, void *, size_t, size_t *);
bool gt_ext_mux_native_info(const gt_ext_mux_t *, gt_ext_mux_native_info_t *);
/* Snapshot of the physical authentication/diagnostics and actual physical IO.
 * Child channels do not impersonate this process or authentication owner. */
gt_status_t gt_ext_mux_transport_info(gt_ext_mux_t *, gt_ext_channel_info_t *);
gt_status_t gt_ext_mux_process_info(const gt_ext_mux_t *, gt_ext_transport_process_info_t *);
void gt_ext_mux_stop(gt_ext_mux_t *);
/* Stop the physical connection, then retain ownership on DRAINING/error until
 * every child channel/Session/Runtime and its resource borrows are destroyed.
 * Normal individual child close only closes its own scope; pump mux_step while
 * closing children. Only stop may race IO/close; exclude stop during ownership
 * adoption/destruction. Success destroy clears *owner, failure keeps it. */
gt_ext_close_t gt_ext_mux_close(gt_ext_mux_t *, int ms);
gt_status_t gt_ext_mux_destroy(gt_ext_mux_t **);

#ifdef __cplusplus
}
#endif
#endif
