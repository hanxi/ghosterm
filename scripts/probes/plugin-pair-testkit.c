/* Paired developer fixture: two checked packages and two real JS runtimes.
 * Borrowed Sessions are used only for Native control on this same worker,
 * like the App's runtime-owned Native RPC facade. Only Runtime destroys them. */
#define GT_TESTKIT_HELPERS_ONLY 1
#include "plugin-testkit.c"
#include "plugin-testkit-resource.inc"

static bool pair_external, pair_child_activated, pair_child_exited;
static bool pair_resources, pair_resource_seen;

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
    kit_yield(executor);
    gt_ext_runtime_info_t c, e;
    kit_check(gt_ext_runtime_info(client, &c) && gt_ext_runtime_info(executor, &e) &&
                  c.phase != GT_EXT_RUNTIME_FAILED && e.phase != GT_EXT_RUNTIME_FAILED,
              "both paired runtimes remain healthy");
    if (pair_resources) {
        gt_ext_resource_usage_t usage;
        kit_check(gt_ext_resource_store_usage(kit_res.host, &usage), "pair resource observation");
        pair_resource_seen |= usage.resources > 0;
    }
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
    kit_check(gt_ext_session_request(
                  caller, lease, method, json, strlen(json), kit_rpc_timeout, &handle) ==
                  GT_EXT_RPC_OK,
              "pair control request admission");
    uint64_t until = ext_now() + kit_rpc_timeout + 1000u;
    for (unsigned i = 0; i < 2000u + 2u * kit_rpc_timeout && ext_now() < until; i++) {
        pair_pump(client, executor);
        gt_ext_rpc_result_t result;
        kit_check(gt_ext_session_request_result(caller, lease, handle, &result) == GT_EXT_RPC_OK,
                  "pair control observation");
        if (result.state != GT_EXT_RPC_QUEUED && result.state != GT_EXT_RPC_PENDING) {
            kit_check(result.confirmed,
                      "complete business response within explicit test request window");
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
        kit_yield(*runtime);
    }
    gt_ext_runtime_info_t info;
    kit_check(released && gt_ext_runtime_info(*runtime, &info) && info.engine_released &&
                  !info.dispose_error && info.cleanup_error == GT_OK,
              "pair independent engine and cleanup");
    if (info.external_process) {
        kit_check(info.activated && info.child_exited, "pair external activation and reap");
        pair_child_activated = info.activated;
        pair_child_exited = info.child_exited;
    }
    gt_ext_runtime_t *owner = *runtime;
    kit_check(gt_ext_runtime_destroy(runtime) == GT_OK && !*runtime, "pair Runtime destroy");
    if (kit_live == owner) {
        kit_live = NULL;
    }
    if (kit_second == owner) {
        kit_second = NULL;
    }
}

