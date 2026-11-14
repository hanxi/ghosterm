#define RUNTIME_RESOURCE_MAIN process_runtime_resource_fixture_main
#include "test_runtime_resources.c"
#include "extension/process-runtime.h"
#include "extension/package-check.h"
#include "extension/package-stage.h"
#include "extension/memory.h"
#include "extension/process-rpc.h"
#include "base/sha256.h"
#include <sys/stat.h>
#include <signal.h>

/* A normal assertion red must still stop/reap our actual child. */
static gt_process_runtime_t *runtime;
static gt_ext_runtime_t *public_runtime;
static bool public_mode;
static int launch_host_socket = -1;
static gt_process_jobs_t *native_jobs;
static gt_runtime_resources_t *native_roots;
static int cleanup_parent = -1;
static char cleanup_root[4097];

static gt_process_runtime_phase_t launch_public_phase(gt_ext_runtime_phase_t p) {
    return p == GT_EXT_RUNTIME_CREATED  ? GT_PROCESS_RUNTIME_CREATED
           : p == GT_EXT_RUNTIME_ACTIVE ? GT_PROCESS_RUNTIME_ACTIVE
           : p == GT_EXT_RUNTIME_CLOSED ? GT_PROCESS_RUNTIME_CLOSED
           : p >= GT_EXT_RUNTIME_FAILED ? GT_PROCESS_RUNTIME_STOPPING
                                        : GT_PROCESS_RUNTIME_STARTING;
}

static gt_process_runtime_phase_t launch_step(gt_process_runtime_t *r) {
    return public_mode ? launch_public_phase(gt_ext_runtime_step(public_runtime))
                       : gt__process_runtime_step(r);
}

static void launch_stop(gt_process_runtime_t *r) {
    if (public_mode) {
        gt_ext_runtime_stop(public_runtime);
    } else {
        gt__process_runtime_stop(r);
    }
}

static gt_status_t launch_destroy(gt_process_runtime_t **r) {
    return public_mode ? gt_ext_runtime_destroy(&public_runtime) : gt__process_runtime_destroy(r);
}

static bool launch_info(gt_process_runtime_t *r, gt_process_runtime_info_t *out) {
    if (!public_mode) {
        return gt__process_runtime_info(r, out);
    }
    gt_ext_runtime_info_t v;
    if (!gt_ext_runtime_info(public_runtime, &v)) {
        return false;
    }
    CHECK(v.external_process);
    *out = (gt_process_runtime_info_t){.phase = launch_public_phase(v.phase),
                                       .error = v.execution_error,
                                       .cleanup_error = v.cleanup_error,
                                       .session_end = v.session_end,
                                       .session_error = v.session_error,
                                       .activated = v.activated,
                                       .child_exited = v.child_exited,
                                       .probe_verified = v.probe_verified,
                                       .exit_code = v.exit_code,
                                       .term_signal = v.term_signal,
                                       .group_signal_error = v.group_signal_error,
                                       .stdout_bytes = v.stdout_bytes,
                                       .stderr_bytes = v.stderr_bytes};
    return true;
}

