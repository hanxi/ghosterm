#define RUNTIME_RESOURCE_MAIN lua_preview_resource_fixture_main
#include "test_runtime_resources.c"
#include "base/sha256.h"

static gt_ext_session_t *preview_session(gt_ext_channel_t **channel,
                                         res_store_t *store,
                                         gt_ext_role_t role,
                                         bool ssh,
                                         const char *digest) {
    *store = res_store(1048576);
    gt_ext_session_config_t config = ext_config();
    if (digest) {
        strcpy(config.executor_digest, digest);
        if (!ssh) {
            strcpy(config.client_digest, digest);
        }
    }
    config.features = GT_EXT_FEATURE_REQUESTS | GT_EXT_FEATURE_RESOURCES;
    config.required_count = config.available_count = 3;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.required[1], "resource.receive");
    strcpy(config.required[2], "file.readScoped");
    memcpy(config.available, config.required, sizeof(config.available));
    if (ssh) {
        config.peer.kind = GT_EXT_PEER_SSH;
    }
    const gt_ext_rpc_method_t method = {
        .method = "preview.open",
        .capability = "session.rpc",
        .receiver = GT_EXT_EXECUTOR,
        .input_shape = GT_EXT_JSON_OBJECT,
        .output_shape = GT_EXT_JSON_OBJECT,
    };
    gt_ext_session_t *session = NULL;
    CHECK(gt_ext_session_open_resources(
              channel, role, &config, &method, 1, store->s, 1048576, &session) == GT_OK);
    return session;
}

static void preview_pair(res_pair_t *pair, const char *digest) {
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_channel_t *host = ext_channel(fd[0]), *executor = ext_channel(fd[1]);
    pair->rpc.pair.h = preview_session(&host, &pair->h, GT_EXT_HOST, false, digest);
    pair->rpc.pair.e = preview_session(&executor, &pair->e, GT_EXT_EXECUTOR, false, digest);
    ext_allow(&pair->rpc.pair);
    CHECK(gt_ext_session_lease(pair->rpc.pair.h, &pair->rpc.h));
}

static gt_ext_runtime_t *open_preview(pub_source_fixture_t *fixture,
                                      const char *path,
                                      gt_ext_role_t role,
                                      const char *resource_path) {
    char source[16384];
    FILE *file = fopen(path, "rb");
    CHECK(file);
    const size_t size = fread(source, 1, sizeof(source), file);
    CHECK(size && feof(file) && !fclose(file));
    const bool executor = role == GT_EXT_EXECUTOR;
    gt_ext_runtime_module_t module = {
        .name = executor ? "plugin:/executor.lua" : "plugin:/main.lua",
        .source = source,
        .size = size,
    };
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    CHECK(gt__sha256_update(&hash, source, size) && gt__sha256_final(&hash, module.sha256));
    const gt_ext_runtime_root_t root = {
        .id = "workspace", .directory_fd = fixture->root, .byte_limit = 262144};
    const char *arguments[] = {resource_path ? resource_path : "./file.bin"};
    const gt_ext_invocation_t invocation = {.command = "preview", .argv = arguments, .argc = 1};
    const gt_ext_runtime_config_t config = {
        .modules = &module,
        .module_count = 1,
        .entry = module.name,
        .role = role,
        .roots = executor ? &root : NULL,
        .root_count = executor ? 1 : 0,
        .invocation = executor ? NULL : &invocation,
    };
    gt_ext_runtime_t *runtime = NULL;
    gt_ext_session_t **session = executor ? &fixture->pair.rpc.pair.e : &fixture->pair.rpc.pair.h;
    if (executor) {
        const gt_ext_runtime_root_t invalid[] = {
            {.id = "workspace", .directory_fd = -1, .byte_limit = 262144},
            {.id = "workspace", .directory_fd = fixture->root, .byte_limit = 16777217},
            {.id = "bad/root", .directory_fd = fixture->root, .byte_limit = 262144},
        };
        gt_ext_runtime_config_t rejected = config;
        gt_ext_session_t *owner = *session;
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            rejected.roots = &invalid[i];
            CHECK(gt_ext_runtime_open(session, &rejected, &runtime) == GT_ERR_INVALID_ARG &&
                  !runtime && *session == owner && fcntl(fixture->root, F_GETFD) >= 0);
        }
        rejected = config;
        rejected.root_count = 9;
        CHECK(gt_ext_runtime_open(session, &rejected, &runtime) == GT_ERR_INVALID_ARG && !runtime &&
              *session == owner && fcntl(fixture->root, F_GETFD) >= 0);
    }
    CHECK(gt_ext_runtime_open(session, &config, &runtime) == GT_OK);
    CHECK(runtime && !*session);
    return runtime;
}

