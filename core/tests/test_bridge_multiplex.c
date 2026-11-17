#define _POSIX_C_SOURCE 200809L
#include "extension/multiplex.h"
#include "extension/session.h"
#include "extension/memory.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #value);                       \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    gt_bridge_mux_t *mux[2];
    gt_bridge_stream_t *branch[2][8];
    gt_bridge_session_t *session[2][8];
    gt_bridge_lease_t lease[2][8];
    int fd[2];
    uint64_t now;
} fixture_t;

static gt_bridge_mux_config_t configuration(unsigned queue) {
    gt_bridge_mux_config_t config = {.frame_bytes = 4096, .receive_frames = queue, .instances = 8};
    memset(config.connection, 'a', 32);
    return config;
}

static void id_for(unsigned slot, char id[33]) {
    CHECK(snprintf(id, 33, "%032x", slot + 1) == 32);
}

static bool clock_now(void *context, uint64_t *out) {
    *out = ((fixture_t *)context)->now;
    return true;
}

static bool json_value(void *context, const uint8_t *bytes, size_t size) {
    (void)context;
    return size > 1 && bytes[0] == '{' && bytes[size - 1] == '}';
}

static gt_bridge_session_config_t session_config(fixture_t *fixture, unsigned slot) {
    gt_bridge_session_config_t config = {
        .required_count = 1,
        .available_count = 1,
        .budgets = {768, 4096, 262144},
        .handshake_ms = 5000,
        .consent_ms = 60000,
        .clock_context = fixture,
        .clock = clock_now,
        .peer = {GT_BRIDGE_PEER_LOCAL, "owned-multiplex-socketpair"},
        .rpc_enabled = true,
        .rpc_method_count = 1};
    CHECK(snprintf(config.plugin_id, sizeof(config.plugin_id), "org.ghosterm.mux%u", slot) > 0);
    strcpy(config.plugin_version, "0.1.0");
    strcpy(config.host_version, "0.1.0");
    strcpy(config.sdk_version, "1.0.0");
    memset(config.client_digest, 'b', 64);
    memset(config.executor_digest, 'c', 64);
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    strcpy(config.rpc_methods[0].method, "fixture.echo");
    strcpy(config.rpc_methods[0].capability, "session.rpc");
    config.rpc_methods[0].receiver = GT_BRIDGE_EXECUTOR;
    config.rpc_methods[0].input = json_value;
    config.rpc_methods[0].output = json_value;
    return config;
}

static void open_fixture(fixture_t *fixture, unsigned queue) {
    *fixture = (fixture_t){.now = 1000};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fixture->fd) == 0);
    gt_bridge_mux_config_t config = configuration(queue);
    for (unsigned side = 0; side < 2; side++) {
        gt_bridge_stream_t *stream = NULL;
        CHECK(gt__bridge_fd_open(fixture->fd[side], fixture->fd[side], NULL, &stream) == GT_OK);
        CHECK(gt__bridge_mux_open(&stream, &config, NULL, &fixture->mux[side]) == GT_OK && !stream);
    }
}

static void branch_pair(fixture_t *fixture, unsigned slot, uint64_t epoch) {
    char id[33];
    id_for(slot, id);
    for (unsigned side = 0; side < 2; side++) {
        CHECK(gt__bridge_mux_branch_open(
                  fixture->mux[side], id, epoch, &fixture->branch[side][slot]) == GT_OK);
    }
}

