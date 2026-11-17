#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    gt_ext_mux_t *mux[2];
    gt_ext_session_t *session[2][8];
    gt_ext_lease_t lease[2][8];
    gt_ext_resource_store_t *store[2];
} fixture_t;

static gt_ext_mux_config_t mux_config(void) {
    gt_ext_mux_config_t c = {.frame_bytes = 4096, .receive_frames = 64, .instances = 8};
    memset(c.connection, 'a', 32);
    return c;
}

static void identity(unsigned slot, char id[33]) {
    CHECK(snprintf(id, 33, "%032x", slot + 1) == 32);
}

static gt_ext_channel_t *channel(int fd) {
    gt_ext_channel_t *c = NULL;
    CHECK(gt_ext_channel_open_fd(fd, fd, &c) == GT_OK && c);
    return c;
}

static gt_ext_session_config_t session_config(void) {
    gt_ext_session_config_t c = {.required_count = 1,
                                 .available_count = 1,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 60000,
                                 .peer = {GT_EXT_PEER_LOCAL, "owned-public-mux"},
                                 .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(c.plugin_id, "org.ghosterm.mux");
    strcpy(c.plugin_version, "0.1.0");
    memset(c.client_digest, 'b', 64);
    memset(c.executor_digest, 'c', 64);
    strcpy(c.host_version, "0.1.0");
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.required[0], "session.rpc");
    strcpy(c.available[0], "session.rpc");
    return c;
}

static void open_fixture(fixture_t *f, bool resources) {
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_mux_config_t config = mux_config();
    gt_ext_rpc_method_t method = {.receiver = GT_EXT_EXECUTOR,
                                  .input_shape = GT_EXT_JSON_OBJECT,
                                  .output_shape = GT_EXT_JSON_OBJECT};
    strcpy(method.method, "fixture.echo");
    strcpy(method.capability, "session.rpc");
    for (unsigned side = 0; side < 2; side++) {
        gt_ext_channel_t *physical = channel(fd[side]);
        gt_ext_mux_config_t invalid = config;
        invalid.instances = 9;
        CHECK(gt_ext_mux_open(&physical, &invalid, &f->mux[side]) == GT_ERR_INVALID_ARG &&
              physical && !f->mux[side]);
        CHECK(gt_ext_mux_open(&physical, &config, &f->mux[side]) == GT_OK && !physical);
        gt_ext_transport_process_info_t process;
        CHECK(gt_ext_mux_process_info(f->mux[side], &process) == GT_ERR_UNSUPPORTED);
        for (unsigned slot = 0; slot < 8; slot++) {
            char id[33];
            identity(slot, id);
            gt_ext_channel_t *logical = NULL;
            CHECK(gt_ext_mux_open_channel(f->mux[side], id, slot + 1, &logical) == GT_OK &&
                  logical);
            gt_ext_mux_t *nested = NULL;
            CHECK(gt_ext_mux_open(&logical, &config, &nested) == GT_ERR_UNSUPPORTED && logical &&
                  !nested);
            CHECK(gt_ext_channel_decide(logical, 0, 1, GT_HOSTKEY_ACCEPT) == GT_ERR_UNSUPPORTED);
            gt_ext_session_config_t c = session_config();
            if (resources) {
                c.features |= GT_EXT_FEATURE_RESOURCES;
                c.required_count = c.available_count = 3;
                strcpy(c.required[1], "resource.receive");
                strcpy(c.available[1], c.required[1]);
                strcpy(c.required[2], "file.readScoped");
                strcpy(c.available[2], c.required[2]);
                CHECK(gt_ext_session_open_resources(&logical,
                                                    side == 0 ? GT_EXT_HOST : GT_EXT_EXECUTOR,
                                                    &c,
                                                    &method,
                                                    1,
                                                    f->store[side],
                                                    262144,
                                                    &f->session[side][slot]) == GT_OK &&
                      !logical);
            } else {
                CHECK(gt_ext_session_open_rpc(&logical,
                                              side == 0 ? GT_EXT_HOST : GT_EXT_EXECUTOR,
                                              &c,
                                              &method,
                                              1,
                                              &f->session[side][slot]) == GT_OK &&
                      !logical);
            }
        }
    }
}

