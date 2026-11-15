/* Developer fixture: public Core, real checked package and bounded QuickJS. */
#define main testkit_session_fixture_main
#include "../../core/tests/test_ext_session.c"
#undef main
#include <inttypes.h>
#include <signal.h>

static void kit_check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "ASSERT: %s\n", message);
        exit(1);
    }
}

static void
kit_bind(char output[GT_EXT_RPC_BODY_MAX + 1], const char *input, const gt_ext_lease_t *lease) {
    size_t at = 0;
    while (*input) {
        const char *value = NULL;
        size_t consumed = 0;
        char epoch[32];
        snprintf(epoch, sizeof(epoch), "%" PRIu64, lease->epoch);
        size_t token = 1;
        if (*input == '"') {
            while (input[token] && input[token] != '"') {
                if (input[token] == '\\' && input[token + 1]) {
                    token++;
                }
                token++;
            }
            kit_check(input[token] == '"', "complete JSON string");
            token++;
        }
        if (token == 15 && !strncmp(input, "\"${instanceId}\"", 15)) {
            value = lease->instance;
            consumed = 15;
        } else if (token == 10 && !strncmp(input, "\"${epoch}\"", 10)) {
            value = epoch;
            consumed = 10;
        }
        if (value) {
            size_t length = strlen(value);
            kit_check(at + length + 2 <= GT_EXT_RPC_BODY_MAX, "bound substituted input");
            output[at++] = '"';
            memcpy(output + at, value, length);
            at += length;
            output[at++] = '"';
            input += consumed;
        } else {
            kit_check(at + token <= GT_EXT_RPC_BODY_MAX, "bound request input");
            memcpy(output + at, input, token);
            at += token;
            input += token;
        }
    }
    output[at] = 0;
}

static gt_ext_runtime_t *kit_live, *kit_second;
static bool kit_business;
static volatile sig_atomic_t kit_interrupted;

static void kit_signal(int number) {
    kit_interrupted = number;
}