static void session_pair(fixture_t *fixture, unsigned slot, uint64_t epoch) {
    branch_pair(fixture, slot, epoch);
    gt_bridge_session_config_t config = session_config(fixture, slot);
    for (unsigned side = 0; side < 2; side++) {
        gt_bridge_stream_t *owned = fixture->branch[side][slot];
        CHECK(gt__bridge_session_open(&owned,
                                      side == 0 ? GT_BRIDGE_HOST : GT_BRIDGE_EXECUTOR,
                                      &config,
                                      NULL,
                                      &fixture->session[side][slot]) == GT_ERR_UNSUPPORTED);
        CHECK(owned == fixture->branch[side][slot] && !fixture->session[side][slot]);
        CHECK(gt__bridge_session_open_multiplexed(&owned,
                                                  side == 0 ? GT_BRIDGE_HOST : GT_BRIDGE_EXECUTOR,
                                                  &config,
                                                  NULL,
                                                  &fixture->session[side][slot]) == GT_OK &&
              !owned);
    }
}

static void pump(fixture_t *fixture) {
    for (unsigned side = 0; side < 2; side++) {
        for (unsigned slot = 0; slot < 8; slot++) {
            if (fixture->session[side][slot]) {
                gt__bridge_session_step(fixture->session[side][slot], 0);
            }
        }
        CHECK(gt__bridge_mux_step(fixture->mux[side], 0) == GT_OK);
    }
}

static gt_bridge_session_info_t info(gt_bridge_session_t *session) {
    gt_bridge_session_info_t state;
    CHECK(gt__bridge_session_info(session, &state));
    return state;
}

static void activate(fixture_t *fixture) {
    bool active = false;
    for (unsigned step = 0; step < 10000 && !active; step++) {
        pump(fixture);
        active = true;
        for (unsigned slot = 0; slot < 8; slot++) {
            if (!fixture->session[0][slot]) {
                continue;
            }
            gt_bridge_session_info_t host = info(fixture->session[0][slot]);
            CHECK(host.end == GT_BRIDGE_END_NONE);
            if (host.phase == GT_BRIDGE_SESSION_CONSENT) {
                CHECK(gt__bridge_session_decide(fixture->session[0][slot],
                                                host.consent_token,
                                                GT_BRIDGE_DECISION_ALLOW) == GT_OK);
            }
            for (unsigned side = 0; side < 2; side++) {
                if (info(fixture->session[side][slot]).phase != GT_BRIDGE_SESSION_ACTIVE) {
                    active = false;
                }
            }
        }
    }
    CHECK(active);
    for (unsigned side = 0; side < 2; side++) {
        for (unsigned slot = 0; slot < 8; slot++) {
            if (fixture->session[side][slot]) {
                CHECK(gt__bridge_session_lease(fixture->session[side][slot],
                                               &fixture->lease[side][slot]));
            }
        }
    }
}