static gt_ext_rpc_result_t
request_preview(rpc_pair_t *pair, gt_ext_runtime_t *runtime, const char *input) {
    uint64_t request;
    CHECK(gt_ext_session_request(
              pair->pair.h, &pair->h, "preview.open", input, strlen(input), 10000, &request) ==
          GT_EXT_RPC_OK);
    gt_ext_rpc_result_t result = {0};
    for (unsigned step = 0; step < 5000 && !result.confirmed; ++step) {
        (void)gt_ext_session_step(pair->pair.h, 0);
        CHECK(gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE);
        result = rpc_result(pair->pair.h, &pair->h, request);
    }
    CHECK(result.confirmed &&
          gt_ext_session_forget_request(pair->pair.h, &pair->h, request) == GT_EXT_RPC_OK);
    return result;
}

static void drop_fixture(pub_source_fixture_t *fixture) {
    res_store_drop(&fixture->pair.h);
    res_store_drop(&fixture->pair.e);
    CHECK(!close(fixture->root));
    CHECK(!unlink(fixture->file) && !rmdir(fixture->directory));
}

static void lua_client_roundtrip(const char *executor_path, const char *client_path) {
    pub_source_fixture_t fixture = {.root = -1};
    preview_pair(&fixture.pair, NULL);
    pub_file(&fixture, 3, "file.bin");
    gt_ext_runtime_t *executor = open_preview(&fixture, executor_path, GT_EXT_EXECUTOR, NULL);
    gt_ext_runtime_t *client = open_preview(&fixture, client_path, GT_EXT_HOST, NULL);
    gt_ext_runtime_phase_t phase = GT_EXT_RUNTIME_CREATED;
    for (unsigned step = 0; step < 5000 && phase != GT_EXT_RUNTIME_ACTIVE; ++step) {
        CHECK(gt_ext_runtime_step(executor) == GT_EXT_RUNTIME_ACTIVE);
        phase = gt_ext_runtime_step(client);
        CHECK(phase == GT_EXT_RUNTIME_STARTING || phase == GT_EXT_RUNTIME_ACTIVE);
    }
    CHECK(phase == GT_EXT_RUNTIME_ACTIVE && res_usage(&fixture.pair.h).resources == 1);
    for (unsigned step = 0; step < 100 && (client || executor); ++step) {
        if (client && gt_ext_runtime_close(client) == GT_EXT_RELEASED) {
            CHECK(gt_ext_runtime_destroy(&client) == GT_OK);
        }
        if (executor && gt_ext_runtime_close(executor) == GT_EXT_RELEASED) {
            CHECK(gt_ext_runtime_destroy(&executor) == GT_OK);
        }
    }
    CHECK(!client && !executor);
    drop_fixture(&fixture);
}

