#define _POSIX_C_SOURCE 200809L
#include "extension/multiplex.h"
#include "extension/frame.h"
#include "extension/wire.h"
#include "extension/memory.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gt_bridge_stream_t stream;
    gt_bridge_mux_t *mux;
    gt_bridge_mux_identity_t identity;
    gt_bridge_decoder_t *outgoing;
    uint8_t *incoming;
    size_t lengths[GT_BRIDGE_MUX_QUEUE_MAX];
    size_t head, count, offset;
    gt_status_t error, close_error;
    uint64_t next_control, received, sent, rejected;
    bool live, bulk, closing, close_queued, close_sent, peer_closed, control_exhausted,
        outgoing_invalid;
} mux_branch_t;

struct gt_bridge_mux {
    gt_allocator_t allocator;
    gt_bridge_mux_config_t config;
    gt_bridge_stream_t *stream;
    gt_bridge_decoder_t *decoder;
    gt_bridge_writer_t *writer;
    gt_bridge_wire_t *wire;
    mux_branch_t branches[GT_BRIDGE_MUX_INSTANCES_MAX];
    atomic_bool stopping;
    gt_status_t error;
    size_t live, cursor, control_burst;
    size_t writing_slot;
    bool writing, writing_close, released;
    uint64_t highest_epoch, admitted_epochs, received, sent, rejected;
    uint8_t input[GT_BRIDGE_IO_QUANTUM];
    size_t input_size, input_used;
};

static const gt_bridge_stream_ops_t branch_ops;
static bool ready(const mux_branch_t *);

static void count_one(uint64_t *value) {
    if (*value != UINT64_MAX) {
        (*value)++;
    }
}

static void *allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void release(void *context, void *bytes, size_t size) {
    (void)context;
    (void)size;
    free(bytes);
}

static bool valid_id(const char *id) {
    if (!id || strnlen(id, 33) != 32) {
        return false;
    }
    bool nonzero = false;
    for (size_t i = 0; i < 32; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
        nonzero |= id[i] != '0';
    }
    return nonzero;
}

static bool stopped(const gt_bridge_mux_t *mux) {
    return atomic_load_explicit(&mux->stopping, memory_order_acquire);
}

static void fail(gt_bridge_mux_t *mux, gt_status_t error) {
    if (mux->error == GT_OK) {
        mux->error = error;
        gt__bridge_stream_stop(mux->stream);
    }
}

static void fail_branch(mux_branch_t *branch, gt_status_t error) {
    if (branch->error == GT_OK) {
        branch->error = error;
    }
    branch->closing = true;
}

static gt_bridge_io_t branch_state(const mux_branch_t *branch) {
    if (branch->mux->error != GT_OK) {
        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, branch->mux->error};
    }
    if (branch->error != GT_OK) {
        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, branch->error};
    }
    return (gt_bridge_io_t){GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED};
}

static size_t stride(const gt_bridge_mux_t *mux) {
    return (size_t)mux->config.frame_bytes + GT_BRIDGE_FRAME_PREFIX;
}

static gt_bridge_io_t branch_read(gt_bridge_stream_t *stream, void *out, size_t size, int ms) {
    (void)ms;
    mux_branch_t *branch = (mux_branch_t *)stream;
    if (branch->error != GT_OK || branch->mux->error != GT_OK || stopped(branch->mux)) {
        return branch_state(branch);
    }
    if (!branch->count) {
        return (gt_bridge_io_t){GT_BRIDGE_WAIT, 0, GT_OK};
    }
    size_t remaining = branch->lengths[branch->head] - branch->offset;
    size_t count = remaining < size ? remaining : size;
    memcpy(out, branch->incoming + branch->head * stride(branch->mux) + branch->offset, count);
    branch->offset += count;
    if (branch->offset == branch->lengths[branch->head]) {
        branch->head = (branch->head + 1) % branch->mux->config.receive_frames;
        branch->count--;
        branch->offset = 0;
    }
    return (gt_bridge_io_t){GT_BRIDGE_DATA, count, GT_OK};
}

