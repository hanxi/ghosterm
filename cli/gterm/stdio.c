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
    const char *prefix[GT_EXT_RUNTIME_PROCESS_PREFIX_MAX];
    size_t prefix_count;
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
        for (unsigned i = 0; i < 10000; i++) {
            if (gt_ext_runtime_close(*runtime) == GT_EXT_RELEASED) {
                break;
            }
            idle();
        }
        gt_ext_runtime_info_t info;
        bool observed = gt_ext_runtime_info(*runtime, &info);
        if (observed && info.external_process && info.group_signal_error) {
            fprintf(
                stderr, "gterm: exited process group signal errno=%d\n", info.group_signal_error);
        }
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

int gterm_stdio_invocation_index(int argc, char **argv) {
    for (int at = 0; at < argc;) {
        const char *option = argv[at];
        if (!strcmp(option, "--")) {
            return at + 2 < argc ? at : -1;
        }
        int count =
            !strcmp(option, "--allow-process-execution") ? 0
            : !strcmp(option, "--process-program")       ? 4
            : !strcmp(option, "--node-runtime")          ? 3
            : !strcmp(option, "--node-sdk") || !strcmp(option, "--root") ||
                    !strcmp(option, "--process-argument")
                ? 2
            : !strcmp(option, "--runtime") || !strcmp(option, "--trust-development-digest") ||
                    !strcmp(option, "--client-digest") || !strcmp(option, "--method") ||
                    !strcmp(option, "--host-method") || !strcmp(option, "--process-environment") ||
                    !strcmp(option, "--client-capability")
                ? 1
                : -1;
        if (count < 0 || count >= argc - at) {
            return -1;
        }
        at += count + 1;
    }
    return -1;
}