static void native_host_routing(void) {
    rpc_pair_t pair;
    rpc_open(&pair, "success");
    const char source[] = "return {['fixture.done'] = function(input) return input end}";
    gt_ext_runtime_module_t module = {
        .name = "plugin:/main.lua", .source = source, .size = sizeof(source) - 1};
    gt_sha256_t sha;
    gt__sha256_init(&sha);
    CHECK(gt__sha256_update(&sha, source, module.size) && gt__sha256_final(&sha, module.sha256));
    const gt_ext_runtime_config_t config = {
        .role = GT_EXT_HOST, .modules = &module, .module_count = 1, .entry = module.name};
    /* Native bindings retain this original owner for Host services while the
     * runtime owns the Session's lifecycle. Both paths use the same worker. */
    gt_ext_session_t *native = pair.pair.h;
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open(&pair.pair.h, &config, &runtime) == GT_OK && !pair.pair.h);
    CHECK(gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE);
    const uint64_t handle = rpc_start(&pair, true, "{}", 1000);
    gt_ext_rpc_request_t request;
    gt_ext_rpc_status_t status = GT_EXT_RPC_WAIT;
    for (unsigned i = 0; i < 100 && status == GT_EXT_RPC_WAIT; ++i) {
        (void)gt_ext_session_step(pair.pair.e, 0);
        CHECK(gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE);
        status = gt_ext_session_take_request(native, &pair.h, &request);
    }
    CHECK(status == GT_EXT_RPC_OK && !strcmp(request.method, "fixture.ask"));
    rpc_reply(native, &pair.h, &request);
    gt_ext_rpc_result_t result = {0};
    for (unsigned i = 0; i < 100 && !result.confirmed; ++i) {
        (void)gt_ext_session_step(pair.pair.e, 0);
        CHECK(gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE);
        result = rpc_result(pair.pair.e, &pair.e, handle);
    }
    CHECK(result.confirmed && result.state == GT_EXT_RPC_SUCCEEDED);
    for (unsigned i = 0; i < 100 && runtime; ++i) {
        (void)gt_ext_session_step(pair.pair.e, 0);
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            CHECK(gt_ext_runtime_destroy(&runtime) == GT_OK);
        }
    }
    CHECK(!runtime);
    ext_drop(&pair.pair.e);
}

static void package_roundtrip(const char *directory) {
    const int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    const gt_ext_package_config_t capture = {fd, 32, 64, 131072, 1048576};
    gt_ext_package_t *package = NULL;
    CHECK(gt_ext_package_open(&capture, &package) == GT_OK && !close(fd));
    gt_ext_package_phase_t phase;
    do {
        phase = gt_ext_package_step(package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    CHECK(phase == GT_EXT_PACKAGE_CAPTURED);
    const gt_ext_package_check_config_t check = {
        .host_json = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"runtimes\":[\"lua\"],"
                     "\"protocols\":[1],\"capabilities\":[\"session.rpc\",\"resource.receive\","
                     "\"file.readScoped\"]}",
    };
    CHECK(gt_ext_package_check(package, &check) == GT_EXT_PACKAGE_CHECKED);
    const char *argv[] = {"--max-bytes", "3", "file.bin"};
    char decoded[128];
    size_t size;
    CHECK(gt_ext_package_decode_command_arguments(
              package, "preview", argv, 3, decoded, sizeof(decoded), &size) == GT_OK &&
          size == strlen(decoded) && strstr(decoded, "\"maxBytes\":3") &&
          strstr(decoded, "\"assets\":\"\"") && strstr(decoded, "\"path\":\"file.bin\""));
    char exact[128];
    size_t queried = 0;
    CHECK(gt_ext_package_decode_command_arguments(package, "preview", argv, 3, NULL, 0, &queried) ==
              GT_OK &&
          queried == size);
    CHECK(gt_ext_package_decode_command_arguments(
              package, "preview", argv, 3, exact, queried, &queried) == GT_OK &&
          queried == size && !memcmp(exact, decoded, size));
    const char *bad[] = {"--max-bytes=0", "file.bin"};
    CHECK(gt_ext_package_decode_command_arguments(
              package, "preview", bad, 2, decoded, sizeof(decoded), &size) == GT_ERR_INVALID_ARG);
    gt_ext_package_metadata_t metadata;
    CHECK(gt_ext_package_metadata(package, &metadata));
    res_pair_t mismatch = {0};
    preview_pair(&mismatch, NULL);
    const gt_ext_runtime_config_t no_roots = {.role = GT_EXT_EXECUTOR};
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open_package(&mismatch.rpc.pair.e, package, &no_roots, false, &runtime) ==
              GT_ERR_PROTOCOL &&
          !runtime && mismatch.rpc.pair.e);
    res_drop(&mismatch);

    pub_source_fixture_t fixture = {.root = -1};
    preview_pair(&fixture.pair, metadata.package_digest);
    pub_file(&fixture, 3, "file.bin");
    const gt_ext_runtime_root_t root = {
        .id = "workspace", .directory_fd = fixture.root, .byte_limit = 262144};
    const gt_ext_runtime_config_t config = {
        .role = GT_EXT_EXECUTOR, .roots = &root, .root_count = 1};
    CHECK(gt_ext_runtime_open_package(
              &fixture.pair.rpc.pair.e, package, &config, false, &runtime) == GT_OK &&
          runtime && !fixture.pair.rpc.pair.e);
    CHECK(gt_ext_package_destroy(&package) == GT_OK);
    const gt_ext_rpc_result_t result = request_preview(
        &fixture.pair.rpc, runtime, "{\"path\":\"file.bin\",\"assets\":\"\",\"maxBytes\":3}");
    CHECK(result.state == GT_EXT_RPC_SUCCEEDED && res_usage(&fixture.pair.h).resources == 1);
    for (unsigned step = 0; step < 100 && runtime; ++step) {
        (void)gt_ext_session_step(fixture.pair.rpc.pair.h, 0);
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            CHECK(gt_ext_runtime_destroy(&runtime) == GT_OK);
        }
    }
    CHECK(!runtime);
    ext_drop(&fixture.pair.rpc.pair.h);
    drop_fixture(&fixture);
}