int main(int argc, char **argv) {
    /* dir, client host, executor host, command, method, input, error input,
     * target side, count, then name=receiver entries, then raw invocation argv. */
    kit_check(argc >= 11, "pair arguments");
    const char *selected = kit_request_mode(argv[8]);
    bool views = !strncmp(selected, "views-", 6);
    selected += views ? 6 : 0;
    kit_business = !strncmp(selected, "process-", 8);
    selected += kit_business ? 8 : 0;
    bool resources = !strncmp(selected, "resources-", 10);
    pair_resources = resources;
    const char *target = selected + (resources ? 10 : 0);
    bool client_reads = resources && !strncmp(target, "read-", 5);
    target += client_reads ? 5 : 0;
    if (resources) {
        kit_check(!atexit(kit_resource_cleanup), "pair resource cleanup registration");
    }
    kit_check(!atexit(kit_emergency_cleanup), "pair cleanup registration");
    kit_check(signal(SIGTERM, kit_signal) != SIG_ERR && signal(SIGINT, kit_signal) != SIG_ERR,
              "pair interruption registration");
    char *end = NULL;
    unsigned long count = strtoul(argv[9], &end, 10);
    kit_check(end && !*end && count > 0 && count <= GT_EXT_RPC_METHODS_MAX &&
                  argc >= 10 + (int)count && argc <= 186 + (int)count + (views ? 1 : 0),
              "pair methods and arguments");
    bool target_client = !strcmp(target, "client");
    kit_check(target_client || !strcmp(target, "executor"), "pair target side");
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX] = {0};
    bool root = false;
    for (size_t i = 0; i < count; i++) {
        kit_view_method(&methods[i], argv[10 + i]);
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
    pair_external = strcmp(em.runtime, "portable-js") != 0;
    kit_check(!pair_external || !strcmp(em.runtime, "node") || !strcmp(em.runtime, "exec"),
              "pair external runtime");
    int resource_offset = 10 + (int)count + (pair_external ? 5 : 0);
    int offset = resource_offset + (resources ? 6 : 0);
    if (kit_business) {
        offset = kit_process_parse(argc, argv, offset);
    }
    if (views) {
        offset = kit_views_parse(argc, argv, offset);
    }
    kit_check(argc >= offset && argc <= offset + 32, "pair exact invocation arguments");
    gt_ext_session_config_t config = ext_config();
    strcpy(config.plugin_id, cm.id);
    strcpy(config.plugin_version, cm.version);
    strcpy(config.client_digest, cm.package_digest);
    strcpy(config.executor_digest, em.package_digest);
    config.required_count = config.available_count = 1;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    config.features = GT_EXT_FEATURE_REQUESTS;
    if (resources) {
        const char *caps[] = {"session.rpc", "resource.receive", "file.readScoped"};
        const gt_ext_package_metadata_t *sides[] = {&cm, &em};
        for (size_t side = 0; side < 2; side++) {
            for (size_t cap = 0; cap < (side ? 3u : 2u); cap++) {
                bool declared = false;
                for (size_t i = 0; i < sides[side]->requires_count; i++) {
                    declared |= !strcmp(sides[side]->requires[i], caps[cap]);
                }
                kit_check(declared, "pair independently declared resource services");
            }
        }
        config.required_count = config.available_count = 3;
        for (size_t i = 0; i < 3; i++) {
            strcpy(config.required[i], caps[i]);
            strcpy(config.available[i], caps[i]);
        }
        config.features |= GT_EXT_FEATURE_RESOURCES;
        kit_resource_open(argv + resource_offset);
    }
    if (kit_business) {
        kit_process_declarations(&em);
        kit_process_capabilities(&config);
    }
    if (views) {
        kit_views_capabilities(&cm, &config);
    }
    int sockets[2];
    kit_check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "pair owned stream");
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    ext_pair_t pair = {0};
    if (resources) {
        kit_check(gt_ext_session_open_resources(
                      &h, GT_EXT_HOST, &config, methods, count, kit_res.host, 1048576, &pair.h) ==
                          GT_OK &&
                      gt_ext_session_open_resources(&e,
                                                    GT_EXT_EXECUTOR,
                                                    &config,
                                                    methods,
                                                    count,
                                                    kit_res.executor,
                                                    1048576,
                                                    &pair.e) == GT_OK,
                  "pair actual resource Sessions");
    } else {
        kit_check(
            gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, methods, count, &pair.h) == GT_OK &&
                gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, methods, count, &pair.e) ==
                    GT_OK,
            "pair native Sessions");
    }
    ext_allow(&pair);
    gt_ext_session_t *resource_receiver = pair.h;
    gt_ext_lease_t resource_lease;
    kit_check(gt_ext_session_lease(resource_receiver, &resource_lease),
              "pair resource receiver lease");
    gt_ext_session_t *caller = target_client ? pair.e : pair.h;
    gt_ext_lease_t lease;
    kit_check(gt_ext_session_lease(caller, &lease), "pair caller lease");
    printf("{\"case\":\"lease\",\"side\":\"paired\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64
           "\"}\n",
           lease.instance,
           lease.epoch);
    gt_ext_invocation_t invocation = {.command = argv[4],
                                      .argv = (const char *const *)(argv + offset),
                                      .argc = (size_t)argc - (size_t)offset};
    gt_ext_runtime_config_t client_options = {.role = GT_EXT_HOST, .invocation = &invocation};
    gt_ext_runtime_config_t executor_options = {.role = GT_EXT_EXECUTOR, .invocation = &invocation};
    gt_ext_runtime_root_t resource_root = {
        .id = "workspace", .directory_fd = kit_res.root_fd, .byte_limit = 262144};
    if (resources) {
        executor_options.roots = &resource_root;
        executor_options.root_count = 1;
    }
    if (kit_business) {
        kit_process_apply(&executor_options);
    }
    gt_ext_runtime_execution_t execution = {.stage_parent_fd = -1, .node_sdk_directory_fd = -1};
    gt_ext_process_environment_t environment = {0};
    if (pair_external) {
        kit_execution(&execution, &environment, argv + 10 + count, !strcmp(em.runtime, "node"));
        executor_options.execution = &execution;
    }
    gt_ext_runtime_t *client = NULL, *executor = NULL;
    client_options.memory_bytes = executor_options.memory_bytes = 128u * 1024u * 1024u + 1u;
    kit_check(gt_ext_runtime_open_package(&pair.h, cp, &client_options, true, &client) ==
                      GT_ERR_INVALID_ARG &&
                  !client && pair.h &&
                  gt_ext_runtime_open_package(&pair.e, ep, &executor_options, true, &executor) ==
                      (pair_external ? GT_ERR_UNSUPPORTED : GT_ERR_INVALID_ARG) &&
                  !executor && pair.e,
              "pair independent budget admission");
    client_options.memory_bytes = executor_options.memory_bytes = 0;
    gt_status_t opened = gt_ext_runtime_open_package(&pair.h, cp, &client_options, true, &client);
    kit_live = client;
    kit_check(opened == GT_OK && client && !pair.h, "pair Client adoption");
    opened = gt_ext_runtime_open_package(&pair.e, ep, &executor_options, true, &executor);
    kit_second = executor;
    kit_execution_inputs_close(&execution);
    kit_process_inputs_close();
    if (kit_res.root_fd >= 0) {
        close(kit_res.root_fd);
        kit_res.root_fd = -1;
    }
    kit_check(opened == GT_OK && executor && !pair.e, "pair Executor adoption");
    if (views) {
        kit_views_enable(client);
        kit_views_executor(executor);
    }
    bool activated = false;
    uint64_t activation_until = ext_now() + 10000;
    for (unsigned i = 0; i < (pair_external ? 10000u : 2000u) && ext_now() < activation_until;
         i++) {
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
    if (resources) {
        if (client_reads) {
            gt_ext_resource_snapshot_t pending;
            gt_ext_resource_usage_t usage;
            kit_check(pair_resource_seen &&
                          gt_ext_session_take_resource(resource_receiver,
                                                       &resource_lease,
                                                       &pending) == GT_EXT_RESOURCE_WAIT &&
                          gt_ext_resource_store_usage(kit_res.host, &usage) && !usage.resources,
                      "Client consumed and released resource before response");
            kit_res.client_received = true;
        } else {
            kit_resource_take(resource_receiver, &resource_lease);
        }
    }
    if (views) {
        kit_views_observe(client);
    }
    gt_ext_session_revoke(caller);
    status = gt_ext_session_request(caller, &lease, argv[5], "{}", 2, 1000, &handle);
    kit_check(status == GT_EXT_RPC_DENIED || status == GT_EXT_RPC_CLOSED, "pair revocation");
    if (views) {
        gt_ext_runtime_stop(client);
        kit_views_revoked(client);
    }
    pair_release(&client);
    pair_release(&executor);
    if (resources) {
        kit_resource_finish(&resource_lease);
    }
    kit_check(gt_ext_package_destroy(&cp) == GT_OK && !cp && gt_ext_package_destroy(&ep) == GT_OK &&
                  !ep,
              "pair package release");
    kit_check(ext_fds() == before, "pair descriptor balance");
    printf("{\"case\":\"lifecycle\",\"activation\":true,\"budgetAdmission\":true,\"stale\":true,"
           "\"revocation\":true,\"engineReleased\":true,\"fdBalance\":true,\"viewsRevoked\":%s,"
           "\"clientActivated\":true,"
           "\"executorActivated\":true,\"clientEngineReleased\":true,\"executorEngineReleased\":"
           "true,\"externalProcess\":%s,\"childActivated\":%s,\"childExited\":%s}\n",
           views ? "true" : "false",
           pair_external ? "true" : "false",
           pair_child_activated ? "true" : "false",
           pair_child_exited ? "true" : "false");
    return 0;
}