static void roundtrip(fixture_t *fixture) {
    uint64_t handles[8] = {0};
    bool replied[8] = {false};
    bool completed[8] = {false};
    char inputs[8][80];
    for (unsigned slot = 0; slot < 8; slot++) {
        CHECK(snprintf(inputs[slot],
                       sizeof(inputs[slot]),
                       "{\"slot\":%u,\"epoch\":%" PRIu64 "}",
                       slot,
                       fixture->lease[0][slot].epoch) > 0);
        CHECK(gt__bridge_session_request(fixture->session[0][slot],
                                         &fixture->lease[0][slot],
                                         "fixture.echo",
                                         inputs[slot],
                                         strlen(inputs[slot]),
                                         1000,
                                         &handles[slot]) == GT_BRIDGE_RPC_OK);
        uint64_t wrong = 99;
        CHECK(gt__bridge_session_request(fixture->session[0][slot],
                                         &fixture->lease[0][(slot + 1) % 8],
                                         "fixture.echo",
                                         "{}",
                                         2,
                                         1000,
                                         &wrong) == GT_BRIDGE_RPC_DENIED &&
              wrong == 0);
    }
    unsigned count = 0;
    for (unsigned step = 0; step < 10000 && count < 8; step++) {
        pump(fixture);
        for (unsigned slot = 0; slot < 8; slot++) {
            if (!replied[slot]) {
                gt_bridge_rpc_request_t request;
                gt_bridge_rpc_status_t status = gt__bridge_session_take_request(
                    fixture->session[1][slot], &fixture->lease[1][slot], &request);
                CHECK(status == GT_BRIDGE_RPC_OK || status == GT_BRIDGE_RPC_WAIT);
                if (status == GT_BRIDGE_RPC_OK) {
                    CHECK(request.input_size == strlen(inputs[slot]));
                    CHECK(!memcmp(request.input, inputs[slot], request.input_size));
                    char outcome[128];
                    int size = snprintf(outcome,
                                        sizeof(outcome),
                                        "{\"kind\":\"success\",\"value\":%s}",
                                        inputs[slot]);
                    CHECK(size > 0 && (size_t)size < sizeof(outcome));
                    CHECK(gt__bridge_session_respond(fixture->session[1][slot],
                                                     &fixture->lease[1][slot],
                                                     request.ticket,
                                                     outcome,
                                                     (size_t)size) == GT_BRIDGE_RPC_OK);
                    replied[slot] = true;
                }
            }
            gt_bridge_rpc_result_t result;
            CHECK(gt__bridge_session_request_result(
                fixture->session[0][slot], &fixture->lease[0][slot], handles[slot], &result));
            if (result.confirmed && !completed[slot]) {
                CHECK(result.state == GT_BRIDGE_RPC_SUCCEEDED);
                char expected[128];
                int size = snprintf(expected,
                                    sizeof(expected),
                                    "{\"kind\":\"success\",\"value\":%s}",
                                    inputs[slot]);
                CHECK(result.outcome_size == (size_t)size &&
                      !memcmp(result.outcome, expected, (size_t)size));
                completed[slot] = true;
                count++;
            }
        }
    }
    CHECK(count == 8);
    for (unsigned slot = 0; slot < 8; slot++) {
        CHECK(gt__bridge_session_forget_request(fixture->session[0][slot],
                                                &fixture->lease[0][slot],
                                                handles[slot]) == GT_BRIDGE_RPC_OK);
    }
}

static void cleanup(fixture_t *fixture) {
    for (unsigned side = 0; side < 2; side++) {
        gt__bridge_mux_stop(fixture->mux[side]);
        CHECK(gt__bridge_mux_close(fixture->mux[side], 0) == GT_BRIDGE_DRAINING ||
              !fixture->branch[side][0]);
        for (unsigned slot = 0; slot < 8; slot++) {
            if (fixture->session[side][slot]) {
                CHECK(gt__bridge_session_close(fixture->session[side][slot], 0) ==
                      GT_BRIDGE_RELEASED);
                CHECK(gt__bridge_session_destroy(fixture->session[side][slot]) == GT_OK);
                fixture->session[side][slot] = NULL;
            } else if (fixture->branch[side][slot]) {
                CHECK(gt__bridge_stream_close(fixture->branch[side][slot], 0) ==
                      GT_BRIDGE_RELEASED);
                CHECK(gt__bridge_stream_destroy(fixture->branch[side][slot]) == GT_OK);
            }
            fixture->branch[side][slot] = NULL;
        }
        CHECK(gt__bridge_mux_close(fixture->mux[side], 0) == GT_BRIDGE_RELEASED);
        CHECK(gt__bridge_mux_destroy(&fixture->mux[side]) == GT_OK && !fixture->mux[side]);
    }
}

static void close_session(fixture_t *fixture, unsigned side, unsigned slot) {
    bool closed = false;
    for (unsigned step = 0; step < 10000; step++) {
        gt_bridge_close_t status = gt__bridge_session_close(fixture->session[side][slot], 0);
        CHECK(status != GT_BRIDGE_CLOSE_ERROR);
        if (status == GT_BRIDGE_RELEASED) {
            closed = true;
            break;
        }
        pump(fixture);
    }
    CHECK(closed);
    CHECK(gt__bridge_session_destroy(fixture->session[side][slot]) == GT_OK);
    fixture->session[side][slot] = NULL;
    fixture->branch[side][slot] = NULL;
}