static void kit_emergency_cleanup(void) {
    gt_ext_runtime_t **owners[] = {&kit_live, &kit_second};
    for (size_t i = 0; i < 2; i++) {
        if (*owners[i]) {
            gt_ext_runtime_stop(*owners[i]);
        }
    }
    uint64_t until = ext_now() + 3000;
    for (size_t i = 0; i < 2; i++) {
        while (*owners[i] && ext_now() < until) {
            if (gt_ext_runtime_close(*owners[i]) == GT_EXT_RELEASED) {
                gt_ext_runtime_destroy(owners[i]);
                break;
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        if (*owners[i]) {
            fprintf(stderr, "ASSERT: development Runtime cleanup retained owner\n");
        }
    }
}

static void kit_yield(gt_ext_runtime_t *runtime) {
    if (kit_interrupted) {
        exit(128 + kit_interrupted);
    }
    gt_ext_runtime_info_t info;
    if (gt_ext_runtime_info(runtime, &info) && (info.external_process || kit_business)) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
}

static void kit_execution(gt_ext_runtime_execution_t *execution,
                          gt_ext_process_environment_t *environment,
                          char **fields,
                          bool node) {
    *execution = (gt_ext_runtime_execution_t){
        .stage_parent_fd = -1, .node_sdk_directory_fd = -1, .execution_authorized = true};
    execution->stage_parent_path = fields[0];
    execution->stage_parent_fd = open(fields[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    kit_check(execution->stage_parent_fd >= 0, "private execution parent");
    *environment = (gt_ext_process_environment_t){.name = "TMPDIR", .value = fields[0]};
    execution->environment = environment;
    execution->environment_count = 1;
    if (node) {
        execution->node_executable = fields[1];
        execution->node_sha256 = fields[2];
        execution->node_sdk_directory_fd =
            open(fields[3], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        execution->node_sdk_catalog_sha256 = fields[4];
        kit_check(execution->node_sdk_directory_fd >= 0, "explicit trusted Node SDK");
    }
}

static void kit_execution_inputs_close(const gt_ext_runtime_execution_t *execution) {
    if (execution->stage_parent_fd >= 0) {
        close(execution->stage_parent_fd);
    }
    if (execution->node_sdk_directory_fd >= 0) {
        close(execution->node_sdk_directory_fd);
    }
}

#include "plugin-testkit-process.inc"

#ifndef GT_TESTKIT_HELPERS_ONLY
#include "plugin-testkit-resource.inc"

static void kit_request(gt_ext_session_t *peer,
                        gt_ext_runtime_t *runtime,
                        const gt_ext_lease_t *lease,
                        const char *method,
                        const char *input,
                        const char *label) {
    char json[GT_EXT_RPC_BODY_MAX + 1];
    kit_bind(json, input, lease);
    uint64_t handle = 0;
    kit_check(gt_ext_session_request(peer, lease, method, json, strlen(json), 1000, &handle) ==
                  GT_EXT_RPC_OK,
              "request admission");
    for (unsigned step = 0; step < 2000; step++) {
        gt_ext_session_step(peer, 0);
        gt_ext_runtime_step(runtime);
        kit_yield(runtime);
        gt_ext_runtime_info_t state;
        bool observed = gt_ext_runtime_info(runtime, &state);
        if (observed && state.phase != GT_EXT_RUNTIME_ACTIVE) {
            fprintf(stderr,
                    "Runtime phase=%d error=%d cleanup=%d\n",
                    state.phase,
                    state.error,
                    state.cleanup_error);
        }
        kit_check(observed && state.phase == GT_EXT_RUNTIME_ACTIVE,
                  "active Runtime during request");
        gt_ext_rpc_result_t result;
        kit_check(gt_ext_session_request_result(peer, lease, handle, &result) == GT_EXT_RPC_OK,
                  "request observation");
        if (result.state != GT_EXT_RPC_QUEUED && result.state != GT_EXT_RPC_PENDING) {
            printf("{\"case\":\"%s\",\"state\":%u,\"confirmed\":%s,\"outcome\":%.*s}\n",
                   label,
                   (unsigned)result.state,
                   result.confirmed ? "true" : "false",
                   (int)result.outcome_size,
                   result.outcome);
            kit_check(gt_ext_session_forget_request(peer, lease, handle) == GT_EXT_RPC_OK,
                      "request retirement");
            return;
        }
    }
    kit_check(false, "bounded request did not settle");
}

int main(int argc, char **argv) {
    /* directory, host JSON, side, command, method, input, error input, then raw argv */
    kit_business = argc >= 8 && !strncmp(argv[3], "process-", 8);
    const char *selected = argc >= 8 ? argv[3] + (kit_business ? 8 : 0) : "";
    bool resources = !strncmp(selected, "resources-", 10);
    const char *mode = selected + (resources ? 10 : 0);
    bool external = !strcmp(mode, "node") || !strcmp(mode, "exec");
    int offset = (external ? 13 : 8) + (resources ? 6 : 0);
    if (kit_business) {
        offset = kit_process_parse(argc, argv, offset);
    }
    if (argc < offset || argc > offset + 32) {
        return 2;
    }
    kit_check(external || !strcmp(mode, "client") || !strcmp(mode, "executor"), "selected side");
    if (resources) {
        kit_check(!atexit(kit_resource_cleanup), "resource cleanup registration");
    }
    kit_check(!atexit(kit_emergency_cleanup), "register owned Runtime cleanup");
    kit_check(signal(SIGTERM, kit_signal) != SIG_ERR && signal(SIGINT, kit_signal) != SIG_ERR,
              "register owned Runtime interruption");
    gt_ext_role_t role = !strcmp(mode, "client") ? GT_EXT_HOST : GT_EXT_EXECUTOR;
    size_t before = ext_fds();
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    kit_check(fd >= 0, "package directory");
    gt_ext_package_config_t limits = {fd, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    kit_check(gt_ext_package_open(&limits, &package) == GT_OK, "package capture");
    close(fd);
    gt_ext_package_info_t info;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_package_step(package);
        kit_check(gt_ext_package_info(package, &info), "capture observation");
        if (info.phase != GT_EXT_PACKAGE_CAPTURING) {
            break;
        }
    }
    kit_check(info.phase == GT_EXT_PACKAGE_CAPTURED, "fixed package bytes");
    gt_ext_package_check_config_t check = {.host_json = argv[2]};
    kit_check(gt_ext_package_check(package, &check) == GT_EXT_PACKAGE_CHECKED,
              "native package compatibility");
    gt_ext_package_metadata_t metadata;
    kit_check(gt_ext_package_metadata(package, &metadata), "checked metadata");
    gt_ext_session_config_t config = ext_config();
    strcpy(config.plugin_id, metadata.id);
    strcpy(config.plugin_version, metadata.version);
    strcpy(config.client_digest, metadata.package_digest);
    strcpy(config.executor_digest, metadata.package_digest);
    config.required_count = config.available_count = 1;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    config.features = GT_EXT_FEATURE_REQUESTS;
    if (resources) {
        const char *caps[] = {"session.rpc", "resource.receive", "file.readScoped"};
        for (size_t i = 0; i < 3; i++) {
            bool declared = false;
            for (size_t j = 0; j < metadata.requires_count; j++) {
                if (!strcmp(metadata.requires[j], caps[i])) {
                    declared = true;
                }
            }
            kit_check(declared, "explicit selected-side resource declaration");
            strcpy(config.required[i], caps[i]);
            strcpy(config.available[i], caps[i]);
        }
        config.required_count = config.available_count = 3;
        config.features |= GT_EXT_FEATURE_RESOURCES;
        kit_resource_open(argv + (external ? 13 : 8));
    }
    if (kit_business) {
        kit_process_declarations(&metadata);
        kit_process_capabilities(&config);
    }
    gt_ext_rpc_method_t method = {
        .receiver = role, .input_shape = GT_EXT_JSON_OBJECT, .output_shape = GT_EXT_JSON_OBJECT};
    kit_check(strlen(argv[5]) < sizeof(method.method), "method length");
    strcpy(method.method, argv[5]);
    strcpy(method.capability, "session.rpc");
    int sockets[2];
    kit_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "owned local stream");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    if (resources) {
        kit_check(
            gt_ext_session_open_resources(
                &h, GT_EXT_HOST, &config, &method, 1, kit_res.host, 1048576, &pair.h) == GT_OK &&
                gt_ext_session_open_resources(
                    &e, GT_EXT_EXECUTOR, &config, &method, 1, kit_res.executor, 1048576, &pair.e) ==
                    GT_OK,
            "actual resources and RPC Session pair");
        kit_res.peer = pair.h;
    } else {
        kit_check(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, &method, 1, &pair.h) == GT_OK &&
                      gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, &method, 1, &pair.e) ==
                          GT_OK,
                  "owned Session pair");
    }
    ext_allow(&pair);
    gt_ext_session_t **slot = role == GT_EXT_HOST ? &pair.h : &pair.e;
    gt_ext_session_t *peer = role == GT_EXT_HOST ? pair.e : pair.h;
    gt_ext_lease_t lease;
    kit_check(gt_ext_session_lease(peer, &lease), "opposite peer lease");
    printf("{\"case\":\"lease\",\"side\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64 "\"}\n",
           external ? "executor" : mode,
           lease.instance,
           lease.epoch);
    gt_ext_invocation_t invocation = {.command = argv[4],
                                      .argv = (const char *const *)(argv + offset),
                                      .argc = (size_t)argc - (size_t)offset};
    gt_ext_runtime_config_t options = {.role = role, .invocation = &invocation};
    gt_ext_runtime_execution_t execution = {.stage_parent_fd = -1, .node_sdk_directory_fd = -1};
    gt_ext_process_environment_t environment = {0};
    if (external) {
        kit_execution(&execution, &environment, argv + 8, !strcmp(mode, "node"));
        options.execution = &execution;
    }
    gt_ext_runtime_root_t root = {
        .id = "workspace", .directory_fd = kit_res.root_fd, .byte_limit = 262144};
    if (resources) {
        options.roots = &root;
        options.root_count = 1;
    }
    if (kit_business) {
        kit_process_apply(&options);
    }
    gt_ext_runtime_t *runtime = NULL;
    options.memory_bytes = 128u * 1024u * 1024u + 1u;
    kit_check(gt_ext_runtime_open_package(slot, package, &options, true, &runtime) ==
                      (external ? GT_ERR_UNSUPPORTED : GT_ERR_INVALID_ARG) &&
                  !runtime && *slot,
              "oversized configured budget refused without adoption");
    options.memory_bytes = 0;
    gt_status_t opened = gt_ext_runtime_open_package(slot, package, &options, true, &runtime);
    kit_live = runtime;
    kit_execution_inputs_close(&execution);
    kit_process_inputs_close();
    if (kit_res.root_fd >= 0) {
        close(kit_res.root_fd);
        kit_res.root_fd = -1;
    }
    kit_check(opened == GT_OK && runtime && !*slot, "real Runtime adoption after rejected budget");
    gt_ext_runtime_info_t state;
    bool activated = false;
    uint64_t activation_until = ext_now() + 10000;
    for (unsigned i = 0; i < (external ? 10000u : 2000u) && ext_now() < activation_until; i++) {
        gt_ext_session_step(peer, 0);
        gt_ext_runtime_step(runtime);
        kit_check(gt_ext_runtime_info(runtime, &state), "Runtime observation");
        if (state.phase == GT_EXT_RUNTIME_ACTIVE) {
            activated = true;
            break;
        }
        kit_check(state.phase != GT_EXT_RUNTIME_FAILED, "plugin activation failed");
        kit_yield(runtime);
    }
    kit_check(activated, "bounded activation");
    kit_request(peer, runtime, &lease, argv[5], argv[6], "response");
    kit_request(peer, runtime, &lease, argv[5], argv[7], "error");
    gt_ext_lease_t stale = lease;
    stale.epoch ^= 1;
    uint64_t handle = 0;
    gt_ext_rpc_status_t status =
        gt_ext_session_request(peer, &stale, argv[5], "{}", 2, 1000, &handle);
    kit_check(status == GT_EXT_RPC_INVALID || status == GT_EXT_RPC_DENIED,
              "old epoch cannot enqueue");
    kit_check(gt_ext_session_request(peer, &lease, argv[5], "[]", 2, 1000, &handle) ==
                  GT_EXT_RPC_INVALID,
              "wrong root shape refused");
    char input[GT_EXT_RPC_BODY_MAX + 1];
    kit_bind(input, argv[6], &lease);
    kit_check(gt_ext_session_request(peer, &lease, argv[5], input, strlen(input), 1000, &handle) ==
                      GT_EXT_RPC_OK &&
                  gt_ext_session_cancel_request(peer, &lease, handle) == GT_EXT_RPC_OK,
              "queued request cancellation");
    gt_ext_rpc_result_t result;
    kit_check(gt_ext_session_request_result(peer, &lease, handle, &result) == GT_EXT_RPC_OK &&
                  result.state == GT_EXT_RPC_CANCELLED,
              "queued cancellation remains cancellation");
    printf("{\"case\":\"cancel\",\"outcome\":%.*s}\n", (int)result.outcome_size, result.outcome);
    kit_check(gt_ext_session_forget_request(peer, &lease, handle) == GT_EXT_RPC_OK,
              "cancelled handle retirement");
    if (resources) {
        kit_resource_take(peer, &lease);
    }
    gt_ext_session_revoke(peer);
    status = gt_ext_session_request(peer, &lease, argv[5], "{}", 2, 1000, &handle);
    kit_check(status == GT_EXT_RPC_DENIED || status == GT_EXT_RPC_CLOSED,
              "revoked capability cannot enqueue");
    gt_ext_runtime_stop(runtime);
    bool released = false;
    for (unsigned i = 0; i < 2000; i++) {
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            released = true;
            break;
        }
        kit_yield(runtime);
    }
    kit_check(released && gt_ext_runtime_info(runtime, &state) && state.engine_released &&
                  !state.dispose_error && state.cleanup_error == GT_OK,
              "bounded engine and cooperative cleanup");
    if (external) {
        kit_check(state.external_process && state.activated && state.child_exited,
                  "external activation and leader reap");
    }
    kit_check(gt_ext_runtime_destroy(&runtime) == GT_OK && !runtime, "Runtime destruction");
    kit_live = NULL;
    ext_drop(role == GT_EXT_HOST ? &pair.e : &pair.h);
    if (resources) {
        kit_res.peer = NULL;
        kit_resource_finish(&lease);
    }
    kit_check(gt_ext_package_destroy(&package) == GT_OK && !package, "package release");
    kit_check(ext_fds() == before, "native descriptor balance");
    printf("{\"case\":\"lifecycle\",\"activation\":true,\"budgetAdmission\":true,"
           "\"stale\":true,\"revocation\":true,\"engineReleased\":true,\"fdBalance\":true,"
           "\"externalProcess\":%s,\"childActivated\":%s,\"childExited\":%s}\n",
           external ? "true" : "false",
           state.activated ? "true" : "false",
           state.child_exited ? "true" : "false");
    return 0;
}
#endif
