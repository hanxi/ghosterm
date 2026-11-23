/* Actual public package/Session/Runtime path, no Native renderer simulation. */
#define main task_session_fixture_main
#include "../../core/tests/test_ext_session.c"
#undef main
#include <inttypes.h>

static void task_check(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "ASSERT: %s\n", message);
        exit(1);
    }
}

static gt_ext_rpc_result_t task_request(ext_pair_t *pair,
                                        gt_ext_runtime_t *runtime,
                                        const gt_ext_lease_t *lease,
                                        const char *method,
                                        const char *input) {
    uint64_t handle = 0;
    task_check(gt_ext_session_request(
                   pair->h, lease, method, input, strlen(input), 1000, &handle) == GT_EXT_RPC_OK,
               "task request admission");
    gt_ext_rpc_result_t result = {0};
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair->h, 0);
        gt_ext_runtime_step(runtime);
        gt_ext_runtime_info_t runtime_info;
        task_check(gt_ext_runtime_info(runtime, &runtime_info), "task request Runtime status");
        if (runtime_info.phase == GT_EXT_RUNTIME_FAILED) {
            fprintf(stderr,
                    "RUNTIME phase=%u error=%u session=%u cleanup=%d steps=%" PRIu64
                    " jobs=%" PRIu64 "\n",
                    (unsigned)runtime_info.phase,
                    (unsigned)runtime_info.error,
                    (unsigned)runtime_info.session_end,
                    runtime_info.cleanup_error,
                    runtime_info.steps,
                    runtime_info.jobs);
            task_check(false, "task Runtime failed while processing request");
        }
        task_check(gt_ext_session_request_result(pair->h, lease, handle, &result) == GT_EXT_RPC_OK,
                   "task result observation");
        if (result.confirmed) {
            printf("%.*s\n", (int)result.outcome_size, result.outcome);
            task_check(gt_ext_session_forget_request(pair->h, lease, handle) == GT_EXT_RPC_OK,
                       "task request retirement");
            return result;
        }
    }
    task_check(false, "task request did not complete");
    return result;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        return 2;
    }
    bool client = !strcmp(argv[2], "client");
    int directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    task_check(directory >= 0, "task package directory");
    gt_ext_package_config_t capture = {directory, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    task_check(gt_ext_package_open(&capture, &package) == GT_OK, "task package capture");
    close(directory);
    gt_ext_package_info_t info;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_package_step(package);
        task_check(gt_ext_package_info(package, &info), "task package observation");
        if (info.phase != GT_EXT_PACKAGE_CAPTURING) {
            break;
        }
    }
    task_check(info.phase == GT_EXT_PACKAGE_CAPTURED, "task fixed bytes captured");
    const char *executor = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                           "\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}";
    const char *host = "{\"side\":\"client\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                       "\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}";
    gt_ext_package_check_config_t facts = {.host_json = client ? host : executor};
    gt_ext_package_phase_t checked = gt_ext_package_check(package, &facts);
    if (client) {
        task_check(gt_ext_package_info(package, &info) && checked == GT_EXT_PACKAGE_FAILED &&
                       info.error == GT_EXT_PACKAGE_UNSUPPORTED,
                   "missing Native task service must be unsupported");
        puts("CLIENT_UNSUPPORTED");
        task_check(gt_ext_package_destroy(&package) == GT_OK, "task rejected package release");
        return 0;
    }
    if (checked != GT_EXT_PACKAGE_CHECKED && gt_ext_package_info(package, &info)) {
        fprintf(stderr,
                "PACKAGE phase=%u error=%u code=%s\n",
                (unsigned)info.phase,
                (unsigned)info.error,
                info.code);
    }
    task_check(checked == GT_EXT_PACKAGE_CHECKED, "task Executor compatibility");
    gt_ext_package_metadata_t metadata;
    task_check(gt_ext_package_metadata(package, &metadata), "task package metadata");
    gt_ext_session_config_t config = ext_config();
    uint64_t controlled_clock = 1000;
    if (!strcmp(argv[3], "lost-ack")) {
        config.clock = ext_clock;
        config.clock_context = &controlled_clock;
    }
    strcpy(config.plugin_id, metadata.id);
    strcpy(config.plugin_version, metadata.version);
    strcpy(config.client_digest, metadata.package_digest);
    strcpy(config.executor_digest, metadata.package_digest);
    config.required_count = config.available_count = 1;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    config.features = GT_EXT_FEATURE_REQUESTS;
    gt_ext_rpc_method_t methods[3] = {0};
    for (size_t i = 0; i < 3; i++) {
        strcpy(methods[i].method,
               i == 0   ? "view.task.describe"
               : i == 1 ? "view.task.read"
                        : "view.task.cancel");
        strcpy(methods[i].capability, "session.rpc");
        methods[i].receiver = GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    int sockets[2];
    task_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "task socket pair");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    task_check(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, methods, 3, &pair.h) == GT_OK &&
                   gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, methods, 3, &pair.e) ==
                       GT_OK,
               "task Session creation");
    gt_ext_lease_t lease = ext_allow(&pair);
    bool finish = !strcmp(argv[3], "complete") || !strcmp(argv[3], "empty") ||
                  !strcmp(argv[3], "one") || !strcmp(argv[3], "two") ||
                  !strcmp(argv[3], "late-cancel");
    unsigned limit = !strcmp(argv[3], "complete")      ? 10000
                     : !strcmp(argv[3], "empty")       ? 0
                     : !strcmp(argv[3], "one")         ? 1
                     : !strcmp(argv[3], "two")         ? 2
                     : !strcmp(argv[3], "late-cancel") ? 100
                                                       : 1000000;
    task_check(finish || !strcmp(argv[3], "cancel") || !strcmp(argv[3], "progress") ||
                   !strcmp(argv[3], "duplicate") || !strcmp(argv[3], "lost-ack"),
               "known task scenario");
    char number[32];
    snprintf(number, sizeof(number), "%u", limit);
    const char *arguments[] = {"--limit", number};
    gt_ext_invocation_t invocation = {.command = "task primes", .argv = arguments, .argc = 2};
    gt_ext_runtime_config_t options = {.role = GT_EXT_EXECUTOR, .invocation = &invocation};
    gt_ext_runtime_t *runtime = NULL;
    task_check(gt_ext_runtime_open_package(&pair.e, package, &options, true, &runtime) == GT_OK &&
                   runtime && !pair.e,
               "actual task Runtime adoption");
    gt_ext_runtime_info_t state;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair.h, 0);
        gt_ext_runtime_step(runtime);
        task_check(gt_ext_runtime_info(runtime, &state), "task Runtime observation");
        if (state.phase != GT_EXT_RUNTIME_CREATED && state.phase != GT_EXT_RUNTIME_STARTING) {
            break;
        }
    }
    if (state.phase != GT_EXT_RUNTIME_ACTIVE) {
        fprintf(stderr,
                "ACTIVATION phase=%u error=%u steps=%" PRIu64 " jobs=%" PRIu64 "\n",
                (unsigned)state.phase,
                (unsigned)state.error,
                state.steps,
                state.jobs);
    }
    task_check(state.phase == GT_EXT_RUNTIME_ACTIVE, "task Runtime activation");
    task_request(&pair, runtime, &lease, "view.task.describe", "{}");
    char read[256], cancel[320];
    int n = snprintf(read,
                     sizeof(read),
                     "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\"}",
                     lease.instance,
                     lease.epoch);
    task_check(n > 0 && (size_t)n < sizeof(read), "task query bound");
    n = snprintf(cancel,
                 sizeof(cancel),
                 "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\",\"requestId\":\"cancel-1\"}",
                 lease.instance,
                 lease.epoch);
    task_check(n > 0 && (size_t)n < sizeof(cancel), "task cancellation bound");
    gt_ext_rpc_result_t observed = task_request(&pair, runtime, &lease, "view.task.read", read);
    if (finish) {
        bool succeeded = strstr(observed.outcome, "\"phase\":\"succeeded\"") != NULL;
        for (unsigned i = 0; i < 2000 && !succeeded; i++) {
            gt_ext_runtime_step(runtime);
            if ((i % 32) == 31) {
                observed = task_request(&pair, runtime, &lease, "view.task.read", read);
                succeeded = strstr(observed.outcome, "\"phase\":\"succeeded\"") != NULL;
            }
        }
        task_check(succeeded, "task did not finish within original observation bound");
        if (!strcmp(argv[3], "late-cancel")) {
            task_request(&pair, runtime, &lease, "view.task.cancel", cancel);
            task_request(&pair, runtime, &lease, "view.task.read", read);
        }
    } else {
        if (!strcmp(argv[3], "progress")) {
            for (unsigned i = 0; i < 32; i++) {
                gt_ext_runtime_step(runtime);
            }
            task_request(&pair, runtime, &lease, "view.task.read", read);
        }
        if (!strcmp(argv[3], "lost-ack")) {
            uint64_t handle = 0;
            task_check(
                gt_ext_session_request(
                    pair.h, &lease, "view.task.cancel", cancel, strlen(cancel), 1000, &handle) ==
                    GT_EXT_RPC_OK,
                "uncertain cancel admission");
            gt_ext_rpc_result_t pending;
            bool sent = false;
            for (unsigned i = 0; i < 2000; i++) {
                gt_ext_session_step(pair.h, 0);
                task_check(gt_ext_session_request_result(pair.h, &lease, handle, &pending) ==
                               GT_EXT_RPC_OK,
                           "uncertain cancel transmission");
                if (pending.state == GT_EXT_RPC_PENDING) {
                    sent = true;
                    break;
                }
            }
            task_check(sent, "cancel must actually be sent before uncertainty");
            // Let the real Executor apply cancellation while the Host does not
            // consume its acknowledgement. Advance only the Session test clock.
            for (unsigned i = 0; i < 80; i++) {
                gt_ext_runtime_step(runtime);
            }
            controlled_clock += 1000;
            task_check(gt_ext_session_request_result(pair.h, &lease, handle, &pending) ==
                               GT_EXT_RPC_OK &&
                           pending.state == GT_EXT_RPC_UNKNOWN && !pending.confirmed &&
                           pending.deadline_elapsed,
                       "unobserved cancellation ack is unknown");
            printf("%.*s\n", (int)pending.outcome_size, pending.outcome);
            task_request(&pair, runtime, &lease, "view.task.read", read);
            task_check(gt_ext_session_forget_request(pair.h, &lease, handle) == GT_EXT_RPC_OK,
                       "uncertain observation retirement");
        } else {
            task_request(&pair, runtime, &lease, "view.task.cancel", cancel);
            task_request(&pair, runtime, &lease, "view.task.read", read);
            if (!strcmp(argv[3], "duplicate")) {
                task_request(&pair, runtime, &lease, "view.task.cancel", cancel);
                task_request(&pair, runtime, &lease, "view.task.read", read);
                char other[320];
                snprintf(other,
                         sizeof(other),
                         "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64
                         "\",\"requestId\":\"cancel-2\"}",
                         lease.instance,
                         lease.epoch);
                task_request(&pair, runtime, &lease, "view.task.cancel", other);
                task_request(&pair, runtime, &lease, "view.task.read", read);
            }
        }
    }
    char stale[256];
    snprintf(stale,
             sizeof(stale),
             "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\"}",
             lease.instance,
             lease.epoch + 1);
    task_request(&pair, runtime, &lease, "view.task.read", stale);
    gt_ext_runtime_stop(runtime);
    bool closed = false;
    for (unsigned i = 0; i < 2000; i++) {
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            closed = true;
            break;
        }
    }
    task_check(closed && gt_ext_runtime_destroy(&runtime) == GT_OK, "task Runtime release");
    ext_drop(&pair.h);
    task_check(gt_ext_package_destroy(&package) == GT_OK, "task package release");
    return 0;
}