static size_t
packet(char *out, size_t capacity, unsigned slot, uint64_t epoch, uint64_t sequence, bool bulk) {
    char id[33];
    id_for(slot, id);
    const char *payload = bulk ? "{\"resourceId\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
                                 "\"generation\":\"1\",\"offset\":\"0\",\"dataBase64\":\"YQ==\"}"
                               : "{\"topic\":\"fixture.tick\",\"value\":{}}";
    int size = snprintf(out + 4,
                        capacity - 4,
                        "{\"wireVersion\":1,\"connectionId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                        "\"instanceId\":\"%s\","
                        "\"epoch\":\"%" PRIu64 "\",\"streamId\":\"%s\",\"seq\":\"%" PRIu64
                        "\",\"kind\":\"%s\",\"payload\":%s}",
                        id,
                        epoch,
                        bulk ? "resource" : "events",
                        sequence,
                        bulk ? "resource.chunk" : "event",
                        payload);
    CHECK(size > 0 && (size_t)size + 4 <= capacity);
    out[0] = (char)((unsigned)size >> 24);
    out[1] = (char)((unsigned)size >> 16);
    out[2] = (char)((unsigned)size >> 8);
    out[3] = (char)size;
    return (size_t)size + 4;
}

static void raw_write(gt_bridge_stream_t *stream, const char *bytes, size_t size) {
    size_t used = 0;
    while (used < size) {
        gt_bridge_io_t result = gt__bridge_stream_write(stream, bytes + used, size - used, 0);
        CHECK(result.state == GT_BRIDGE_DATA);
        used += result.count;
    }
}

static void test_sessions(bool reuse) {
    fixture_t fixture;
    open_fixture(&fixture, 64);
    for (unsigned slot = 0; slot < 8; slot++) {
        session_pair(&fixture, slot, slot + 1);
    }
    activate(&fixture);
    for (unsigned slot = 0; slot < 8; slot++) {
        CHECK(!strcmp(fixture.lease[0][0].connection, fixture.lease[0][slot].connection));
        CHECK(fixture.lease[0][slot].epoch == slot + 1);
        CHECK(!strcmp(fixture.lease[0][slot].instance, fixture.lease[1][slot].instance));
    }
    char id[33];
    id_for(8, id);
    gt_bridge_stream_t *extra = NULL;
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, 9, &extra) == GT_ERR_IO && !extra);
    roundtrip(&fixture);
    if (reuse) {
        gt_bridge_lease_t previous = fixture.lease[0][3];
        close_session(&fixture, 0, 3);
        for (unsigned step = 0; step < 200; step++) {
            pump(&fixture);
        }
        CHECK(info(fixture.session[1][3]).end == GT_BRIDGE_END_PEER);
        for (unsigned slot = 0; slot < 8; slot++) {
            if (slot != 3) {
                CHECK(info(fixture.session[0][slot]).phase == GT_BRIDGE_SESSION_ACTIVE);
                CHECK(info(fixture.session[1][slot]).phase == GT_BRIDGE_SESSION_ACTIVE);
            }
        }
        close_session(&fixture, 1, 3);
        CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, 4, &extra) == GT_ERR_INVALID_ARG &&
              !extra);
        session_pair(&fixture, 3, 9);
        activate(&fixture);
        CHECK(gt__bridge_session_access(fixture.session[0][3], &previous, "session.rpc") ==
              GT_BRIDGE_ACCESS_DENIED);
        /* A retired wire scope with the same instance ID cannot reach the new
         * Session; its native generation is the routing boundary. */
        char bytes[1024];
        size_t size = packet(bytes, sizeof(bytes), 3, 4, 0, false);
        CHECK(write(fixture.fd[0], bytes, size) == (ssize_t)size);
        for (unsigned step = 0; step < 100; step++) {
            pump(&fixture);
        }
        gt_bridge_mux_info_t state;
        CHECK(gt__bridge_mux_info(fixture.mux[1], &state) && state.rejected == 1 &&
              state.error == GT_OK);
        roundtrip(&fixture);
    }
    cleanup(&fixture);
}