static gt_bridge_io_t
branch_write(gt_bridge_stream_t *stream, const void *bytes, size_t size, int ms) {
    (void)ms;
    mux_branch_t *branch = (mux_branch_t *)stream;
    gt_bridge_mux_t *mux = branch->mux;
    if (branch->closing || branch->error != GT_OK || mux->error != GT_OK || stopped(mux)) {
        return branch_state(branch);
    }
    const uint8_t *payload;
    size_t length;
    if (gt__bridge_decoder_view(branch->outgoing, &payload, &length)) {
        return (gt_bridge_io_t){GT_BRIDGE_WAIT, 0, GT_OK};
    }
    size_t used = 0;
    gt_status_t status = gt__bridge_decoder_feed(branch->outgoing, bytes, size, &used);
    if (status == GT_OK && gt__bridge_decoder_view(branch->outgoing, &payload, &length)) {
        gt_bridge_message_t message;
        status = gt__bridge_wire_parse(mux->wire, payload, length, &message);
        if (status == GT_OK && (strcmp(message.connection, branch->identity.connection) ||
                                strcmp(message.instance, branch->identity.instance) ||
                                message.epoch != branch->identity.epoch)) {
            status = GT_ERR_PROTOCOL;
        }
        if (status == GT_OK && !strcmp(message.stream, "control")) {
            if (branch->control_exhausted || message.sequence != branch->next_control ||
                (message.sequence == UINT64_MAX && message.kind != GT_BRIDGE_CLOSE)) {
                status = GT_ERR_PROTOCOL;
            } else if (message.sequence == UINT64_MAX) {
                branch->control_exhausted = true;
            } else {
                branch->next_control++;
            }
        }
        if (status == GT_OK) {
            branch->bulk = message.kind == GT_BRIDGE_RESOURCE_CHUNK;
            branch->close_queued =
                message.kind == GT_BRIDGE_CLOSE && !strcmp(message.stream, "control");
            if (branch->close_queued) {
                branch->closing = true;
            }
        }
    }
    if (status != GT_OK) {
        /* No byte of this unvalidated frame reached the physical stream. */
        branch->outgoing_invalid = true;
        fail_branch(branch, status);
        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, status};
    }
    return (gt_bridge_io_t){GT_BRIDGE_DATA, used, GT_OK};
}

static void branch_wake(gt_bridge_stream_t *stream) {
    /* The wrapper has already set this child's atomic stop flag. Parent polls
     * are bounded to 10ms; stopping a child must not stop/wake-poison the fd. */
    (void)stream;
}

static gt_bridge_close_t branch_close(gt_bridge_stream_t *stream, int ms) {
    (void)ms;
    mux_branch_t *branch = (mux_branch_t *)stream;
    branch->closing = true;
    if (stopped(branch->mux) || branch->mux->error != GT_OK) {
        return GT_BRIDGE_RELEASED;
    }
    bool writing =
        branch->mux->writing && &branch->mux->branches[branch->mux->writing_slot] == branch;
    return !writing && (branch->close_sent || (branch->peer_closed && !ready(branch)))
               ? GT_BRIDGE_RELEASED
               : GT_BRIDGE_DRAINING;
}

static void branch_destroy(gt_bridge_stream_t *stream) {
    mux_branch_t *branch = (mux_branch_t *)stream;
    gt_bridge_mux_t *mux = branch->mux;
    gt__bridge_decoder_free(branch->outgoing);
    gt__plugin_memory_free(
        &mux->allocator, branch->incoming, stride(mux) * mux->config.receive_frames);
    branch->outgoing = NULL;
    branch->incoming = NULL;
    branch->live = false;
    mux->live--;
}

static const gt_bridge_stream_ops_t branch_ops = {
    branch_read, branch_write, branch_wake, branch_close, branch_destroy};

