#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include "stdio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int diagnostic(const char *message, int code) {
    fprintf(stderr, "gterm: %s\n", message); return code;
}
static bool digest(const char *text) {
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; i++) if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}
static void idle(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static bool close_owners(gt_ext_runtime_t **runtime, gt_ext_session_t **session, gt_ext_channel_t **channel) {
    if (*runtime) {
        gt_ext_runtime_stop(*runtime);
        for (unsigned i = 0; i < 100; i++) {
            if (gt_ext_runtime_close(*runtime) == GT_EXT_RELEASED) break;
            idle();
        }
        gt_ext_runtime_info_t info; bool observed = gt_ext_runtime_info(*runtime, &info);
        if (gt_ext_runtime_destroy(runtime) != GT_OK || !observed || info.dispose_error || info.cleanup_error != GT_OK) return false;
    }
    if (*session) {
        gt_ext_session_stop(*session);
        for (unsigned i = 0; i < 100; i++) if (gt_ext_session_close(*session, 1) == GT_EXT_RELEASED) break;
        if (gt_ext_session_destroy(session) != GT_OK) return false;
    }
    if (*channel) {
        gt_ext_channel_stop(*channel);
        for (unsigned i = 0; i < 100; i++) if (gt_ext_channel_close(*channel, 1) == GT_EXT_RELEASED) break;
        if (gt_ext_channel_destroy(channel) != GT_OK) return false;
    }
    return true;
}
int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled) {
    const char *trusted = NULL, *client = NULL;
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX] = {0}; size_t method_count = 0;
    int at = 0;
    for (; at < argc && strcmp(argv[at], "--"); at += 2) {
        if (at + 1 >= argc) return diagnostic("missing stdio option value", 2);
        if (!strcmp(argv[at], "--trust-development-digest") && !trusted) trusted = argv[at + 1];
        else if (!strcmp(argv[at], "--client-digest") && !client) client = argv[at + 1];
        else if (!strcmp(argv[at], "--method") && method_count < GT_EXT_RPC_METHODS_MAX && strlen(argv[at + 1]) <= 128) {
            gt_ext_rpc_method_t *m = &methods[method_count++]; strcpy(m->method, argv[at + 1]);
            strcpy(m->capability, "session.rpc"); m->receiver = GT_EXT_EXECUTOR;
            m->input_shape = m->output_shape = GT_EXT_JSON_OBJECT;
        } else return diagnostic("invalid or duplicate stdio option", 2);
    }
    if (!digest(trusted) || !digest(client) || at + 2 >= argc || strcmp(argv[at], "--")) return diagnostic("stdio requires explicit development trust, client digest, directory and command", 2);
    const char *directory = argv[at + 1], *command = argv[at + 2];
    gt_ext_invocation_t invocation = {command, (const char *const *)(argv + at + 3), (size_t)(argc - at - 3)};
    if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) return diagnostic("stdio protocol refuses a terminal", 2);
    if (gt_ext_runtime_support() != GT_OK) return diagnostic("portable JS is unsupported", 4);
    gt_ext_package_t *package = NULL; gt_ext_channel_t *channel = NULL;
    gt_ext_session_t *session = NULL; gt_ext_runtime_t *runtime = NULL; int result = 5;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return diagnostic("package directory unavailable", 5);
    gt_ext_package_config_t capture = {fd, 1024, 4096, 16777216, 67108864};
    gt_status_t status = gt_ext_package_open(&capture, &package); int closed = close(fd);
    if (status != GT_OK || closed) goto done;
    gt_ext_package_phase_t phase;
    do { if (*cancelled) gt_ext_package_stop(package); phase = gt_ext_package_step(package); } while (phase == GT_EXT_PACKAGE_CAPTURING);
    if (*cancelled) { result = 130; goto done; }
    if (phase != GT_EXT_PACKAGE_CAPTURED) goto done;
    const char *host = "{\"side\":\"executor\",\"sdkVersion\":\"" GTERM_SDK_VERSION "\",\"protocols\":[" GTERM_PROTOCOL "],\"runtimes\":[\"portable-js\"],\"capabilities\":[\"session.rpc\"]}";
    gt_ext_package_check_config_t check = {.host_json = host};
    if (gt_ext_package_check(package, &check) != GT_EXT_PACKAGE_CHECKED) {
        gt_ext_package_info_t info;
        if (gt_ext_package_info(package, &info)) result = info.error == GT_EXT_PACKAGE_UNSUPPORTED ? 4 : info.error == GT_EXT_PACKAGE_UNKNOWN ? 3 : 5;
        goto done;
    }
    gt_ext_package_metadata_t metadata;
    if (!gt_ext_package_metadata(package, &metadata)) goto done;
    if (strcmp(metadata.package_digest, trusted)) { result = diagnostic("development package digest changed", 6); goto done; }
    uint32_t count; bool selected = false;
    if (!gt_ext_package_command_count(package, &count)) goto done;
    for (uint32_t i = 0; i < count; i++) {
        gt_ext_package_command_t c;
        if (!gt_ext_package_command(package, i, &c)) goto done;
        if (!strcmp(c.path, command)) selected = true;
    }
    if (!selected) { result = diagnostic("command is not declared by this package", 2); goto done; }
    gt_ext_session_config_t config = {.available_count = 1, .budgets = {768, 65536, 262144}, .handshake_ms = 5000, .consent_ms = 60000,
        .peer = {GT_EXT_PEER_LOCAL, "explicit-stdio-owner"}, .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(config.plugin_id, metadata.id); strcpy(config.plugin_version, metadata.version);
    strcpy(config.client_digest, client); strcpy(config.executor_digest, metadata.package_digest);
    strcpy(config.host_version, "0.1.0"); strcpy(config.sdk_version, metadata.sdk_version); strcpy(config.available[0], "session.rpc");
    config.required_count = metadata.requires_count;
    bool rpc = false;
    for (size_t i = 0; i < config.required_count; i++) { strcpy(config.required[i], metadata.requires[i]); if (!strcmp(metadata.requires[i], "session.rpc")) rpc = true; }
    if (method_count && !rpc) { result = diagnostic("methods require a declared session.rpc capability", 4); goto done; }
    int input = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3), output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (input < 0 || output < 0) { if (input >= 0) close(input); if (output >= 0) close(output); goto done; }
    status = gt_ext_channel_open_fd(input, output, &channel);
    if (status != GT_OK) { close(input); close(output); goto done; }
    if (gt_ext_session_open_rpc(&channel, GT_EXT_EXECUTOR, &config, methods, method_count, &session) != GT_OK) goto done;
    for (;;) {
        if (*cancelled) { result = 130; goto done; }
        gt_ext_session_phase_t state = gt_ext_session_step(session, 10);
        if (state == GT_EXT_SESSION_ACTIVE) break;
        if (state >= GT_EXT_SESSION_ENDING) {
            gt_ext_session_info_t info;
            result = gt_ext_session_info(session, &info) && info.end == GT_EXT_END_DENIED ? 6 : 7;
            goto done;
        }
    }
    gt_ext_runtime_config_t runtime_config = {.role = GT_EXT_EXECUTOR, .invocation = &invocation};
    if (gt_ext_runtime_open_package(&session, package, &runtime_config, true, &runtime) != GT_OK) goto done;
    if (gt_ext_package_destroy(&package) != GT_OK) goto done;
    bool activated = false;
    for (;;) {
        if (*cancelled) { result = 130; break; }
        gt_ext_runtime_phase_t state = gt_ext_runtime_step(runtime);
        if (state == GT_EXT_RUNTIME_ACTIVE) activated = true;
        if (state >= GT_EXT_RUNTIME_FAILED) {
            gt_ext_runtime_info_t info;
            if (!gt_ext_runtime_info(runtime, &info)) break;
            result = activated && info.session_end == GT_EXT_END_PEER ? 0 : info.session_end == GT_EXT_END_LOST ? 7 : 5;
            break;
        }
        idle();
    }
done:
    if (!close_owners(&runtime, &session, &channel)) result = 5;
    if (package && gt_ext_package_destroy(&package) != GT_OK) result = 5;
    if (result) diagnostic(result == 6 ? "executor denied" : result == 7 ? "executor channel outcome unknown" : result == 130 ? "executor cancelled" : "executor failed", result);
    return result;
}
