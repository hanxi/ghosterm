#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static pid_t child;

static void cleanup(void) {
    if (child > 0) {
        kill(child, SIGTERM);
        for (unsigned i = 0; i < 200; i++) {
            if (waitpid(child, NULL, WNOHANG) == child) {
                return;
            }
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
}

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void idle(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
    CHECK(argc == 8 && !atexit(cleanup));
    int directory = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 0);
    gt_ext_package_config_t capture = {directory, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    CHECK(gt_ext_package_open(&capture, &package) == GT_OK && !close(directory));
    while (gt_ext_package_step(package) == GT_EXT_PACKAGE_CAPTURING) {
    }
    gt_ext_package_check_config_t check = {
        .host_json = "{\"side\":\"client\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],\"runtimes\":"
                     "[\"lua\"],\"capabilities\":[\"session.rpc\"]}"};
    CHECK(gt_ext_package_check(package, &check) == GT_EXT_PACKAGE_CHECKED);
    gt_ext_package_metadata_t metadata;
    CHECK(gt_ext_package_metadata(package, &metadata) && !strcmp(metadata.package_digest, argv[3]));
    int sockets[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(sockets[0]);
        CHECK(dup2(sockets[1], 0) == 0 && dup2(sockets[1], 1) == 1);
        close(sockets[1]);
        execl(argv[1],
              argv[1],
              "relay",
              "--stdio",
              "--trust-development-digest",
              argv[3],
              "--client-digest",
              argv[3],
              "--root",
              "workspace",
              argv[4],
              "--process-program",
              "codex",
              argv[5],
              argv[6],
              argv[7],
              "--method",
              "codex.thread.start",
              "--method",
              "codex.turn.send",
              "--method",
              "codex.turn.interrupt",
              "--method",
              "codex.approval.respond",
              "--method",
              "codex.change.read",
              "--host-method",
              "codex.event",
              "--",
              argv[2],
              "ai codex",
              (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    gt_ext_channel_t *channel = NULL;
    CHECK(gt_ext_channel_open_fd(sockets[0], sockets[0], &channel) == GT_OK);
    gt_ext_session_config_t config = {.required_count = 3,
                                      .available_count = 3,
                                      .budgets = {768, 65536, 262144},
                                      .handshake_ms = 5000,
                                      .consent_ms = 10000,
                                      .peer = {GT_EXT_PEER_LOCAL, "owned-codex-gterm"},
                                      .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(config.plugin_id, "org.ghosterm.ai-codex");
    strcpy(config.plugin_version, "0.1.0");
    strcpy(config.sdk_version, "1.0.0");
    strcpy(config.host_version, "0.1.0");
    strcpy(config.client_digest, argv[3]);
    strcpy(config.executor_digest, argv[3]);
    const char *capabilities[] = {"session.rpc", "process.spawn", "process.pipes"};
    for (size_t i = 0; i < 3; i++) {
        strcpy(config.required[i], capabilities[i]);
        strcpy(config.available[i], capabilities[i]);
    }
    gt_ext_rpc_method_t methods[6] = {0};
    const char *names[] = {"codex.thread.start",
                           "codex.turn.send",
                           "codex.turn.interrupt",
                           "codex.approval.respond",
                           "codex.change.read",
                           "codex.event"};
    for (size_t i = 0; i < 6; i++) {
        strcpy(methods[i].method, names[i]);
        strcpy(methods[i].capability, "session.rpc");
        methods[i].receiver = i == 5 ? GT_EXT_HOST : GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    gt_ext_session_t *session = NULL;
    CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &config, methods, 6, &session) == GT_OK);
    uint64_t deadline = now() + 30000;
    bool consent = false;
    while (now() < deadline) {
        gt_ext_session_step(session, 0);
        gt_ext_session_info_t info;
        CHECK(gt_ext_session_info(session, &info));
        if (info.phase == GT_EXT_SESSION_CONSENT && !consent) {
            CHECK(gt_ext_session_decide(session, info.consent_token, GT_EXT_DECISION_ALLOW) ==
                  GT_OK);
            consent = true;
        }
        if (info.phase == GT_EXT_SESSION_ACTIVE) {
            break;
        }
        CHECK(info.phase < GT_EXT_SESSION_ENDING);
        idle();
    }
    gt_ext_lease_t lease;
    CHECK(consent && gt_ext_session_lease(session, &lease));
    gt_ext_session_t *native_session = session;
    gt_ext_invocation_t invocation = {.command = "ai codex"};
    gt_ext_runtime_config_t runtime_config = {
        .role = GT_EXT_HOST, .cpu_ms = 100, .invocation = &invocation};
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open_package(&session, package, &runtime_config, true, &runtime) ==
              GT_OK &&
          !session);
    CHECK(gt_ext_package_destroy(&package) == GT_OK);
    uint64_t handle = 0;
    bool started = false, event_seen = false;
    while (now() < deadline && !(started && event_seen)) {
        gt_ext_runtime_phase_t phase = gt_ext_runtime_step(runtime);
        if (phase != GT_EXT_RUNTIME_ACTIVE && phase != GT_EXT_RUNTIME_STARTING &&
            phase != GT_EXT_RUNTIME_CREATED) {
            gt_ext_runtime_info_t info;
            CHECK(gt_ext_runtime_info(runtime, &info));
            fprintf(stderr,
                    "Runtime phase %d error %d session %d\n",
                    phase,
                    info.error,
                    info.session_end);
            CHECK(false);
        }
        if (phase == GT_EXT_RUNTIME_ACTIVE && !handle) {
            CHECK(gt_ext_session_request(
                      native_session, &lease, "codex.thread.start", "{}", 2, 20000, &handle) ==
                  GT_EXT_RPC_OK);
        }
        if (handle && !started) {
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(native_session, &lease, handle, &result) ==
                  GT_EXT_RPC_OK);
            if (result.confirmed) {
                if (result.state != GT_EXT_RPC_SUCCEEDED) {
                    fprintf(
                        stderr, "Thread result %.*s\n", (int)result.outcome_size, result.outcome);
                }
                CHECK(result.state == GT_EXT_RPC_SUCCEEDED && strstr(result.outcome, "threadID"));
                CHECK(gt_ext_session_forget_request(native_session, &lease, handle) ==
                      GT_EXT_RPC_OK);
                started = true;
            }
        }
        if (phase == GT_EXT_RUNTIME_ACTIVE) {
            gt_ext_rpc_request_t request;
            gt_ext_rpc_status_t status =
                gt_ext_session_take_request(native_session, &lease, &request);
            CHECK(status == GT_EXT_RPC_WAIT || status == GT_EXT_RPC_OK);
            if (status == GT_EXT_RPC_OK) {
                CHECK(!strcmp(request.method, "codex.event") &&
                      strstr(request.input, "\"event\":"));
                if (strstr(request.input, "thread/started")) {
                    event_seen = true;
                }
                const char outcome[] = "{\"kind\":\"success\",\"value\":{\"accepted\":true}}";
                CHECK(gt_ext_session_respond(
                          native_session, &lease, request.ticket, outcome, sizeof(outcome) - 1) ==
                      GT_EXT_RPC_OK);
            }
        }
        idle();
    }
    CHECK(started && event_seen);
    const char missing_change[] = "{\"snapshotID\":\"change-missing\",\"offset\":0}";
    CHECK(gt_ext_session_request(native_session,
                                 &lease,
                                 "codex.change.read",
                                 missing_change,
                                 sizeof(missing_change) - 1,
                                 10000,
                                 &handle) == GT_EXT_RPC_OK);
    bool change_checked = false;
    while (now() < deadline && !change_checked) {
        CHECK(gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE);
        gt_ext_rpc_result_t result;
        CHECK(gt_ext_session_request_result(native_session, &lease, handle, &result) ==
              GT_EXT_RPC_OK);
        if (result.confirmed) {
            CHECK(result.state == GT_EXT_RPC_SUCCEEDED &&
                  strstr(result.outcome, "\"status\":\"expired\"") &&
                  strstr(result.outcome, "\"done\":false") &&
                  strstr(result.outcome, "\"content\":\"\""));
            CHECK(gt_ext_session_forget_request(native_session, &lease, handle) == GT_EXT_RPC_OK);
            change_checked = true;
        }
        idle();
    }
    CHECK(change_checked);
    gt_ext_runtime_stop(runtime);
    gt_ext_close_t closed;
    do {
        closed = gt_ext_runtime_close(runtime);
        idle();
    } while (closed == GT_EXT_DRAINING && now() < deadline);
    CHECK(closed == GT_EXT_RELEASED && gt_ext_runtime_destroy(&runtime) == GT_OK);
    int status = 0;
    pid_t reaped = 0;
    while (now() < deadline && !(reaped = waitpid(child, &status, WNOHANG))) {
        idle();
    }
    /* Native Runtime close tears down the route. The peer must report loss (7),
     * while still completing its own Runtime/process cleanup (cleanup failure is 5). */
    pid_t expected_child = child;
    if (reaped == child) {
        child = 0;
    }
    CHECK(reaped == expected_child && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    puts("PASS Codex checked package / Native Client Runtime / gterm Executor / real CLI thread "
         "and event / explicit expired change status / close");
    return 0;
}