static void test_bounded(void) {
    fixture_t fixture;
    open_fixture(&fixture, 1);
    branch_pair(&fixture, 0, 1);
    branch_pair(&fixture, 1, 2);
    char bytes[1024];
    size_t size = packet(bytes, sizeof(bytes), 0, 1, 0, false);
    raw_write(fixture.branch[0][0], bytes, size);
    gt_bridge_io_t held = gt__bridge_stream_write(fixture.branch[0][0], bytes, size, 0);
    CHECK(held.state == GT_BRIDGE_WAIT && held.count == 0);
    for (unsigned step = 0; step < 20; step++) {
        pump(&fixture);
    }
    raw_write(fixture.branch[0][0], bytes, size);
    size = packet(bytes, sizeof(bytes), 1, 2, 0, false);
    raw_write(fixture.branch[0][1], bytes, size);
    for (unsigned step = 0; step < 40; step++) {
        pump(&fixture);
    }
    gt_bridge_mux_branch_info_t state;
    CHECK(gt__bridge_mux_branch_info(fixture.branch[1][0], &state));
    CHECK(state.error == GT_ERR_PROTOCOL && state.rejected == 1);
    CHECK(gt__bridge_mux_branch_info(fixture.branch[1][1], &state));
    CHECK(state.error == GT_OK && state.queued_frames == 1);
    char received[1024];
    gt_bridge_io_t result =
        gt__bridge_stream_read(fixture.branch[1][1], received, sizeof(received), 0);
    CHECK(result.state == GT_BRIDGE_DATA && result.count == size && !memcmp(received, bytes, size));
    cleanup(&fixture);
}

static void test_isolation(void) {
    fixture_t fixture;
    open_fixture(&fixture, 4);
    branch_pair(&fixture, 0, 1);
    branch_pair(&fixture, 1, 2);
    char bytes[1024];
    size_t size = packet(bytes, sizeof(bytes), 1, 2, 0, false);
    gt_bridge_io_t rejected = gt__bridge_stream_write(fixture.branch[0][0], bytes, size, 0);
    CHECK(rejected.state == GT_BRIDGE_ERROR && rejected.error == GT_ERR_PROTOCOL);
    raw_write(fixture.branch[0][1], bytes, size);
    for (unsigned step = 0; step < 40; step++) {
        pump(&fixture);
    }
    char received[1024];
    gt_bridge_io_t result =
        gt__bridge_stream_read(fixture.branch[1][1], received, sizeof(received), 0);
    CHECK(result.state == GT_BRIDGE_DATA && result.count == size && !memcmp(received, bytes, size));
    gt_bridge_mux_info_t state;
    CHECK(gt__bridge_mux_info(fixture.mux[0], &state) && state.error == GT_OK);
    cleanup(&fixture);
}

static void test_priority(void) {
    fixture_t fixture;
    open_fixture(&fixture, 64);
    branch_pair(&fixture, 0, 1);
    branch_pair(&fixture, 1, 2);
    char bulk[1024], control[1024];
    size_t bulk_size = packet(bulk, sizeof(bulk), 0, 1, 0, true);
    size_t control_size = packet(control, sizeof(control), 1, 2, 0, false);
    raw_write(fixture.branch[0][0], bulk, bulk_size);
    for (unsigned count = 0; count < 8; count++) {
        raw_write(fixture.branch[0][1], control, control_size);
        CHECK(gt__bridge_mux_step(fixture.mux[0], 0) == GT_OK);
        CHECK(gt__bridge_mux_step(fixture.mux[1], 0) == GT_OK);
        gt_bridge_mux_branch_info_t state;
        CHECK(gt__bridge_mux_branch_info(fixture.branch[1][0], &state) && state.received == 0);
    }
    raw_write(fixture.branch[0][1], control, control_size);
    pump(&fixture);
    gt_bridge_mux_branch_info_t state;
    CHECK(gt__bridge_mux_branch_info(fixture.branch[1][0], &state) && state.received == 1);
    pump(&fixture);
    CHECK(gt__bridge_mux_branch_info(fixture.branch[1][1], &state) && state.received == 9);
    cleanup(&fixture);
}