static void pump(fixture_t *f) {
    for (unsigned side = 0; side < 2; side++) {
        for (unsigned slot = 0; slot < 8; slot++) {
            if (f->session[side][slot]) {
                gt_ext_session_step(f->session[side][slot], 0);
            }
        }
        CHECK(gt_ext_mux_step(f->mux[side], 0) == GT_OK);
    }
}

static void activate(fixture_t *f) {
    unsigned active = 0;
    for (unsigned step = 0; step < 10000 && active != 16; step++) {
        pump(f);
        active = 0;
        for (unsigned slot = 0; slot < 8; slot++) {
            gt_ext_session_info_t state;
            CHECK(gt_ext_session_info(f->session[0][slot], &state));
            if (state.end != GT_EXT_END_NONE) {
                fprintf(stderr,
                        "slot %u phase %u end %u error %u\n",
                        slot,
                        state.phase,
                        state.end,
                        state.error);
            }
            CHECK(state.end == GT_EXT_END_NONE);
            if (state.phase == GT_EXT_SESSION_CONSENT) {
                CHECK(state.decision == GT_EXT_DECISION_UNKNOWN);
                CHECK(gt_ext_session_decide(f->session[0][slot],
                                            state.consent_token,
                                            GT_EXT_DECISION_ALLOW) == GT_OK);
            }
            for (unsigned side = 0; side < 2; side++) {
                CHECK(gt_ext_session_info(f->session[side][slot], &state));
                active += state.phase == GT_EXT_SESSION_ACTIVE;
            }
        }
    }
    CHECK(active == 16);
    for (unsigned side = 0; side < 2; side++) {
        for (unsigned slot = 0; slot < 8; slot++) {
            CHECK(gt_ext_session_lease(f->session[side][slot], &f->lease[side][slot]));
            CHECK(f->lease[side][slot].epoch == slot + 1);
        }
    }
}

static void cleanup(fixture_t *f) {
    for (unsigned side = 0; side < 2; side++) {
        CHECK(gt_ext_mux_close(f->mux[side], 0) == GT_EXT_DRAINING);
        CHECK(gt_ext_mux_destroy(&f->mux[side]) == GT_ERR_INVALID_ARG && f->mux[side]);
        for (unsigned slot = 0; slot < 8; slot++) {
            if (f->session[side][slot]) {
                CHECK(gt_ext_session_close(f->session[side][slot], 0) == GT_EXT_RELEASED);
                CHECK(gt_ext_session_destroy(&f->session[side][slot]) == GT_OK &&
                      !f->session[side][slot]);
            }
        }
        gt_ext_mux_info_t state;
        CHECK(gt_ext_mux_info(f->mux[side], &state) && state.instances == 0);
        CHECK(gt_ext_mux_close(f->mux[side], 0) == GT_EXT_RELEASED);
        CHECK(gt_ext_mux_close(f->mux[side], 0) == GT_EXT_RELEASED);
        CHECK(gt_ext_mux_destroy(&f->mux[side]) == GT_OK && !f->mux[side]);
    }
}

