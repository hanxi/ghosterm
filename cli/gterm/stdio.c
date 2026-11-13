#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include "stdio.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *id;
    const char *path;
    int fd;
    gt_ext_runtime_root_t runtime;
} source_root_t;

typedef struct {
    gt_ext_resource_store_t *store;
    char directory[PATH_MAX];
} resource_owner_t;

typedef struct {
    const char *name;
    const char *executable;
    const char *version;
    const char *sha256;
} process_program_t;

static int diagnostic(const char *message, int code) {
    fprintf(stderr, "gterm: %s\n", message);
    return code;
}

static bool digest(const char *text) {
    if (!text || strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void idle(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static bool root_id(const char *id) {
    if (!id || !((id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z')) ||
        strlen(id) > 64) {
        return false;
    }
    for (size_t i = 1; id[i]; i++) {
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= 'a' && id[i] <= 'z') ||
              (id[i] >= '0' && id[i] <= '9') || id[i] == '-' || id[i] == '_' || id[i] == '.')) {
            return false;
        }
    }
    return true;
}

static bool open_roots(source_root_t *roots, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!root_id(roots[i].id) || !roots[i].path || !roots[i].path[0] ||
            strlen(roots[i].path) >= PATH_MAX) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(roots[i].id, roots[j].id)) {
                return false;
            }
        }
        roots[i].fd = open(roots[i].path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info;
        if (roots[i].fd < 0 || fstat(roots[i].fd, &info) || !S_ISDIR(info.st_mode) ||
            info.st_uid != geteuid()) {
            return false;
        }
        roots[i].runtime = (gt_ext_runtime_root_t){roots[i].id, roots[i].fd, 16777216};
    }
    return true;
}

