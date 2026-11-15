/* Developer fixture: public Core, real checked package and bounded QuickJS. */
#define main testkit_session_fixture_main
#include "../../core/tests/test_ext_session.c"
#undef main
#include <inttypes.h>

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

#ifndef GT_TESTKIT_HELPERS_ONLY
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
        gt_ext_runtime_info_t state;
        kit_check(gt_ext_runtime_info(runtime, &state) && state.phase == GT_EXT_RUNTIME_ACTIVE,
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
    if (argc < 8 || argc > 40) {
        return 2;
    }
    kit_check(!strcmp(argv[3], "client") || !strcmp(argv[3], "executor"), "selected side");
    gt_ext_role_t role = !strcmp(argv[3], "client") ? GT_EXT_HOST : GT_EXT_EXECUTOR;
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
    gt_ext_rpc_method_t method = {
        .receiver = role, .input_shape = GT_EXT_JSON_OBJECT, .output_shape = GT_EXT_JSON_OBJECT};
    kit_check(strlen(argv[5]) < sizeof(method.method), "method length");
    strcpy(method.method, argv[5]);
    strcpy(method.capability, "session.rpc");
    int sockets[2];
    kit_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "owned local stream");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    kit_check(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, &method, 1, &pair.h) == GT_OK &&
                  gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, &method, 1, &pair.e) ==
                      GT_OK,
              "owned Session pair");
    ext_allow(&pair);
    gt_ext_session_t **slot = role == GT_EXT_HOST ? &pair.h : &pair.e;
    gt_ext_session_t *peer = role == GT_EXT_HOST ? pair.e : pair.h;
    gt_ext_lease_t lease;
    kit_check(gt_ext_session_lease(peer, &lease), "opposite peer lease");
    printf("{\"case\":\"lease\",\"side\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64 "\"}\n",
           argv[3],
           lease.instance,
           lease.epoch);
    gt_ext_invocation_t invocation = {
        .command = argv[4], .argv = (const char *const *)(argv + 8), .argc = (size_t)argc - 8};
    gt_ext_runtime_config_t options = {.role = role, .invocation = &invocation};
    gt_ext_runtime_t *runtime = NULL;
    options.memory_bytes = 128u * 1024u * 1024u + 1u;
    kit_check(gt_ext_runtime_open_package(slot, package, &options, true, &runtime) ==
                      GT_ERR_INVALID_ARG &&
                  !runtime && *slot,
              "oversized configured budget refused without adoption");
    options.memory_bytes = 0;
    kit_check(gt_ext_runtime_open_package(slot, package, &options, true, &runtime) == GT_OK &&
                  runtime && !*slot,
              "real Runtime adoption after rejected budget");
    gt_ext_runtime_info_t state;
    bool activated = false;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(peer, 0);
        gt_ext_runtime_step(runtime);
        kit_check(gt_ext_runtime_info(runtime, &state), "Runtime observation");
        if (state.phase == GT_EXT_RUNTIME_ACTIVE) {
            activated = true;
            break;
        }
        kit_check(state.phase != GT_EXT_RUNTIME_FAILED, "plugin activation failed");
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
    }
    kit_check(released && gt_ext_runtime_info(runtime, &state) && state.engine_released &&
                  !state.dispose_error && state.cleanup_error == GT_OK,
              "bounded engine and cooperative cleanup");
    kit_check(gt_ext_runtime_destroy(&runtime) == GT_OK && !runtime, "Runtime destruction");
    ext_drop(role == GT_EXT_HOST ? &pair.e : &pair.h);
    kit_check(gt_ext_package_destroy(&package) == GT_OK && !package, "package release");
    kit_check(ext_fds() == before, "native descriptor balance");
    puts("{\"case\":\"lifecycle\",\"activation\":true,\"budgetAdmission\":true,"
         "\"stale\":true,\"revocation\":true,\"engineReleased\":true,\"fdBalance\":true}");
    return 0;
}
#endif