static void roundtrip(void) {
    fixture_t f = {0};
    open_fixture(&f, false);
    activate(&f);
    uint64_t handles[8];
    for (unsigned slot = 0; slot < 8; slot++) {
        char input[32];
        int size = snprintf(input, sizeof(input), "{\"slot\":%u}", slot);
        CHECK(gt_ext_session_request(f.session[0][slot],
                                     &f.lease[0][slot],
                                     "fixture.echo",
                                     input,
                                     (size_t)size,
                                     1000,
                                     &handles[slot]) == GT_EXT_RPC_OK);
    }
    bool replied[8] = {0}, completed[8] = {0};
    unsigned total = 0;
    for (unsigned step = 0; step < 10000 && total != 8; step++) {
        pump(&f);
        for (unsigned slot = 0; slot < 8; slot++) {
            if (!replied[slot]) {
                gt_ext_rpc_request_t request;
                gt_ext_rpc_status_t state =
                    gt_ext_session_take_request(f.session[1][slot], &f.lease[1][slot], &request);
                CHECK(state == GT_EXT_RPC_OK || state == GT_EXT_RPC_WAIT);
                if (state == GT_EXT_RPC_OK) {
                    char expected[32];
                    int length = snprintf(expected, sizeof(expected), "{\"slot\":%u}", slot);
                    CHECK(request.input_size == (uint32_t)length &&
                          !memcmp(request.input, expected, (size_t)length));
                    char outcome[96];
                    length = snprintf(
                        outcome, sizeof(outcome), "{\"kind\":\"success\",\"value\":%s}", expected);
                    CHECK(gt_ext_session_respond(f.session[1][slot],
                                                 &f.lease[1][slot],
                                                 request.ticket,
                                                 outcome,
                                                 (size_t)length) == GT_EXT_RPC_OK);
                    replied[slot] = true;
                }
            }
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(
                      f.session[0][slot], &f.lease[0][slot], handles[slot], &result) ==
                  GT_EXT_RPC_OK);
            if (result.confirmed && !completed[slot]) {
                CHECK(result.state == GT_EXT_RPC_SUCCEEDED);
                completed[slot] = true;
                total++;
            }
        }
    }
    CHECK(total == 8);
    gt_ext_channel_info_t physical;
    CHECK(gt_ext_mux_transport_info(f.mux[0], &physical) == GT_OK && physical.accepted &&
          physical.sent && physical.received);
    gt_ext_transport_process_info_t process;
    CHECK(gt_ext_session_process_info(f.session[0][0], &process) == GT_ERR_UNSUPPORTED);
    bool released = false;
    for (unsigned step = 0; step < 1000; step++) {
        if (gt_ext_session_close(f.session[0][3], 0) == GT_EXT_RELEASED) {
            released = true;
            break;
        }
        pump(&f);
    }
    CHECK(released);
    CHECK(gt_ext_session_destroy(&f.session[0][3]) == GT_OK);
    for (unsigned step = 0; step < 100; step++) {
        pump(&f);
    }
    gt_ext_session_info_t session;
    CHECK(gt_ext_session_info(f.session[1][3], &session) && session.end == GT_EXT_END_PEER);
    char id[33];
    identity(3, id);
    gt_ext_mux_scope_info_t scope;
    CHECK(gt_ext_mux_scope_info(f.mux[0], id, 4, &scope) == GT_OK && scope.released &&
          scope.close_sent);
    CHECK(gt_ext_mux_scope_info(f.mux[0], id, 99, &scope) == GT_ERR_NOT_FOUND && !scope.epoch);
    for (unsigned slot = 0; slot < 8; slot++) {
        if (slot != 3) {
            CHECK(gt_ext_session_info(f.session[0][slot], &session) &&
                  session.phase == GT_EXT_SESSION_ACTIVE);
        }
    }
    cleanup(&f);
}

