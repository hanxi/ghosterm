#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL codex-peer:%d: %s\n", __LINE__, #x);                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static pid_t child = -1;
static volatile sig_atomic_t stopped;

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void idle(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static void cancel(int number) {
    stopped = number;
}

static void cleanup(void) {
    if (child <= 0) {
        return;
    }
    kill(child, SIGTERM);
    for (unsigned i = 0; i < 5000; i++) {
        if (waitpid(child, NULL, WNOHANG) == child) {
            child = -1;
            return;
        }
        idle();
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    child = -1;
}

static void emit(const char *kind, const char *bytes, size_t count) {
    CHECK(printf("{\"%s\":", kind) > 0 && fwrite(bytes, 1, count, stdout) == count &&
          puts("}") >= 0 && !fflush(stdout));
}

/* Explicit interactive Native owner, for opted-in real-model integration only.
 * The Python controller supplies bounded RPCs and handles untrusted events.
 * No credentials are passed here; no hidden model call is made by this helper. */
int main(int argc, char **argv) {
    CHECK(argc >= 8 && argc <= 32 && !atexit(cleanup));
    struct sigaction action = {.sa_handler = cancel};
    CHECK(!sigaction(SIGINT, &action, NULL) && !sigaction(SIGTERM, &action, NULL));
    signal(SIGPIPE, SIG_IGN);
    int directory = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(directory >= 0);
    gt_ext_package_config_t capture = {directory, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    CHECK(gt_ext_package_open(&capture, &package) == GT_OK && !close(directory));
    while (gt_ext_package_step(package) == GT_EXT_PACKAGE_CAPTURING) {
    }
    gt_ext_package_check_config_t check = {
        .host_json = "{\"side\":\"client\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],\"runtimes\":"
                     "[\"lua\"],\"capabilities\":[\"session.rpc\"]}"};
    gt_ext_package_metadata_t metadata;
    gt_ext_package_phase_t checked = gt_ext_package_check(package, &check);
    if (checked != GT_EXT_PACKAGE_CHECKED) {
        gt_ext_package_info_t info;
        if (gt_ext_package_info(package, &info)) {
            fprintf(stderr, "gterm: live package check %s\n", info.code);
        }
    }
    CHECK(checked == GT_EXT_PACKAGE_CHECKED && gt_ext_package_metadata(package, &metadata) &&
          !strcmp(metadata.package_digest, argv[3]));
    int up[2], down[2];
    CHECK(!pipe(up) && !pipe(down));
    child = fork();
    CHECK(child >= 0);
    const char *names[] = {"codex.thread.start",
                           "codex.turn.send",
                           "codex.turn.interrupt",
                           "codex.approval.respond",
                           "codex.change.read",
                           "codex.event"};
    if (!child) {
        CHECK(dup2(up[0], 0) == 0 && dup2(down[1], 1) == 1);
        close(up[0]);
        close(up[1]);
        close(down[0]);
        close(down[1]);
        char *args[180];
        size_t n = 0;
#define ARG(x) args[n++] = (char *)(x)
        ARG(argv[1]);
        ARG("relay");
        ARG("--stdio");
        ARG("--trust-development-digest");
        ARG(argv[3]);
        ARG("--client-digest");
        ARG(argv[3]);
        ARG("--root");
        ARG("workspace");
        ARG(argv[4]);
        ARG("--process-program");
        ARG("codex");
        ARG(argv[5]);
        ARG(argv[6]);
        ARG(argv[7]);
        for (int i = 8; i < argc; i++) {
            ARG("--process-argument");
            ARG("codex");
            ARG(argv[i]);
        }
        for (size_t i = 0; i < 6; i++) {
            ARG(i == 5 ? "--host-method" : "--method");
            ARG(names[i]);
        }
        ARG("--");
        ARG(argv[2]);
        ARG("ai codex");
        ARG("--ephemeral");
        if (!getenv("GHOSTERM_CODEX_LIVE_FILE_SCOPE")) {
            ARG("--read-only");
        }
        CHECK(n < sizeof(args) / sizeof(*args));
        args[n] = NULL;
        execv(argv[1], args);
        _exit(127);
    }
    close(up[0]);
    close(down[1]);
    gt_ext_channel_t *channel = NULL;
    CHECK(gt_ext_channel_open_fd(down[0], up[1], &channel) == GT_OK);
    gt_ext_session_config_t config = {.features = GT_EXT_FEATURE_REQUESTS,
                                      .budgets = {768, 65536, 262144},
                                      .handshake_ms = 5000,
                                      .consent_ms = 10000,
                                      .peer = {GT_EXT_PEER_LOCAL, "owned-codex-live-peer"},
                                      .required_count = 3,
                                      .available_count = 3};
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
    for (size_t i = 0; i < 6; i++) {
        strcpy(methods[i].method, names[i]);
        strcpy(methods[i].capability, "session.rpc");
        methods[i].receiver = i == 5 ? GT_EXT_HOST : GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    gt_ext_session_t *session = NULL;
    CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &config, methods, 6, &session) == GT_OK);
    uint64_t deadline = now() + 10000;
    gt_ext_lease_t lease;
    bool active = false;
    while (!stopped && now() < deadline) {
        gt_ext_session_step(session, 0);
        gt_ext_session_info_t info;
        CHECK(gt_ext_session_info(session, &info));
        if (info.phase == GT_EXT_SESSION_CONSENT) {
            CHECK(gt_ext_session_decide(session, info.consent_token, GT_EXT_DECISION_ALLOW) ==
                  GT_OK);
        }
        if (info.phase == GT_EXT_SESSION_ACTIVE) {
            CHECK(gt_ext_session_lease(session, &lease));
            active = true;
            break;
        }
        CHECK(info.phase < GT_EXT_SESSION_ENDING);
        idle();
    }
    CHECK(active);
    gt_ext_session_t *native = session;
    gt_ext_invocation_t invocation = {.command = "ai codex"};
    gt_ext_runtime_config_t runtime_config = {
        .role = GT_EXT_HOST, .cpu_ms = 100, .invocation = &invocation};
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open_package(&session, package, &runtime_config, true, &runtime) ==
              GT_OK &&
          !session);
    CHECK(gt_ext_package_destroy(&package) == GT_OK);
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    CHECK(flags >= 0 && !fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK));
    char control[GT_EXT_RPC_BODY_MAX + 260];
    size_t used = 0;
    uint64_t handle = 0;
    bool ready = false, eof = false;
    deadline = now() + 300000;
    while (!stopped && !eof && now() < deadline) {
        gt_ext_runtime_phase_t phase = gt_ext_runtime_step(runtime);
        CHECK(phase == GT_EXT_RUNTIME_ACTIVE || phase == GT_EXT_RUNTIME_STARTING ||
              phase == GT_EXT_RUNTIME_CREATED);
        if (phase == GT_EXT_RUNTIME_ACTIVE && !ready) {
            emit("ready", "true", 4);
            ready = true;
        }
        gt_ext_rpc_request_t event;
        while (gt_ext_session_take_request(native, &lease, &event) == GT_EXT_RPC_OK) {
            CHECK(!strcmp(event.method, "codex.event"));
            emit("event", event.input, event.input_size);
            const char reply[] = "{\"kind\":\"success\",\"value\":{\"accepted\":true}}";
            CHECK(gt_ext_session_respond(native, &lease, event.ticket, reply, sizeof(reply) - 1) ==
                  GT_EXT_RPC_OK);
        }
        if (handle) {
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(native, &lease, handle, &result) == GT_EXT_RPC_OK);
            if (result.confirmed || result.state >= GT_EXT_RPC_SUCCEEDED) {
                if (result.outcome_size) {
                    emit("result", result.outcome, result.outcome_size);
                } else {
                    const char unknown[] = "{\"kind\":\"unknown\"}";
                    emit("result", unknown, sizeof(unknown) - 1);
                }
                CHECK(gt_ext_session_forget_request(native, &lease, handle) == GT_EXT_RPC_OK);
                handle = 0;
            }
        }
        if (!handle && ready) {
            ssize_t count = read(STDIN_FILENO, control + used, sizeof(control) - 1 - used);
            if (count == 0) {
                eof = true;
            } else if (count < 0) {
                CHECK(errno == EAGAIN || errno == EINTR);
            } else {
                used += (size_t)count;
                control[used] = 0;
            }
            char *method_end = memchr(control, '\n', used);
            char *input_end =
                method_end ? memchr(method_end + 1, '\n', used - (size_t)(method_end + 1 - control))
                           : NULL;
            if (input_end) {
                size_t method_size = (size_t)(method_end - control),
                       input_size = (size_t)(input_end - method_end - 1);
                CHECK(method_size > 0 && method_size <= 128 && input_size <= GT_EXT_RPC_BODY_MAX);
                *method_end = 0;
                CHECK(gt_ext_session_request(
                          native, &lease, control, method_end + 1, input_size, 30000, &handle) ==
                      GT_EXT_RPC_OK);
                size_t consumed = (size_t)(input_end - control) + 1;
                used -= consumed;
                memmove(control, input_end + 1, used);
            }
            CHECK(used < sizeof(control) - 1);
        }
        idle();
    }
    CHECK(eof && !handle && !stopped && now() < deadline);
    gt_ext_runtime_stop(runtime);
    deadline = now() + 10000;
    gt_ext_close_t closed;
    do {
        closed = gt_ext_runtime_close(runtime);
        idle();
    } while (closed == GT_EXT_DRAINING && now() < deadline);
    CHECK(closed == GT_EXT_RELEASED && gt_ext_runtime_destroy(&runtime) == GT_OK);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, WNOHANG);
        CHECK(waited >= 0);
        idle();
    } while (!waited && now() < deadline);
    pid_t expected = child;
    if (waited == child) {
        child = -1;
    }
    CHECK(waited == expected && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    emit("closed", "true", 4);
    return 0;
}
