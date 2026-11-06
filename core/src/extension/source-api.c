#include "extension/memory.h"
#include "ghosterm/extension_source.h"
#include "extension/session-owner.h"
#include "extension/source.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>

struct gt_ext_source {
    gt_ext_session_t *session;
    gt_bridge_source_t *inner;
    gt_ext_source_status_t cleanup_error;
};

static gt_ext_source_status_t ext_source_status(gt_bridge_source_status_t p) {
    switch (p) {
        case GT_BRIDGE_SOURCE_OK:
            return GT_EXT_SOURCE_OK;
        case GT_BRIDGE_SOURCE_WAIT:
            return GT_EXT_SOURCE_WAIT;
        case GT_BRIDGE_SOURCE_INVALID:
            return GT_EXT_SOURCE_INVALID;
        case GT_BRIDGE_SOURCE_DENIED:
            return GT_EXT_SOURCE_DENIED;
        case GT_BRIDGE_SOURCE_UNSUPPORTED:
            return GT_EXT_SOURCE_UNSUPPORTED;
        case GT_BRIDGE_SOURCE_IO:
            return GT_EXT_SOURCE_IO;
        case GT_BRIDGE_SOURCE_LIMIT:
            return GT_EXT_SOURCE_LIMIT_REACHED;
        case GT_BRIDGE_SOURCE_CHANGED:
            return GT_EXT_SOURCE_CHANGED;
        case GT_BRIDGE_SOURCE_CANCELLED:
            return GT_EXT_SOURCE_CANCELLED;
        case GT_BRIDGE_SOURCE_BUSY:
            return GT_EXT_SOURCE_BUSY;
        case GT_BRIDGE_SOURCE_UNKNOWN:
            return GT_EXT_SOURCE_UNKNOWN;
        case GT_BRIDGE_SOURCE_CLOSED:
            return GT_EXT_SOURCE_CLOSED;
        case GT_BRIDGE_SOURCE_NOT_FOUND:
            return GT_EXT_SOURCE_NOT_FOUND;
    }
    return GT_EXT_SOURCE_IO;
}

static gt_ext_source_phase_t ext_source_phase(gt_bridge_source_phase_t p) {
    switch (p) {
        case GT_BRIDGE_SOURCE_HASHING:
            return GT_EXT_SOURCE_HASHING;
        case GT_BRIDGE_SOURCE_READY:
            return GT_EXT_SOURCE_READY;
        case GT_BRIDGE_SOURCE_SENDING:
            return GT_EXT_SOURCE_SENDING;
        case GT_BRIDGE_SOURCE_RECEIPT:
            return GT_EXT_SOURCE_RECEIPT;
        case GT_BRIDGE_SOURCE_CANCELLING:
            return GT_EXT_SOURCE_CANCELLING;
        case GT_BRIDGE_SOURCE_SUCCEEDED:
            return GT_EXT_SOURCE_SUCCEEDED;
        case GT_BRIDGE_SOURCE_FAILED:
            return GT_EXT_SOURCE_FAILED;
        case GT_BRIDGE_SOURCE_STOPPED:
            return GT_EXT_SOURCE_STOPPED;
        case GT_BRIDGE_SOURCE_UNCERTAIN:
            return GT_EXT_SOURCE_UNCERTAIN;
    }
    return GT_EXT_SOURCE_FAILED;
}

static gt_ext_transfer_state_t ext_source_transfer(gt_bridge_transfer_state_t p) {
    switch (p) {
        case GT_BRIDGE_TRANSFER_QUEUED:
            return GT_EXT_TRANSFER_QUEUED;
        case GT_BRIDGE_TRANSFER_SENDING:
            return GT_EXT_TRANSFER_SENDING;
        case GT_BRIDGE_TRANSFER_PENDING:
            return GT_EXT_TRANSFER_PENDING;
        case GT_BRIDGE_TRANSFER_SUCCEEDED:
            return GT_EXT_TRANSFER_SUCCEEDED;
        case GT_BRIDGE_TRANSFER_FAILED:
            return GT_EXT_TRANSFER_FAILED;
        case GT_BRIDGE_TRANSFER_CANCELLED:
            return GT_EXT_TRANSFER_CANCELLED;
        case GT_BRIDGE_TRANSFER_UNKNOWN:
            return GT_EXT_TRANSFER_UNKNOWN;
    }
    return GT_EXT_TRANSFER_UNKNOWN;
}