static bool fresh_epoch(const gt_bridge_mux_t *mux, uint64_t epoch) {
    if (!epoch) {
        return false;
    }
    if (epoch > mux->highest_epoch) {
        return true;
    }
    uint64_t distance = mux->highest_epoch - epoch;
    return distance < 64 && !(mux->admitted_epochs & (UINT64_C(1) << distance));
}

static void admit_epoch(gt_bridge_mux_t *mux, uint64_t epoch) {
    if (epoch > mux->highest_epoch) {
        uint64_t distance = epoch - mux->highest_epoch;
        mux->admitted_epochs =
            distance >= 64 ? UINT64_C(1) : (mux->admitted_epochs << distance) | UINT64_C(1);
        mux->highest_epoch = epoch;
    } else {
        mux->admitted_epochs |= UINT64_C(1) << (mux->highest_epoch - epoch);
    }
}

gt_status_t gt__bridge_mux_branch_open(gt_bridge_mux_t *mux,
                                       const char *instance,
                                       uint64_t epoch,
                                       gt_bridge_stream_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!mux || !out || !valid_id(instance) || mux->released || stopped(mux) ||
        mux->error != GT_OK || !fresh_epoch(mux, epoch)) {
        return GT_ERR_INVALID_ARG;
    }
    mux_branch_t *available = NULL;
    for (size_t i = 0; i < mux->config.instances; i++) {
        mux_branch_t *branch = &mux->branches[i];
        if (branch->live && !strcmp(branch->identity.instance, instance)) {
            return GT_ERR_INVALID_ARG;
        }
        if (!branch->live && !available) {
            available = branch;
        }
    }
    if (!available) {
        return GT_ERR_IO;
    }
    gt_bridge_decoder_t *decoder = NULL;
    gt_status_t status = gt__bridge_decoder_new(mux->config.frame_bytes, &mux->allocator, &decoder);
    if (status != GT_OK) {
        return status;
    }
    uint8_t *incoming =
        gt__plugin_memory_alloc(&mux->allocator, stride(mux) * mux->config.receive_frames);
    if (!incoming) {
        gt__bridge_decoder_free(decoder);
        return GT_ERR_IO;
    }
    *available =
        (mux_branch_t){.mux = mux, .outgoing = decoder, .incoming = incoming, .live = true};
    gt__bridge_stream_init(&available->stream, &branch_ops);
    memcpy(available->identity.connection, mux->config.connection, 33);
    memcpy(available->identity.instance, instance, 33);
    available->identity.epoch = epoch;
    admit_epoch(mux, epoch);
    mux->live++;
    *out = &available->stream;
    return GT_OK;
}

bool gt__bridge_mux_branch_identity(const gt_bridge_stream_t *stream,
                                    gt_bridge_mux_identity_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!stream || stream->ops != &branch_ops || !out) {
        return false;
    }
    const mux_branch_t *branch = (const mux_branch_t *)stream;
    if (!branch->live) {
        return false;
    }
    *out = branch->identity;
    return true;
}

bool gt__bridge_mux_branch_info(const gt_bridge_stream_t *stream,
                                gt_bridge_mux_branch_info_t *out) {
    if (!out || !gt__bridge_mux_branch_identity(stream, &out->identity)) {
        return false;
    }
    const mux_branch_t *branch = (const mux_branch_t *)stream;
    *out = (gt_bridge_mux_branch_info_t){
        .identity = branch->identity,
        .error = branch->error,
        .close_error = branch->close_error,
        .queued_frames = branch->count,
        .received = branch->received,
        .sent = branch->sent,
        .rejected = branch->rejected,
        .closing = branch->closing || atomic_load_explicit(&stream->stopping, memory_order_acquire),
        .close_sent = branch->close_sent};
    return true;
}

static bool ready(const mux_branch_t *branch) {
    const uint8_t *bytes;
    size_t size;
    return branch->live && gt__bridge_decoder_view(branch->outgoing, &bytes, &size);
}

