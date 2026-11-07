#include "ghosterm.h"
#include "extension/route.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL route API:%d: %s\n", __LINE__, #x);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static int marker;
static gt_bridge_route_info_t state;
static gt_bridge_close_t closing;
static bool open_error;
static unsigned destroyed;

gt_status_t gt__bridge_route_open_session(gt_ssh_t **owned,
                                          const gt_bridge_route_hop_t *hops,
                                          size_t count,
                                          const gt_bridge_route_options_t *options,
                                          const gt_allocator_t *a,
                                          gt_bridge_stream_t **out) {
    CHECK(!a && count == 2 && owned[0] && owned[1]);
    CHECK(options->total_ms == 60000 && options->stream.admission_ms &&
          options->stream.stderr_limit);
    CHECK(hops[0].connector.family == 6 && hops[0].connector.address[15] == 1 &&
          hops[0].connector.scope == 9);
    CHECK(hops[0].connector.timeout_ms == 60000 && hops[1].connector.timeout_ms == 123);
    CHECK(hops[0].connector.defer_credentials && !hops[1].connector.defer_credentials);
    CHECK(hops[0].connector.clock == hops[1].connector.clock && !hops[0].via_host &&
          !strcmp(hops[1].via_host, "inner.example"));
    uint64_t now;
    CHECK(hops[0].connector.clock(NULL, &now));
    if (open_error) {
        return GT_ERR_UNSUPPORTED;
    }
    owned[0] = owned[1] = NULL;
    *out = (void *)&marker;
    return GT_OK;
}

gt_bridge_route_phase_t gt__bridge_route_step(gt_bridge_stream_t *s, int ms) {
    CHECK(s == (void *)&marker && ms == 0);
    return state.phase;
}

bool gt__bridge_route_info(const gt_bridge_stream_t *s, gt_bridge_route_info_t *out) {
    CHECK(s == (void *)&marker);
    *out = state;
    return true;
}

gt_status_t gt__bridge_route_decide(gt_bridge_stream_t *s, gt_hostkey_decision_t d) {
    CHECK(s == (void *)&marker && d == GT_HOSTKEY_REJECT);
    return GT_ERR_HOST_KEY;
}

gt_status_t gt__bridge_route_supply(gt_bridge_stream_t *s, gt_ssh_t **m) {
    CHECK(s == (void *)&marker && m && *m);
    *m = NULL;
    return GT_OK;
}

gt_status_t gt__bridge_route_borrow_session(gt_bridge_stream_t *s, gt_ssh_t **out) {
    CHECK(s == (void *)&marker);
    *out = (void *)&marker;
    return GT_OK;
}

gt_bridge_close_t gt__bridge_stream_close(gt_bridge_stream_t *s, int ms) {
    CHECK(s == (void *)&marker && ms == 0);
    return closing;
}

gt_status_t gt__bridge_stream_destroy(gt_bridge_stream_t *s) {
    CHECK(s == (void *)&marker);
    destroyed++;
    return GT_OK;
}

