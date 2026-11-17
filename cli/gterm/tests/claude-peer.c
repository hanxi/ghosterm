#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t child = -1;
static volatile sig_atomic_t interrupted;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            fprintf(stderr, "FAIL claude-peer:%d: %s\n", __LINE__, #value);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint64_t now(void) {
    struct timespec time;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &time));
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static void idle(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static void cancel(int signal) {
    interrupted = signal;
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

int main(int argc, char **argv) {
    CHECK(argc >= 10 && argc <= 32 && !atexit(cleanup));
    struct sigaction action = {0};
    action.sa_handler = cancel;
    CHECK(!sigaction(SIGINT, &action, NULL) && !sigaction(SIGTERM, &action, NULL));
    int up[2], down[2];
    CHECK(!pipe(up) && !pipe(down));
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(dup2(up[0], 0) == 0 && dup2(down[1], 1) == 1);
        close(up[0]);
        close(up[1]);
        close(down[0]);
        close(down[1]);
        char *args[160];
        size_t n = 0;
#define ARG(value) args[n++] = (char *)(value)
        ARG(argv[1]);
        ARG("relay");
        ARG("--stdio");
        ARG("--trust-development-digest");
        ARG(argv[3]);
        ARG("--client-digest");
        ARG(argv[3]);
        ARG("--root");
        ARG("workspace");
        ARG(argv[9]);
        ARG("--process-program");
        ARG("claude-node");
        ARG(argv[4]);
        ARG(argv[5]);
        ARG(argv[6]);
        ARG("--process-argument");
        ARG("claude-node");
        ARG(argv[7]);
        ARG("--process-argument");
        ARG("claude-node");
        ARG("--profile");
        ARG("--process-argument");
        ARG("claude-node");
        ARG(argv[8]);
        ARG("--process-environment");
        ARG("CLAUDE_CONFIG_DIR");
        ARG("--process-environment");
        ARG("CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC");
        for (int i = 10; i < argc; i++) {
            ARG("--process-environment");
            ARG(argv[i]);
        }
        const char *methods[] = {"claude.input.open",
                                 "claude.input.append",
                                 "claude.input.seal",
                                 "claude.input.discard",
                                 "claude.call",
                                 "claude.response.read",
                                 "claude.response.forget",
                                 "claude.status"};
        for (size_t i = 0; i < 8; i++) {
            ARG("--method");
            ARG(methods[i]);
        }
        ARG("--");
        ARG(argv[2]);
        ARG("ai claude");
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
                                      .peer = {GT_EXT_PEER_LOCAL, "owned-claude-peer"}};
    config.required_count = config.available_count = 3;
    strcpy(config.plugin_id, "org.ghosterm.ai-claude");
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
    gt_ext_rpc_method_t methods[8] = {0};
    const char *names[] = {"claude.input.open",
                           "claude.input.append",
                           "claude.input.seal",
                           "claude.input.discard",
                           "claude.call",
                           "claude.response.read",
                           "claude.response.forget",
                           "claude.status"};
    for (size_t i = 0; i < 8; i++) {
        strcpy(methods[i].method, names[i]);
        strcpy(methods[i].capability, "session.rpc");
        methods[i].receiver = GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    gt_ext_session_t *session = NULL;
    CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &config, methods, 8, &session) == GT_OK);
    uint64_t deadline = now() + 10000;
    gt_ext_lease_t lease;
    bool active = false;
    while (!interrupted && now() < deadline) {
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
    puts("READY");
    fflush(stdout);
    char method[130], input[GT_EXT_RPC_BODY_MAX + 2];
    while (!interrupted && fgets(method, sizeof(method), stdin)) {
        size_t length = strlen(method);
        CHECK(length && method[length - 1] == '\n');
        method[length - 1] = 0;
        CHECK(fgets(input, sizeof(input), stdin));
        length = strlen(input);
        CHECK(length && input[length - 1] == '\n');
        input[--length] = 0;
        uint64_t handle = 0;
        CHECK(gt_ext_session_request(session, &lease, method, input, length, 30000, &handle) ==
              GT_EXT_RPC_OK);
        deadline = now() + 35000;
        bool done = false;
        while (!interrupted && now() < deadline) {
            gt_ext_session_step(session, 0);
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(session, &lease, handle, &result) == GT_EXT_RPC_OK);
            if (result.confirmed || result.state >= GT_EXT_RPC_SUCCEEDED) {
                CHECK(fwrite(result.outcome, 1, result.outcome_size, stdout) ==
                      result.outcome_size);
                puts("");
                fflush(stdout);
                CHECK(gt_ext_session_forget_request(session, &lease, handle) == GT_EXT_RPC_OK);
                done = true;
                break;
            }
            idle();
        }
        CHECK(done);
    }
    gt_ext_session_revoke(session);
    deadline = now() + 10000;
    int status = 0;
    bool ended = false;
    while (!interrupted && now() < deadline) {
        gt_ext_session_step(session, 1);
        pid_t waited = waitpid(child, &status, WNOHANG);
        CHECK(waited >= 0);
        if (waited == child) {
            child = -1;
            ended = true;
            break;
        }
        idle();
    }
    CHECK(ended && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    bool released = false;
    for (unsigned i = 0; i < 3000; i++) {
        if (gt_ext_session_close(session, 1) == GT_EXT_RELEASED) {
            released = true;
            break;
        }
        idle();
    }
    CHECK(released && gt_ext_session_destroy(&session) == GT_OK);
    CHECK(!interrupted);
    return 0;
}