static gt_status_t queue_close(gt_bridge_mux_t *mux, mux_branch_t *branch, size_t slot) {
    char bytes[512];
    int size = snprintf(
        bytes,
        sizeof(bytes),
        "{\"wireVersion\":1,\"connectionId\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64
        "\",\"streamId\":\"control\",\"seq\":\"%" PRIu64
        "\",\"kind\":\"close\",\"payload\":{\"outcome\":"
        "{\"kind\":\"unknown\",\"reason\":\"native instance closed without a business completion "
        "receipt\"}}}",
        branch->identity.connection,
        branch->identity.instance,
        branch->identity.epoch,
        branch->next_control);
    if (size < 0 || (size_t)size >= sizeof(bytes) || branch->control_exhausted) {
        branch->close_error = GT_ERR_PROTOCOL;
        return GT_ERR_PROTOCOL;
    }
    gt_status_t status = gt__bridge_writer_queue(mux->writer, bytes, (size_t)size);
    if (status == GT_OK) {
        mux->writing = true;
        mux->writing_close = true;
        mux->writing_slot = slot;
    } else {
        branch->close_error = status;
    }
    return status;
}

static gt_status_t choose_write(gt_bridge_mux_t *mux) {
    for (size_t i = 0; i < mux->config.instances; i++) {
        size_t slot = (mux->cursor + i) % mux->config.instances;
        mux_branch_t *branch = &mux->branches[slot];
        if (!branch->live) {
            continue;
        }
        if (atomic_load_explicit(&branch->stream.stopping, memory_order_acquire)) {
            branch->closing = true;
        }
        if ((branch->outgoing_invalid || branch->peer_closed) && ready(branch)) {
            gt__bridge_decoder_consume(branch->outgoing);
        }
    }
    bool bulk_waiting = false;
    bool control_waiting = false;
    for (size_t i = 0; i < mux->config.instances; i++) {
        if (ready(&mux->branches[i])) {
            if (mux->branches[i].bulk) {
                bulk_waiting = true;
            } else {
                control_waiting = true;
            }
        }
        mux_branch_t *branch = &mux->branches[i];
        if (branch->live && branch->closing && !ready(branch) && !branch->close_sent &&
            !branch->peer_closed) {
            control_waiting = true;
        }
    }
    bool bulk = bulk_waiting && (!control_waiting || mux->control_burst >= 8);
    for (size_t i = 0; i < mux->config.instances; i++) {
        size_t slot = (mux->cursor + i) % mux->config.instances;
        mux_branch_t *branch = &mux->branches[slot];
        if (!bulk && branch->live && branch->closing && !ready(branch) && !branch->close_sent &&
            !branch->peer_closed) {
            gt_status_t status = queue_close(mux, branch, slot);
            if (status == GT_OK && mux->control_burst < 8) {
                mux->control_burst++;
            }
            return status;
        }
        if (!ready(branch) || branch->bulk != bulk) {
            continue;
        }
        const uint8_t *bytes;
        size_t size;
        gt__bridge_decoder_view(branch->outgoing, &bytes, &size);
        gt_status_t status = gt__bridge_writer_queue(mux->writer, bytes, size);
        if (status != GT_OK) {
            return status;
        }
        mux->writing = true;
        mux->writing_slot = slot;
        mux->writing_close = branch->close_queued;
        gt__bridge_decoder_consume(branch->outgoing);
        mux->control_burst = bulk ? 0 : (mux->control_burst < 8 ? mux->control_burst + 1 : 8);
        return GT_OK;
    }
    return GT_OK;
}