int main(void) {
    gt_ssh_t *owned[2] = {(void *)&marker, (void *)&marker};
    gt_ssh_route_t *r = NULL;
    gt_ssh_route_hop_t hops[2] = {0};
    hops[0].authentication.family = 6;
    hops[0].authentication.address[15] = 1;
    hops[0].authentication.scope = 9;
    hops[0].authentication.defer_credentials = true;
    hops[1].authentication.timeout_ms = 123;
    hops[1].via_host = "inner.example";
    CHECK(gt_ssh_route_open(owned, hops, 2, 0, &r) == GT_OK && r && !owned[0] && !owned[1]);
    const gt_ssh_route_phase_t phases[] = {GT_SSH_ROUTE_AUTHENTICATING,
                                           GT_SSH_ROUTE_FORWARDING,
                                           GT_SSH_ROUTE_FAILED,
                                           GT_SSH_ROUTE_READY,
                                           GT_SSH_ROUTE_FAILED,
                                           GT_SSH_ROUTE_CANCELLED,
                                           GT_SSH_ROUTE_CANCELLED};
    for (size_t i = 0; i < sizeof(phases) / sizeof(*phases); i++) {
        state.phase = (gt_bridge_route_phase_t)i;
        CHECK(gt_ssh_route_step(r) == phases[i]);
    }
    state = (gt_bridge_route_info_t){.phase = GT_BRIDGE_ROUTE_AUTH,
                                     .error = GT_ERR_AUTH,
                                     .hop = 1,
                                     .count = 2,
                                     .deadline = true};
    state.authentication[1] = (gt_ssh_connector_info_t){.phase = GT_SSH_CONNECT_CREDENTIALS,
                                                        .host = GT_SSH_HOST_KNOWN,
                                                        .error = GT_ERR_AUTH,
                                                        .auth_methods = 3};
    strcpy(state.authentication[1].key_type, "ssh-ed25519");
    strcpy(state.authentication[1].fingerprint, "SHA256:fixture");
    gt_ssh_route_info_t info;
    CHECK(gt_ssh_route_info(r, &info) && info.phase == GT_SSH_ROUTE_AUTHENTICATING &&
          info.error == GT_ERR_AUTH && info.hop == 1 && info.count == 2 && info.deadline);
    CHECK(info.authentication.phase == GT_SSH_AUTH_PHASE_CREDENTIALS &&
          info.authentication.host == GT_SSH_AUTH_HOST_KNOWN &&
          info.authentication.error == GT_ERR_AUTH && info.authentication.auth_methods == 3 &&
          !strcmp(info.authentication.key_type, "ssh-ed25519") &&
          !strcmp(info.authentication.fingerprint, "SHA256:fixture"));
    CHECK(gt_ssh_route_decide(r, GT_HOSTKEY_REJECT) == GT_ERR_HOST_KEY);
    gt_ssh_t *material = (void *)&marker;
    CHECK(gt_ssh_route_supply(r, &material) == GT_OK && !material);
    CHECK(gt_ssh_route_borrow(r, &material) == GT_OK && material == (void *)&marker);
    gt_ssh_route_t *saved = r;
    bool released = true;
    closing = GT_BRIDGE_DRAINING;
    CHECK(gt_ssh_route_dispose(&r, &released) == GT_OK && !released && r == saved && !destroyed);
    closing = GT_BRIDGE_CLOSE_ERROR;
    CHECK(gt_ssh_route_dispose(&r, &released) == GT_ERR_IO && !released && r == saved &&
          !destroyed);
    closing = GT_BRIDGE_RELEASED;
    CHECK(gt_ssh_route_dispose(&r, &released) == GT_OK && released && !r && destroyed == 1);
    CHECK(gt_ssh_route_dispose(&r, &released) == GT_OK && released);
    open_error = true;
    owned[0] = owned[1] = (void *)&marker;
    CHECK(gt_ssh_route_open(owned, hops, 2, 0, &r) == GT_ERR_UNSUPPORTED && !r && owned[0] &&
          owned[1]);
    CHECK(gt_ssh_route_open(owned, hops, 9, 0, &r) == GT_ERR_INVALID_ARG && !r);
    CHECK(gt_ssh_route_step(NULL) == GT_SSH_ROUTE_FAILED && !gt_ssh_route_info(NULL, &info));
    CHECK(gt_ssh_route_decide(NULL, GT_HOSTKEY_ACCEPT) == GT_ERR_INVALID_ARG &&
          gt_ssh_route_supply(NULL, &material) == GT_ERR_INVALID_ARG);
    material = (void *)&marker;
    CHECK(gt_ssh_route_borrow(NULL, &material) == GT_ERR_INVALID_ARG && !material);
    CHECK(gt_ssh_route_dispose(NULL, &released) == GT_ERR_INVALID_ARG && !released);
    puts("PASS route API ownership, translation and pending/error disposal");
    return 0;
}
