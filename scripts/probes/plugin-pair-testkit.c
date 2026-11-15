/* Paired developer fixture: two checked packages and two real JS runtimes.
 * Borrowed Sessions are used only for Native control on this same worker,
 * like the App's runtime-owned Native RPC facade. Only Runtime destroys them. */
#define GT_TESTKIT_HELPERS_ONLY 1
#include "plugin-testkit.c"

static gt_ext_package_t *pair_package(const char *directory, const char *host) {
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    kit_check(fd >= 0, "pair package directory");
    gt_ext_package_config_t config = {fd, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    kit_check(gt_ext_package_open(&config, &package) == GT_OK, "pair package capture");
    close(fd);
    gt_ext_package_info_t info;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_package_step(package);
        kit_check(gt_ext_package_info(package, &info), "pair package observation");
        if (info.phase != GT_EXT_PACKAGE_CAPTURING) {
            break;
        }
    }
    kit_check(info.phase == GT_EXT_PACKAGE_CAPTURED, "pair fixed package");
    gt_ext_package_check_config_t check = {.host_json = host};
    kit_check(gt_ext_package_check(package, &check) == GT_EXT_PACKAGE_CHECKED,
              "pair independent compatibility");
    return package;
}

static void pair_pump(gt_ext_runtime_t *client, gt_ext_runtime_t *executor) {
    gt_ext_runtime_step(client);
    gt_ext_runtime_step(executor);
    gt_ext_runtime_info_t c, e;
    kit_check(gt_ext_runtime_info(client, &c) && gt_ext_runtime_info(executor, &e) &&
                  c.phase != GT_EXT_RUNTIME_FAILED && e.phase != GT_EXT_RUNTIME_FAILED,
              "both paired runtimes remain healthy");
}

static void pair_request(gt_ext_session_t *caller,
                         const gt_ext_lease_t *lease,
                         gt_ext_runtime_t *client,
                         gt_ext_runtime_t *executor,
                         const char *method,
                         const char *input,
                         const char *label) {
    char json[GT_EXT_RPC_BODY_MAX + 1];
    kit_bind(json, input, lease);
    uint64_t handle = 0;
    kit_check(gt_ext_session_request(caller, lease, method, json, strlen(json), 1000, &handle) ==
                  GT_EXT_RPC_OK,
              "pair control request admission");
    for (unsigned i = 0; i < 2000; i++) {
        pair_pump(client, executor);
        gt_ext_rpc_result_t result;
        kit_check(gt_ext_session_request_result(caller, lease, handle, &result) == GT_EXT_RPC_OK,
                  "pair control observation");
        if (result.state != GT_EXT_RPC_QUEUED && result.state != GT_EXT_RPC_PENDING) {
            printf("{\"case\":\"%s\",\"confirmed\":%s,\"outcome\":%.*s}\n",
                   label,
                   result.confirmed ? "true" : "false",
                   (int)result.outcome_size,
                   result.outcome);
            kit_check(gt_ext_session_forget_request(caller, lease, handle) == GT_EXT_RPC_OK,
                      "pair control retirement");
            return;
        }
    }
    kit_check(false, "paired JS request did not settle");
}

static void pair_release(gt_ext_runtime_t **runtime) {
    gt_ext_runtime_stop(*runtime);
    bool released = false;
    for (unsigned i = 0; i < 2000; i++) {
        if (gt_ext_runtime_close(*runtime) == GT_EXT_RELEASED) {
            released = true;
            break;
        }
    }
    gt_ext_runtime_info_t info;
    kit_check(released && gt_ext_runtime_info(*runtime, &info) && info.engine_released &&
                  !info.dispose_error && info.cleanup_error == GT_OK,
              "pair independent engine and cleanup");
    kit_check(gt_ext_runtime_destroy(runtime) == GT_OK && !*runtime, "pair Runtime destroy");
}

