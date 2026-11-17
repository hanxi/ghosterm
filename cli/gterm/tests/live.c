#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL live:%d %s\n", __LINE__, #x);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static gt_ext_mux_t *mux;
static gt_ext_session_t *sessions[8];
static gt_ext_lease_t leases[8];
static unsigned calls[8], ticket;
static uint64_t deadline;
static char response[8192];

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void pump(void) {
    if (now() >= deadline) {
        gt_ext_mux_info_t info;
        gt_ext_mux_native_info_t native;
        gt_ext_transport_process_info_t process;
        gt_ext_mux_info(mux, &info);
        gt_ext_mux_native_info(mux, &native);
        gt_ext_mux_process_info(mux, &process);
        fprintf(stderr,
                "live deadline ticket=%u error=%d in=%u out=%u awaiting=%u child=%u exit=%d "
                "diagnostic=%.*s\n",
                ticket,
                info.error,
                native.incoming_bytes,
                native.outgoing_bytes,
                native.awaiting,
                process.child_exited,
                process.exit_code,
                (int)process.diagnostic_size,
                process.diagnostic);
        CHECK(false);
    }
    (void)gt_ext_mux_step(mux, 0);
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            (void)gt_ext_session_step(sessions[i], 0);
        }
    }
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static const char *operation(const char *method, const char *input) {
    gt_ext_mux_native_info_t info;
    CHECK(gt_ext_mux_native_info(mux, &info));
    char frame[8192];
    char id[33];
    CHECK(snprintf(id, sizeof(id), "%032x", ++ticket) == 32);
    int size = snprintf(frame,
                        sizeof(frame),
                        "{\"wireVersion\":1,\"connectionId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                        "\"instanceId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"epoch\":\"1\","
                        "\"streamId\":\"rpc.request\",\"seq\":\"%" PRIu64
                        "\",\"kind\":\"request\",\"payload\":{\"requestId\":\"%s\",\"method\":"
                        "\"native.multiplex.%s\",\"timeoutMs\":10000,\"input\":%s}}",
                        info.next_send,
                        id,
                        method,
                        input);
    CHECK(size > 0 && (size_t)size < sizeof(frame));
    CHECK(gt_ext_mux_native_send(mux, frame, (size_t)size) == GT_OK);
    size_t used = 0;
    for (;;) {
        pump();
        gt_status_t status = gt_ext_mux_native_take(mux, response, sizeof(response) - 1, &used);
        if (status == GT_ERR_NOT_FOUND) {
            continue;
        }
        CHECK(status == GT_OK);
        break;
    }
    response[used] = 0;
    CHECK(strstr(response, id));
    return response;
}

static void released(unsigned slot, uint64_t epoch) {
    while (gt_ext_session_close(sessions[slot], 0) != GT_EXT_RELEASED) {
        pump();
    }
    CHECK(gt_ext_session_destroy(&sessions[slot]) == GT_OK);
    char input[128];
    CHECK(snprintf(input,
                   sizeof(input),
                   "{\"instance\":\"%032x\",\"epoch\":\"%" PRIu64 "\"}",
                   slot + 1,
                   epoch) > 0);
    while (!strstr(operation("status", input), "\"present\":false")) {
        pump();
    }
}

static void
opened(unsigned slot, uint64_t epoch, const char *profile, const char *plugin, const char *digest) {
    char id[33], input[4608];
    CHECK(snprintf(id, sizeof(id), "%032x", slot + 1) == 32);
    gt_ext_channel_t *channel = NULL;
    CHECK(gt_ext_mux_open_channel(mux, id, epoch, &channel) == GT_OK);
    int size = snprintf(input,
                        sizeof(input),
                        "{\"instance\":\"%s\",\"epoch\":\"%" PRIu64 "\",\"profile\":\"%s\"}",
                        id,
                        epoch,
                        profile);
    CHECK(size > 0 && (size_t)size < sizeof(input));
    const char *reply = operation("open", input);
    if (!strstr(reply, "\"admitted\":true")) {
        fprintf(stderr, "Native admission %s\n", reply);
        CHECK(false);
    }
    gt_ext_session_config_t config = {.required_count = 1,
                                      .available_count = 1,
                                      .budgets = {768, 65536, 262144},
                                      .handshake_ms = 5000,
                                      .consent_ms = 60000,
                                      .peer = {GT_EXT_PEER_LOCAL, "owned-live-mux"},
                                      .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(config.plugin_id, plugin);
    strcpy(config.plugin_version, "0.1.0");
    memset(config.client_digest, 'd', 64);
    strcpy(config.executor_digest, digest);
    strcpy(config.host_version, "0.1.0");
    strcpy(config.sdk_version, "1.0.0");
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    gt_ext_rpc_method_t method = {.method = "fixture.echo",
                                  .capability = "session.rpc",
                                  .receiver = GT_EXT_EXECUTOR,
                                  .input_shape = GT_EXT_JSON_OBJECT,
                                  .output_shape = GT_EXT_JSON_OBJECT};
    CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &config, &method, 1, &sessions[slot]) ==
              GT_OK &&
          !channel);
    for (;;) {
        pump();
        gt_ext_session_info_t state;
        CHECK(gt_ext_session_info(sessions[slot], &state));
        if (state.phase == GT_EXT_SESSION_CONSENT) {
            CHECK(gt_ext_session_decide(
                      sessions[slot], state.consent_token, GT_EXT_DECISION_ALLOW) == GT_OK);
        }
        CHECK(state.end == GT_EXT_END_NONE);
        if (state.phase == GT_EXT_SESSION_ACTIVE) {
            break;
        }
    }
    CHECK(gt_ext_session_lease(sessions[slot], &leases[slot]) && leases[slot].epoch == epoch);
    calls[slot] = 0;
}