static void test_malformed(void) {
    fixture_t fixture;
    open_fixture(&fixture, 4);
    branch_pair(&fixture, 0, 1);
    const unsigned char invalid[] = {0, 1, 0, 1};
    CHECK(write(fixture.fd[0], invalid, sizeof(invalid)) == sizeof(invalid));
    CHECK(gt__bridge_mux_step(fixture.mux[1], 0) == GT_ERR_PROTOCOL);
    gt_bridge_mux_info_t state;
    CHECK(gt__bridge_mux_info(fixture.mux[1], &state) && state.error == GT_ERR_PROTOCOL);
    cleanup(&fixture);
}

static void test_loss(void) {
    fixture_t fixture;
    open_fixture(&fixture, 64);
    uint64_t handles[8];
    for (unsigned slot = 0; slot < 8; slot++) {
        session_pair(&fixture, slot, slot + 1);
    }
    activate(&fixture);
    for (unsigned slot = 0; slot < 8; slot++) {
        CHECK(gt__bridge_session_request(fixture.session[0][slot],
                                         &fixture.lease[0][slot],
                                         "fixture.echo",
                                         "{}",
                                         2,
                                         1000,
                                         &handles[slot]) == GT_BRIDGE_RPC_OK);
    }
    for (unsigned step = 0; step < 100; step++) {
        pump(&fixture);
    }
    gt__bridge_mux_stop(fixture.mux[0]);
    for (unsigned slot = 0; slot < 8; slot++) {
        gt__bridge_session_step(fixture.session[0][slot], 0);
        gt_bridge_rpc_result_t result;
        CHECK(gt__bridge_session_request_result(
            fixture.session[0][slot], &fixture.lease[0][slot], handles[slot], &result));
        CHECK(result.state == GT_BRIDGE_RPC_UNKNOWN && !result.confirmed);
        CHECK(info(fixture.session[0][slot]).end == GT_BRIDGE_END_LOST);
    }
    cleanup(&fixture);
}

static void test_epochs(void) {
    fixture_t fixture;
    open_fixture(&fixture, 4);
    branch_pair(&fixture, 0, 64);
    branch_pair(&fixture, 1, 1);
    branch_pair(&fixture, 2, 63);
    branch_pair(&fixture, 3, 128);
    char id[33];
    id_for(4, id);
    gt_bridge_stream_t *out = NULL;
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, 65, &out) == GT_OK);
    fixture.branch[0][4] = out;
    id_for(5, id);
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, 65, &out) == GT_ERR_INVALID_ARG && !out);
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, 64, &out) == GT_ERR_INVALID_ARG && !out);
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, 0, &out) == GT_ERR_INVALID_ARG && !out);
    char packet_bytes[1024];
    size_t size = packet(packet_bytes, sizeof(packet_bytes), 1, 1, 0, false);
    raw_write(fixture.branch[0][1], packet_bytes, size);
    for (unsigned step = 0; step < 20; step++) {
        pump(&fixture);
    }
    char received[1024];
    gt_bridge_io_t result =
        gt__bridge_stream_read(fixture.branch[1][1], received, sizeof(received), 0);
    CHECK(result.state == GT_BRIDGE_DATA && result.count == size &&
          !memcmp(received, packet_bytes, size));
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, UINT64_MAX, &out) == GT_OK);
    fixture.branch[0][5] = out;
    id_for(6, id);
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, UINT64_MAX - 64, &out) ==
              GT_ERR_INVALID_ARG &&
          !out);
    CHECK(gt__bridge_mux_branch_open(fixture.mux[0], id, UINT64_MAX, &out) == GT_ERR_INVALID_ARG &&
          !out);
    cleanup(&fixture);
}

