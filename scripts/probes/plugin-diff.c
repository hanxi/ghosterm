/* Actual public package/Session/Runtime path, no Native renderer simulation. */
#define main diff_session_fixture_main
#include "../../core/tests/test_ext_session.c"
#undef main
#include <inttypes.h>

static void diff_check(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "ASSERT: %s\n", message);
        exit(1);
    }
}

static gt_ext_rpc_result_t diff_request(ext_pair_t *pair,
                                        gt_ext_runtime_t *runtime,
                                        const gt_ext_lease_t *lease,
                                        const char *method,
                                        const char *input) {
    uint64_t handle = 0;
    diff_check(gt_ext_session_request(
                   pair->h, lease, method, input, strlen(input), 1000, &handle) == GT_EXT_RPC_OK,
               "diff request admission");
    gt_ext_rpc_result_t result = {0};
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair->h, 0);
        gt_ext_runtime_step(runtime);
        gt_ext_runtime_info_t runtime_info;
        diff_check(gt_ext_runtime_info(runtime, &runtime_info), "diff request Runtime status");
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
            diff_check(false, "diff Runtime failed while processing request");
        }
        diff_check(gt_ext_session_request_result(pair->h, lease, handle, &result) == GT_EXT_RPC_OK,
                   "diff result observation");
        if (result.confirmed) {
            printf("%.*s\n", (int)result.outcome_size, result.outcome);
            diff_check(gt_ext_session_forget_request(pair->h, lease, handle) == GT_EXT_RPC_OK,
                       "diff request retirement");
            return result;
        }
    }
    diff_check(false, "diff request did not complete");
    return result;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        return 2;
    }
    bool client = !strcmp(argv[2], "client");
    int directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    diff_check(directory >= 0, "diff package directory");
    gt_ext_package_config_t capture = {directory, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    diff_check(gt_ext_package_open(&capture, &package) == GT_OK, "diff package capture");
    close(directory);
    gt_ext_package_info_t info;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_package_step(package);
        diff_check(gt_ext_package_info(package, &info), "diff package observation");
        if (info.phase != GT_EXT_PACKAGE_CAPTURING) {
            break;
        }
    }
    diff_check(info.phase == GT_EXT_PACKAGE_CAPTURED, "diff fixed bytes captured");
    const char *executor = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                           "\"runtimes\":[\"portable-js\"],\"capabilities\":[\"session.rpc\"]}";
    const char *host = "{\"side\":\"client\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                       "\"runtimes\":[\"portable-js\"],\"capabilities\":[\"session.rpc\"]}";
    gt_ext_package_check_config_t facts = {.host_json = client ? host : executor};
    gt_ext_package_phase_t checked = gt_ext_package_check(package, &facts);
    if (client) {
        diff_check(gt_ext_package_info(package, &info) && checked == GT_EXT_PACKAGE_FAILED &&
                       info.error == GT_EXT_PACKAGE_UNSUPPORTED,
                   "missing Native diff service must be unsupported");
        puts("CLIENT_UNSUPPORTED");
        diff_check(gt_ext_package_destroy(&package) == GT_OK, "diff rejected package release");
        return 0;
    }
    if (checked != GT_EXT_PACKAGE_CHECKED && gt_ext_package_info(package, &info)) {
        fprintf(stderr,
                "PACKAGE phase=%u error=%u code=%s\n",
                (unsigned)info.phase,
                (unsigned)info.error,
                info.code);
    }
    diff_check(checked == GT_EXT_PACKAGE_CHECKED, "diff Executor compatibility");
    gt_ext_package_metadata_t metadata;
    diff_check(gt_ext_package_metadata(package, &metadata), "diff package metadata");
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
        strcpy(methods[i].method, i ? "view.diff.page" : "view.diff.describe");
        strcpy(methods[i].capability, "session.rpc");
        methods[i].receiver = GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    int sockets[2];
    diff_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "diff socket pair");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    diff_check(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, methods, 2, &pair.h) == GT_OK &&
                   gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, methods, 2, &pair.e) ==
                       GT_OK,
               "diff Session creation");
    gt_ext_lease_t lease = ext_allow(&pair);
    const char *before = "\"alpha\\r\\nbeta\\nlast\"";
    const char *after = "\"alpha\\r\\ngamma\\nlast\\n\"";
    unsigned total = 5;
    char maximum_before[3073], maximum_after[3073];
    if (!strcmp(argv[3], "empty")) {
        before = after = "\"\"";
        total = 0;
    } else if (!strcmp(argv[3], "equal")) {
        before = after = "\"same\\n\"";
        total = 1;
    } else if (!strcmp(argv[3], "unicode")) {
        before = "\"é\\n😀\"";
        after = "\"é\\n😀\\n\"";
        total = 4;
    } else if (!strcmp(argv[3], "maximum") || !strcmp(argv[3], "interleaved")) {
        maximum_before[0] = maximum_after[0] = '"';
        size_t at = 1;
        for (unsigned i = 0; i < 128; i++) {
            memcpy(maximum_before + at, "a\\n", 3);
            memcpy(maximum_after + at, "b\\n", 3);
            if (!strcmp(argv[3], "interleaved") && (i & 1)) {
                maximum_before[at] = 'b';
                maximum_after[at] = 'a';
            }
            at += 3;
        }
        maximum_before[at] = maximum_after[at] = '"';
        maximum_before[at + 1] = maximum_after[at + 1] = 0;
        before = maximum_before;
        after = maximum_after;
        total = !strcmp(argv[3], "interleaved") ? 129 : 256;
    } else if (!strcmp(argv[3], "wide")) {
        size_t at = 0;
        maximum_before[at++] = '"';
        for (unsigned line = 0; line < 3; line++) {
            for (unsigned column = 0; column < 128; column++) {
                memcpy(maximum_before + at, "😀", 4);
                at += 4;
            }
            memcpy(maximum_before + at, "\\n", 2);
            at += 2;
        }
        maximum_before[at++] = '"';
        maximum_before[at] = 0;
        before = after = maximum_before;
        total = 3;
    } else {
        diff_check(!strcmp(argv[3], "change"), "known diff scenario");
    }
    const char *arguments[] = {"--before", before, "--after", after};
    gt_ext_invocation_t invocation = {.command = "diff", .argv = arguments, .argc = 4};
    gt_ext_runtime_config_t options = {.role = GT_EXT_EXECUTOR, .invocation = &invocation};
    gt_ext_runtime_t *runtime = NULL;
    diff_check(gt_ext_runtime_open_package(&pair.e, package, &options, true, &runtime) == GT_OK &&
                   runtime && !pair.e,
               "actual diff Runtime adoption");
    gt_ext_runtime_info_t state;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair.h, 0);
        gt_ext_runtime_step(runtime);
        diff_check(gt_ext_runtime_info(runtime, &state), "diff Runtime observation");
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
    diff_check(state.phase == GT_EXT_RUNTIME_ACTIVE, "diff Runtime activation");
    diff_request(&pair, runtime, &lease, "view.diff.describe", "{}");
    for (unsigned offset = 0;; offset = total - offset < 3 ? total : offset + 3) {
        char input[256];
        int n =
            snprintf(input,
                     sizeof(input),
                     "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\",\"offset\":%u,\"limit\":3}",
                     lease.instance,
                     lease.epoch,
                     offset);
        diff_check(n > 0 && (size_t)n < sizeof(input), "diff input bound");
        diff_request(&pair, runtime, &lease, "view.diff.page", input);
        if (offset == total) {
            break;
        }
    }
    char stale[256];
    snprintf(stale,
             sizeof(stale),
             "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\",\"offset\":0,\"limit\":3}",
             lease.instance,
             lease.epoch + 1);
    diff_request(&pair, runtime, &lease, "view.diff.page", stale);
    gt_ext_runtime_stop(runtime);
    bool closed = false;
    for (unsigned i = 0; i < 2000; i++) {
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            closed = true;
            break;
        }
    }
    diff_check(closed && gt_ext_runtime_destroy(&runtime) == GT_OK, "diff Runtime release");
    ext_drop(&pair.h);
    diff_check(gt_ext_package_destroy(&package) == GT_OK, "diff package release");
    return 0;
}