gt_ext_source_status_t gt_ext_source_open(gt_ext_session_t *s,
                                          const gt_ext_lease_t *lease,
                                          const gt_ext_source_config_t *config,
                                          gt_ext_source_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!s || !lease || !config || !out) {
        return GT_EXT_SOURCE_INVALID;
    }
    if (!s->store) {
        return GT_EXT_SOURCE_UNSUPPORTED;
    }
    if (s->sources >= GT_EXT_SOURCE_LIMIT) {
        return GT_EXT_SOURCE_LIMIT_REACHED;
    }
    gt_bridge_lease_t l;
    memcpy(l.connection, lease->connection, 33);
    memcpy(l.instance, lease->instance, 33);
    memcpy(l.token, lease->token, 33);
    l.epoch = lease->epoch;
    gt_bridge_source_config_t c = {.session = s->inner,
                                   .lease = l,
                                   .root_fd = config->root_fd,
                                   .relative_path = config->relative_path,
                                   .mime = config->mime,
                                   .byte_limit = config->byte_limit};
    gt_ext_source_t *source = gt__plugin_memory_calloc(1, sizeof(*source));
    if (!source) {
        return GT_EXT_SOURCE_IO;
    }
    gt_ext_source_status_t status =
        ext_source_status(gt__bridge_source_open(&c, NULL, &source->inner));
    if (status != GT_EXT_SOURCE_OK) {
        gt__secure_erase(source, sizeof(*source));
        gt__plugin_memory_free(NULL, source, sizeof(*source));
        return status;
    }
    source->session = s;
    s->sources++;
    *out = source;
    return GT_EXT_SOURCE_OK;
}

gt_ext_source_phase_t gt_ext_source_step(gt_ext_source_t *s) {
    return s ? ext_source_phase(gt__bridge_source_step(s->inner)) : GT_EXT_SOURCE_FAILED;
}

gt_ext_source_status_t gt_ext_source_send(gt_ext_source_t *s) {
    return s ? ext_source_status(gt__bridge_source_send(s->inner)) : GT_EXT_SOURCE_INVALID;
}

void gt_ext_source_cancel(gt_ext_source_t *s) {
    if (s) {
        gt__bridge_source_cancel(s->inner);
    }
}

bool gt_ext_source_info(const gt_ext_source_t *s, gt_ext_source_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!s || !out) {
        return false;
    }
    gt_bridge_source_info_t i;
    if (!gt__bridge_source_info(s->inner, &i)) {
        return false;
    }
    out->phase = ext_source_phase(i.phase);
    out->error = ext_source_status(i.error);
    out->cleanup_error = s->cleanup_error;
    out->closed = i.closed;
    out->has_transfer = i.transfer_handle != 0;
    out->hashed = i.hashed;
    out->read_bytes = i.read_bytes;
    out->accepted = i.accepted;
    memcpy(out->descriptor.resource_id, i.descriptor.resource_id, 33);
    memcpy(out->descriptor.sha256, i.descriptor.sha256, 65);
    memcpy(out->descriptor.mime, i.descriptor.mime, 129);
    memcpy(out->descriptor.display_name, i.descriptor.display_name, 1025);
    out->descriptor.generation = i.descriptor.generation;
    out->descriptor.size = i.descriptor.size;
    out->descriptor.size_known = i.descriptor.size_known;
    out->transfer_state = ext_source_transfer(i.transfer.state);
    out->confirmed = i.transfer.confirmed;
    out->cancel_requested = i.transfer.cancel_requested;
    out->transfer_accepted = i.transfer.accepted;
    out->generation = i.transfer.generation;
    memcpy(out->resource_id, i.transfer.resource_id, 33);
    memcpy(out->code, i.transfer.code, 33);
    return true;
}

gt_ext_source_status_t gt_ext_source_close(gt_ext_source_t *s) {
    if (!s) {
        return GT_EXT_SOURCE_INVALID;
    }
    gt_ext_source_status_t status = ext_source_status(gt__bridge_source_close(s->inner));
    if (status == GT_EXT_SOURCE_IO) {
        s->cleanup_error = status;
    } else if (status == GT_EXT_SOURCE_OK) {
        s->cleanup_error = GT_EXT_SOURCE_OK;
    }
    return status;
}

gt_ext_source_status_t gt_ext_source_destroy(gt_ext_source_t **owned) {
    if (!owned || !*owned) {
        return GT_EXT_SOURCE_INVALID;
    }
    gt_ext_source_t *s = *owned;
    gt_ext_source_status_t status = ext_source_status(gt__bridge_source_destroy(s->inner));
    if (status != GT_EXT_SOURCE_OK) {
        if (status == GT_EXT_SOURCE_IO) {
            s->cleanup_error = status;
        }
        return status;
    }
    s->session->sources--;
    gt__secure_erase(s, sizeof(*s));
    gt__plugin_memory_free(NULL, s, sizeof(*s));
    *owned = NULL;
    return GT_EXT_SOURCE_OK;
}