static gt_status_t route_frame(gt_bridge_mux_t *mux, const uint8_t *bytes, size_t size) {
    gt_bridge_message_t message;
    gt_status_t status = gt__bridge_wire_parse(mux->wire, bytes, size, &message);
    if (status != GT_OK || strcmp(message.connection, mux->config.connection)) {
        return GT_ERR_PROTOCOL;
    }
    count_one(&mux->received);
    for (size_t i = 0; i < mux->config.instances; i++) {
        mux_branch_t *branch = &mux->branches[i];
        if (!branch->live || strcmp(branch->identity.instance, message.instance)) {
            continue;
        }
        if (branch->identity.epoch != message.epoch || branch->closing || branch->peer_closed ||
            branch->error != GT_OK ||
            atomic_load_explicit(&branch->stream.stopping, memory_order_acquire)) {
            count_one(&branch->rejected);
            count_one(&mux->rejected);
            return GT_OK;
        }
        if (branch->count == mux->config.receive_frames) {
            count_one(&branch->rejected);
            count_one(&mux->rejected);
            fail_branch(branch, GT_ERR_PROTOCOL);
            return GT_OK;
        }
        size_t tail = (branch->head + branch->count) % mux->config.receive_frames;
        uint8_t *frame = branch->incoming + tail * stride(mux);
        frame[0] = (uint8_t)(size >> 24);
        frame[1] = (uint8_t)(size >> 16);
        frame[2] = (uint8_t)(size >> 8);
        frame[3] = (uint8_t)size;
        memcpy(frame + 4, bytes, size);
        branch->lengths[tail] = size + 4;
        branch->count++;
        count_one(&branch->received);
        branch->peer_closed = message.kind == GT_BRIDGE_CLOSE && !strcmp(message.stream, "control");
        return GT_OK;
    }
    count_one(&mux->rejected);
    return GT_OK;
}

gt_status_t gt__bridge_mux_step(gt_bridge_mux_t *mux, int ms) {
    if (!mux || ms < 0 || mux->released) {
        return GT_ERR_INVALID_ARG;
    }
    if (stopped(mux)) {
        return GT_ERR_CANCELLED;
    }
    if (mux->error != GT_OK) {
        return mux->error;
    }
    gt_status_t status = GT_OK;
    if (!mux->writing) {
        status = choose_write(mux);
    }
    if (status == GT_OK && mux->writing) {
        gt_bridge_io_t result = gt__bridge_writer_step(mux->writer, mux->stream, 0);
        if (result.state != GT_BRIDGE_DATA && result.state != GT_BRIDGE_WAIT) {
            status = result.error == GT_OK ? GT_ERR_IO : result.error;
        } else if (!gt__bridge_writer_pending(mux->writer)) {
            mux_branch_t *branch = &mux->branches[mux->writing_slot];
            count_one(&branch->sent);
            count_one(&mux->sent);
            if (mux->writing_close) {
                branch->close_sent = true;
            }
            mux->cursor = (mux->writing_slot + 1) % mux->config.instances;
            mux->writing = false;
        }
    }
    if (status != GT_OK) {
        fail(mux, status);
        return status;
    }
    if (mux->input_used == mux->input_size) {
        gt_bridge_io_t result =
            gt__bridge_stream_read(mux->stream, mux->input, sizeof(mux->input), ms > 10 ? 10 : ms);
        if (result.state == GT_BRIDGE_WAIT) {
            return GT_OK;
        }
        if (result.state != GT_BRIDGE_DATA) {
            status =
                result.state == GT_BRIDGE_EOF && gt__bridge_decoder_finish(mux->decoder) != GT_OK
                    ? GT_ERR_PROTOCOL
                    : GT_ERR_IO;
            fail(mux, status);
            return status;
        }
        mux->input_used = 0;
        mux->input_size = result.count;
    }
    const uint8_t *bytes;
    size_t size;
    size_t used = 0;
    status = gt__bridge_decoder_feed(
        mux->decoder, mux->input + mux->input_used, mux->input_size - mux->input_used, &used);
    mux->input_used += used;
    if (status == GT_OK && gt__bridge_decoder_view(mux->decoder, &bytes, &size)) {
        status = route_frame(mux, bytes, size);
        gt__bridge_decoder_consume(mux->decoder);
    }
    if (status != GT_OK) {
        fail(mux, status);
    }
    return status;
}

