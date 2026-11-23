/* Run explicitly selected Lua development entries in an isolated Session. */
#define main lua_client_session_fixture_main
#include "test_ext_session.c"
#undef main

static gt_ext_package_t *checked_package(const char *directory, bool executor) {
    const int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    const gt_ext_package_config_t limits = {fd, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    CHECK(gt_ext_package_open(&limits, &package) == GT_OK && !close(fd));
    gt_ext_package_phase_t phase;
    do {
        phase = gt_ext_package_step(package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    CHECK(phase == GT_EXT_PACKAGE_CAPTURED);
    char host[192];
    int count = snprintf(host,
                         sizeof(host),
                         "{\"side\":\"%s\",\"sdkVersion\":\"1.0.0\",\"runtimes\":[\"lua\"],"
                         "\"protocols\":[1],\"capabilities\":[\"session.rpc\"]}",
                         executor ? "executor" : "client");
    CHECK(count > 0 && (size_t)count < sizeof(host));
    const gt_ext_package_check_config_t profile = {.host_json = host};
    CHECK(gt_ext_package_check(package, &profile) == GT_EXT_PACKAGE_CHECKED);
    return package;
}

static gt_ext_runtime_t *activate(gt_ext_session_t **session,
                                  gt_ext_package_t **package,
                                  gt_ext_role_t role,
                                  const gt_ext_invocation_t *invocation) {
    const gt_ext_runtime_config_t config = {.role = role, .invocation = invocation};
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open_package(session, *package, &config, false, &runtime) == GT_OK);
    CHECK(gt_ext_package_destroy(package) == GT_OK);
    return runtime;
}

static void close_runtime(gt_ext_runtime_t **runtime) {
    const uint64_t deadline = ext_now() + 5000;
    while (gt_ext_runtime_close(*runtime) != GT_EXT_RELEASED) {
        CHECK(ext_now() < deadline);
    }
    CHECK(gt_ext_runtime_destroy(runtime) == GT_OK);
}

static bool
activation_ok(gt_ext_runtime_t *runtime, gt_ext_runtime_phase_t phase, const char *role) {
    if (phase == GT_EXT_RUNTIME_ACTIVE) {
        return true;
    }
    gt_ext_runtime_info_t info;
    CHECK(gt_ext_runtime_info(runtime, &info));
    fprintf(stderr,
            "Lua %s activation failed: phase=%u error=%u\n",
            role,
            (unsigned)phase,
            (unsigned)info.error);
    return false;
}

int main(int argc, char **argv) {
    CHECK(argc >= 4);
    const bool paired = !strcmp(argv[1], "paired");
    CHECK(paired || !strcmp(argv[1], "client"));
    gt_ext_package_t *client = checked_package(argv[2], false);
    gt_ext_package_t *executor = paired ? checked_package(argv[2], true) : NULL;
    gt_ext_package_metadata_t client_info, executor_info;
    CHECK(gt_ext_package_metadata(client, &client_info));
    CHECK(!paired || gt_ext_package_metadata(executor, &executor_info));
    CHECK(!paired || !strcmp(client_info.package_digest, executor_info.package_digest));
    gt_ext_session_config_t config = ext_config();
    strcpy(config.plugin_id, client_info.id);
    strcpy(config.plugin_version, client_info.version);
    strcpy(config.client_digest, client_info.package_digest);
    if (paired) {
        strcpy(config.executor_digest, executor_info.package_digest);
    }
    config.features = GT_EXT_FEATURE_REQUESTS;
    config.required_count = config.available_count =
        client_info.requires_count || (paired && executor_info.requires_count) ? 1 : 0;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");

    size_t method_count = 0;
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX] = {0};
    int argument_start = 4;
    if (paired) {
        while (argument_start < argc && strcmp(argv[argument_start], "--")) {
            const char *value = argv[argument_start++];
            CHECK(method_count < GT_EXT_RPC_METHODS_MAX && strlen(value) > 2 &&
                  strlen(value + 2) <= 128 && value[1] == ':' &&
                  (value[0] == 'C' || value[0] == 'E'));
            gt_ext_rpc_method_t *method = &methods[method_count++];
            strcpy(method->method, value + 2);
            strcpy(method->capability, "session.rpc");
            method->receiver = value[0] == 'C' ? GT_EXT_HOST : GT_EXT_EXECUTOR;
            method->input_shape = method->output_shape = GT_EXT_JSON_OBJECT;
        }
        CHECK(argument_start < argc);
        argument_start++;
    }
    const gt_ext_invocation_t invocation = {.command = argv[3],
                                            .argv = (const char *const *)(argv + argument_start),
                                            .argc = (size_t)(argc - argument_start)};
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_channel_t *host = ext_channel(fd[0]), *peer = ext_channel(fd[1]);
    ext_pair_t pair = {0};
    CHECK(gt_ext_session_open_rpc(&host, GT_EXT_HOST, &config, methods, method_count, &pair.h) ==
          GT_OK);
    CHECK(gt_ext_session_open_rpc(
              &peer, GT_EXT_EXECUTOR, &config, methods, method_count, &pair.e) == GT_OK);
    (void)ext_allow(&pair);
    gt_ext_runtime_t *remote =
        paired ? activate(&pair.e, &executor, GT_EXT_EXECUTOR, &invocation) : NULL;
    gt_ext_invocation_t host_invocation = invocation;
    if (getenv("GHOSTERM_TEST_LUA_CONTEXT")) {
        host_invocation.workspace_id = "workspace-a";
        host_invocation.pane_id = "pane-b";
        host_invocation.directory = "/owned/project";
    }
    gt_ext_runtime_t *local = activate(&pair.h, &client, GT_EXT_HOST, &host_invocation);
    const uint64_t deadline = ext_now() + 6000;
    gt_ext_runtime_phase_t host_phase = GT_EXT_RUNTIME_CREATED;
    gt_ext_runtime_phase_t peer_phase = GT_EXT_RUNTIME_ACTIVE;
    do {
        if (remote) {
            peer_phase = gt_ext_runtime_step(remote);
        } else {
            (void)gt_ext_session_step(pair.e, 0);
        }
        host_phase = gt_ext_runtime_step(local);
        CHECK(ext_now() < deadline);
    } while (host_phase <= GT_EXT_RUNTIME_ACTIVE && peer_phase <= GT_EXT_RUNTIME_ACTIVE &&
             (host_phase != GT_EXT_RUNTIME_ACTIVE || peer_phase != GT_EXT_RUNTIME_ACTIVE));
    bool success = activation_ok(local, host_phase, "Client");
    if (remote) {
        success = activation_ok(remote, peer_phase, "Executor") && success;
        close_runtime(&remote);
    } else {
        ext_drop(&pair.e);
    }
    close_runtime(&local);
    return success ? 0 : 1;
}