static void capacity(void) {
    fixture_t f = {0};
    open_fixture(&f, false);
    gt_ext_channel_t *fillers[GT_EXT_CHANNEL_LIMIT - 2] = {0};
    int peers[GT_EXT_CHANNEL_LIMIT - 2];
    for (unsigned i = 0; i < GT_EXT_CHANNEL_LIMIT - 2; i++) {
        int fd[2];
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
        fillers[i] = channel(fd[0]);
        peers[i] = fd[1];
    }
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_channel_t *extra = NULL;
    CHECK(gt_ext_channel_open_fd(fd[0], fd[0], &extra) == GT_ERR_IO && !extra);
    cleanup(&f);
    CHECK(gt_ext_channel_open_fd(fd[0], fd[0], &extra) == GT_OK);
    CHECK(gt_ext_channel_close(extra, 0) == GT_EXT_RELEASED &&
          gt_ext_channel_destroy(&extra) == GT_OK);
    CHECK(!close(fd[1]));
    for (unsigned i = 0; i < GT_EXT_CHANNEL_LIMIT - 2; i++) {
        CHECK(gt_ext_channel_close(fillers[i], 0) == GT_EXT_RELEASED);
        CHECK(gt_ext_channel_destroy(&fillers[i]) == GT_OK);
        CHECK(!close(peers[i]));
    }
}

static void source_borrow(const char *temporary_root) {
    fixture_t f = {0};
    char directories[2][4096];
    int roots[2];
    for (unsigned side = 0; side < 2; side++) {
        CHECK(snprintf(directories[side],
                       sizeof(directories[side]),
                       "%s/public-mux-%u-XXXXXX",
                       temporary_root,
                       side) > 0);
        CHECK(mkdtemp(directories[side]));
        roots[side] = open(directories[side], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(roots[side] >= 0);
        gt_ext_resource_store_config_t config = {roots[side], 4 * 1024 * 1024, 262144, 32};
        CHECK(gt_ext_resource_store_open(&config, &f.store[side]) == GT_EXT_RESOURCE_OK);
    }
    int fd = openat(roots[0], "source.txt", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3 && !close(fd));
    open_fixture(&f, true);
    activate(&f);
    gt_ext_source_config_t config = {roots[0], "source.txt", "text/plain", 1024};
    gt_ext_source_t *source = NULL;
    CHECK(gt_ext_source_open(f.session[0][0], &f.lease[0][0], &config, &source) ==
          GT_EXT_SOURCE_OK);
    CHECK(gt_ext_mux_close(f.mux[0], 0) == GT_EXT_DRAINING);
    CHECK(gt_ext_session_close(f.session[0][0], 0) == GT_EXT_DRAINING);
    CHECK(gt_ext_session_destroy(&f.session[0][0]) == GT_ERR_INVALID_ARG && f.session[0][0]);
    CHECK(gt_ext_mux_destroy(&f.mux[0]) == GT_ERR_INVALID_ARG && f.mux[0]);
    gt_ext_mux_info_t state;
    CHECK(gt_ext_mux_info(f.mux[0], &state) && state.instances == 8 && !state.released);
    gt_ext_resource_usage_t usage;
    CHECK(gt_ext_resource_store_usage(f.store[0], &usage) && usage.sessions == 8);
    CHECK(gt_ext_resource_store_destroy(&f.store[0]) == GT_EXT_RESOURCE_WAIT && f.store[0]);
    CHECK(gt_ext_source_close(source) == GT_EXT_SOURCE_OK);
    CHECK(gt_ext_source_destroy(&source) == GT_EXT_SOURCE_OK && !source);
    cleanup(&f);
    for (unsigned side = 0; side < 2; side++) {
        CHECK(gt_ext_resource_store_destroy(&f.store[side]) == GT_EXT_RESOURCE_OK &&
              !f.store[side]);
        if (!side) {
            CHECK(!unlinkat(roots[side], "source.txt", 0));
        }
        CHECK(!close(roots[side]) && !rmdir(directories[side]));
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    if (!strcmp(argv[1], "roundtrip")) {
        roundtrip();
    } else if (!strcmp(argv[1], "capacity")) {
        capacity();
    } else if (!strcmp(argv[1], "source-borrow")) {
        CHECK(argc == 3);
        source_borrow(argv[2]);
    } else {
        CHECK(false);
    }
    printf("PASS public mux %s\n", argv[1]);
    return 0;
}