typedef struct {
    gt_bridge_stream_t stream;
    const void *retained;
    size_t length;
    unsigned close_calls;
    uint8_t snapshot[1024];
} retained_stream_t;

static gt_bridge_io_t retained_read(gt_bridge_stream_t *stream, void *bytes, size_t size, int ms) {
    (void)stream;
    (void)bytes;
    (void)size;
    (void)ms;
    return (gt_bridge_io_t){GT_BRIDGE_WAIT, 0, GT_OK};
}

static gt_bridge_io_t
retained_write(gt_bridge_stream_t *stream, const void *bytes, size_t size, int ms) {
    (void)ms;
    retained_stream_t *retained = (retained_stream_t *)stream;
    CHECK(size <= sizeof(retained->snapshot));
    if (retained->retained) {
        CHECK(retained->retained == bytes && retained->length == size);
        CHECK(!memcmp(retained->snapshot, bytes, size));
    } else {
        retained->retained = bytes;
        retained->length = size;
        memcpy(retained->snapshot, bytes, size);
    }
    return (gt_bridge_io_t){GT_BRIDGE_WAIT, 0, GT_OK};
}

static void retained_wake(gt_bridge_stream_t *stream) {
    (void)stream;
}

static gt_bridge_close_t retained_close(gt_bridge_stream_t *stream, int ms) {
    (void)ms;
    retained_stream_t *retained = (retained_stream_t *)stream;
    CHECK(retained->retained && !memcmp(retained->snapshot, retained->retained, retained->length));
    return ++retained->close_calls == 1 ? GT_BRIDGE_DRAINING : GT_BRIDGE_RELEASED;
}

static void retained_destroy(gt_bridge_stream_t *stream) {
    retained_stream_t *retained = (retained_stream_t *)stream;
    CHECK(retained->close_calls == 2);
    free(stream);
}

static void test_retained(void) {
    static const gt_bridge_stream_ops_t operations = {
        retained_read, retained_write, retained_wake, retained_close, retained_destroy};
    retained_stream_t *retained = calloc(1, sizeof(*retained));
    CHECK(retained && gt__bridge_stream_init(&retained->stream, &operations) == GT_OK);
    gt_bridge_stream_t *stream = &retained->stream;
    gt_bridge_mux_t *mux = NULL;
    gt_bridge_mux_config_t config = configuration(4);
    CHECK(gt__bridge_mux_open(&stream, &config, NULL, &mux) == GT_OK && !stream);
    char id[33];
    id_for(0, id);
    CHECK(gt__bridge_mux_branch_open(mux, id, 1, &stream) == GT_OK);
    char bytes[1024];
    size_t size = packet(bytes, sizeof(bytes), 0, 1, 0, false);
    raw_write(stream, bytes, size);
    CHECK(gt__bridge_mux_step(mux, 0) == GT_OK && retained->retained);
    CHECK(gt__bridge_mux_step(mux, 0) == GT_OK);
    CHECK(gt__bridge_stream_close(stream, 0) == GT_BRIDGE_DRAINING);
    CHECK(gt__bridge_mux_close(mux, 0) == GT_BRIDGE_DRAINING);
    CHECK(gt__bridge_stream_close(stream, 0) == GT_BRIDGE_RELEASED);
    CHECK(gt__bridge_stream_destroy(stream) == GT_OK);
    CHECK(gt__bridge_mux_close(mux, 0) == GT_BRIDGE_DRAINING);
    CHECK(gt__bridge_mux_destroy(&mux) == GT_ERR_INVALID_ARG);
    CHECK(gt__bridge_mux_close(mux, 0) == GT_BRIDGE_RELEASED);
    CHECK(gt__bridge_mux_destroy(&mux) == GT_OK && !mux);
}