int main(int argc, char **argv) {
    /* dir, client host, executor host, command, method, input, error input,
     * target side, count, then name=receiver entries, then raw invocation argv. */
    kit_check(argc >= 11, "pair arguments");
    char *end = NULL;
    unsigned long count = strtoul(argv[9], &end, 10);
    kit_check(end && !*end && count > 0 && count <= GT_EXT_RPC_METHODS_MAX &&
                  argc >= 10 + (int)count && argc <= 42 + (int)count,
              "pair methods and arguments");
    bool target_client = !strcmp(argv[8], "client");
    kit_check(target_client || !strcmp(argv[8], "executor"), "pair target side");
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX] = {0};
    bool root = false;
    for (size_t i = 0; i < count; i++) {
        const char *split = strchr(argv[10 + i], '=');
        kit_check(split && split != argv[10 + i] &&
                      (size_t)(split - argv[10 + i]) < sizeof(methods[i].method),
                  "pair method entry");
        memcpy(methods[i].method, argv[10 + i], (size_t)(split - argv[10 + i]));
        kit_check(!strcmp(split + 1, "client") || !strcmp(split + 1, "executor"), "pair receiver");
        methods[i].receiver = !strcmp(split + 1, "client") ? GT_EXT_HOST : GT_EXT_EXECUTOR;
        methods[i].input_shape = methods[i].output_shape = GT_EXT_JSON_OBJECT;
        strcpy(methods[i].capability, "session.rpc");
        if (!strcmp(methods[i].method, argv[5])) {
            root = methods[i].receiver == (target_client ? GT_EXT_HOST : GT_EXT_EXECUTOR);
        }
    }
    kit_check(root, "pair root method direction");
    size_t before = ext_fds();
    gt_ext_package_t *cp = pair_package(argv[1], argv[2]);
    gt_ext_package_t *ep = pair_package(argv[1], argv[3]);
    gt_ext_package_metadata_t cm, em;
    kit_check(gt_ext_package_metadata(cp, &cm) && gt_ext_package_metadata(ep, &em) &&
                  !strcmp(cm.package_digest, em.package_digest),
              "pair same fixed package");
    gt_ext_session_config_t config = ext_config();
    strcpy(config.plugin_id, cm.id);
    strcpy(config.plugin_version, cm.version);
    strcpy(config.client_digest, cm.package_digest);
    strcpy(config.executor_digest, em.package_digest);
    config.required_count = config.available_count = 1;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    config.features = GT_EXT_FEATURE_REQUESTS;
    int sockets[2];
    kit_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "pair owned stream");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    kit_check(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, methods, count, &pair.h) == GT_OK &&
                  gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, methods, count, &pair.e) ==
                      GT_OK,
              "pair native Sessions");
    ext_allow(&pair);
    gt_ext_session_t *caller = target_client ? pair.e : pair.h;
    gt_ext_lease_t lease;
    kit_check(gt_ext_session_lease(caller, &lease), "pair caller lease");
    printf("{\"case\":\"lease\",\"side\":\"paired\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64
           "\"}\n",
           lease.instance,
           lease.epoch);
    gt_ext_invocation_t invocation = {.command = argv[4],
                                      .argv = (const char *const *)(argv + 10 + count),
                                      .argc = (size_t)argc - 10 - count};
    gt_ext_runtime_config_t client_options = {.role = GT_EXT_HOST, .invocation = &invocation};
    gt_ext_runtime_config_t executor_options = {.role = GT_EXT_EXECUTOR, .invocation = &invocation};
    gt_ext_runtime_t *client = NULL, *executor = NULL;
    client_options.memory_bytes = executor_options.memory_bytes = 128u * 1024u * 1024u + 1u;
    kit_check(gt_ext_runtime_open_package(&pair.h, cp, &client_options, true, &client) ==
                      GT_ERR_INVALID_ARG &&
                  !client && pair.h &&
                  gt_ext_runtime_open_package(&pair.e, ep, &executor_options, true, &executor) ==
                      GT_ERR_INVALID_ARG &&
                  !executor && pair.e,
              "pair independent budget admission");
    client_options.memory_bytes = executor_options.memory_bytes = 0;
    kit_check(gt_ext_runtime_open_package(&pair.h, cp, &client_options, true, &client) == GT_OK &&
                  client && !pair.h,
              "pair Client adoption");
    kit_check(gt_ext_runtime_open_package(&pair.e, ep, &executor_options, true, &executor) ==
                      GT_OK &&
                  executor && !pair.e,
              "pair Executor adoption");
    bool activated = false;
    for (unsigned i = 0; i < 2000; i++) {
        pair_pump(client, executor);
        gt_ext_runtime_info_t c, x;
        kit_check(gt_ext_runtime_info(client, &c) && gt_ext_runtime_info(executor, &x),
                  "pair activation observations");
        if (c.phase == GT_EXT_RUNTIME_ACTIVE && x.phase == GT_EXT_RUNTIME_ACTIVE) {
            activated = true;
            break;
        }
    }
    kit_check(activated, "both paired JS entries activate");
    pair_request(caller, &lease, client, executor, argv[5], argv[6], "response");
    pair_request(caller, &lease, client, executor, argv[5], argv[7], "error");
    gt_ext_lease_t stale = lease;
    stale.epoch ^= 1;
    uint64_t handle = 0;
    gt_ext_rpc_status_t status =
        gt_ext_session_request(caller, &stale, argv[5], "{}", 2, 1000, &handle);
    kit_check(status == GT_EXT_RPC_INVALID || status == GT_EXT_RPC_DENIED, "pair old epoch");
    char input[GT_EXT_RPC_BODY_MAX + 1];
    kit_bind(input, argv[6], &lease);
    kit_check(gt_ext_session_request(
                  caller, &lease, argv[5], input, strlen(input), 1000, &handle) == GT_EXT_RPC_OK &&
                  gt_ext_session_cancel_request(caller, &lease, handle) == GT_EXT_RPC_OK,
              "pair queued cancellation");
    gt_ext_rpc_result_t result;
    kit_check(gt_ext_session_request_result(caller, &lease, handle, &result) == GT_EXT_RPC_OK &&
                  result.state == GT_EXT_RPC_CANCELLED,
              "pair cancellation observation");
    printf("{\"case\":\"cancel\",\"outcome\":%.*s}\n", (int)result.outcome_size, result.outcome);
    kit_check(gt_ext_session_forget_request(caller, &lease, handle) == GT_EXT_RPC_OK,
              "pair cancellation retirement");
    gt_ext_session_revoke(caller);
    status = gt_ext_session_request(caller, &lease, argv[5], "{}", 2, 1000, &handle);
    kit_check(status == GT_EXT_RPC_DENIED || status == GT_EXT_RPC_CLOSED, "pair revocation");
    pair_release(&client);
    pair_release(&executor);
    kit_check(gt_ext_package_destroy(&cp) == GT_OK && !cp && gt_ext_package_destroy(&ep) == GT_OK &&
                  !ep,
              "pair package release");
    kit_check(ext_fds() == before, "pair descriptor balance");
    puts("{\"case\":\"lifecycle\",\"activation\":true,\"budgetAdmission\":true,\"stale\":true,"
         "\"revocation\":true,\"engineReleased\":true,\"fdBalance\":true,\"clientActivated\":true,"
         "\"executorActivated\":true,\"clientEngineReleased\":true,\"executorEngineReleased\":"
         "true}");
    return 0;
}