static gt_status_t launch_public_open(gt_ext_session_t **session,
                                      gt_package_snapshot_t **sdk,
                                      const gt_process_runtime_config_t *c,
                                      const char *package_path,
                                      const char *sdk_path,
                                      const char *facts,
                                      const char *mode) {
    int fd = open(package_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(fd >= 0);
    gt_ext_package_config_t capture = {fd, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    CHECK(gt_ext_package_open(&capture, &package) == GT_OK && !close(fd));
    gt_ext_package_phase_t phase;
    do {
        phase = gt_ext_package_step(package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    CHECK(phase == GT_EXT_PACKAGE_CAPTURED);
    gt_ext_package_check_config_t check = {.host_json = facts};
    CHECK(gt_ext_package_check(package, &check) == GT_EXT_PACKAGE_CHECKED);
    gt_ext_runtime_root_t roots[8];
    for (size_t i = 0; i < c->root_count; i++) {
        roots[i] = (gt_ext_runtime_root_t){
            c->roots[i].id, c->roots[i].directory_fd, c->roots[i].byte_limit};
    }
    char names[32][129] = {{0}}, business_names[32][129] = {{0}};
    gt_ext_process_environment_t values[32], business_values[32];
    for (size_t i = 0; i < c->environment_count; i++) {
        const char *equal = strchr(c->environment[i], '=');
        CHECK(equal && equal - c->environment[i] < 129);
        memcpy(names[i], c->environment[i], (size_t)(equal - c->environment[i]));
        values[i] = (gt_ext_process_environment_t){names[i], equal + 1};
    }
    for (size_t i = 0; i < c->process.environment_count; i++) {
        const char *equal = strchr(c->process.environment[i], '=');
        CHECK(equal && equal - c->process.environment[i] < 129);
        memcpy(business_names[i],
               c->process.environment[i],
               (size_t)(equal - c->process.environment[i]));
        business_values[i] = (gt_ext_process_environment_t){business_names[i], equal + 1};
    }
    int sdk_fd = *sdk ? open(sdk_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    CHECK(!*sdk || sdk_fd >= 0);
    gt_ext_runtime_execution_t execution = {.stage_parent_fd = c->stage_parent_fd,
                                            .stage_parent_path = c->stage_parent_path,
                                            .execution_authorized = c->execution_authorized,
                                            .node_executable = c->node_executable,
                                            .node_sha256 = c->node_sha256,
                                            .node_sdk_directory_fd = sdk_fd,
                                            .node_sdk_catalog_sha256 = c->sdk_catalog_sha256,
                                            .environment = values,
                                            .environment_count = c->environment_count,
                                            .lifetime_ms = c->lifetime_ms,
                                            .stop_grace_ms = c->stop_grace_ms,
                                            .stdout_bytes = c->stdout_bytes,
                                            .stderr_bytes = c->stderr_bytes};
    if (!*sdk) {
        execution.node_executable = NULL;
        execution.node_sha256 = NULL;
        execution.node_sdk_catalog_sha256 = NULL;
    }
    gt_ext_runtime_config_t config = {.role = GT_EXT_EXECUTOR,
                                      .activation_ms = c->activation_ms,
                                      .roots = roots,
                                      .root_count = c->root_count,
                                      .process_programs = c->process.programs,
                                      .process_program_count = c->process.program_count,
                                      .process_environment = business_values,
                                      .process_environment_count = c->process.environment_count,
                                      .process_lifetime_ms = c->process.lifetime_ms,
                                      .process_output_bytes = c->process.output_bytes,
                                      .invocation = c->invocation,
                                      .execution = &execution};
    if (!strcmp(mode, "public-denied-sdk")) {
        execution.execution_authorized = false;
        execution.node_sdk_directory_fd = -1;
    }
    if (!strcmp(mode, "public-bad-name")) {
        values[0].name = "PATH=INJECT";
    }
    if (!strcmp(mode, "public-no-execution")) {
        config.execution = NULL;
    }
    if (!strcmp(mode, "public-budget")) {
        config.memory_bytes = 16777216;
    }
    if (!strcmp(mode, "public-role")) {
        config.role = GT_EXT_HOST;
    }
    gt_status_t status = gt_ext_runtime_open_package(
        session, package, &config, c->command_associated, &public_runtime);
    CHECK(gt_ext_package_destroy(&package) == GT_OK && !package);
    if (sdk_fd >= 0) {
        CHECK(!close(sdk_fd));
    }
    if (status == GT_OK && *sdk) {
        CHECK(gt__package_snapshot_destroy(sdk) == GT_PACKAGE_OK);
    }
    return status;
}

static void launch_cleanup(void) {
    if (native_jobs) {
        gt__process_jobs_stop(native_jobs);
        uint64_t until = ext_now() + 3000;
        while (native_jobs && ext_now() < until) {
            (void)gt__process_jobs_step(native_jobs);
            if (gt__process_jobs_destroy(&native_jobs) == GT_OK) {
                break;
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    if (native_roots && !native_jobs) {
        (void)gt__runtime_resources_destroy(&native_roots);
    }
    if (runtime || public_runtime) {
        launch_stop(runtime);
        uint64_t until = ext_now() + 6000;
        while (ext_now() < until && launch_step(runtime) != GT_PROCESS_RUNTIME_CLOSED) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        (void)launch_destroy(&runtime);
    }
    if (cleanup_parent >= 0) {
        (void)close(cleanup_parent);
    }
    if (cleanup_root[0]) {
        (void)rmdir(cleanup_root);
    }
}

static gt_package_snapshot_t *launch_capture(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(fd >= 0);
    gt_package_capture_config_t c = {fd, 1024, 4096, 16777216, 67108864};
    gt_package_snapshot_t *p = NULL;
    CHECK(gt__package_capture_open(&c, NULL, &p) == GT_PACKAGE_OK && !close(fd));
    gt_package_status_t status;
    do {
        status = gt__package_capture_step(p);
    } while (status == GT_PACKAGE_WAIT);
    CHECK(status == GT_PACKAGE_OK);
    return p;
}

static void launch_pair(rpc_pair_t *f,
                        const gt_package_check_info_t *package,
                        res_store_t *hs,
                        res_store_t *es) {
    memset(f, 0, sizeof(*f));
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    launch_host_socket = fd[0];
    gt_ext_channel_t *h = ext_channel(fd[0]), *e = ext_channel(fd[1]);
    gt_ext_session_config_t c = ext_config();
    c.features = GT_EXT_FEATURE_REQUESTS;
    strcpy(c.plugin_id, package->id);
    strcpy(c.plugin_version, package->version);
    strcpy(c.executor_digest, package->package_digest);
    strcpy(c.required[0], "session.rpc");
    strcpy(c.available[0], "session.rpc");
    c.required_count = c.available_count = 1;
    if (package->process_program_count) {
        strcpy(c.required[1], "process.spawn");
        strcpy(c.required[2], "process.pipes");
        strcpy(c.available[1], "process.spawn");
        strcpy(c.available[2], "process.pipes");
        c.required_count = c.available_count = 3;
    }
    gt_ext_rpc_method_t methods[4];
    size_t count = rpc_methods(methods);
    for (size_t i = 0; i < count; i++) {
        strcpy(methods[i].capability, "session.rpc");
    }
    if (hs) {
        c.features |= GT_EXT_FEATURE_RESOURCES;
        strcpy(c.required[1], "resource.receive");
        strcpy(c.required[2], "file.readScoped");
        strcpy(c.available[1], "resource.receive");
        strcpy(c.available[2], "file.readScoped");
        c.required_count = c.available_count = 3;
        CHECK(gt_ext_session_open_resources(
                  &h, GT_EXT_HOST, &c, methods, count, hs->s, 1048576, &f->pair.h) == GT_OK);
        CHECK(gt_ext_session_open_resources(
                  &e, GT_EXT_EXECUTOR, &c, methods, count, es->s, 1048576, &f->pair.e) == GT_OK);
    } else {
        f->pair.h = rpc_adopt(&h, GT_EXT_HOST, &c, methods, count);
        f->pair.e = rpc_adopt(&e, GT_EXT_EXECUTOR, &c, methods, count);
    }
    ext_allow(&f->pair);
    CHECK(gt_ext_session_lease(f->pair.h, &f->h) && gt_ext_session_lease(f->pair.e, &f->e));
}

static void launch_jobs_native(rpc_pair_t *f,
                               gt_package_checked_t *package,
                               const gt_runtime_root_t *root,
                               const gt_process_jobs_config_t *config) {
    size_t fds = ext_fds();
    CHECK(gt__runtime_resources_open(
              f->pair.e, NULL, &f->e, root, 1, NULL, resource_clock, &native_roots) == GT_OK);
    CHECK(gt__process_jobs_open(f->pair.e, native_roots, package, config, &native_jobs) == GT_OK);
    gt_process_rpc_t *rpc = NULL;
    CHECK(gt__process_rpc_open_services(f->pair.h, native_roots, native_jobs, NULL, &rpc) ==
              GT_ERR_INVALID_ARG &&
          !rpc);
    const char *args[] = {"cat"}, *version = NULL;
    uint64_t ids[8], extra = 0;
    for (size_t i = 0; i < 8; i++) {
        CHECK(gt__process_jobs_spawn(native_jobs, "cat", "workspace", args, 1, &ids[i], &version) ==
                  GT_OK &&
              !strcmp(version, "fixture-cat") && (!i || ids[i] > ids[i - 1]));
    }
    CHECK(gt__process_jobs_spawn(native_jobs, "cat", "workspace", args, 1, &extra, &version) ==
              GT_PROCESS_JOBS_WAIT &&
          !extra);
    CHECK(gt__process_jobs_destroy(&native_jobs) == GT_PROCESS_JOBS_WAIT && native_jobs);
    CHECK(gt__process_rpc_open_services(f->pair.e, native_roots, native_jobs, NULL, &rpc) ==
              GT_ERR_INVALID_ARG &&
          !rpc);
    CHECK(gt__process_jobs_release(native_jobs, ids[0]) == GT_PROCESS_JOBS_WAIT);
    CHECK(gt__process_jobs_spawn(native_jobs, "cat", "outside", args, 1, &extra, &version) ==
          GT_ERR_AUTH);
    for (size_t i = 0; i < 8; i++) {
        CHECK(gt__process_jobs_signal(native_jobs, ids[i], true) == GT_OK);
    }
    uint64_t until = ext_now() + 3000;
    bool done = false;
    while (ext_now() < until) {
        CHECK(gt__process_jobs_step(native_jobs) == GT_OK);
        done = true;
        for (size_t i = 0; i < 8; i++) {
            gt_process_job_info_t v;
            CHECK(gt__process_jobs_poll(native_jobs, ids[i], &v) == GT_OK);
            done = done && v.process.exited;
        }
        if (done) {
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(done);
    for (size_t i = 0; i < 8; i++) {
        CHECK(gt__process_jobs_release(native_jobs, ids[i]) == GT_OK);
    }
    CHECK(gt__process_jobs_release(native_jobs, ids[0]) == GT_ERR_INVALID_ARG);
    CHECK(gt__process_jobs_spawn(native_jobs, "cat", "workspace", args, 1, &extra, &version) ==
              GT_OK &&
          extra > ids[7]);
    gt_ext_session_revoke(f->pair.e);
    uint8_t byte = 1;
    CHECK(gt__process_jobs_write(native_jobs, extra, &byte, 1).state == GT_EXT_PROCESS_CLOSED);
    CHECK(gt__process_jobs_read(native_jobs, extra, false, &byte, 1).state ==
          GT_EXT_PROCESS_CLOSED);
    uint64_t denied = 0;
    CHECK(gt__process_jobs_spawn(native_jobs, "cat", "workspace", args, 1, &denied, &version) ==
              GT_ERR_AUTH &&
          !denied);
    gt__process_jobs_stop(native_jobs);
    until = ext_now() + 3000;
    while (native_jobs && ext_now() < until) {
        CHECK(gt__process_jobs_step(native_jobs) == GT_OK);
        if (gt__process_jobs_destroy(&native_jobs) == GT_OK) {
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(!native_jobs && gt__runtime_resources_destroy(&native_roots) == GT_RUNTIME_RESOURCE_OK &&
          !native_roots);
    CHECK(ext_fds() == fds);
}

int main(int argc, char **argv) {
    /* mode package sdk temp canonical-node node-sha sdk-catalog facts.json */
    CHECK(argc == 9 || (argc == 10 && !strcmp(argv[9], "public")));
    public_mode = argc == 10;
    CHECK(!atexit(launch_cleanup));
    const char *mode = argv[1];
    bool with_resources = !strncmp(mode, "resource-", 9) && strcmp(mode, "resource-missing");
    bool with_process = !strncmp(mode, "process-", 8);
    res_root = argv[4];
    res_store_t hs = {0}, es = {0};
    if (with_resources) {
        hs = res_store(1048576);
        es = res_store(1048576);
    }
    int fact = open(argv[8], O_RDONLY | O_CLOEXEC);
    CHECK(fact >= 0);
    char facts[8192] = {0};
    ssize_t n = read(fact, facts, sizeof(facts) - 1);
    CHECK(n > 0 && !close(fact));
    gt_package_snapshot_t *snapshot = launch_capture(argv[2]);
    gt_package_checked_t *package = NULL;
    gt_package_check_config_t check = {.host_json = facts, .host_size = (size_t)n};
    gt_package_check_report_t report;
    CHECK(gt__package_check(&snapshot, &check, NULL, &package, &report) ==
          GT_PACKAGE_CHECK_COMPATIBLE);
    gt_package_check_info_t package_info;
    CHECK(gt__package_checked_info(package, &package_info));
    bool node = !strcmp(package_info.runtime, "node");
    gt_package_snapshot_t *sdk = node ? launch_capture(argv[3]) : NULL;
    if (node) {
        char digest[65];
        CHECK(gt__package_stage_catalog_digest(sdk, digest) && !strcmp(digest, argv[7]));
    }
    rpc_pair_t f;
    if (!strcmp(mode, "wrong-identity")) {
        strcpy(package_info.id, "org.ghosterm.bad");
    }
    launch_pair(&f, &package_info, with_resources ? &hs : NULL, with_resources ? &es : NULL);
    char root[4097];
    CHECK(snprintf(root, sizeof(root), "%s/process-runtime-XXXXXX", argv[4]) > 0 && mkdtemp(root));
    int parent = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(parent >= 0);
    cleanup_parent = parent;
    strcpy(cleanup_root, root);
    pub_source_fixture_t file = {.root = -1};
    gt_runtime_resources_t *host_resources = NULL;
    gt_runtime_root_t workspace = {0};
    if (with_resources || with_process) {
        pub_file(
            &file, 262144, !strcmp(mode, "resource-unicode") ? "展示\"\\file.bin" : "file.bin");
        workspace = (gt_runtime_root_t){"workspace", file.root, 262144};
        if (with_resources) {
            host_resources = resource_owner(f.pair.h, hs.s, &f.h, file.root);
        }
    }
    const char *arguments[] = {"--label", "中文 \"';$()"};
    gt_ext_invocation_t invocation = {"fixture", arguments, 2};
    char large[4097];
    memset(large, 1, sizeof(large) - 1);
    large[4096] = 0;
    const char *large_arguments[] = {"--label", large, "--second", large, "--third", large};
    if (!strcmp(mode, "argument-boundary")) {
        invocation = (gt_ext_invocation_t){"fixture", large_arguments, 6};
    }
    char fixture_mode[256];
    CHECK(snprintf(fixture_mode, sizeof(fixture_mode), "GT_RUNTIME_MODE=%s", mode) > 0);
    char descendant_path[4097], descendant_env[4200];
    CHECK(snprintf(descendant_path, sizeof(descendant_path), "%s/descendant.pid", root) > 0);
    CHECK(snprintf(descendant_env,
                   sizeof(descendant_env),
                   "GT_RUNTIME_DESCENDANT_FILE=%s",
                   descendant_path) > 0);
    const char *environment[] = {"PATH=/usr/bin:/bin",
                                 "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1",
                                 "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1",
                                 fixture_mode,
                                 descendant_env};
    gt_process_runtime_config_t config = {.stage_parent_fd = parent,
                                          .stage_parent_path = root,
                                          .execution_authorized = true,
                                          .node_executable = argv[5],
                                          .node_sha256 = argv[6],
                                          .sdk_catalog_sha256 = argv[7],
                                          .invocation = &invocation,
                                          .environment = environment,
                                          .environment_count = 5,
                                          .activation_ms = 5000,
                                          .lifetime_ms = 10000,
                                          .stop_grace_ms = 1000};
    if (with_resources || with_process) {
        config.roots = &workspace;
        config.root_count = 1;
    }
    char process_digests[3][65];
    gt_ext_process_program_mapping_t programs[] = {
        {"cat", "/bin/cat", "fixture-cat", process_digests[0], NULL, 0},
        {"env", "/usr/bin/env", "fixture-env", process_digests[1], NULL, 0},
        {"pwd", "/bin/pwd", "fixture-pwd", process_digests[2], NULL, 0}};
    char workspace_environment[4200];
    CHECK(snprintf(workspace_environment,
                   sizeof(workspace_environment),
                   "GT_RUNTIME_WORKSPACE=%s",
                   file.directory) > 0);
    const char *process_environment[] = {"GT_SELECTED=approved"};
    const char *process_runtime_environment[6];
    if (with_process) {
        for (size_t i = 0; i < 3; i++) {
            int fd = open(programs[i].executable, O_RDONLY | O_CLOEXEC);
            CHECK(fd >= 0);
            gt_sha256_t hash;
            gt__sha256_init(&hash);
            uint8_t buffer[32768];
            ssize_t count;
            while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
                CHECK(gt__sha256_update(&hash, buffer, (size_t)count));
            }
            CHECK(!count && !close(fd) && gt__sha256_final(&hash, process_digests[i]));
        }
        for (size_t i = 0; i < 5; i++) {
            process_runtime_environment[i] = environment[i];
        }
        process_runtime_environment[5] = workspace_environment;
        config.environment = process_runtime_environment;
        config.environment_count = 6;
        config.process = (gt_process_jobs_config_t){
            programs,
            3,
            process_environment,
            1,
            (!strcmp(mode, "process-deadline") || !strcmp(mode, "process-first-reason")) ? 300
                                                                                         : 10000,
            50,
            (!strcmp(mode, "process-output") || !strcmp(mode, "process-first-reason")) ? 16
                                                                                       : 1048576};
        if (!strcmp(mode, "process-bad-digest")) {
            programs[0].verified_sha256 =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        }
        if (!strcmp(mode, "process-missing")) {
            config.process.program_count = 0;
        }
    }
    gt_runtime_root_t duplicate_roots[2] = {workspace, workspace};
    if (!strcmp(mode, "resource-bad-root")) {
        workspace.directory_fd = -1;
    }
    if (!strcmp(mode, "resource-duplicate-root")) {
        config.roots = duplicate_roots;
        config.root_count = 2;
    }
    if (!strcmp(mode, "denied")) {
        config.execution_authorized = false;
    }
    if (!strcmp(mode, "stdout-limit")) {
        config.stdout_bytes = 1;
    }
    if (!strcmp(mode, "stderr-limit")) {
        config.stderr_bytes = 1;
    }
    if (!strcmp(mode, "activation-timeout")) {
        config.activation_ms = 800;
    }
    if (!strcmp(mode, "lifetime")) {
        config.lifetime_ms = 1000;
    }
    if (!strcmp(mode, "busy") || !strcmp(mode, "stubborn")) {
        config.stop_grace_ms = 50;
    }
    if (!strcmp(mode, "wrong-sha")) {
        config.node_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
    char node_alias[4097];
    if (!strcmp(mode, "node-alias")) {
        CHECK(snprintf(node_alias, sizeof(node_alias), "/%s", argv[5]) > 0);
        config.node_executable = node_alias;
    }
    if (!strcmp(mode, "bad-env")) {
        environment[3] = "NODE_OPTIONS=--invalid";
    }
    if (!strcmp(mode, "bad-env-name")) {
        environment[3] = "=invalid";
    }
    if (!strcmp(mode, "duplicate-env")) {
        environment[3] = "PATH=duplicate";
    }
    if (!strcmp(mode, "bad-arguments")) {
        arguments[0] = "--unknown";
    }
    if (!strcmp(mode, "bad-command")) {
        invocation.command = "missing";
    }
    if (!strcmp(mode, "bad-sdk")) {
        config.sdk_catalog_sha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
    gt_ext_session_t *original = f.pair.e;
    gt_package_snapshot_t *original_sdk = sdk;
    bool direct = !strcmp(mode, "process-native");
    if (direct) {
        launch_jobs_native(&f, package, &workspace, &config.process);
    }
    gt_status_t status =
        direct ? GT_OK
               : (public_mode
                      ? launch_public_open(&f.pair.e, &sdk, &config, argv[2], argv[3], facts, mode)
                      : gt__process_runtime_open(&f.pair.e, package, &sdk, &config, &runtime));
    bool rejected = !strcmp(mode, "denied") || !strcmp(mode, "wrong-sha") ||
                    !strcmp(mode, "bad-env") || !strcmp(mode, "bad-arguments") ||
                    !strcmp(mode, "bad-command") || !strcmp(mode, "bad-sdk") ||
                    !strcmp(mode, "wrong-identity") || !strcmp(mode, "bad-env-name") ||
                    !strcmp(mode, "duplicate-env") || !strcmp(mode, "node-alias") ||
                    !strcmp(mode, "resource-missing") || !strcmp(mode, "resource-bad-root") ||
                    !strcmp(mode, "resource-duplicate-root") ||
                    !strcmp(mode, "process-bad-digest") || !strcmp(mode, "process-missing") ||
                    !strncmp(mode, "public-", 7);
    if (direct) {
        CHECK(!runtime && f.pair.e == original && sdk == original_sdk);
    } else if (rejected) {
        gt_status_t expected = !strcmp(mode, "denied") || !strcmp(mode, "wrong-sha") ||
                                       !strcmp(mode, "bad-sdk") || !strcmp(mode, "node-alias")
                                   ? GT_ERR_AUTH
                               : !strcmp(mode, "wrong-identity") ? GT_ERR_PROTOCOL
                               : !strcmp(mode, "bad-command")    ? GT_ERR_NOT_FOUND
                                                                 : GT_ERR_INVALID_ARG;
        if (!strcmp(mode, "resource-missing") || !strcmp(mode, "process-missing")) {
            expected = GT_ERR_UNSUPPORTED;
        }
        if (!strcmp(mode, "process-bad-digest")) {
            expected = GT_ERR_AUTH;
        }
        if (!strncmp(mode, "public-", 7)) {
            expected = !strcmp(mode, "public-denied-sdk") ? GT_ERR_AUTH
                       : !strcmp(mode, "public-bad-name") ? GT_ERR_INVALID_ARG
                                                          : GT_ERR_UNSUPPORTED;
        }
        CHECK(status == expected && !public_runtime && !runtime && f.pair.e == original &&
              sdk == original_sdk);
        CHECK(gt_ext_session_access(original, &f.e, "session.rpc") == GT_EXT_ACCESS_ALLOWED);
    } else {
        CHECK(status == GT_OK && (runtime || public_runtime) && !f.pair.e && (!node || !sdk));
        if (with_resources || with_process) {
            CHECK(!close(file.root));
            file.root = -1;
        }
        CHECK(launch_destroy(&runtime) == GT_ERR_INVALID_ARG && (runtime || public_runtime));
        if (!strcmp(mode, "retained")) {
            CHECK(gt__package_checked_destroy(&package) == GT_PACKAGE_OK);
        }
        if (!strcmp(mode, "stop-created")) {
            launch_stop(runtime);
        }
        if (!strcmp(mode, "frozen")) {
            char path[4097];
            CHECK(snprintf(path, sizeof(path), "%s/executor/main.mjs", argv[2]) > 0);
            int file = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
            CHECK(file >= 0);
            CHECK(write(file, "throw Error('CHANGED');", 23) == 23 && !close(file));
        }
        if (!strcmp(mode, "sdk-frozen")) {
            char path[4097];
            CHECK(snprintf(path, sizeof(path), "%s/node/runtime/bootstrap.mjs", argv[3]) > 0);
            int file = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
            CHECK(file >= 0 && write(file, "throw Error('CHANGED');", 23) == 23 && !close(file));
        }
        gt_process_runtime_info_t info = {0};
        uint64_t handle = 0;
        bool roundtrip = !strcmp(mode, "close-public") || !strcmp(mode, "roundtrip") ||
                         !strcmp(mode, "retained") || !strcmp(mode, "frozen") ||
                         !strcmp(mode, "busy") || !strcmp(mode, "stubborn") ||
                         !strcmp(mode, "sdk-frozen") || !strcmp(mode, "argument-boundary") ||
                         !strcmp(mode, "stderr-eof") || !strcmp(mode, "resource-roundtrip") ||
                         !strcmp(mode, "resource-unicode") ||
                         (with_process && strcmp(mode, "process-stop") &&
                          strcmp(mode, "process-extra-field") && strcmp(mode, "process-bad-hex"));
        bool stopping = !strcmp(mode, "stop-created"), completed = false;
        uint64_t began = ext_now(), deadline = began + 15000;
        bool resource_received = false;
        uint64_t host_publication = 0;
        while (ext_now() < deadline) {
            gt_ext_session_step(f.pair.h, 0);
            if (host_resources) {
                gt__runtime_resources_step(host_resources);
                if (!strcmp(mode, "resource-stop") && res_usage(&hs).reserved_bytes) {
                    launch_stop(runtime);
                    stopping = true;
                }
                uint64_t pin;
                gt_ext_resource_descriptor_t d;
                if (!resource_received && gt__runtime_resources_take(host_resources, &pin, &d) ==
                                              GT_RUNTIME_RESOURCE_OK) {
                    CHECK(d.size == 262144 && d.size_known);
                    CHECK(gt__runtime_resources_begin_read(host_resources, pin, 262144, 10000) ==
                          GT_RUNTIME_RESOURCE_OK);
                    size_t at = 0;
                    for (size_t i = 0; i < 1000; i++) {
                        gt__runtime_resources_step(host_resources);
                        uint8_t bytes[16384];
                        size_t nread = 0;
                        CHECK(gt__runtime_resources_read(
                                  host_resources, pin, at, bytes, sizeof(bytes), &nread) ==
                              GT_RUNTIME_RESOURCE_OK);
                        for (size_t j = 0; j < nread; j++) {
                            CHECK(bytes[j] == (at + j) % 251);
                        }
                        at += nread;
                        if (!nread) {
                            break;
                        }
                    }
                    CHECK(at == 262144 && gt__runtime_resources_release(host_resources, pin) ==
                                              GT_RUNTIME_RESOURCE_OK);
                    resource_received = true;
                    CHECK(gt__runtime_resources_publish(
                              host_resources,
                              "workspace",
                              !strcmp(mode, "resource-unicode") ? "展示\"\\file.bin" : "file.bin",
                              "application/octet-stream",
                              262144,
                              10000,
                              &host_publication) == GT_RUNTIME_RESOURCE_OK);
                }
            }
            launch_step(runtime);
            CHECK(launch_info(runtime, &info));
            if (info.phase == GT_PROCESS_RUNTIME_CLOSED) {
                break;
            }
            if (!strcmp(mode, "peer-loss") && info.phase == GT_PROCESS_RUNTIME_ACTIVE &&
                !stopping) {
                CHECK(!shutdown(launch_host_socket, SHUT_RDWR));
                stopping = true;
            }
            if (!strcmp(mode, "process-stop") && info.stderr_bytes && !stopping) {
                launch_stop(runtime);
                stopping = true;
            }
            if (!stopping && info.phase == GT_PROCESS_RUNTIME_ACTIVE) {
                if (!strcmp(mode, "busy") && info.stderr_bytes) {
                    launch_stop(runtime);
                    stopping = true;
                } else if (!strcmp(mode, "stubborn")) {
                    launch_stop(runtime);
                    stopping = true;
                } else if (roundtrip && !handle) {
                    CHECK(!node || info.probe_verified);
                    CHECK(
                        gt_ext_session_request(
                            f.pair.h, &f.h, "fixture.echo", "{\"peer\":true}", 13, 2000, &handle) ==
                        GT_EXT_RPC_OK);
                }
                if (handle) {
                    gt_ext_rpc_result_t result = rpc_result(f.pair.h, &f.h, handle);
                    if (result.confirmed) {
                        CHECK(result.state == GT_EXT_RPC_SUCCEEDED &&
                              strstr(result.outcome, "true"));
                        CHECK(gt_ext_session_forget_request(f.pair.h, &f.h, handle) ==
                              GT_EXT_RPC_OK);
                        completed = true;
                        if (public_mode && !strcmp(mode, "close-public")) {
                            gt_ext_close_t close = gt_ext_runtime_close(public_runtime);
                            gt_ext_runtime_info_t v;
                            CHECK(gt_ext_runtime_info(public_runtime, &v));
                            CHECK(close != GT_EXT_RELEASED ||
                                  (v.phase == GT_EXT_RUNTIME_CLOSED && v.engine_released &&
                                   v.child_exited));
                        } else {
                            launch_stop(runtime);
                        }
                        stopping = true;
                    }
                }
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        if (info.phase != GT_PROCESS_RUNTIME_CLOSED) {
            launch_stop(runtime);
            for (size_t i = 0; i < 5000 && info.phase != GT_PROCESS_RUNTIME_CLOSED; i++) {
                launch_step(runtime);
                launch_info(runtime, &info);
                struct timespec pause = {0, 1000000};
                nanosleep(&pause, NULL);
            }
        }
        if (info.phase != GT_PROCESS_RUNTIME_CLOSED || info.cleanup_error ||
            (roundtrip && !completed && strcmp(mode, "busy") && strcmp(mode, "stubborn"))) {
            fprintf(
                stderr,
                "runtime phase=%d error=%d cleanup=%d activated=%d probe=%d exit=%d signal=%d\n",
                info.phase,
                info.error,
                info.cleanup_error,
                info.activated,
                info.probe_verified,
                info.exit_code,
                info.term_signal);
        }
        CHECK(info.phase == GT_PROCESS_RUNTIME_CLOSED && info.cleanup_error == GT_OK);
        if (!strcmp(mode, "resource-roundtrip") || !strcmp(mode, "resource-unicode")) {
            CHECK(resource_received && host_publication &&
                  publication(host_resources, host_publication).source.confirmed);
        }
        if (!strcmp(mode, "resource-stop")) {
            CHECK(!info.activated && info.child_exited && info.error == GT_ERR_CANCELLED &&
                  stopping);
        }
        if (!strcmp(mode, "leader-exit")) {
            int file = open(descendant_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            pid_t descendant;
            CHECK(file >= 0 && read(file, &descendant, sizeof(descendant)) == sizeof(descendant) &&
                  !close(file) && descendant > 1);
            bool timely = ext_now() - began < 2000;
            uint64_t until = ext_now() + 8000;
            int observed = 0;
            do {
                errno = 0;
                observed = kill(descendant, 0);
                if (observed && errno == ESRCH) {
                    break;
                }
                struct timespec pause = {0, 10000000};
                nanosleep(&pause, NULL);
            } while (ext_now() < until);
            bool dead = observed < 0 && errno == ESRCH;
            CHECK(!unlink(descendant_path));
            if (!timely || !dead || info.group_signal_error) {
                fprintf(stderr,
                        "descendant timely=%d dead=%d group=%d elapsed=%llu\n",
                        timely,
                        dead,
                        info.group_signal_error,
                        (unsigned long long)(ext_now() - began));
            }
            CHECK(timely && dead && info.child_exited && !info.activated && !info.exit_code &&
                  !info.term_signal && !info.group_signal_error);
        }
        if (roundtrip && strcmp(mode, "busy") && strcmp(mode, "stubborn")) {
            CHECK(completed && info.activated && info.child_exited);
        }
        if (!strcmp(mode, "busy") || !strcmp(mode, "stubborn")) {
            CHECK(info.child_exited && info.term_signal == SIGKILL);
        }
        if (!strcmp(mode, "peer-loss")) {
            CHECK(info.activated && info.child_exited && info.session_end == GT_EXT_END_LOST &&
                  info.session_error != GT_OK &&
                  (info.term_signal == 0 || info.term_signal == SIGTERM));
        }
        if (!strcmp(mode, "unsupported")) {
            CHECK(!info.activated && info.child_exited && info.exit_code == 77 &&
                  !info.term_signal && info.error == GT_ERR_UNSUPPORTED);
        }
        if (!strcmp(mode, "wrong-version")) {
            CHECK(!info.activated && info.error == GT_ERR_AUTH);
        }
        if (!strcmp(mode, "activation-timeout")) {
            CHECK(!info.activated && info.error == GT_ERR_CANCELLED && info.probe_verified &&
                  info.stderr_bytes && info.child_exited);
        }
        if (!strcmp(mode, "stdout-eof")) {
            CHECK(!info.activated && info.child_exited && info.error == GT_ERR_IO &&
                  info.term_signal == SIGTERM);
        }
        if (!strcmp(mode, "lifetime")) {
            CHECK(info.activated && info.error == GT_ERR_CANCELLED && info.child_exited);
        }
        if (!strcmp(mode, "stop-created")) {
            CHECK(!info.activated && !info.child_exited && !info.probe_verified &&
                  info.error == GT_ERR_CANCELLED);
        }
        if (!strcmp(mode, "process-extra-field") || !strcmp(mode, "process-bad-hex")) {
            CHECK(!info.activated && info.child_exited && info.error == GT_ERR_PROTOCOL);
        }
        if (!strcmp(mode, "activation-reject") || !strcmp(mode, "ambient")) {
            CHECK(!info.activated && info.exit_code != 0);
        }
        if (!strcmp(mode, "stdout-limit") || !strcmp(mode, "stderr-limit")) {
            CHECK(info.error == GT_ERR_IO);
        }
        CHECK(launch_destroy(&runtime) == GT_OK && !runtime && !public_runtime);
    }
    if (sdk) {
        CHECK(gt__package_snapshot_destroy(&sdk) == GT_PACKAGE_OK);
    }
    if (package) {
        CHECK(gt__package_checked_destroy(&package) == GT_PACKAGE_OK);
    }
    if (f.pair.e) {
        ext_drop(&f.pair.e);
    }
    if (host_resources) {
        gt_ext_session_stop(f.pair.h);
        gt_ext_session_step(f.pair.h, 0);
        CHECK(gt__runtime_resources_destroy(&host_resources) == GT_RUNTIME_RESOURCE_OK);
    }
    ext_drop(&f.pair.h);
    if (with_resources || with_process) {
        if (file.root >= 0) {
            CHECK(!close(file.root));
        }
        CHECK(!unlink(file.file) && !rmdir(file.directory));
        if (with_resources) {
            res_store_drop(&hs);
            res_store_drop(&es);
        }
    }
    CHECK(!close(parent));
    cleanup_parent = -1;
    CHECK(!rmdir(root));
    cleanup_root[0] = 0;
    CHECK(!gt__plugin_memory_info().used);
    return 0;
}