static void wait_session(gt_ext_session_t *session, bool host) {
    const uint64_t deadline = ext_now() + 10000;
    for (;;) {
        const gt_ext_session_phase_t phase = gt_ext_session_step(session, 5);
        CHECK(phase < GT_EXT_SESSION_ENDED && ext_now() < deadline);
        if (phase == GT_EXT_SESSION_ACTIVE) {
            return;
        }
        if (host && phase == GT_EXT_SESSION_CONSENT) {
            const gt_ext_session_info_t info = ext_info(session);
            CHECK(gt_ext_session_decide(session, info.consent_token, GT_EXT_DECISION_ALLOW) ==
                  GT_OK);
        }
    }
}

static void close_runtime(gt_ext_runtime_t **runtime) {
    const uint64_t deadline = ext_now() + 10000;
    while (gt_ext_runtime_close(*runtime) != GT_EXT_RELEASED) {
        CHECK(ext_now() < deadline);
    }
    CHECK(gt_ext_runtime_destroy(runtime) == GT_OK);
}

static void wait_client(gt_ext_runtime_t *runtime) {
    const uint64_t deadline = ext_now() + 10000;
    for (;;) {
        const gt_ext_runtime_phase_t phase = gt_ext_runtime_step(runtime);
        CHECK(phase == GT_EXT_RUNTIME_STARTING || phase == GT_EXT_RUNTIME_ACTIVE);
        CHECK(ext_now() < deadline);
        if (phase == GT_EXT_RUNTIME_ACTIVE) {
            break;
        }
        struct timespec delay = {.tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
    }
}

static void attached_client(const char *source, const char *digest) {
    gt_ext_channel_t *channel = NULL;
    CHECK(strlen(digest) == 64 && gt_ext_channel_open_fd(0, 1, &channel) == GT_OK);
    pub_source_fixture_t fixture = {.root = -1};
    gt_ext_session_t *session =
        preview_session(&channel, &fixture.pair.h, GT_EXT_HOST, false, digest);
    fixture.pair.rpc.pair.h = session;
    wait_session(session, true);
    gt_ext_lease_t lease;
    CHECK(gt_ext_session_lease(session, &lease));
    gt_ext_runtime_t *runtime = open_preview(&fixture, source, GT_EXT_HOST, NULL);
    wait_client(runtime);
    gt_ext_resource_snapshot_t snapshot;
    CHECK(gt_ext_session_take_resource(session, &lease, &snapshot) == GT_EXT_RESOURCE_OK);
    char bytes[3];
    size_t size;
    CHECK(gt_ext_resource_store_read(
              fixture.pair.h.s, &lease, &snapshot, 0, bytes, sizeof(bytes), &size) ==
              GT_EXT_RESOURCE_OK &&
          size == sizeof(bytes) && !memcmp(bytes, "abc", size));
    CHECK(gt_ext_resource_store_release(fixture.pair.h.s, &lease, &snapshot) == GT_EXT_RESOURCE_OK);
    gt_ext_session_revoke(session);
    const uint64_t deadline = ext_now() + 5000;
    while (gt_ext_session_step(session, 5) < GT_EXT_SESSION_ENDED) {
        CHECK(ext_now() < deadline);
    }
    close_runtime(&runtime);
    res_store_drop(&fixture.pair.h);
}

static void attached_pending_client(const char *digest) {
    gt_ext_channel_t *channel = NULL;
    CHECK(strlen(digest) == 64 && gt_ext_channel_open_fd(0, 1, &channel) == GT_OK);
    res_store_t store = {0};
    gt_ext_session_t *session = preview_session(&channel, &store, GT_EXT_HOST, false, digest);
    wait_session(session, true);
    gt_ext_lease_t lease;
    CHECK(gt_ext_session_lease(session, &lease));
    static const char input[] = "{\"path\":\"./file.bin\",\"assets\":\"\",\"maxBytes\":3}";
    uint64_t request = 0;
    CHECK(gt_ext_session_request(
              session, &lease, "preview.open", input, sizeof(input) - 1, 10000, &request) ==
              GT_EXT_RPC_OK &&
          request);
    uint64_t deadline = ext_now() + 8000;
    gt_ext_resource_snapshot_t resource;
    for (;;) {
        CHECK(gt_ext_session_step(session, 5) < GT_EXT_SESSION_ENDED && ext_now() < deadline);
        gt_ext_resource_status_t status = gt_ext_session_take_resource(session, &lease, &resource);
        if (status == GT_EXT_RESOURCE_OK) {
            break;
        }
        CHECK(status == GT_EXT_RESOURCE_WAIT);
    }
    CHECK(gt_ext_resource_store_release(store.s, &lease, &resource) == GT_EXT_RESOURCE_OK);
    gt_ext_rpc_result_t result;
    CHECK(gt_ext_session_request_result(session, &lease, request, &result) == GT_EXT_RPC_OK &&
          !result.confirmed);
    fputs("PENDING\n", stderr);
    deadline = ext_now() + 8000;
    for (;;) {
        (void)gt_ext_session_step(session, 5);
        CHECK(ext_now() < deadline &&
              gt_ext_session_request_result(session, &lease, request, &result) == GT_EXT_RPC_OK &&
              !result.confirmed);
        if (result.state == GT_EXT_RPC_UNKNOWN) {
            break;
        }
    }
    CHECK(gt_ext_session_forget_request(session, &lease, request) == GT_EXT_RPC_OK);
    ext_drop(&session);
    res_store_drop(&store);
}

#ifdef GT_HAVE_LIBSSH2
static void ssh_executor(const char *source, const char *workspace) {
    gt_ext_channel_t *channel = NULL;
    CHECK(gt_ext_channel_open_fd(0, 1, &channel) == GT_OK);
    pub_source_fixture_t fixture = {.root = open(workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    CHECK(fixture.root >= 0);
    fixture.pair.rpc.pair.e =
        preview_session(&channel, &fixture.pair.e, GT_EXT_EXECUTOR, false, NULL);
    wait_session(fixture.pair.rpc.pair.e, false);
    gt_ext_runtime_t *runtime = open_preview(&fixture, source, GT_EXT_EXECUTOR, NULL);
    const uint64_t deadline = ext_now() + 10000;
    while (gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE) {
        CHECK(ext_now() < deadline);
        struct timespec delay = {.tv_nsec = 1000000};
        (void)nanosleep(&delay, NULL);
    }
    close_runtime(&runtime);
    res_store_drop(&fixture.pair.e);
    CHECK(!close(fixture.root));
}

static void ssh_client(int argc, char **argv) {
    gt_ext_channel_t *channel = rpc_dial(argc, argv);
    pub_source_fixture_t fixture = {.root = -1};
    const char *digest = getenv("GHOSTERM_TEST_PREVIEW_EXECUTOR_DIGEST");
    CHECK(!digest || strlen(digest) == 64);
    fixture.pair.rpc.pair.h = preview_session(&channel, &fixture.pair.h, GT_EXT_HOST, true, digest);
    wait_session(fixture.pair.rpc.pair.h, true);
    const char *rejected_path = getenv("GHOSTERM_TEST_PREVIEW_REJECTED_PATH");
    gt_ext_runtime_t *runtime = open_preview(&fixture, argv[1], GT_EXT_HOST, rejected_path);
    if (!rejected_path) {
        wait_client(runtime);
        CHECK(res_usage(&fixture.pair.h).resources == 1);
    } else {
        const uint64_t deadline = ext_now() + 10000;
        gt_ext_runtime_phase_t phase;
        do {
            phase = gt_ext_runtime_step(runtime);
            CHECK(ext_now() < deadline);
            struct timespec delay = {.tv_nsec = 1000000};
            (void)nanosleep(&delay, NULL);
        } while (phase == GT_EXT_RUNTIME_STARTING);
        gt_ext_runtime_info_t info;
        CHECK(phase == GT_EXT_RUNTIME_FAILED && gt_ext_runtime_info(runtime, &info) &&
              info.error == GT_EXT_RUNTIME_SCRIPT);
        CHECK(res_usage(&fixture.pair.h).resources == 0);
    }
    CHECK(gt_ext_test_dials == 1);
    close_runtime(&runtime);
    res_store_drop(&fixture.pair.h);
}
#endif

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--attached-pending-client")) {
        res_root = argv[3];
        attached_pending_client(argv[2]);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--attached-client")) {
        res_root = argv[4];
        attached_client(argv[2], argv[3]);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--package")) {
        res_root = argv[3];
        package_roundtrip(argv[2]);
        return 0;
    }
#ifdef GT_HAVE_LIBSSH2
    if (argc == 5 && !strcmp(argv[1], "--executor")) {
        res_root = argv[4];
        ssh_executor(argv[2], argv[3]);
        return 0;
    }
    if (argc >= 8) {
        res_root = getenv("TMPDIR");
        CHECK(res_root);
        ssh_client(argc, argv);
        return 0;
    }
#endif
    CHECK(argc == 4);
    res_root = argv[3];
    const size_t before = ext_fds();
    pub_source_fixture_t fixture = {.root = -1};
    preview_pair(&fixture.pair, NULL);
    pub_file(&fixture, 3, "file.bin");
    gt_ext_runtime_t *runtime = open_preview(&fixture, argv[1], GT_EXT_EXECUTOR, NULL);
    gt_runtime_resources_t *receiver =
        resource_owner(fixture.pair.rpc.pair.h, fixture.pair.h.s, &fixture.pair.rpc.h, -1);

    gt_ext_rpc_result_t result = request_preview(
        &fixture.pair.rpc, runtime, "{\"path\":\"../file.bin\",\"assets\":\"\",\"maxBytes\":3}");
    CHECK(result.state == GT_EXT_RPC_FAILED && strstr(result.outcome, "INVALID_INPUT") &&
          !res_usage(&fixture.pair.h).resources);

    result = request_preview(
        &fixture.pair.rpc, runtime, "{\"path\":\"./file.bin\",\"assets\":\"\",\"maxBytes\":3}");
    CHECK(result.state == GT_EXT_RPC_SUCCEEDED);
    uint64_t resource;
    gt_ext_resource_descriptor_t descriptor;
    CHECK(gt__runtime_resources_take(receiver, &resource, &descriptor) == GT_RUNTIME_RESOURCE_OK);
    CHECK(descriptor.size == 3 && strstr(result.outcome, descriptor.sha256) &&
          strstr(result.outcome, descriptor.resource_id));
    verify_resource(&fixture, receiver, resource, 3, "roundtrip");
    CHECK(gt__runtime_resources_release(receiver, resource) == GT_RUNTIME_RESOURCE_OK);
    CHECK(gt__runtime_resources_destroy(&receiver) == GT_RUNTIME_RESOURCE_OK);

    for (unsigned step = 0; step < 100 && runtime; ++step) {
        (void)gt_ext_session_step(fixture.pair.rpc.pair.h, 0);
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            CHECK(gt_ext_runtime_destroy(&runtime) == GT_OK);
        }
    }
    CHECK(!runtime);
    ext_drop(&fixture.pair.rpc.pair.h);
    drop_fixture(&fixture);
    lua_client_roundtrip(argv[1], argv[2]);
    native_host_routing();
    CHECK(ext_fds() == before);
    return 0;
}
