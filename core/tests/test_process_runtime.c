#define EXT_RPC_MAIN process_runtime_rpc_fixture_main
#include "test_ext_rpc.c"
#include "extension/process-runtime.h"
#include "extension/package-check.h"
#include "extension/package-stage.h"
#include "extension/memory.h"
#include "base/sha256.h"
#include <sys/stat.h>
#include <signal.h>

/* A normal assertion red must still stop/reap our actual child. */
static gt_process_runtime_t *runtime;
static int cleanup_parent = -1;
static char cleanup_root[4097];

static void launch_cleanup(void) {
    if (runtime) {
        gt__process_runtime_stop(runtime);
        uint64_t until = ext_now() + 6000;
        while (ext_now() < until &&
               gt__process_runtime_step(runtime) != GT_PROCESS_RUNTIME_CLOSED) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        (void)gt__process_runtime_destroy(&runtime);
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

static void launch_pair(rpc_pair_t *f, const gt_package_check_info_t *package) {
    memset(f, 0, sizeof(*f));
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_channel_t *h = ext_channel(fd[0]), *e = ext_channel(fd[1]);
    gt_ext_session_config_t c = ext_config();
    c.features = GT_EXT_FEATURE_REQUESTS;
    strcpy(c.plugin_id, package->id);
    strcpy(c.plugin_version, package->version);
    strcpy(c.executor_digest, package->package_digest);
    strcpy(c.required[0], "session.rpc");
    strcpy(c.available[0], "session.rpc");
    c.required_count = c.available_count = 1;
    gt_ext_rpc_method_t methods[4];
    size_t count = rpc_methods(methods);
    for (size_t i = 0; i < count; i++) {
        strcpy(methods[i].capability, "session.rpc");
    }
    f->pair.h = rpc_adopt(&h, GT_EXT_HOST, &c, methods, count);
    f->pair.e = rpc_adopt(&e, GT_EXT_EXECUTOR, &c, methods, count);
    ext_allow(&f->pair);
    CHECK(gt_ext_session_lease(f->pair.h, &f->h) && gt_ext_session_lease(f->pair.e, &f->e));
}

int main(int argc, char **argv) {
    /* mode package sdk temp canonical-node node-sha sdk-catalog facts.json */
    CHECK(argc == 9);
    CHECK(!atexit(launch_cleanup));
    const char *mode = argv[1];
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
    launch_pair(&f, &package_info);
    char root[4097];
    CHECK(snprintf(root, sizeof(root), "%s/process-runtime-XXXXXX", argv[4]) > 0 && mkdtemp(root));
    int parent = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(parent >= 0);
    cleanup_parent = parent;
    strcpy(cleanup_root, root);
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
    gt_status_t status = gt__process_runtime_open(&f.pair.e, package, &sdk, &config, &runtime);
    bool rejected = !strcmp(mode, "denied") || !strcmp(mode, "wrong-sha") ||
                    !strcmp(mode, "bad-env") || !strcmp(mode, "bad-arguments") ||
                    !strcmp(mode, "bad-command") || !strcmp(mode, "bad-sdk") ||
                    !strcmp(mode, "wrong-identity") || !strcmp(mode, "bad-env-name") ||
                    !strcmp(mode, "duplicate-env") || !strcmp(mode, "node-alias");
    if (rejected) {
        gt_status_t expected = !strcmp(mode, "denied") || !strcmp(mode, "wrong-sha") ||
                                       !strcmp(mode, "bad-sdk") || !strcmp(mode, "node-alias")
                                   ? GT_ERR_AUTH
                               : !strcmp(mode, "wrong-identity") ? GT_ERR_PROTOCOL
                               : !strcmp(mode, "bad-command")    ? GT_ERR_NOT_FOUND
                                                                 : GT_ERR_INVALID_ARG;
        CHECK(status == expected && !runtime && f.pair.e == original && sdk == original_sdk);
        CHECK(gt_ext_session_access(original, &f.e, "session.rpc") == GT_EXT_ACCESS_ALLOWED);
    } else {
        CHECK(status == GT_OK && runtime && !f.pair.e && (!node || !sdk));
        CHECK(gt__process_runtime_destroy(&runtime) == GT_ERR_INVALID_ARG && runtime);
        if (!strcmp(mode, "retained")) {
            CHECK(gt__package_checked_destroy(&package) == GT_PACKAGE_OK);
        }
        if (!strcmp(mode, "stop-created")) {
            gt__process_runtime_stop(runtime);
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
        bool roundtrip = !strcmp(mode, "roundtrip") || !strcmp(mode, "retained") ||
                         !strcmp(mode, "frozen") || !strcmp(mode, "busy") ||
                         !strcmp(mode, "stubborn") || !strcmp(mode, "sdk-frozen") ||
                         !strcmp(mode, "argument-boundary") || !strcmp(mode, "stderr-eof");
        bool stopping = !strcmp(mode, "stop-created"), completed = false;
        uint64_t began = ext_now(), deadline = began + 15000;
        while (ext_now() < deadline) {
            gt_ext_session_step(f.pair.h, 0);
            gt__process_runtime_step(runtime);
            CHECK(gt__process_runtime_info(runtime, &info));
            if (info.phase == GT_PROCESS_RUNTIME_CLOSED) {
                break;
            }
            if (!stopping && info.phase == GT_PROCESS_RUNTIME_ACTIVE) {
                if (!strcmp(mode, "busy") && info.stderr_bytes) {
                    gt__process_runtime_stop(runtime);
                    stopping = true;
                } else if (!strcmp(mode, "stubborn")) {
                    gt__process_runtime_stop(runtime);
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
                        gt__process_runtime_stop(runtime);
                        stopping = true;
                    }
                }
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        if (info.phase != GT_PROCESS_RUNTIME_CLOSED) {
            gt__process_runtime_stop(runtime);
            for (size_t i = 0; i < 5000 && info.phase != GT_PROCESS_RUNTIME_CLOSED; i++) {
                gt__process_runtime_step(runtime);
                gt__process_runtime_info(runtime, &info);
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
        if (!strcmp(mode, "activation-reject") || !strcmp(mode, "ambient")) {
            CHECK(!info.activated && info.exit_code != 0);
        }
        if (!strcmp(mode, "stdout-limit") || !strcmp(mode, "stderr-limit")) {
            CHECK(info.error == GT_ERR_IO);
        }
        CHECK(gt__process_runtime_destroy(&runtime) == GT_OK && !runtime);
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
    ext_drop(&f.pair.h);
    CHECK(!close(parent));
    cleanup_parent = -1;
    CHECK(!rmdir(root));
    cleanup_root[0] = 0;
    CHECK(!gt__plugin_memory_info().used);
    return 0;
}
