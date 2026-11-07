#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include "extension/route.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct gt_ssh_route {
    gt_bridge_stream_t *stream;
};

static bool route_clock(void *context, uint64_t *out) {
    (void)context;
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 ||
        (uint64_t)t.tv_sec > (UINT64_MAX - 999) / 1000) {
        return false;
    }
    *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return true;
}

static gt_ssh_route_phase_t phase(gt_bridge_route_phase_t value) {
    switch (value) {
        case GT_BRIDGE_ROUTE_AUTH:
            return GT_SSH_ROUTE_AUTHENTICATING;
        case GT_BRIDGE_ROUTE_FORWARD:
            return GT_SSH_ROUTE_FORWARDING;
        case GT_BRIDGE_ROUTE_READY:
            return GT_SSH_ROUTE_READY;
        case GT_BRIDGE_ROUTE_STOPPED:
        case GT_BRIDGE_ROUTE_CLOSED:
            return GT_SSH_ROUTE_CANCELLED;
        default:
            return GT_SSH_ROUTE_FAILED;
    }
}

gt_status_t gt_ssh_route_open(gt_ssh_t **owned,
                              const gt_ssh_route_hop_t *hops,
                              uint32_t count,
                              uint32_t total_ms,
                              gt_ssh_route_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out || !owned || !hops || !count || count > GT_SSH_ROUTE_MAX_HOPS || total_ms > 60000) {
        return GT_ERR_INVALID_ARG;
    }
    gt_bridge_route_hop_t configs[GT_SSH_ROUTE_MAX_HOPS] = {0};
    for (uint32_t i = 0; i < count; i++) {
        const gt_ssh_auth_options_t *o = &hops[i].authentication;
        configs[i].connector =
            (gt_ssh_connector_config_t){.family = o->family,
                                        .scope = o->scope,
                                        .timeout_ms = o->timeout_ms ? o->timeout_ms : 60000,
                                        .clock = o->clock ? o->clock : route_clock,
                                        .clock_context = o->clock_context,
                                        .defer_credentials = o->defer_credentials};
        memcpy(configs[i].connector.address, o->address, 16);
        configs[i].via_host = hops[i].via_host;
    }
    gt_ssh_route_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return GT_ERR_IO;
    }
    gt_bridge_route_options_t options = {total_ms ? total_ms : 60000, {5000, 262144}};
    gt_status_t status =
        gt__bridge_route_open_session(owned, configs, count, &options, NULL, &r->stream);
    if (status != GT_OK) {
        free(r);
        return status;
    }
    *out = r;
    return GT_OK;
}

gt_ssh_route_phase_t gt_ssh_route_step(gt_ssh_route_t *r) {
    return r ? phase(gt__bridge_route_step(r->stream, 0)) : GT_SSH_ROUTE_FAILED;
}

bool gt_ssh_route_info(const gt_ssh_route_t *r, gt_ssh_route_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!r || !out) {
        return false;
    }
    gt_bridge_route_info_t info;
    if (!gt__bridge_route_info(r->stream, &info) || info.hop >= info.count) {
        return false;
    }
    const gt_ssh_connector_info_t *a = &info.authentication[info.hop];
    *out = (gt_ssh_route_info_t){.phase = phase(info.phase),
                                 .error = info.error,
                                 .hop = (uint32_t)info.hop,
                                 .count = (uint32_t)info.count,
                                 .deadline = info.deadline || a->deadline,
                                 .authentication = {.phase = (gt_ssh_auth_phase_t)a->phase,
                                                    .error = a->error,
                                                    .host = (gt_ssh_auth_host_state_t)a->host,
                                                    .deadline = a->deadline,
                                                    .auth_methods = a->auth_methods}};
    memcpy(out->authentication.key_type, a->key_type, sizeof(a->key_type));
    memcpy(out->authentication.fingerprint, a->fingerprint, sizeof(a->fingerprint));
    return true;
}

gt_status_t gt_ssh_route_decide(gt_ssh_route_t *r, gt_hostkey_decision_t decision) {
    return r ? gt__bridge_route_decide(r->stream, decision) : GT_ERR_INVALID_ARG;
}

gt_status_t gt_ssh_route_supply(gt_ssh_route_t *r, gt_ssh_t **material) {
    return r ? gt__bridge_route_supply(r->stream, material) : GT_ERR_INVALID_ARG;
}

gt_status_t gt_ssh_route_borrow(gt_ssh_route_t *r, gt_ssh_t **out) {
    if (out) {
        *out = NULL;
    }
    return r ? gt__bridge_route_borrow_session(r->stream, out) : GT_ERR_INVALID_ARG;
}

gt_status_t gt_ssh_route_dispose(gt_ssh_route_t **owned, bool *released) {
    if (released) {
        *released = false;
    }
    if (!owned || !released) {
        return GT_ERR_INVALID_ARG;
    }
    if (!*owned) {
        *released = true;
        return GT_OK;
    }
    gt_ssh_route_t *r = *owned;
    gt_bridge_close_t result = gt__bridge_stream_close(r->stream, 0);
    if (result == GT_BRIDGE_CLOSE_ERROR) {
        return GT_ERR_IO;
    }
    if (result != GT_BRIDGE_RELEASED) {
        return GT_OK;
    }
    gt_status_t status = gt__bridge_stream_destroy(r->stream);
    if (status != GT_OK) {
        return status;
    }
    free(r);
    *owned = NULL;
    *released = true;
    return GT_OK;
}