static bool open_resource_owner(resource_owner_t *owner) {
    const char *temporary = getenv("TMPDIR");
    if (!temporary || !temporary[0]) {
        temporary = "/tmp";
    }
    const char *separator = temporary[strlen(temporary) - 1] == '/' ? "" : "/";
    int size = snprintf(owner->directory,
                        sizeof(owner->directory),
                        "%s%sghosterm-resource-XXXXXX",
                        temporary,
                        separator);
    if (size <= 0 || (size_t)size >= sizeof(owner->directory) || !mkdtemp(owner->directory)) {
        owner->directory[0] = 0;
        return false;
    }
    int fd = open(owner->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    gt_ext_resource_store_config_t config = {fd, 16777216, 1048576, 8};
    gt_ext_resource_status_t status = gt_ext_resource_store_open(&config, &owner->store);
    int closed = close(fd);
    return status == GT_EXT_RESOURCE_OK && !closed;
}

static bool close_resource_owner(resource_owner_t *owner) {
    bool ok = true;
    if (owner->store) {
        gt_ext_resource_status_t status = gt_ext_resource_store_destroy(&owner->store);
        if (status != GT_EXT_RESOURCE_OK || owner->store) {
            ok = false;
        }
    }
    if (owner->directory[0]) {
        if (rmdir(owner->directory) && errno != ENOENT) {
            ok = false;
        } else {
            owner->directory[0] = 0;
        }
    }
    return ok;
}

static bool close_owners(gt_ext_runtime_t **runtime,
                         gt_ext_session_t **session,
                         gt_ext_channel_t **channel,
                         resource_owner_t *resource_owner,
                         source_root_t *roots,
                         size_t root_count) {
    bool ok = true;
    if (*runtime) {
        gt_ext_runtime_stop(*runtime);
        for (unsigned i = 0; i < 100; i++) {
            if (gt_ext_runtime_close(*runtime) == GT_EXT_RELEASED) {
                break;
            }
            idle();
        }
        gt_ext_runtime_info_t info;
        bool observed = gt_ext_runtime_info(*runtime, &info);
        if (gt_ext_runtime_destroy(runtime) != GT_OK || !observed || info.dispose_error ||
            info.cleanup_error != GT_OK) {
            ok = false;
        }
    }
    if (*session) {
        gt_ext_session_stop(*session);
        bool released = false;
        for (unsigned i = 0; i < 100; i++) {
            if (gt_ext_session_close(*session, 1) == GT_EXT_RELEASED) {
                released = true;
                break;
            }
        }
        if (!released || gt_ext_session_destroy(session) != GT_OK) {
            ok = false;
        }
    }
    if (!close_resource_owner(resource_owner)) {
        ok = false;
    }
    if (*channel) {
        gt_ext_channel_stop(*channel);
        bool released = false;
        for (unsigned i = 0; i < 100; i++) {
            if (gt_ext_channel_close(*channel, 1) == GT_EXT_RELEASED) {
                released = true;
                break;
            }
        }
        if (!released || gt_ext_channel_destroy(channel) != GT_OK) {
            ok = false;
        }
    }
    for (size_t i = 0; i < root_count; i++) {
        if (roots[i].fd >= 0) {
            if (close(roots[i].fd)) {
                ok = false;
            }
            roots[i].fd = -1;
        }
    }
    return ok;
}

int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled) {
    const char *trusted = NULL, *client = NULL;
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX] = {0};
    process_program_t process_programs[GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX] = {0};
    source_root_t roots[GT_EXT_RUNTIME_ROOTS_MAX] = {0};
    gt_ext_runtime_root_t runtime_roots[GT_EXT_RUNTIME_ROOTS_MAX] = {0};
    gt_ext_process_program_mapping_t runtime_programs[GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX] = {0};
    gt_ext_process_environment_t runtime_environment[GT_EXT_RUNTIME_PROCESS_ENV_MAX] = {0};
    char environment_values[4][4097] = {{0}};
    size_t method_count = 0;
    size_t process_program_count = 0;
    size_t process_environment_count = 0;
    size_t root_count = 0;
    int at = 0;
    for (; at < argc && strcmp(argv[at], "--");) {
        if (!strcmp(argv[at], "--root")) {
            if (at + 2 >= argc || root_count == GT_EXT_RUNTIME_ROOTS_MAX) {
                return diagnostic("missing or excess resource root", 2);
            }
            roots[root_count++] =
                (source_root_t){.id = argv[at + 1], .path = argv[at + 2], .fd = -1};
            at += 3;
            continue;
        }
        if (!strcmp(argv[at], "--process-program")) {
            if (at + 4 >= argc || process_program_count == GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX) {
                return diagnostic("missing or excess process program mapping", 2);
            }
            process_programs[process_program_count++] =
                (process_program_t){argv[at + 1], argv[at + 2], argv[at + 3], argv[at + 4]};
            at += 5;
            continue;
        }
        if (at + 1 >= argc) {
            return diagnostic("missing stdio option value", 2);
        }
        if (!strcmp(argv[at], "--trust-development-digest") && !trusted) {
            trusted = argv[at + 1];
        } else if (!strcmp(argv[at], "--client-digest") && !client) {
            client = argv[at + 1];
        } else if ((!strcmp(argv[at], "--method") || !strcmp(argv[at], "--host-method")) &&
                   method_count < GT_EXT_RPC_METHODS_MAX && strlen(argv[at + 1]) <= 128) {
            gt_ext_rpc_method_t *m = &methods[method_count++];
            strcpy(m->method, argv[at + 1]);
            strcpy(m->capability, "session.rpc");
            m->receiver = !strcmp(argv[at], "--host-method") ? GT_EXT_HOST : GT_EXT_EXECUTOR;
            m->input_shape = m->output_shape = GT_EXT_JSON_OBJECT;
        } else {
            return diagnostic("invalid or duplicate stdio option", 2);
        }
        at += 2;
    }
    if (!digest(trusted) || !digest(client) || at + 2 >= argc || strcmp(argv[at], "--")) {
        return diagnostic(
            "stdio requires explicit development trust, client digest, directory and command", 2);
    }
    const char *directory = argv[at + 1], *command = argv[at + 2];
    gt_ext_invocation_t invocation = {
        command, (const char *const *)(argv + at + 3), (size_t)(argc - at - 3)};
    if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) {
        return diagnostic("stdio protocol refuses a terminal", 2);
    }
    if (gt_ext_runtime_support() != GT_OK) {
        return diagnostic("portable JS is unsupported", 4);
    }
    gt_ext_package_t *package = NULL;
    gt_ext_channel_t *channel = NULL;
    gt_ext_session_t *session = NULL;
    gt_ext_runtime_t *runtime = NULL;
    resource_owner_t resource_owner = {0};
    int result = 5;
    const char *stage = "Executor startup";
    if (!open_roots(roots, root_count)) {
        result = diagnostic("resource root unavailable or invalid", 2);
        goto done;
    }
    if (process_program_count && !root_count) {
        result = diagnostic("process mappings require an explicit workspace root", 4);
        goto done;
    }
    for (size_t i = 0; i < root_count; i++) {
        runtime_roots[i] = roots[i].runtime;
    }
    bool resource_store_available = open_resource_owner(&resource_owner);
    stage = "package capture";
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        result = diagnostic("package directory unavailable", 5);
        stage = "package directory";
        goto done;
    }
    gt_ext_package_config_t capture = {fd, 1024, 4096, 16777216, 67108864};
    gt_status_t status = gt_ext_package_open(&capture, &package);
    int closed = close(fd);
    if (status != GT_OK || closed) {
        goto done;
    }
    gt_ext_package_phase_t phase;
    do {
        if (*cancelled) {
            gt_ext_package_stop(package);
        }
        phase = gt_ext_package_step(package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    if (*cancelled) {
        result = 130;
        goto done;
    }
    if (phase != GT_EXT_PACKAGE_CAPTURED) {
        goto done;
    }
    const bool process_available = process_program_count && root_count;
    char host_json[512];
    int host_size = snprintf(host_json,
                             sizeof(host_json),
                             "{\"side\":\"executor\",\"sdkVersion\":\"" GTERM_SDK_VERSION
                             "\",\"protocols\":[" GTERM_PROTOCOL "],\"runtimes\":[\"portable-js\"],"
                             "\"capabilities\":[\"session.rpc\"%s%s%s]}",
                             process_available ? ",\"process.spawn\",\"process.pipes\"" : "",
                             resource_store_available ? ",\"resource.receive\"" : "",
                             root_count && resource_store_available ? ",\"file.readScoped\"" : "");
    if (host_size <= 0 || (size_t)host_size >= sizeof(host_json)) {
        goto done;
    }
    stage = "package compatibility check";
    gt_ext_package_check_config_t check = {.host_json = host_json};
    if (gt_ext_package_check(package, &check) != GT_EXT_PACKAGE_CHECKED) {
        gt_ext_package_info_t info;
        if (gt_ext_package_info(package, &info)) {
            result = info.error == GT_EXT_PACKAGE_UNSUPPORTED ? 4
                     : info.error == GT_EXT_PACKAGE_UNKNOWN   ? 3
                                                              : 5;
            if (info.code[0]) {
                fprintf(stderr, "gterm: package compatibility failed (%s)\n", info.code);
            }
        }
        goto done;
    }
    gt_ext_package_metadata_t metadata;
    if (!gt_ext_package_metadata(package, &metadata)) {
        goto done;
    }
    if (strcmp(metadata.package_digest, trusted)) {
        result = diagnostic("development package digest changed", 6);
        goto done;
    }
    uint32_t count;
    bool selected = false;
    if (!gt_ext_package_command_count(package, &count)) {
        goto done;
    }
    for (uint32_t i = 0; i < count; i++) {
        gt_ext_package_command_t c;
        if (!gt_ext_package_command(package, i, &c)) {
            goto done;
        }
        if (!strcmp(c.path, command)) {
            selected = true;
        }
    }
    if (!selected) {
        result = diagnostic("command is not declared by this package", 2);
        goto done;
    }
    gt_ext_session_config_t config = {.available_count = 1,
                                      .budgets = {768, 65536, 262144},
                                      .handshake_ms = 5000,
                                      .consent_ms = 60000,
                                      .peer = {GT_EXT_PEER_LOCAL, "explicit-stdio-owner"},
                                      .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(config.plugin_id, metadata.id);
    strcpy(config.plugin_version, metadata.version);
    strcpy(config.client_digest, client);
    strcpy(config.executor_digest, metadata.package_digest);
    strcpy(config.host_version, "0.1.0");
    strcpy(config.sdk_version, metadata.sdk_version);
    strcpy(config.available[0], "session.rpc");
    if (process_available) {
        strcpy(config.available[config.available_count++], "process.spawn");
        strcpy(config.available[config.available_count++], "process.pipes");
    }
    if (resource_store_available) {
        strcpy(config.available[config.available_count++], "resource.receive");
        if (root_count) {
            strcpy(config.available[config.available_count++], "file.readScoped");
        }
    }
    config.required_count = metadata.requires_count;
    bool rpc = false, receive_resources = false, scoped_reads = false;
    bool process_spawn = false, process_pipes = false;
    for (size_t i = 0; i < config.required_count; i++) {
        strcpy(config.required[i], metadata.requires[i]);
        if (!strcmp(metadata.requires[i], "session.rpc")) {
            rpc = true;
        } else if (!strcmp(metadata.requires[i], "resource.receive")) {
            receive_resources = true;
        } else if (!strcmp(metadata.requires[i], "file.readScoped")) {
            scoped_reads = true;
        } else if (!strcmp(metadata.requires[i], "process.spawn")) {
            process_spawn = true;
        } else if (!strcmp(metadata.requires[i], "process.pipes")) {
            process_pipes = true;
        }
    }
    if ((receive_resources && !resource_store_available) || (scoped_reads && !root_count) ||
        (scoped_reads && !receive_resources)) {
        result = diagnostic("resource capability requires an available store and explicit root", 4);
        goto done;
    }
    if ((process_spawn || process_pipes) &&
        (!process_available || !process_spawn || !process_pipes)) {
        result = diagnostic(
            "process capabilities require explicit program mappings and a workspace root", 4);
        goto done;
    }
    if (process_program_count && !process_spawn) {
        result = diagnostic(
            "process mappings require both process.spawn and process.pipes in the package", 4);
        goto done;
    }
    if (receive_resources) {
        config.features |= GT_EXT_FEATURE_RESOURCES;
    }
    if (process_spawn) {
        /* Executor children receive only selected user environment values.
         * Do not inherit arbitrary variables such as SSH_AUTH_SOCK or secrets. */
        static const char *const selected_names[] = {
            "HOME", "PATH", "XDG_CONFIG_HOME", "CODEX_HOME"};
        for (size_t i = 0; i < sizeof(selected_names) / sizeof(selected_names[0]); i++) {
            const char *value = getenv(selected_names[i]);
            if (!value || !value[0]) {
                continue;
            }
            size_t length = strnlen(value, sizeof(environment_values[0]));
            if (length >= sizeof(environment_values[0])) {
                result = diagnostic("selected process environment value exceeds its limit", 4);
                goto done;
            }
            strcpy(environment_values[process_environment_count], value);
            runtime_environment[process_environment_count] = (gt_ext_process_environment_t){
                selected_names[i], environment_values[process_environment_count]};
            process_environment_count++;
        }
        for (size_t i = 0; i < process_program_count; i++) {
            runtime_programs[i] = (gt_ext_process_program_mapping_t){process_programs[i].name,
                                                                     process_programs[i].executable,
                                                                     process_programs[i].version,
                                                                     process_programs[i].sha256};
        }
    }
    if (method_count && !rpc) {
        result = diagnostic("methods require a declared session.rpc capability", 4);
        goto done;
    }
    int input = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3),
        output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (input < 0 || output < 0) {
        if (input >= 0) {
            close(input);
        }
        if (output >= 0) {
            close(output);
        }
        goto done;
    }
    stage = "stdio channel open";
    status = gt_ext_channel_open_fd(input, output, &channel);
    if (status != GT_OK) {
        close(input);
        close(output);
        goto done;
    }
    stage = "Session owner open";
    gt_status_t session_status =
        receive_resources
            ? gt_ext_session_open_resources(&channel,
                                            GT_EXT_EXECUTOR,
                                            &config,
                                            methods,
                                            method_count,
                                            resource_owner.store,
                                            16777216,
                                            &session)
            : gt_ext_session_open_rpc(
                  &channel, GT_EXT_EXECUTOR, &config, methods, method_count, &session);
    if (session_status != GT_OK) {
        goto done;
    }
    stage = "Session handshake";
    for (;;) {
        if (*cancelled) {
            result = 130;
            goto done;
        }
        gt_ext_session_phase_t state = gt_ext_session_step(session, 10);
        if (state == GT_EXT_SESSION_ACTIVE) {
            break;
        }
        if (state >= GT_EXT_SESSION_ENDING) {
            gt_ext_session_info_t info;
            result = gt_ext_session_info(session, &info) && info.end == GT_EXT_END_DENIED ? 6 : 7;
            goto done;
        }
    }
    stage = "runtime activation";
    gt_ext_runtime_config_t runtime_config = {
        .role = GT_EXT_EXECUTOR,
        .invocation = &invocation,
        .roots = root_count ? runtime_roots : NULL,
        .root_count = root_count,
        .process_programs = process_spawn ? runtime_programs : NULL,
        .process_program_count = process_spawn ? process_program_count : 0,
        .process_environment = process_spawn ? runtime_environment : NULL,
        .process_environment_count = process_spawn ? process_environment_count : 0};
    if (gt_ext_runtime_open_package(&session, package, &runtime_config, true, &runtime) != GT_OK) {
        goto done;
    }
    if (gt_ext_package_destroy(&package) != GT_OK) {
        goto done;
    }
    stage = "runtime execution";
    bool activated = false;
    for (;;) {
        if (*cancelled) {
            result = 130;
            break;
        }
        gt_ext_runtime_phase_t state = gt_ext_runtime_step(runtime);
        if (state == GT_EXT_RUNTIME_ACTIVE) {
            activated = true;
        }
        if (state >= GT_EXT_RUNTIME_FAILED) {
            gt_ext_runtime_info_t info;
            if (!gt_ext_runtime_info(runtime, &info)) {
                break;
            }
            result = activated && info.session_end == GT_EXT_END_PEER ? 0
                     : info.session_end == GT_EXT_END_LOST            ? 7
                                                                      : 5;
            break;
        }
        idle();
    }
done:
    if (!close_owners(&runtime, &session, &channel, &resource_owner, roots, root_count)) {
        result = 5;
        stage = "owner cleanup";
    }
    if (package && gt_ext_package_destroy(&package) != GT_OK) {
        result = 5;
        stage = "package cleanup";
    }
    if (result) {
        diagnostic(result == 6     ? "executor denied"
                   : result == 7   ? "executor channel outcome unknown"
                   : result == 130 ? "executor cancelled"
                                   : stage,
                   result);
    }
    return result;
}
