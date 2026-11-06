#include "extension/memory.h"
#include "extension/frame.h"
#include <stdlib.h>
#include <string.h>

struct gt_bridge_decoder {
    gt_allocator_t allocator;
    uint8_t *bytes;
    size_t limit, header_used, length, used;
    uint8_t header[4];
    bool ready, ended;
    gt_status_t error;
};

struct gt_bridge_writer {
    gt_allocator_t allocator;
    uint8_t *bytes;
    size_t limit, length, sent;
    gt_bridge_stream_t *stream;
    gt_bridge_io_t failure;
};

static void *allocate(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}

static void release(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)n;
    free(p);
}

static gt_status_t parameters(size_t limit, const gt_allocator_t *input, gt_allocator_t *out) {
    if (!limit || limit > GT_BRIDGE_FRAME_MAX || (input && (!input->allocate || !input->release))) {
        return GT_ERR_INVALID_ARG;
    }
    *out = input ? *input : (gt_allocator_t){NULL, allocate, release};
    return GT_OK;
}

gt_status_t
gt__bridge_decoder_new(size_t limit, const gt_allocator_t *input, gt_bridge_decoder_t **out) {
    if (!out) {
        return GT_ERR_INVALID_ARG;
    }
    *out = NULL;
    gt_allocator_t a;
    gt_status_t status = parameters(limit, input, &a);
    if (status != GT_OK) {
        return status;
    }
    gt_bridge_decoder_t *d = gt__plugin_memory_alloc(&a, sizeof(*d));
    if (!d) {
        return GT_ERR_IO;
    }
    *d = (gt_bridge_decoder_t){.allocator = a, .limit = limit};
    d->bytes = gt__plugin_memory_alloc(&a, limit);
    if (!d->bytes) {
        gt__plugin_memory_free(&a, d, sizeof(*d));
        return GT_ERR_IO;
    }
    *out = d;
    return GT_OK;
}

gt_status_t
gt__bridge_decoder_feed(gt_bridge_decoder_t *d, const void *input, size_t n, size_t *consumed) {
    if (consumed) {
        *consumed = 0;
    }
    if (!d || !consumed || (!input && n)) {
        return GT_ERR_INVALID_ARG;
    }
    if (d->error != GT_OK) {
        return d->error;
    }
    if (d->ended || d->ready) {
        return GT_ERR_INVALID_ARG;
    }
    const uint8_t *p = input;
    while (*consumed < n) {
        if (d->header_used < 4) {
            d->header[d->header_used++] = p[(*consumed)++];
            if (d->header_used < 4) {
                continue;
            }
            uint32_t length = ((uint32_t)d->header[0] << 24) | ((uint32_t)d->header[1] << 16) |
                              ((uint32_t)d->header[2] << 8) | (uint32_t)d->header[3];
            if (!length || length > d->limit) {
                d->error = GT_ERR_PROTOCOL;
                return d->error;
            }
            d->length = length;
        }
        size_t take = d->length - d->used;
        if (take > n - *consumed) {
            take = n - *consumed;
        }
        if (take) {
            memcpy(d->bytes + d->used, p + *consumed, take);
        }
        d->used += take;
        *consumed += take;
        if (d->used == d->length) {
            d->ready = true;
            break;
        }
    }
    return GT_OK;
}

bool gt__bridge_decoder_view(const gt_bridge_decoder_t *d, const uint8_t **p, size_t *n) {
    if (p) {
        *p = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!d || !p || !n || !d->ready || d->error != GT_OK) {
        return false;
    }
    *p = d->bytes;
    *n = d->length;
    return true;
}

gt_status_t gt__bridge_decoder_consume(gt_bridge_decoder_t *d) {
    if (!d || !d->ready) {
        return GT_ERR_INVALID_ARG;
    }
    d->ready = false;
    d->header_used = d->length = d->used = 0;
    return GT_OK;
}

gt_status_t gt__bridge_decoder_finish(gt_bridge_decoder_t *d) {
    if (!d) {
        return GT_ERR_INVALID_ARG;
    }
    d->ended = true;
    if (!d->ready && d->header_used) {
        d->error = GT_ERR_PROTOCOL;
    }
    return d->error;
}

void gt__bridge_decoder_free(gt_bridge_decoder_t *d) {
    if (!d) {
        return;
    }
    gt_allocator_t a = d->allocator;
    gt__plugin_memory_free(&a, d->bytes, d->limit);
    gt__plugin_memory_free(&a, d, sizeof(*d));
}

gt_status_t
gt__bridge_writer_new(size_t limit, const gt_allocator_t *input, gt_bridge_writer_t **out) {
    if (!out) {
        return GT_ERR_INVALID_ARG;
    }
    *out = NULL;
    gt_allocator_t a;
    gt_status_t status = parameters(limit, input, &a);
    if (status != GT_OK) {
        return status;
    }
    gt_bridge_writer_t *w = gt__plugin_memory_alloc(&a, sizeof(*w));
    if (!w) {
        return GT_ERR_IO;
    }
    *w =
        (gt_bridge_writer_t){.allocator = a, .limit = limit, .failure = {GT_BRIDGE_WAIT, 0, GT_OK}};
    w->bytes = gt__plugin_memory_alloc(&a, limit + 4);
    if (!w->bytes) {
        gt__plugin_memory_free(&a, w, sizeof(*w));
        return GT_ERR_IO;
    }
    *out = w;
    return GT_OK;
}

gt_status_t gt__bridge_writer_queue(gt_bridge_writer_t *w, const void *p, size_t n) {
    if (!w || !p || !n || n > w->limit || w->length != w->sent ||
        w->failure.state != GT_BRIDGE_WAIT) {
        return GT_ERR_INVALID_ARG;
    }
    w->bytes[0] = (uint8_t)(n >> 24);
    w->bytes[1] = (uint8_t)(n >> 16);
    w->bytes[2] = (uint8_t)(n >> 8);
    w->bytes[3] = (uint8_t)n;
    memcpy(w->bytes + 4, p, n);
    w->length = n + 4;
    w->sent = 0;
    w->stream = NULL;
    return GT_OK;
}

size_t gt__bridge_writer_pending(const gt_bridge_writer_t *w) {
    return w ? w->length - w->sent : 0;
}

gt_bridge_io_t gt__bridge_writer_step(gt_bridge_writer_t *w, gt_bridge_stream_t *s, int ms) {
    if (!w || !s || ms < 0 || !gt__bridge_writer_pending(w)) {
        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, GT_ERR_INVALID_ARG};
    }
    if (w->failure.state != GT_BRIDGE_WAIT) {
        return w->failure;
    }
    if (w->stream && w->stream != s) {
        return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, GT_ERR_INVALID_ARG};
    }
    w->stream = s;
    gt_bridge_io_t r = gt__bridge_stream_write(s, w->bytes + w->sent, w->length - w->sent, ms);
    if (r.state == GT_BRIDGE_DATA) {
        w->sent += r.count;
    } else if (r.state != GT_BRIDGE_WAIT) {
        w->failure = r;
    }
    return r;
}

void gt__bridge_writer_free(gt_bridge_writer_t *w) {
    if (!w) {
        return;
    }
    /* Provider must have completed writes or been closed before freeing pending
   * buffers: a future SSH provider can retain an EAGAIN continuation address. */
    gt_allocator_t a = w->allocator;
    gt__plugin_memory_free(&a, w->bytes, w->limit + 4);
    gt__plugin_memory_free(&a, w, sizeof(*w));
}