int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled) {
    if (gterm_stdio_invocation_index(argc, argv) < 0) {
        return diagnostic("incomplete or unknown stdio options", 2);
    }
    const char *trusted = NULL, *client = NULL;
    const char *runtime_name = NULL, *node_executable = NULL, *node_version = NULL,
               *node_sha = NULL;
    const char *sdk_directory = NULL, *sdk_sha = NULL;
    bool execution_authorized = false;
    bool client_table = false, client_diff = false, client_task = false;
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX] = {0};
    process_program_t process_programs[GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX] = {0};
    source_root_t roots[GT_EXT_RUNTIME_ROOTS_MAX] = {0};
    gt_ext_runtime_root_t runtime_roots[GT_EXT_RUNTIME_ROOTS_MAX] = {0};
    gt_ext_process_program_mapping_t runtime_programs[GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX] = {0};
    gt_ext_process_environment_t runtime_environment[GT_EXT_RUNTIME_PROCESS_ENV_MAX] = {0};
    char environment_values[GT_EXT_RUNTIME_PROCESS_ENV_MAX][4097] = {{0}};
    const char *extra_environment[GT_EXT_RUNTIME_PROCESS_ENV_MAX - 5] = {0};
    size_t extra_environment_count = 0;
    size_t method_count = 0;
    size_t process_program_count = 0;
    size_t process_environment_count = 0;
    size_t root_count = 0;
    int at = 0;
    for (; at < argc && strcmp(argv[at], "--");) {
        if (!strcmp(argv[at], "--client-capability")) {
            if (at + 1 >= argc) {
                return diagnostic("missing Client service requirement", 2);
            }
            if (!strcmp(argv[at + 1], "ui.table")) {
                if (client_table) {
                    return diagnostic("unknown or duplicate Client service requirement", 2);
                }
                client_table = true;
            } else if (!strcmp(argv[at + 1], "ui.diff")) {
                if (client_diff) {
                    return diagnostic("unknown or duplicate Client service requirement", 2);
                }
                client_diff = true;
            } else if (!strcmp(argv[at + 1], "ui.task")) {
                if (client_task) {
                    return diagnostic("unknown or duplicate Client service requirement", 2);
                }
                client_task = true;
            } else {
                return diagnostic("unknown or duplicate Client service requirement", 2);
            }
            at += 2;
            continue;
        }
        if (!strcmp(argv[at], "--allow-process-execution")) {
            if (execution_authorized) {
                return diagnostic("duplicate execution approval", 2);
            }
            execution_authorized = true;
            at++;
            continue;
        }
        if (!strcmp(argv[at], "--runtime")) {
            if (runtime_name || at + 1 >= argc) {
                return diagnostic("missing or duplicate runtime", 2);
            }
            runtime_name = argv[at + 1];
            at += 2;
            continue;
        }
        if (!strcmp(argv[at], "--node-runtime")) {
            if (node_executable || at + 3 >= argc) {
                return diagnostic("missing or duplicate Node identity", 2);
            }
            node_executable = argv[at + 1];
            node_version = argv[at + 2];
            node_sha = argv[at + 3];
            at += 4;
            continue;
        }
        if (!strcmp(argv[at], "--node-sdk")) {
            if (sdk_directory || at + 2 >= argc) {
                return diagnostic("missing or duplicate Host SDK", 2);
            }
            sdk_directory = argv[at + 1];
            sdk_sha = argv[at + 2];
            at += 3;
            continue;
        }
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
                (process_program_t){.name = argv[at + 1],
                                    .executable = argv[at + 2],
                                    .version = argv[at + 3],
                                    .sha256 = argv[at + 4]};
            at += 5;
            continue;
        }
        if (!strcmp(argv[at], "--process-argument")) {
            if (at + 2 >= argc) {
                return diagnostic("missing fixed process argument", 2);
            }
            size_t selected = process_program_count;
            for (size_t i = 0; i < process_program_count; i++) {
                if (!strcmp(process_programs[i].name, argv[at + 1])) {
                    selected = i;
                    break;
                }
            }
            if (selected == process_program_count ||
                process_programs[selected].prefix_count == GT_EXT_RUNTIME_PROCESS_PREFIX_MAX) {
                return diagnostic("fixed argument requires an earlier mapping and available slot",
                                  2);
            }
            process_program_t *program = &process_programs[selected];
            program->prefix[program->prefix_count++] = argv[at + 2];
            at += 3;
            continue;
        }
        if (!strcmp(argv[at], "--process-environment")) {
            if (at + 1 >= argc || extra_environment_count == GT_EXT_RUNTIME_PROCESS_ENV_MAX - 5) {
                return diagnostic("missing or excess selected environment name", 2);
            }
            const char *name = argv[at + 1];
            size_t length = strlen(name);
            if (!length || length > 128) {
                return diagnostic("invalid selected environment name", 2);
            }
            for (size_t i = 0; i < length; i++) {
                char c = name[i];
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                      (i && c >= '0' && c <= '9'))) {
                    return diagnostic("invalid selected environment name", 2);
                }
            }
            extra_environment[extra_environment_count++] = name;
            at += 2;
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
    if (!runtime_name) {
        runtime_name = "portable-js";
    }
    bool external = strcmp(runtime_name, "portable-js") != 0, node = !strcmp(runtime_name, "node");
    if (external && !node && strcmp(runtime_name, "exec")) {
        return diagnostic("unsupported runtime selection", 4);
    }
    if (external && !execution_authorized) {
        return diagnostic("explicit process execution approval required", 6);
    }
    if (!external && (execution_authorized || node_executable || sdk_directory)) {
        return diagnostic("process configuration requires Node or Exec selection", 2);
    }
    if (node && (!node_executable || !node_version || !digest(node_sha) || !sdk_directory ||
                 !digest(sdk_sha))) {
        return diagnostic("Node requires pinned executable/version and Host SDK", 4);
    }
    if (external && !node && (node_executable || sdk_directory)) {
        return diagnostic("Node configuration does not apply to Exec", 2);
    }
    if (node) {
        size_t n = strlen(node_version);
        if (!n || n > 128) {
            return diagnostic("invalid Node version", 2);
        }
        for (size_t i = 0; i < n; i++) {
            char c = node_version[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '.' || c == '-' || c == '+')) {
                return diagnostic("invalid Node version", 2);
            }
        }
    }
    gt_ext_runtime_execution_t execution = {.stage_parent_fd = -1,
                                            .node_sdk_directory_fd = -1,
                                            .execution_authorized = execution_authorized,
                                            .node_executable = node_executable,
                                            .node_sha256 = node_sha,
                                            .node_sdk_catalog_sha256 = sdk_sha};
    char stage_directory[PATH_MAX] = {0};
    gt_ext_package_t *package = NULL;
    gt_ext_channel_t *channel = NULL;
    gt_ext_session_t *session = NULL;
    gt_ext_runtime_t *runtime = NULL;
    resource_owner_t resource_owner = {0};
    int result = 5;
    const char *stage = "Executor startup";
    if (external) {
        const char *temporary = getenv("TMPDIR");
        if (!temporary || temporary[0] != '/') {
            result = diagnostic("process execution requires an explicit absolute TMPDIR", 4);
            goto done;
        }
        int n = snprintf(
            stage_directory, sizeof(stage_directory), "%s/gterm-execution-XXXXXX", temporary);
        if (n <= 0 || (size_t)n >= sizeof(stage_directory) || !mkdtemp(stage_directory)) {
            stage_directory[0] = 0;
            goto done;
        }
        execution.stage_parent_path = stage_directory;
        execution.stage_parent_fd =
            open(stage_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (execution.stage_parent_fd < 0) {
            goto done;
        }
        if (node) {
            execution.node_sdk_directory_fd =
                open(sdk_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (execution.node_sdk_directory_fd < 0) {
                result = diagnostic("Host SDK directory unavailable", 4);
                goto done;
            }
        }
    }
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
    char programs_json[512] = {0};
    if (node) {
        int n = snprintf(programs_json,
                         sizeof(programs_json),
                         ",\"programs\":{\"node\":{\"state\":\"available\",\"version\":\"%s\"}}",
                         node_version);
        if (n <= 0 || (size_t)n >= sizeof(programs_json)) {
            goto done;
        }
    }
#if defined(__APPLE__)
    const char *host_os = "macos";
#elif defined(__linux__)
    const char *host_os = "linux";
#else
    const char *host_os = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    const char *host_arch = "aarch64";
#elif defined(__x86_64__)
    const char *host_arch = "x86_64";
#else
    const char *host_arch = "unknown";
#endif
    char host_json[1024];
    int host_size = snprintf(host_json,
                             sizeof(host_json),
                             "{\"side\":\"executor\",\"sdkVersion\":\"" GTERM_SDK_VERSION
                             "\",\"protocols\":[" GTERM_PROTOCOL
                             "],\"runtimes\":[\"%s\"],\"capabilities\":[\"session.rpc\"%s%s%s],"
                             "\"os\":\"%s\",\"arch\":\"%s\"%s}",
                             runtime_name,
                             process_available ? ",\"process.spawn\",\"process.pipes\"" : "",
                             resource_store_available ? ",\"resource.receive\"" : "",
                             root_count && resource_store_available ? ",\"file.readScoped\"" : "",
                             host_os,
                             host_arch,
                             programs_json);
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
    /* A Native Client explicitly asks to use its own view service. This is
     * a shared Session requirement, not an Executor rendering capability or
     * execution approval. The peer must negotiate and consent to the same set. */
    if (client_table) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            result = diagnostic("Client table requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.table");
        strcpy(config.available[config.available_count++], "ui.table");
    }
    if (client_diff) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            result = diagnostic("Client diff requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.diff");
        strcpy(config.available[config.available_count++], "ui.diff");
    }
    if (client_task) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            result = diagnostic("Client task requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.task");
        strcpy(config.available[config.available_count++], "ui.task");
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
    if (extra_environment_count && !process_spawn && !external) {
        result = diagnostic("selected environment requires an authorized process service", 4);
        goto done;
    }
    if (receive_resources) {
        config.features |= GT_EXT_FEATURE_RESOURCES;
    }
    if (process_spawn || external) {
        /* Executor children receive only selected user environment values.
         * Do not inherit arbitrary variables such as SSH_AUTH_SOCK or secrets. */
        static const char *const selected_names[] = {
            "HOME", "PATH", "XDG_CONFIG_HOME", "CODEX_HOME", "TMPDIR"};
        size_t default_count = sizeof(selected_names) / sizeof(selected_names[0]);
        for (size_t i = 0; i < default_count + extra_environment_count; i++) {
            const char *name =
                i < default_count ? selected_names[i] : extra_environment[i - default_count];
            bool duplicate = false;
            for (size_t j = 0; j < process_environment_count; j++) {
                duplicate |= !strcmp(runtime_environment[j].name, name);
            }
            if (duplicate) {
                result = diagnostic("duplicate selected process environment name", 2);
                goto done;
            }
            const char *value = getenv(name);
            if (!value || !value[0]) {
                if (i >= default_count) {
                    result =
                        diagnostic("explicitly selected process environment is unavailable", 4);
                    goto done;
                }
                continue;
            }
            size_t length = strnlen(value, sizeof(environment_values[0]));
            if (length >= sizeof(environment_values[0])) {
                result = diagnostic("selected process environment value exceeds its limit", 4);
                goto done;
            }
            strcpy(environment_values[process_environment_count], value);
            runtime_environment[process_environment_count] =
                (gt_ext_process_environment_t){name, environment_values[process_environment_count]};
            process_environment_count++;
        }
        for (size_t i = 0; i < process_program_count; i++) {
            runtime_programs[i] =
                (gt_ext_process_program_mapping_t){process_programs[i].name,
                                                   process_programs[i].executable,
                                                   process_programs[i].version,
                                                   process_programs[i].sha256,
                                                   process_programs[i].prefix,
                                                   process_programs[i].prefix_count};
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
        /* Cold process/protocol activation over SSH exceeded the default 25ms.
         * Keep a finite Host-selected worker slice, matching the Android Host. */
        .cpu_ms = external ? 0 : 100,
        .invocation = &invocation,
        .roots = root_count ? runtime_roots : NULL,
        .root_count = root_count,
        .process_programs = process_spawn ? runtime_programs : NULL,
        .process_program_count = process_spawn ? process_program_count : 0,
        .process_environment = process_spawn ? runtime_environment : NULL,
        .process_environment_count = process_spawn ? process_environment_count : 0,
        .execution = external ? &execution : NULL};
    execution.environment = runtime_environment;
    execution.environment_count = process_environment_count;
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
            activated = activated || (info.external_process && info.activated);
            result = info.external_process && info.execution_error == GT_ERR_UNSUPPORTED ? 4
                     : activated && info.session_end == GT_EXT_END_PEER                  ? 0
                     : info.session_end == GT_EXT_END_LOST                               ? 7
                                                                                         : 5;
            if (result) {
                fprintf(stderr,
                        "gterm: runtime phase=%u error=%u execution-error=%d child-exit=%d "
                        "child-signal=%d session-end=%u session-error=%d cleanup=%d\n",
                        (unsigned)info.phase,
                        (unsigned)info.error,
                        (int)info.execution_error,
                        info.exit_code,
                        info.term_signal,
                        (unsigned)info.session_end,
                        (int)info.session_error,
                        (int)info.cleanup_error);
            }
            break;
        }
        idle();
    }
done:
    if (!close_owners(&runtime, &session, &channel, &resource_owner, roots, root_count)) {
        result = 5;
        stage = "owner cleanup";
    }
    if (execution.node_sdk_directory_fd >= 0 && close(execution.node_sdk_directory_fd)) {
        result = 5;
    }
    if (execution.stage_parent_fd >= 0 && close(execution.stage_parent_fd)) {
        result = 5;
    }
    if (stage_directory[0] && rmdir(stage_directory)) {
        result = 5;
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
