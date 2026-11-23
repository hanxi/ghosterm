/* Actual public package/Session/Runtime path, no Native renderer simulation. */
#define main table_session_fixture_main
#include "../../core/tests/test_ext_session.c"
#undef main
#include <inttypes.h>

static void table_check(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "ASSERT: %s\n", message);
        exit(1);
    }
}

static gt_ext_rpc_result_t table_request(ext_pair_t *pair,
                                         gt_ext_runtime_t *runtime,
                                         const gt_ext_lease_t *lease,
                                         const char *method,
                                         const char *input) {
    uint64_t handle = 0;
    table_check(gt_ext_session_request(
                    pair->h, lease, method, input, strlen(input), 1000, &handle) == GT_EXT_RPC_OK,
                "table request admission");
    gt_ext_rpc_result_t result = {0};
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair->h, 0);
        gt_ext_runtime_step(runtime);
        gt_ext_runtime_info_t runtime_info;
        table_check(gt_ext_runtime_info(runtime, &runtime_info), "table request Runtime status");
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
            table_check(false, "table Runtime failed while processing request");
        }
        table_check(gt_ext_session_request_result(pair->h, lease, handle, &result) == GT_EXT_RPC_OK,
                    "table result observation");
        if (result.confirmed) {
            printf("%.*s\n", (int)result.outcome_size, result.outcome);
            table_check(gt_ext_session_forget_request(pair->h, lease, handle) == GT_EXT_RPC_OK,
                        "table request retirement");
            return result;
        }
    }
    table_check(false, "table request did not complete");
    return result;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        return 2;
    }
    bool client = !strcmp(argv[2], "client");
    int directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    table_check(directory >= 0, "table package directory");
    gt_ext_package_config_t capture = {directory, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    table_check(gt_ext_package_open(&capture, &package) == GT_OK, "table package capture");
    close(directory);
    gt_ext_package_info_t info;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_package_step(package);
        table_check(gt_ext_package_info(package, &info), "table package observation");
        if (info.phase != GT_EXT_PACKAGE_CAPTURING) {
            break;
        }
    }
    table_check(info.phase == GT_EXT_PACKAGE_CAPTURED, "table fixed bytes captured");
    const char *executor = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                           "\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}";
    const char *host = "{\"side\":\"client\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                       "\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}";
    gt_ext_package_check_config_t facts = {.host_json = client ? host : executor};
    gt_ext_package_phase_t checked = gt_ext_package_check(package, &facts);
    if (client) {
        table_check(gt_ext_package_info(package, &info) && checked == GT_EXT_PACKAGE_FAILED &&
                        info.error == GT_EXT_PACKAGE_UNSUPPORTED,
                    "missing Native table service must be unsupported");
        puts("CLIENT_UNSUPPORTED");
        table_check(gt_ext_package_destroy(&package) == GT_OK, "table rejected package release");
        return 0;
    }
    table_check(checked == GT_EXT_PACKAGE_CHECKED, "table Executor compatibility");
    gt_ext_package_metadata_t metadata;
    table_check(gt_ext_package_metadata(package, &metadata), "table package metadata");
    gt_ext_session_config_t config = ext_config();
    strcpy(config.plugin_id, metadata.id);
    strcpy(config.plugin_version, metadata.version);
    strcpy(config.client_digest, metadata.package_digest);
    strcpy(config.executor_digest, metadata.package_digest);
    config.required_count = config.available_count = 1;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    config.features = GT_EXT_FEATURE_REQUESTS;
    gt_ext_rpc_method_t methods[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        strcpy(methods[i].method, i ? "view.table.page" : "view.table.describe");
        strcpy(methods[i].capability, "session.rpc");
        methods[i].receiver = GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    int sockets[2];
    table_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "table socket pair");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    table_check(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, methods, 2, &pair.h) == GT_OK &&
                    gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, methods, 2, &pair.e) ==
                        GT_OK,
                "table Session creation");
    gt_ext_lease_t lease = ext_allow(&pair);
    const char *arguments[] = {"--count", argv[3]};
    gt_ext_invocation_t invocation = {.command = "table", .argv = arguments, .argc = 2};
    gt_ext_runtime_config_t options = {.role = GT_EXT_EXECUTOR, .invocation = &invocation};
    gt_ext_runtime_t *runtime = NULL;
    table_check(gt_ext_runtime_open_package(&pair.e, package, &options, true, &runtime) == GT_OK &&
                    runtime && !pair.e,
                "actual table Runtime adoption");
    gt_ext_runtime_info_t state;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair.h, 0);
        gt_ext_runtime_step(runtime);
        table_check(gt_ext_runtime_info(runtime, &state), "table Runtime observation");
        if (state.phase != GT_EXT_RUNTIME_CREATED && state.phase != GT_EXT_RUNTIME_STARTING) {
            break;
        }
    }
    table_check(state.phase == GT_EXT_RUNTIME_ACTIVE, "table Runtime activation");
    table_request(&pair, runtime, &lease, "view.table.describe", "{}");
    const unsigned offsets[] = {0, 999984, 999999, 1000000};
    for (size_t i = 0; i < (!strcmp(argv[3], "0") ? 1u : 4u); i++) {
        char input[256];
        int n =
            snprintf(input,
                     sizeof(input),
                     "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\",\"offset\":%u,\"limit\":16}",
                     lease.instance,
                     lease.epoch,
                     offsets[i]);
        table_check(n > 0 && (size_t)n < sizeof(input), "table input bound");
        table_request(&pair, runtime, &lease, "view.table.page", input);
    }
    char stale[256];
    snprintf(stale,
             sizeof(stale),
             "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\",\"offset\":0,\"limit\":16}",
             lease.instance,
             lease.epoch + 1);
    table_request(&pair, runtime, &lease, "view.table.page", stale);
    gt_ext_runtime_stop(runtime);
    bool closed = false;
    for (unsigned i = 0; i < 2000; i++) {
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            closed = true;
            break;
        }
    }
    table_check(closed && gt_ext_runtime_destroy(&runtime) == GT_OK, "table Runtime release");
    ext_drop(&pair.h);
    table_check(gt_ext_package_destroy(&package) == GT_OK, "table package release");
    return 0;
}