bool gt__bridge_mux_info(const gt_bridge_mux_t *mux, gt_bridge_mux_info_t *out) {
    if (!mux || !out) {
        return false;
    }
    *out = (gt_bridge_mux_info_t){.error = mux->error,
                                  .instances = mux->live,
                                  .received = mux->received,
                                  .sent = mux->sent,
                                  .rejected = mux->rejected,
                                  .stopping = stopped(mux),
                                  .released = mux->released};
    return true;
}

void gt__bridge_mux_stop(gt_bridge_mux_t *mux) {
    if (mux && !atomic_exchange_explicit(&mux->stopping, true, memory_order_acq_rel)) {
        gt__bridge_stream_stop(mux->stream);
    }
}

gt_bridge_close_t gt__bridge_mux_close(gt_bridge_mux_t *mux, int ms) {
    if (!mux || ms < 0) {
        return GT_BRIDGE_CLOSE_ERROR;
    }
    if (mux->released) {
        return GT_BRIDGE_RELEASED;
    }
    gt__bridge_mux_stop(mux);
    if (mux->live) {
        return GT_BRIDGE_DRAINING;
    }
    gt_bridge_close_t result = gt__bridge_stream_close(mux->stream, ms);
    if (result == GT_BRIDGE_RELEASED) {
        mux->released = true;
    }
    return result;
}

static void free_mux(gt_bridge_mux_t *mux) {
    gt__bridge_wire_free(mux->wire);
    gt__bridge_writer_free(mux->writer);
    gt__bridge_decoder_free(mux->decoder);
    gt_allocator_t allocator = mux->allocator;
    gt__plugin_memory_free(&allocator, mux, sizeof(*mux));
}

gt_status_t gt__bridge_mux_open(gt_bridge_stream_t **owned,
                                const gt_bridge_mux_config_t *config,
                                const gt_allocator_t *input,
                                gt_bridge_mux_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out || !owned || !*owned || !config || !valid_id(config->connection) || (*owned)->closed ||
        atomic_load_explicit(&(*owned)->stopping, memory_order_acquire) || config->instances == 0 ||
        config->instances > GT_BRIDGE_MUX_INSTANCES_MAX || config->receive_frames == 0 ||
        config->receive_frames > GT_BRIDGE_MUX_QUEUE_MAX || config->frame_bytes < 512 ||
        config->frame_bytes > GT_BRIDGE_FRAME_MAX ||
        (input && (!input->allocate || !input->release))) {
        return GT_ERR_INVALID_ARG;
    }
    gt_allocator_t allocator = input ? *input : (gt_allocator_t){NULL, allocate, release};
    gt_bridge_mux_t *mux = gt__plugin_memory_alloc(&allocator, sizeof(*mux));
    if (!mux) {
        return GT_ERR_IO;
    }
    *mux = (gt_bridge_mux_t){.allocator = allocator, .config = *config, .stream = *owned};
    atomic_init(&mux->stopping, false);
    gt_status_t status = gt__bridge_wire_new(&allocator, &mux->wire);
    if (status == GT_OK) {
        status = gt__bridge_decoder_new(config->frame_bytes, &allocator, &mux->decoder);
    }
    if (status == GT_OK) {
        status = gt__bridge_writer_new(config->frame_bytes, &allocator, &mux->writer);
    }
    if (status != GT_OK) {
        free_mux(mux);
        return status;
    }
    *owned = NULL;
    *out = mux;
    return GT_OK;
}

gt_status_t gt__bridge_mux_destroy(gt_bridge_mux_t **owner) {
    if (!owner) {
        return GT_ERR_INVALID_ARG;
    }
    gt_bridge_mux_t *mux = *owner;
    if (!mux) {
        return GT_OK;
    }
    if (!mux->released || mux->live) {
        return GT_ERR_INVALID_ARG;
    }
    gt_status_t status = gt__bridge_stream_destroy(mux->stream);
    if (status != GT_OK) {
        return status;
    }
    *owner = NULL;
    free_mux(mux);
    return GT_OK;
}
