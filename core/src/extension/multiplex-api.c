#include "ghosterm/extension_multiplex.h"
#include "extension/multiplex.h"
#include "extension/channel-owner.h"
#include "extension/memory.h"
#include <string.h>

struct gt_ext_mux {
    gt_bridge_mux_t *inner;
    gt_ext_channel_t *physical;
    gt_bridge_stream_t *transport; /* borrowed until inner destruction */
    size_t children;
    gt_status_t cleanup_error;
    bool released;
};

static void child_retired(void *context) {
    gt_ext_mux_t *mux = context;
    mux->children--;
}

gt_status_t
gt_ext_mux_open(gt_ext_channel_t **owned, const gt_ext_mux_config_t *config, gt_ext_mux_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out || !owned || !*owned || !config) {
        return GT_ERR_INVALID_ARG;
    }
    if ((*owned)->multiplexed) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_bridge_stream_t *stream = NULL;
    gt_status_t status = gt__ext_channel_admit(*owned, (*owned)->ssh, &stream);
    if (status != GT_OK) {
        return status;
    }
    gt_ext_mux_t *mux = gt__plugin_memory_calloc(1, sizeof(*mux));
    if (!mux) {
        return GT_ERR_IO;
    }
    gt_bridge_mux_config_t selected = {.frame_bytes = config->frame_bytes,
                                       .receive_frames = config->receive_frames,
                                       .instances = config->instances};
    memcpy(selected.connection, config->connection, sizeof(selected.connection));
    mux->transport = stream;
    status = gt__bridge_mux_open(&stream, &selected, NULL, &mux->inner);
    if (status != GT_OK) {
        gt__plugin_memory_free(NULL, mux, sizeof(*mux));
        return status;
    }
    mux->physical = *owned;
    gt__ext_channel_commit(mux->physical);
    *owned = NULL;
    *out = mux;
    return GT_OK;
}

gt_status_t gt_ext_mux_open_channel(gt_ext_mux_t *mux,
                                    const char *instance,
                                    uint64_t epoch,
                                    gt_ext_channel_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!mux || !out || mux->released) {
        return GT_ERR_INVALID_ARG;
    }
    /* Allocate the facade before committing the private branch/epoch. Native
     * allocation failure must not consume an invocation generation. */
    gt_ext_channel_t *channel = gt__ext_channel_new_branch(mux->physical->ssh);
    if (!channel) {
        return GT_ERR_IO;
    }
    gt_status_t status = gt__bridge_mux_branch_open(mux->inner, instance, epoch, &channel->stream);
    if (status != GT_OK) {
        gt__ext_channel_retire(channel);
        return status;
    }
    channel->retire_context = mux;
    channel->did_retire = child_retired;
    mux->children++;
    *out = channel;
    return GT_OK;
}

gt_status_t gt_ext_mux_step(gt_ext_mux_t *mux, int ms) {
    return mux ? gt__bridge_mux_step(mux->inner, ms) : GT_ERR_INVALID_ARG;
}

bool gt_ext_mux_info(const gt_ext_mux_t *mux, gt_ext_mux_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    gt_bridge_mux_info_t info;
    if (!mux || !out || !gt__bridge_mux_info(mux->inner, &info)) {
        return false;
    }
    *out = (gt_ext_mux_info_t){.error = info.error,
                               .cleanup_error = mux->cleanup_error,
                               .instances = (uint32_t)mux->children,
                               .received = info.received,
                               .sent = info.sent,
                               .rejected = info.rejected,
                               .stopping = info.stopping,
                               .released = mux->released};
    return true;
}

gt_status_t gt_ext_mux_scope_info(const gt_ext_mux_t *mux,
                                  const char *instance,
                                  uint64_t epoch,
                                  gt_ext_mux_scope_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!mux || !out) {
        return GT_ERR_INVALID_ARG;
    }
    gt_bridge_mux_branch_info_t info;
    gt_status_t status = gt__bridge_mux_scope_info(mux->inner, instance, epoch, &info);
    if (status != GT_OK) {
        return status;
    }
    memcpy(out->connection, info.identity.connection, sizeof(out->connection));
    memcpy(out->instance, info.identity.instance, sizeof(out->instance));
    out->epoch = info.identity.epoch;
    out->error = info.error;
    out->close_error = info.close_error;
    out->queued_frames = (uint32_t)info.queued_frames;
    out->received = info.received;
    out->sent = info.sent;
    out->rejected = info.rejected;
    out->closing = info.closing;
    out->close_sent = info.close_sent;
    out->released = info.released;
    return GT_OK;
}

gt_status_t gt_ext_mux_transport_info(gt_ext_mux_t *mux, gt_ext_channel_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    gt_bridge_mux_info_t info;
    if (!mux || !out) {
        return GT_ERR_INVALID_ARG;
    }
    if (!gt__ext_channel_retained_info(mux->physical, mux->transport, out) ||
        !gt__bridge_mux_info(mux->inner, &info)) {
        memset(out, 0, sizeof(*out));
        return GT_ERR_INTERNAL;
    }
    out->accepted = info.accepted_bytes;
    out->sent = info.sent_bytes;
    out->received = info.received_bytes;
    out->buffered = info.buffered;
    return GT_OK;
}

gt_status_t gt_ext_mux_process_info(const gt_ext_mux_t *mux, gt_ext_transport_process_info_t *out) {
    return gt_ext_channel_process_info(mux ? mux->physical : NULL, out);
}

void gt_ext_mux_stop(gt_ext_mux_t *mux) {
    if (mux) {
        gt__bridge_mux_stop(mux->inner);
    }
}

gt_ext_close_t gt_ext_mux_close(gt_ext_mux_t *mux, int ms) {
    if (!mux || ms < 0) {
        return GT_EXT_CLOSE_ERROR;
    }
    if (mux->released) {
        return GT_EXT_RELEASED;
    }
    gt_bridge_close_t status = gt__bridge_mux_close(mux->inner, ms);
    if (status == GT_BRIDGE_CLOSE_ERROR) {
        mux->cleanup_error = GT_ERR_IO;
        return GT_EXT_CLOSE_ERROR;
    }
    if (status == GT_BRIDGE_DRAINING || mux->children) {
        return GT_EXT_DRAINING;
    }
    mux->released = true;
    mux->cleanup_error = GT_OK;
    return GT_EXT_RELEASED;
}

gt_status_t gt_ext_mux_destroy(gt_ext_mux_t **owned) {
    if (!owned || !*owned || !(*owned)->released || (*owned)->children) {
        return GT_ERR_INVALID_ARG;
    }
    gt_ext_mux_t *mux = *owned;
    gt_status_t status = gt__bridge_mux_destroy(&mux->inner);
    if (status != GT_OK) {
        mux->cleanup_error = status;
        return status;
    }
    gt__ext_channel_retire(mux->physical);
    gt__plugin_memory_free(NULL, mux, sizeof(*mux));
    *owned = NULL;
    return GT_OK;
}