static void wave(void) {
    uint64_t handles[8] = {0};
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            CHECK(gt_ext_session_request(sessions[i],
                                         &leases[i],
                                         "fixture.echo",
                                         "{\"text\":\"live\",\"count\":17}",
                                         sizeof("{\"text\":\"live\",\"count\":17}") - 1,
                                         5000,
                                         &handles[i]) == GT_EXT_RPC_OK);
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            gt_ext_rpc_result_t result;
            do {
                pump();
                CHECK(gt_ext_session_request_result(sessions[i], &leases[i], handles[i], &result) ==
                      GT_EXT_RPC_OK);
            } while (!result.confirmed);
            char expected[160];
            int size = snprintf(expected,
                                sizeof(expected),
                                "{\"kind\":\"success\",\"value\":{\"text\":\"live\",\"count\":17,"
                                "\"owner\":%u,\"calls\":%u}}",
                                i,
                                ++calls[i]);
            CHECK(result.state == GT_EXT_RPC_SUCCEEDED && result.outcome_size == (uint32_t)size &&
                  !memcmp(result.outcome, expected, (size_t)size));
            CHECK(gt_ext_session_forget_request(sessions[i], &leases[i], handles[i]) ==
                  GT_EXT_RPC_OK);
        }
    }
}

static void cleanup(void) {
    if (!mux) {
        return;
    }
    gt_ext_mux_stop(mux);
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            gt_ext_session_stop(sessions[i]);
            if (gt_ext_session_close(sessions[i], 0) == GT_EXT_RELEASED) {
                gt_ext_session_destroy(&sessions[i]);
            }
        }
    }
    for (unsigned i = 0; i < 1000; i++) {
        if (gt_ext_mux_close(mux, 0) == GT_EXT_RELEASED) {
            gt_ext_mux_destroy(&mux);
            return;
        }
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 13);
    CHECK(!atexit(cleanup));
    deadline = now() + 60000;
    const char *arguments[] = {argv[1],
                               "relay",
                               "--multiplex",
                               "--live",
                               "--connection",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    char temporary[4096];
    CHECK(snprintf(temporary, sizeof(temporary), "TMPDIR=%s", argv[4]) > 0);
    const char *environment[] = {temporary};
    int fd = open(argv[4], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    gt_ext_process_channel_config_t process = {.executable = argv[1],
                                               .arguments = arguments,
                                               .argument_count = 6,
                                               .cwd_fd = fd,
                                               .environment = environment,
                                               .environment_count = 1,
                                               .execution_authorized = true,
                                               .lifetime_ms = 65000,
                                               .stdout_limit = 16777216};
    gt_ext_channel_t *physical = NULL;
    CHECK(gt_ext_channel_open_process(&process, &physical) == GT_OK && !close(fd));
    gt_ext_mux_config_t config = {.frame_bytes = 8192, .receive_frames = 64, .instances = 8};
    strcpy(config.connection, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(gt_ext_mux_open(&physical, &config, &mux) == GT_OK && !physical);
    CHECK(gt_ext_mux_native_enable(mux, true) == GT_OK);
    CHECK(strstr(
        operation("status", "{\"instance\":\"00000000000000000000000000000001\",\"epoch\":\"1\"}"),
        "NOT_NEGOTIATED"));
    CHECK(!strstr(operation("hello", "[]"), "\"nativeControl\":1"));
    CHECK(strstr(operation("hello", "{}"), "\"nativeControl\":1"));
    CHECK(strstr(operation("open",
                           "{\"instance\":\"00000000000000000000000000000001\",\"epoch\":\"1\","
                           "\"profile\":\"00\",\"extra\":true}"),
                 "INVALID_INPUT"));
    CHECK(strstr(operation("open",
                           "{\"instance\":\"00000000000000000000000000000001\",\"epoch\":\"1\","
                           "\"profile\":\"00\"}"),
                 "INVALID_PROFILE"));
    for (unsigned i = 0; i < 8; i++) {
        opened(i, i + 1, argv[5 + i], argv[2], argv[3]);
    }
    wave();
    gt_ext_mux_info_t info;
    CHECK(gt_ext_mux_info(mux, &info) && info.instances == 8);
    uint64_t old = 4;
    const uint64_t generations[] = {9, 10, 70};
    for (unsigned i = 0; i < 3; i++) {
        released(3, old);
        wave();
        opened(3, generations[i], argv[8], argv[2], argv[3]);
        wave();
        old = generations[i];
    }
    for (unsigned i = 0; i < 8; i++) {
        released(i, i == 3 ? old : i + 1);
    }
    CHECK(gt_ext_mux_info(mux, &info) && info.instances == 0);
    gt_ext_transport_process_info_t observation;
    CHECK(gt_ext_mux_process_info(mux, &observation) == GT_OK && !observation.child_exited);
    opened(0, 71, argv[5], argv[2], argv[3]);
    wave();
    released(0, 71);
    CHECK(strstr(operation("finish", "{}"), "\"released\":true"));
    while (gt_ext_mux_close(mux, 0) != GT_EXT_RELEASED) {
        CHECK(now() < deadline);
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    CHECK(gt_ext_mux_process_info(mux, &observation) == GT_OK && observation.child_exited &&
          observation.reaped && observation.exit_code == 0 && !observation.term_signal);
    CHECK(gt_ext_mux_destroy(&mux) == GT_OK && !mux);
    puts("PASS live multiplex: empty start, eight online owners, fresh epochs 9/10/70, active old "
         "siblings, empty reconnect-free reuse, cleanup ACK and actual reap");
    return 0;
}