typedef struct {
    size_t calls, fail, live;
} budget_t;

static void *budget_allocate(void *context, size_t size) {
    budget_t *budget = context;
    if (++budget->calls == budget->fail) {
        return NULL;
    }
    void *result = malloc(size);
    if (result) {
        budget->live += size;
    }
    return result;
}

static void budget_release(void *context, void *bytes, size_t size) {
    budget_t *budget = context;
    CHECK(budget->live >= size);
    for (size_t i = 0; i < size; i++) {
        CHECK(((unsigned char *)bytes)[i] == 0);
    }
    budget->live -= size;
    free(bytes);
}

static void test_allocation(void) {
    for (unsigned failure = 1; failure <= 9; failure++) {
        int fd[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        gt_bridge_stream_t *stream = NULL;
        CHECK(gt__bridge_fd_open(fd[0], fd[0], NULL, &stream) == GT_OK);
        gt_bridge_stream_t *original = stream;
        gt_bridge_mux_t *mux = NULL;
        gt_bridge_mux_config_t config = configuration(4);
        budget_t budget = {.fail = failure};
        gt_allocator_t allocator = {&budget, budget_allocate, budget_release};
        gt_status_t status = gt__bridge_mux_open(&stream, &config, &allocator, &mux);
        if (status != GT_OK) {
            CHECK(stream == original && !mux && budget.live == 0);
            CHECK(gt__bridge_stream_close(stream, 0) == GT_BRIDGE_RELEASED);
            CHECK(gt__bridge_stream_destroy(stream) == GT_OK);
        } else {
            CHECK(!stream);
            char id[33];
            id_for(0, id);
            size_t baseline = budget.live;
            gt_bridge_stream_t *branch = NULL;
            status = gt__bridge_mux_branch_open(mux, id, 1, &branch);
            if (status != GT_OK) {
                CHECK(!branch && budget.live == baseline);
                budget.fail = 0;
                CHECK(gt__bridge_mux_branch_open(mux, id, 1, &branch) == GT_OK);
            }
            gt__bridge_mux_stop(mux);
            CHECK(gt__bridge_mux_destroy(&mux) == GT_ERR_INVALID_ARG);
            CHECK(gt__bridge_stream_close(branch, 0) == GT_BRIDGE_RELEASED);
            CHECK(gt__bridge_stream_destroy(branch) == GT_OK);
            CHECK(gt__bridge_mux_close(mux, 0) == GT_BRIDGE_RELEASED);
            CHECK(gt__bridge_mux_destroy(&mux) == GT_OK && budget.live == 0);
        }
        CHECK(close(fd[1]) == 0);
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    size_t baseline = gt__plugin_memory_info().used;
    if (!strcmp(argv[1], "sessions")) {
        test_sessions(false);
    } else if (!strcmp(argv[1], "reuse")) {
        test_sessions(true);
    } else if (!strcmp(argv[1], "bounded")) {
        test_bounded();
    } else if (!strcmp(argv[1], "isolation")) {
        test_isolation();
    } else if (!strcmp(argv[1], "priority")) {
        test_priority();
    } else if (!strcmp(argv[1], "malformed")) {
        test_malformed();
    } else if (!strcmp(argv[1], "allocation")) {
        test_allocation();
    } else if (!strcmp(argv[1], "loss")) {
        test_loss();
    } else if (!strcmp(argv[1], "epochs")) {
        test_epochs();
    } else if (!strcmp(argv[1], "retained")) {
        test_retained();
    } else {
        CHECK(false);
    }
    CHECK(gt__plugin_memory_info().used == baseline);
    printf("PASS multiplex %s\n", argv[1]);
    return 0;
}
