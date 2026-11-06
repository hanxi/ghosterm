#include "extension/stream.h"

static gt_bridge_io_t error(gt_status_t status) {
    return (gt_bridge_io_t){GT_BRIDGE_ERROR, 0, status};
}

gt_status_t gt__bridge_stream_init(gt_bridge_stream_t *s, const gt_bridge_stream_ops_t *ops) {
    if (!s || !ops || !ops->read || !ops->write || !ops->wake || !ops->close_step ||
        !ops->destroy) {
        return GT_ERR_INVALID_ARG;
    }
    s->ops = ops;
    atomic_init(&s->stopping, false);
    s->closed = false;
    return GT_OK;
}

static gt_bridge_io_t checked(gt_bridge_io_t r, size_t n, bool writing) {
    if (r.state < GT_BRIDGE_DATA || r.state > GT_BRIDGE_ERROR ||
        (r.state == GT_BRIDGE_DATA && (!r.count || r.count > n || r.error != GT_OK)) ||
        (r.state != GT_BRIDGE_DATA && r.count) ||
        (r.state == GT_BRIDGE_ERROR && r.error == GT_OK) ||
        (r.state == GT_BRIDGE_WAIT && r.error != GT_OK) ||
        (r.state == GT_BRIDGE_EOF && (writing || r.error != GT_OK))) {
        return error(GT_ERR_INTERNAL);
    }
    return r;
}

static bool stopped(gt_bridge_stream_t *s) {
    return s->closed || atomic_load_explicit(&s->stopping, memory_order_acquire);
}

gt_bridge_io_t gt__bridge_stream_read(gt_bridge_stream_t *s, void *p, size_t n, int ms) {
    if (!s || !p || !n || ms < 0) {
        return error(GT_ERR_INVALID_ARG);
    }
    if (stopped(s)) {
        return (gt_bridge_io_t){GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED};
    }
    if (n > GT_BRIDGE_IO_QUANTUM) {
        n = GT_BRIDGE_IO_QUANTUM;
    }
    return checked(s->ops->read(s, p, n, ms), n, false);
}

gt_bridge_io_t gt__bridge_stream_write(gt_bridge_stream_t *s, const void *p, size_t n, int ms) {
    if (!s || !p || !n || ms < 0) {
        return error(GT_ERR_INVALID_ARG);
    }
    if (stopped(s)) {
        return (gt_bridge_io_t){GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED};
    }
    if (n > GT_BRIDGE_IO_QUANTUM) {
        n = GT_BRIDGE_IO_QUANTUM;
    }
    return checked(s->ops->write(s, p, n, ms), n, true);
}

void gt__bridge_stream_stop(gt_bridge_stream_t *s) {
    if (s && !atomic_exchange_explicit(&s->stopping, true, memory_order_acq_rel)) {
        s->ops->wake(s);
    }
}

gt_bridge_close_t gt__bridge_stream_close(gt_bridge_stream_t *s, int ms) {
    if (!s || ms < 0) {
        return GT_BRIDGE_CLOSE_ERROR;
    }
    if (s->closed) {
        return GT_BRIDGE_RELEASED;
    }
    gt__bridge_stream_stop(s);
    gt_bridge_close_t r = s->ops->close_step(s, ms);
    if (r == GT_BRIDGE_RELEASED) {
        s->closed = true;
    } else if (r != GT_BRIDGE_DRAINING && r != GT_BRIDGE_CLOSE_ERROR) {
        return GT_BRIDGE_CLOSE_ERROR;
    }
    return r;
}

gt_status_t gt__bridge_stream_destroy(gt_bridge_stream_t *s) {
    if (!s || !s->closed) {
        return GT_ERR_INVALID_ARG;
    }
    s->ops->destroy(s);
    return GT_OK;
}
