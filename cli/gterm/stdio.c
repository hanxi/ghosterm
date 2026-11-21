#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include "release.h"
#include "catalog.h"
#include "stdio.h"
#include "extension/process-runtime.h"
#include "extension/wire.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
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

typedef struct {
    int argc;
    char **argv;
    const char *trusted, *stage;
    int result, reference;
    bool prepared, started, activated, finished, released, cleanup_failed;
    uint64_t close_deadline;
    char instance[33];
    uint64_t epoch;
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX];
    process_program_t process_programs[GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX];
    source_root_t roots[GT_EXT_RUNTIME_ROOTS_MAX];
    gt_ext_runtime_root_t runtime_roots[GT_EXT_RUNTIME_ROOTS_MAX];
    gt_ext_process_program_mapping_t runtime_programs[GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX];
    gt_ext_process_environment_t runtime_environment[GT_EXT_RUNTIME_PROCESS_ENV_MAX];
    char environment_values[GT_EXT_RUNTIME_PROCESS_ENV_MAX][4097];
    size_t method_count, process_program_count, process_environment_count, root_count;
    gt_ext_invocation_t invocation;
    gt_ext_runtime_execution_t execution;
    gt_ext_runtime_config_t runtime_config;
    gt_ext_session_config_t session_config;
    gt_ext_package_metadata_t metadata;
    char stage_directory[PATH_MAX];
    gt_ext_package_t *package;
    gt_ext_channel_t *channel;
    gt_ext_session_t *session;
    gt_ext_runtime_t *runtime;
    resource_owner_t resource_owner;
    gt_ext_resource_store_t *shared_store, *store;
} stdio_job_t;

static stdio_job_t *job_new(gt_ext_resource_store_t *shared) {
    stdio_job_t *job = calloc(1, sizeof(*job));
    if (job) {
        job->shared_store = shared;
        job->reference = -1;
        job->execution.stage_parent_fd = job->execution.node_sdk_directory_fd = -1;
        job->result = 5;
        job->stage = "Executor startup";
    }
    return job;
}

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

static bool open_resource_owner(resource_owner_t *owner, bool shared) {
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
    gt_ext_resource_store_config_t config = {
        fd, shared ? 134217728 : 16777216, 1048576, shared ? 64 : 8};
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

int gterm_stdio_option_arity(const char *option) {
    return !strcmp(option, "--allow-process-execution") ? 0
           : !strcmp(option, "--process-program")       ? 4
           : !strcmp(option, "--node-runtime")          ? 3
           : !strcmp(option, "--release-evidence")      ? 3
           : !strcmp(option, "--node-sdk") || !strcmp(option, "--root") ||
                   !strcmp(option, "--process-argument")
               ? 2
           : !strcmp(option, "--variant") || !strcmp(option, "--runtime") ||
                   !strcmp(option, "--trust-development-digest") ||
                   !strcmp(option, "--client-digest") || !strcmp(option, "--method") ||
                   !strcmp(option, "--host-method") || !strcmp(option, "--process-environment") ||
                   !strcmp(option, "--client-capability")
               ? 1
               : -1;
}

int gterm_stdio_invocation_index(int argc, char **argv) {
    for (int at = 0; at < argc;) {
        const char *option = argv[at];
        if (!strcmp(option, "--")) {
            return at + 2 < argc ? at : -1;
        }
        int count = gterm_stdio_option_arity(option);
        if (count < 0 || count >= argc - at) {
            return -1;
        }
        at += count + 1;
    }
    return -1;
}

static int stdio_prepare(
    stdio_job_t *job, int argc, char **argv, volatile sig_atomic_t *cancelled, bool validate_only) {
    if (gterm_stdio_invocation_index(argc, argv) < 0) {
        return diagnostic("incomplete or unknown stdio options", 2);
    }
    const char *trusted = NULL, *client = NULL;
    const char *runtime_name = NULL, *node_executable = NULL, *node_version = NULL,
               *node_sha = NULL;
    const char *sdk_directory = NULL, *sdk_sha = NULL;
    const char *variant_id = NULL;
    bool execution_authorized = false;
    bool release_evidence = false;
    bool client_table = false, client_diff = false, client_task = false, client_views = false,
         client_form = false;
    const char *extra_environment[GT_EXT_RUNTIME_PROCESS_ENV_MAX - 5] = {0};
    size_t extra_environment_count = 0;
    int at = 0;
    for (; at < argc && strcmp(argv[at], "--");) {
        if (!strcmp(argv[at], "--variant")) {
            if (variant_id || at + 1 >= argc || !argv[at + 1][0] || strlen(argv[at + 1]) > 128 ||
                strspn(argv[at + 1],
                       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                    strlen(argv[at + 1])) {
                return diagnostic("invalid Executor variant", 2);
            }
            variant_id = argv[at + 1];
            at += 2;
            continue;
        }
        if (!strcmp(argv[at], "--release-evidence")) {
            if (release_evidence || at + 3 >= argc || !gterm_catalog_path(argv[at + 1]) ||
                !gterm_catalog_path(argv[at + 2]) || !gterm_catalog_digest(argv[at + 3])) {
                return diagnostic("invalid release evidence", 2);
            }
            release_evidence = true;
            at += 4;
            continue;
        }
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
            } else if (!strcmp(argv[at + 1], "ui.views")) {
                if (client_views) {
                    return diagnostic("unknown or duplicate Client service requirement", 2);
                }
                client_views = true;
            } else if (!strcmp(argv[at + 1], "ui.form")) {
                if (client_form) {
                    return diagnostic("unknown or duplicate Client service requirement", 2);
                }
                client_form = true;
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
            if (at + 2 >= argc || job->root_count == GT_EXT_RUNTIME_ROOTS_MAX) {
                return diagnostic("missing or excess resource root", 2);
            }
            job->roots[job->root_count++] =
                (source_root_t){.id = argv[at + 1], .path = argv[at + 2], .fd = -1};
            at += 3;
            continue;
        }
        if (!strcmp(argv[at], "--process-program")) {
            if (at + 4 >= argc ||
                job->process_program_count == GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX) {
                return diagnostic("missing or excess process program mapping", 2);
            }
            job->process_programs[job->process_program_count++] =
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
            size_t selected = job->process_program_count;
            for (size_t i = 0; i < job->process_program_count; i++) {
                if (!strcmp(job->process_programs[i].name, argv[at + 1])) {
                    selected = i;
                    break;
                }
            }
            if (selected == job->process_program_count ||
                job->process_programs[selected].prefix_count == GT_EXT_RUNTIME_PROCESS_PREFIX_MAX) {
                return diagnostic("fixed argument requires an earlier mapping and available slot",
                                  2);
            }
            process_program_t *program = &job->process_programs[selected];
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
                   job->method_count < GT_EXT_RPC_METHODS_MAX && strlen(argv[at + 1]) <= 128) {
            gt_ext_rpc_method_t *m = &job->methods[job->method_count++];
            strcpy(m->method, argv[at + 1]);
            m->receiver = !strcmp(argv[at], "--host-method") ? GT_EXT_HOST : GT_EXT_EXECUTOR;
            strcpy(m->capability,
                   m->receiver == GT_EXT_HOST && !strcmp(m->method, "view.form.request")
                       ? "ui.form"
                       : "session.rpc");
            m->input_shape = m->output_shape = GT_EXT_JSON_OBJECT;
        } else {
            return diagnostic("invalid or duplicate stdio option", 2);
        }
        at += 2;
    }
    bool form_method = false;
    for (size_t i = 0; i < job->method_count; i++) {
        if (!strcmp(job->methods[i].capability, "ui.form")) {
            form_method = true;
        }
    }
    if (client_form != form_method) {
        return diagnostic("Client form requires ui.form and the form Host method", 2);
    }
    if (!digest(trusted) || !digest(client) || at + 2 >= argc || strcmp(argv[at], "--")) {
        return diagnostic(
            "stdio requires explicit development trust, client digest, directory and command", 2);
    }
    const char *directory = argv[at + 1], *command = argv[at + 2];
    job->invocation = (gt_ext_invocation_t){
        command, (const char *const *)(argv + at + 3), (size_t)(argc - at - 3)};
    if (!validate_only && (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO))) {
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
    if (validate_only) {
        return 0;
    }
    job->execution = (gt_ext_runtime_execution_t){.stage_parent_fd = -1,
                                                  .node_sdk_directory_fd = -1,
                                                  .execution_authorized = execution_authorized,
                                                  .node_executable = node_executable,
                                                  .node_sha256 = node_sha,
                                                  .node_sdk_catalog_sha256 = sdk_sha};
    if (external) {
        const char *temporary = getenv("TMPDIR");
        if (!temporary || temporary[0] != '/') {
            job->result = diagnostic("process execution requires an explicit absolute TMPDIR", 4);
            goto done;
        }
        int n = snprintf(job->stage_directory,
                         sizeof(job->stage_directory),
                         "%s/gterm-execution-XXXXXX",
                         temporary);
        if (n <= 0 || (size_t)n >= sizeof(job->stage_directory) || !mkdtemp(job->stage_directory)) {
            job->stage_directory[0] = 0;
            goto done;
        }
        job->execution.stage_parent_path = job->stage_directory;
        job->execution.stage_parent_fd =
            open(job->stage_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (job->execution.stage_parent_fd < 0) {
            goto done;
        }
        if (node) {
            job->execution.node_sdk_directory_fd =
                open(sdk_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (job->execution.node_sdk_directory_fd < 0) {
                job->result = diagnostic("Host SDK directory unavailable", 4);
                goto done;
            }
        }
    }
    if (!open_roots(job->roots, job->root_count)) {
        job->result = diagnostic("resource root unavailable or invalid", 2);
        goto done;
    }
    if (job->process_program_count && !job->root_count) {
        job->result = diagnostic("process mappings require an explicit workspace root", 4);
        goto done;
    }
    for (size_t i = 0; i < job->root_count; i++) {
        job->runtime_roots[i] = job->roots[i].runtime;
    }
    bool resource_store_available =
        job->shared_store || open_resource_owner(&job->resource_owner, false);
    job->store = job->shared_store ? job->shared_store : job->resource_owner.store;
    job->stage = "package capture";
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        job->result = diagnostic("package directory unavailable", 5);
        job->stage = "package directory";
        goto done;
    }
    gt_ext_package_config_t capture = {fd, 1024, 4096, 16777216, 67108864};
    gt_status_t status = gt_ext_package_open(&capture, &job->package);
    int closed = close(fd);
    if (status != GT_OK || closed) {
        goto done;
    }
    gt_ext_package_phase_t phase;
    do {
        if (*cancelled) {
            gt_ext_package_stop(job->package);
        }
        phase = gt_ext_package_step(job->package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    if (*cancelled) {
        job->result = 130;
        goto done;
    }
    if (phase != GT_EXT_PACKAGE_CAPTURED) {
        goto done;
    }
    const bool process_available = job->process_program_count && job->root_count;
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
    char variant_json[160] = {0};
    if (variant_id) {
        int count =
            snprintf(variant_json, sizeof(variant_json), ",\"variantId\":\"%s\"", variant_id);
        if (count < 0 || (size_t)count >= sizeof(variant_json)) {
            goto done;
        }
    }
    char host_json[1024];
    int host_size =
        snprintf(host_json,
                 sizeof(host_json),
                 "{\"side\":\"executor\",\"sdkVersion\":\"" GTERM_SDK_VERSION
                 "\",\"protocols\":[" GTERM_PROTOCOL
                 "],\"runtimes\":[\"%s\"],\"capabilities\":[\"session.rpc\"%s%s%s],"
                 "\"os\":\"%s\",\"arch\":\"%s\"%s%s}",
                 runtime_name,
                 process_available ? ",\"process.spawn\",\"process.pipes\"" : "",
                 resource_store_available ? ",\"resource.receive\"" : "",
                 job->root_count && resource_store_available ? ",\"file.readScoped\"" : "",
                 host_os,
                 host_arch,
                 programs_json,
                 variant_json);
    if (host_size <= 0 || (size_t)host_size >= sizeof(host_json)) {
        goto done;
    }
    job->stage = "package compatibility check";
    gt_ext_package_check_config_t check = {.host_json = host_json};
    if (gt_ext_package_check(job->package, &check) != GT_EXT_PACKAGE_CHECKED) {
        gt_ext_package_info_t info;
        if (gt_ext_package_info(job->package, &info)) {
            job->result = info.error == GT_EXT_PACKAGE_UNSUPPORTED ? 4
                          : info.error == GT_EXT_PACKAGE_UNKNOWN   ? 3
                                                                   : 5;
            if (info.code[0]) {
                fprintf(stderr, "gterm: package compatibility failed (%s)\n", info.code);
            }
        }
        goto done;
    }
    gt_ext_package_metadata_t metadata;
    if (!gt_ext_package_metadata(job->package, &metadata)) {
        goto done;
    }
    if (strcmp(metadata.package_digest, trusted)) {
        job->result = diagnostic("development package digest changed", 6);
        goto done;
    }
    int release_status = gterm_release_profile(argc, argv, trusted, &metadata, cancelled);
    if (release_status) {
        job->result = release_status;
        job->stage = "signed release revalidation";
        goto done;
    }
    uint32_t count;
    bool selected = false;
    if (!gt_ext_package_command_count(job->package, &count)) {
        goto done;
    }
    for (uint32_t i = 0; i < count; i++) {
        gt_ext_package_command_t c;
        if (!gt_ext_package_command(job->package, i, &c)) {
            goto done;
        }
        if (!strcmp(c.path, command)) {
            selected = true;
        }
    }
    if (!selected) {
        job->result = diagnostic("command is not declared by this package", 2);
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
        if (job->root_count) {
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
            job->result = diagnostic("Client table requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.table");
        strcpy(config.available[config.available_count++], "ui.table");
    }
    if (client_diff) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            job->result = diagnostic("Client diff requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.diff");
        strcpy(config.available[config.available_count++], "ui.diff");
    }
    if (client_task) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            job->result = diagnostic("Client task requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.task");
        strcpy(config.available[config.available_count++], "ui.task");
    }
    if (client_views) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            job->result = diagnostic("Client views requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.views");
        strcpy(config.available[config.available_count++], "ui.views");
    }
    if (client_form) {
        if (!rpc || config.required_count >= 16 || config.available_count >= 16) {
            job->result = diagnostic("Client form requires declared RPC and a capability slot", 4);
            goto done;
        }
        strcpy(config.required[config.required_count++], "ui.form");
        strcpy(config.available[config.available_count++], "ui.form");
    }
    if ((receive_resources && !resource_store_available) || (scoped_reads && !job->root_count) ||
        (scoped_reads && !receive_resources)) {
        job->result =
            diagnostic("resource capability requires an available store and explicit root", 4);
        goto done;
    }
    if ((process_spawn || process_pipes) &&
        (!process_available || !process_spawn || !process_pipes)) {
        job->result = diagnostic(
            "process capabilities require explicit program mappings and a workspace root", 4);
        goto done;
    }
    if (job->process_program_count && !process_spawn) {
        job->result = diagnostic(
            "process mappings require both process.spawn and process.pipes in the package", 4);
        goto done;
    }
    if (extra_environment_count && !process_spawn && !external) {
        job->result = diagnostic("selected environment requires an authorized process service", 4);
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
            for (size_t j = 0; j < job->process_environment_count; j++) {
                duplicate |= !strcmp(job->runtime_environment[j].name, name);
            }
            if (duplicate) {
                job->result = diagnostic("duplicate selected process environment name", 2);
                goto done;
            }
            const char *value = getenv(name);
            if (!value || !value[0]) {
                if (i >= default_count) {
                    job->result =
                        diagnostic("explicitly selected process environment is unavailable", 4);
                    goto done;
                }
                continue;
            }
            size_t length = strnlen(value, sizeof(job->environment_values[0]));
            if (length >= sizeof(job->environment_values[0])) {
                job->result = diagnostic("selected process environment value exceeds its limit", 4);
                goto done;
            }
            strcpy(job->environment_values[job->process_environment_count], value);
            job->runtime_environment[job->process_environment_count] =
                (gt_ext_process_environment_t){
                    name, job->environment_values[job->process_environment_count]};
            job->process_environment_count++;
        }
        for (size_t i = 0; i < job->process_program_count; i++) {
            job->runtime_programs[i] =
                (gt_ext_process_program_mapping_t){job->process_programs[i].name,
                                                   job->process_programs[i].executable,
                                                   job->process_programs[i].version,
                                                   job->process_programs[i].sha256,
                                                   job->process_programs[i].prefix,
                                                   job->process_programs[i].prefix_count};
        }
    }
    if (job->method_count && !rpc) {
        job->result = diagnostic("methods require a declared session.rpc capability", 4);
        goto done;
    }
    job->session_config = config;
    job->trusted = trusted;
    job->metadata = metadata;
    job->argc = argc;
    job->argv = argv;
    job->runtime_config = (gt_ext_runtime_config_t){
        .role = GT_EXT_EXECUTOR,
        /* Cold process/protocol activation over SSH exceeded the default 25ms.
         * Keep a finite Host-selected worker slice, matching the Android Host. */
        .cpu_ms = external ? 0 : 100,
        .invocation = &job->invocation,
        .roots = job->root_count ? job->runtime_roots : NULL,
        .root_count = job->root_count,
        .process_programs = process_spawn ? job->runtime_programs : NULL,
        .process_program_count = process_spawn ? job->process_program_count : 0,
        .process_environment = process_spawn ? job->runtime_environment : NULL,
        .process_environment_count = process_spawn ? job->process_environment_count : 0,
        .execution = external ? &job->execution : NULL};
    job->execution.environment = job->runtime_environment;
    job->execution.environment_count = job->process_environment_count;
    job->prepared = true;
    return 0;
done:
    return job->result;
}

static uint64_t clock_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        return UINT64_MAX;
    }
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static gt_status_t open_stdio_channel(gt_ext_channel_t **channel) {
    int input = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3),
        output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (input < 0 || output < 0) {
        if (input >= 0) {
            close(input);
        }
        if (output >= 0) {
            close(output);
        }
        return GT_ERR_IO;
    }
    gt_status_t status = gt_ext_channel_open_fd(input, output, channel);
    if (status != GT_OK) {
        close(input);
        close(output);
    }
    return status;
}

static int job_prepare(stdio_job_t *job, int argc, char **argv, volatile sig_atomic_t *cancelled) {
    int status = gterm_release_pin_profile(argc, argv, &job->reference);
    if (status) {
        return diagnostic("signed version reference unavailable", status);
    }
    return stdio_prepare(job, argc, argv, cancelled, false);
}

static void job_start(stdio_job_t *job) {
    job->stage = "Session owner open";
    gt_status_t status = (job->session_config.features & GT_EXT_FEATURE_RESOURCES)
                             ? gt_ext_session_open_resources(&job->channel,
                                                             GT_EXT_EXECUTOR,
                                                             &job->session_config,
                                                             job->methods,
                                                             job->method_count,
                                                             job->store,
                                                             16777216,
                                                             &job->session)
                             : gt_ext_session_open_rpc(&job->channel,
                                                       GT_EXT_EXECUTOR,
                                                       &job->session_config,
                                                       job->methods,
                                                       job->method_count,
                                                       &job->session);
    job->started = status == GT_OK;
    job->finished = !job->started;
    if (job->started) {
        job->stage = "Session handshake";
    }
}

static void job_step(stdio_job_t *job, volatile sig_atomic_t *cancelled) {
    if (job->finished) {
        return;
    }
    if (*cancelled) {
        job->result = 130;
        job->finished = true;
        return;
    }
    if (job->session) {
        gt_ext_session_phase_t state = gt_ext_session_step(job->session, 0);
        if (state >= GT_EXT_SESSION_ENDING) {
            gt_ext_session_info_t info;
            job->result =
                gt_ext_session_info(job->session, &info) && info.end == GT_EXT_END_DENIED ? 6 : 7;
            job->finished = true;
            return;
        }
        if (state != GT_EXT_SESSION_ACTIVE) {
            return;
        }
        job->stage = "signed release before activation";
        int status =
            gterm_release_profile(job->argc, job->argv, job->trusted, &job->metadata, cancelled);
        if (status) {
            job->result = status;
            job->finished = true;
            return;
        }
        job->stage = "runtime activation";
        gt_status_t opened = gt_ext_runtime_open_package(
            &job->session, job->package, &job->runtime_config, true, &job->runtime);
        if (opened != GT_OK) {
            fprintf(stderr, "gterm: runtime open status=%d\n", (int)opened);
            job->finished = true;
            return;
        }
        gt_status_t destroyed = gt_ext_package_destroy(&job->package);
        if (destroyed != GT_OK) {
            fprintf(stderr, "gterm: activated package destroy status=%d\n", (int)destroyed);
            job->finished = true;
            return;
        }
        job->stage = "runtime execution";
    }
    gt_ext_runtime_phase_t state = gt_ext_runtime_step(job->runtime);
    if (state == GT_EXT_RUNTIME_ACTIVE) {
        job->activated = true;
    }
    if (state < GT_EXT_RUNTIME_FAILED) {
        return;
    }
    gt_ext_runtime_info_t info;
    if (gt_ext_runtime_info(job->runtime, &info)) {
        job->activated |= info.external_process && info.activated;
        job->result = info.external_process && info.execution_error == GT_ERR_UNSUPPORTED ? 4
                      : job->activated && info.session_end == GT_EXT_END_PEER             ? 0
                      : info.session_end == GT_EXT_END_LOST                               ? 7
                                                                                          : 5;
        if (job->result) {
            const char *failure_stage = gt__ext_runtime_failure_stage(job->runtime);
            if (failure_stage) {
                fprintf(stderr, "gterm: external runtime failure stage=%s\n", failure_stage);
            }
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
    }
    job->finished = true;
}

/* One bounded close step, interleaved with every sibling and the physical mux.
 * Never block one draining instance in a private ten-second cleanup loop. */
static bool job_close_step(stdio_job_t *job) {
    if (job->released) {
        return true;
    }
    if (job->runtime) {
        gt_ext_runtime_stop(job->runtime);
        gt_ext_close_t status = gt_ext_runtime_close(job->runtime);
        if (status != GT_EXT_RELEASED) {
            job->cleanup_failed |= status == GT_EXT_CLOSE_ERROR;
            return false;
        }
        gt_ext_runtime_info_t info = {0};
        bool observed = gt_ext_runtime_info(job->runtime, &info);
        if (observed && info.external_process && job->result && info.execution_error != GT_OK &&
            info.execution_error != GT_ERR_CANCELLED) {
            fprintf(stderr,
                    "gterm: external runtime final probe-verified=%u activated=%u "
                    "child-reaped=%u child-exit=%d child-signal=%d stdout-bytes=%" PRIu64
                    " stderr-bytes=%" PRIu64 "\n",
                    info.probe_verified ? 1u : 0u,
                    info.activated ? 1u : 0u,
                    info.child_exited ? 1u : 0u,
                    info.exit_code,
                    info.term_signal,
                    info.stdout_bytes,
                    info.stderr_bytes);
        }
        if (observed && info.external_process && info.group_signal_error) {
            fprintf(
                stderr, "gterm: exited process group signal errno=%d\n", info.group_signal_error);
        }
        gt_status_t destroyed = gt_ext_runtime_destroy(&job->runtime);
        if (destroyed != GT_OK) {
            job->cleanup_failed = true;
            return false;
        }
        if (!observed || info.dispose_error || info.cleanup_error != GT_OK) {
            fprintf(stderr,
                    "gterm: runtime cleanup observed=%u destroy=%d dispose-error=%u cleanup=%d\n",
                    observed ? 1u : 0u,
                    (int)destroyed,
                    info.dispose_error ? 1u : 0u,
                    (int)info.cleanup_error);
            job->cleanup_failed = true;
        }
    }
    if (job->session) {
        gt_ext_session_stop(job->session);
        gt_ext_close_t status = gt_ext_session_close(job->session, 0);
        if (status != GT_EXT_RELEASED) {
            job->cleanup_failed |= status == GT_EXT_CLOSE_ERROR;
            return false;
        }
        if (gt_ext_session_destroy(&job->session) != GT_OK) {
            job->cleanup_failed = true;
            return false;
        }
    }
    if (job->channel) {
        gt_ext_channel_stop(job->channel);
        gt_ext_close_t status = gt_ext_channel_close(job->channel, 0);
        if (status != GT_EXT_RELEASED) {
            job->cleanup_failed |= status == GT_EXT_CLOSE_ERROR;
            return false;
        }
        if (gt_ext_channel_destroy(&job->channel) != GT_OK) {
            job->cleanup_failed = true;
            return false;
        }
    }
    if (job->package && gt_ext_package_destroy(&job->package) != GT_OK) {
        job->cleanup_failed = true;
        return false;
    }
    if (!close_resource_owner(&job->resource_owner)) {
        job->cleanup_failed = true;
        if (job->resource_owner.store) {
            return false;
        }
    }
    int *fds[] = {
        &job->execution.node_sdk_directory_fd, &job->execution.stage_parent_fd, &job->reference};
    for (size_t i = 0; i < sizeof(fds) / sizeof(*fds); i++) {
        int fd = *fds[i];
        *fds[i] = -1;
        if (fd >= 0 && close(fd)) {
            job->cleanup_failed = true;
        }
    }
    if (job->stage_directory[0] && rmdir(job->stage_directory)) {
        job->cleanup_failed = true;
    }
    for (size_t i = 0; i < job->root_count; i++) {
        int fd = job->roots[i].fd;
        job->roots[i].fd = -1;
        if (fd >= 0 && close(fd)) {
            job->cleanup_failed = true;
        }
    }
    if (job->cleanup_failed) {
        job->result = 5;
        job->stage = "owner cleanup";
    }
    if (job->result) {
        diagnostic(job->result == 6     ? "executor denied"
                   : job->result == 7   ? "executor channel outcome unknown"
                   : job->result == 130 ? "executor cancelled"
                                        : job->stage,
                   job->result);
    }
    if (job->instance[0]) {
        fprintf(stderr,
                "gterm: instance=%s epoch=%" PRIu64 " exit=%d cleanup-error=%u\n",
                job->instance,
                job->epoch,
                job->result,
                job->cleanup_failed ? 1u : 0u);
    }
    job->released = true;
    return true;
}

static void job_free(stdio_job_t *job) {
    if (!job || !job->released) {
        return;
    } /* Retain a failed owner until process exit. */
    volatile unsigned char *bytes = (volatile unsigned char *)job;
    for (size_t i = 0; i < sizeof(*job); i++) {
        bytes[i] = 0;
    }
    free(job);
}

static int
jobs_drive(stdio_job_t **jobs, size_t count, gt_ext_mux_t *mux, volatile sig_atomic_t *cancelled) {
    size_t remaining = 0, first = 0;
    for (size_t i = 0; i < count; i++) {
        if (jobs[i] && !jobs[i]->released) {
            remaining++;
        }
    }
    while (remaining) {
        if (*cancelled && mux) {
            gt_ext_mux_stop(mux);
        }
        if (mux) {
            (void)gt_ext_mux_step(mux, 0);
        }
        for (size_t offset = 0; offset < count; offset++) {
            stdio_job_t *job = jobs[(first + offset) % count];
            if (!job || job->released) {
                continue;
            }
            job_step(job, cancelled);
            if (!job->finished) {
                continue;
            }
            if (!job->close_deadline) {
                uint64_t now = clock_ms();
                if (now > UINT64_MAX - 10000) {
                    return diagnostic("cleanup clock unavailable", 5);
                }
                job->close_deadline = now + 10000;
            }
            if (job_close_step(job)) {
                remaining--;
            } else if (clock_ms() >= job->close_deadline) {
                if (mux) {
                    gt_ext_mux_stop(mux);
                }
                return diagnostic("instance cleanup deadline exceeded; outcome unknown", 5);
            }
        }
        first = (first + 1) % count;
        if (remaining) {
            idle();
        }
    }
    int result = 0;
    for (size_t i = 0; i < count; i++) {
        if (!jobs[i]) {
            continue;
        }
        if (jobs[i]->cleanup_failed) {
            return 5;
        }
        if (!result && jobs[i]->result) {
            result = jobs[i]->result;
        }
    }
    return result;
}

int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled) {
    stdio_job_t *job = job_new(NULL);
    if (!job) {
        return diagnostic("Executor owner allocation", 5);
    }
    int status = job_prepare(job, argc, argv, cancelled);
    if (!status) {
        status = open_stdio_channel(&job->channel) == GT_OK ? 0 : 5;
    }
    if (!status) {
        job_start(job);
    } else {
        job->result = status;
        job->finished = true;
    }
    int result = jobs_drive(&job, 1, NULL, cancelled);
    job_free(job);
    return result;
}

int gterm_stdio_validate(int argc, char **argv) {
    stdio_job_t *job = job_new(NULL);
    if (!job) {
        return diagnostic("Executor owner allocation", 5);
    }
    volatile sig_atomic_t cancelled = 0;
    int result = stdio_prepare(job, argc, argv, &cancelled, true);
    job->released = true;
    job_free(job);
    return result;
}

static bool native_id(const char *id) {
    return id && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32 && strspn(id, "0") != 32;
}

static bool decimal(const char *text, uint64_t *value) {
    if (!text || text[0] < '1' || text[0] > '9') {
        return false;
    }
    uint64_t parsed = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9' || parsed > (UINT64_MAX - (*p - '0')) / 10) {
            return false;
        }
        parsed = parsed * 10 + (*p - '0');
    }
    *value = parsed;
    return true;
}

typedef struct {
    const char *id;
    uint64_t epoch;
    int argc;
    char *argv[257], *storage;
    size_t storage_bytes;
} mux_instance_t;

static int unhex(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Native argv-v1: version byte 1, BEu16 argc, repeated BEu16 length/raw bytes.
 * Compact transport fits the native process argument-count bound, without a
 * shell, separator escaping, JSON dependency or a path supplied by the peer. */
static void clear_profile(mux_instance_t *instance) {
    if (!instance->storage) {
        return;
    }
    volatile unsigned char *bytes = (volatile unsigned char *)instance->storage;
    for (size_t i = 0; i < instance->storage_bytes; i++) {
        bytes[i] = 0;
    }
    free(instance->storage);
    instance->storage = NULL;
}

static int decode_profile(const char *hex, mux_instance_t *instance) {
    size_t length = strnlen(hex, 131073);
    if (length < 6 || length > 131072 || length % 2) {
        return 2;
    }
    size_t bytes = length / 2;
    instance->storage_bytes = bytes * 2 + 256;
    instance->storage = malloc(instance->storage_bytes);
    if (!instance->storage) {
        return 5;
    }
    unsigned char *raw = (unsigned char *)instance->storage;
    for (size_t i = 0; i < bytes; i++) {
        int high = unhex((unsigned char)hex[2 * i]), low = unhex((unsigned char)hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return 2;
        }
        raw[i] = (unsigned char)((high << 4) | low);
    }
    unsigned count = (unsigned)raw[1] * 256 + raw[2];
    if (raw[0] != 1 || !count || count > 256) {
        return 2;
    }
    size_t at = 3;
    char *text = instance->storage + bytes;
    for (unsigned i = 0; i < count; i++) {
        if (bytes - at < 2) {
            return 2;
        }
        size_t size = (size_t)raw[at] * 256 + raw[at + 1];
        at += 2;
        if (size > bytes - at || memchr(raw + at, 0, size) || !gterm_catalog_utf8(raw + at, size)) {
            return 2;
        }
        instance->argv[i] = text;
        memcpy(text, raw + at, size);
        text[size] = 0;
        text += size + 1;
        at += size;
    }
    instance->argc = (int)count;
    return at == bytes ? 0 : 2;
}

/* The Native control owner is deliberately outside the eight plugin slots.
 * It only consumes explicit management requests on a --live connection; no
 * unknown business frame registers a branch or activates a module. */
static bool
native_text(gt_bridge_wire_t *wire, uint16_t object, const char *key, char *out, size_t capacity) {
    const uint8_t *bytes;
    size_t size;
    if (!gt__bridge_wire_string(wire, gt__bridge_wire_member(wire, object, key), &bytes, &size) ||
        size >= capacity || memchr(bytes, 0, size)) {
        return false;
    }
    memcpy(out, bytes, size);
    out[size] = 0;
    return true;
}

static bool native_reply(gt_ext_mux_t *mux,
                         const char *connection,
                         const char *request,
                         const char *value,
                         const char *error) {
    gt_ext_mux_native_info_t state;
    if (!gt_ext_mux_native_info(mux, &state)) {
        return false;
    }
    char outcome[1024], frame[2048];
    const char *code = error;
    if (error && !strcmp(error, "NOT_NEGOTIATED")) {
        code = "PROTOCOL";
    } else if (error && !strcmp(error, "INVALID_PROFILE")) {
        code = "INVALID_INPUT";
    } else if (error && !strcmp(error, "NO_MEMORY")) {
        code = "RESOURCE_LIMIT";
    } else if (error && !strcmp(error, "START_FAILED")) {
        code = "INTERNAL";
    }
    int length =
        error ? snprintf(outcome,
                         sizeof(outcome),
                         "{\"kind\":\"failed\",\"error\":{\"code\":\"%s\",\"message\":\"Native "
                         "instance management: %s\",\"effect\":\"none\"}}",
                         code,
                         error)
              : snprintf(outcome, sizeof(outcome), "{\"kind\":\"success\",\"value\":%s}", value);
    if (length < 0 || (size_t)length >= sizeof(outcome)) {
        return false;
    }
    length =
        snprintf(frame,
                 sizeof(frame),
                 "{\"wireVersion\":1,\"connectionId\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"1\","
                 "\"streamId\":\"rpc.response\",\"seq\":\"%" PRIu64 "\",\"kind\":\"response\","
                 "\"payload\":{\"requestId\":\"%s\",\"outcome\":%s}}",
                 connection,
                 connection,
                 state.next_send,
                 request,
                 outcome);
    return length > 0 && (size_t)length < sizeof(frame) &&
           gt_ext_mux_native_send(mux, frame, (size_t)length) == GT_OK;
}

static int jobs_drive_live(stdio_job_t **jobs,
                           mux_instance_t *profiles,
                           gt_ext_mux_t *mux,
                           resource_owner_t *shared,
                           const char *connection,
                           volatile sig_atomic_t *cancelled) {
    gt_bridge_wire_t *wire = NULL;
    if (gt__bridge_wire_new(NULL, &wire) != GT_OK) {
        return 5;
    }
    char incoming[8192], finish_id[33] = {0};
    const char *open_keys[] = {"instance", "epoch", "profile"};
    const char *status_keys[] = {"instance", "epoch"};
    int result = 0;
    bool draining = false, replied = false, negotiated = false;
    for (;;) {
        if (*cancelled) {
            draining = true;
            result = 130;
            gt_ext_mux_stop(mux);
        }
        gt_status_t io = gt_ext_mux_step(mux, 0);
        if (io != GT_OK) {
            draining = true;
            if (!result) {
                result = 7;
            }
        }
        size_t active = 0;
        for (size_t i = 0; i < GT_EXT_MUX_INSTANCES_MAX; i++) {
            stdio_job_t *job = jobs[i];
            if (!job) {
                continue;
            }
            if (draining && !job->finished) {
                job->finished = true;
                job->result = result;
            }
            job_step(job, cancelled);
            if (job->finished) {
                if (!job->close_deadline) {
                    job->close_deadline = clock_ms() + 10000;
                }
                if (job_close_step(job)) {
                    if (job->cleanup_failed) {
                        result = 5;
                    } else if (!result && job->result) {
                        result = job->result;
                    }
                    job_free(job);
                    jobs[i] = NULL;
                    clear_profile(&profiles[i]);
                    memset(&profiles[i], 0, sizeof(profiles[i]));
                    continue;
                }
                if (clock_ms() >= job->close_deadline) {
                    result = 5;
                    goto done;
                }
            }
            active++;
        }
        if (draining && !active) {
            if (!finish_id[0] || io != GT_OK) {
                break;
            }
            if (!replied) {
                if (!close_resource_owner(shared)) {
                    result = 5;
                    break;
                }
                if (!native_reply(mux, connection, finish_id, "{\"released\":true}", NULL)) {
                    result = 5;
                    break;
                }
                replied = true;
            }
            gt_ext_mux_native_info_t state;
            if (!gt_ext_mux_native_info(mux, &state)) {
                result = 5;
                break;
            }
            if (!state.outgoing_bytes) {
                break;
            }
        }
        if (!draining) {
            size_t size = 0;
            gt_status_t status = gt_ext_mux_native_take(mux, incoming, sizeof(incoming), &size);
            if (status == GT_OK) {
                gt_bridge_message_t message;
                char id[33], method[129];
                if (gt__bridge_wire_parse(wire, incoming, size, &message) != GT_OK ||
                    !native_text(wire, message.payload, "requestId", id, sizeof(id)) ||
                    !native_text(wire, message.payload, "method", method, sizeof(method))) {
                    result = 5;
                    goto done;
                }
                uint16_t input = gt__bridge_wire_member(wire, message.payload, "input");
                const char *error = negotiated ? "UNSUPPORTED" : "NOT_NEGOTIATED";
                char value[256] = "{}";
                if (!strcmp(method, "native.multiplex.hello") &&
                    gt__bridge_wire_object_fields(wire, input, NULL, 0)) {
                    error = NULL;
                    negotiated = true;
                    strcpy(value, "{\"nativeControl\":1,\"maximumInstances\":8}");
                } else if (negotiated && !strcmp(method, "native.multiplex.finish") &&
                           gt__bridge_wire_object_fields(wire, input, NULL, 0)) {
                    memcpy(finish_id, id, sizeof(id));
                    draining = true;
                    continue;
                } else if (negotiated && !strcmp(method, "native.multiplex.open")) {
                    char instance[33], epoch[21], profile[4097];
                    uint64_t generation = 0;
                    size_t slot = GT_EXT_MUX_INSTANCES_MAX;
                    error = "INVALID_INPUT";
                    if (gt__bridge_wire_object_fields(wire, input, open_keys, 3) &&
                        native_text(wire, input, "instance", instance, sizeof(instance)) &&
                        native_id(instance) &&
                        native_text(wire, input, "epoch", epoch, sizeof(epoch)) &&
                        decimal(epoch, &generation) &&
                        native_text(wire, input, "profile", profile, sizeof(profile))) {
                        for (size_t i = 0; i < GT_EXT_MUX_INSTANCES_MAX; i++) {
                            if (!jobs[i]) {
                                slot = i;
                                break;
                            }
                        }
                        error = "BUSY";
                        if (slot < GT_EXT_MUX_INSTANCES_MAX) {
                            mux_instance_t *selected = &profiles[slot];
                            int prepared = decode_profile(profile, selected);
                            if (!prepared) {
                                prepared = gterm_stdio_validate(selected->argc, selected->argv);
                            }
                            error = prepared == 5 ? "NO_MEMORY" : "INVALID_PROFILE";
                            if (!prepared) {
                                stdio_job_t *job = job_new(shared->store);
                                if (job) {
                                    jobs[slot] = job;
                                    memcpy(job->instance, instance, sizeof(instance));
                                    job->epoch = generation;
                                    selected->id = job->instance;
                                    selected->epoch = generation;
                                    prepared =
                                        job_prepare(job, selected->argc, selected->argv, cancelled);
                                    if (prepared) {
                                        error = prepared == 4   ? "UNSUPPORTED"
                                                : prepared == 6 ? "INTEGRITY"
                                                                : "IO";
                                    } else {
                                        gt_status_t status = gt_ext_mux_open_channel(
                                            mux, instance, generation, &job->channel);
                                        if (status != GT_OK) {
                                            prepared = status == GT_ERR_INVALID_ARG ? 2 : 5;
                                            error = status == GT_ERR_INVALID_ARG ? "INVALID_INPUT"
                                                                                 : "RESOURCE_LIMIT";
                                        }
                                    }
                                    if (!prepared) {
                                        job_start(job);
                                        error = job->started ? NULL : "START_FAILED";
                                        strcpy(value, "{\"admitted\":true}");
                                    } else {
                                        job->finished = true;
                                        job->result = prepared;
                                    }
                                } else {
                                    error = "NO_MEMORY";
                                }
                            }
                            if (!jobs[slot]) {
                                clear_profile(selected);
                                memset(selected, 0, sizeof(*selected));
                            }
                        }
                    }
                } else if (negotiated && !strcmp(method, "native.multiplex.status")) {
                    char instance[33], epoch[21];
                    uint64_t generation;
                    error = "INVALID_INPUT";
                    if (gt__bridge_wire_object_fields(wire, input, status_keys, 2) &&
                        native_text(wire, input, "instance", instance, sizeof(instance)) &&
                        native_id(instance) &&
                        native_text(wire, input, "epoch", epoch, sizeof(epoch)) &&
                        decimal(epoch, &generation)) {
                        bool present = false;
                        for (size_t i = 0; i < GT_EXT_MUX_INSTANCES_MAX; i++) {
                            if (jobs[i] && jobs[i]->epoch == generation &&
                                !strcmp(jobs[i]->instance, instance)) {
                                present = true;
                            }
                        }
                        snprintf(value,
                                 sizeof(value),
                                 "{\"present\":%s,\"instances\":%zu}",
                                 present ? "true" : "false",
                                 active);
                        error = NULL;
                    }
                }
                if (!native_reply(mux, connection, id, value, error)) {
                    result = 5;
                    goto done;
                }
            } else if (status != GT_ERR_NOT_FOUND) {
                result = 5;
                break;
            }
        }
        idle();
    }
done:
    gt__bridge_wire_free(wire);
    return result;
}

int gterm_multiplex(int argc, char **argv, volatile sig_atomic_t *cancelled) {
    mux_instance_t instances[GT_EXT_MUX_INSTANCES_MAX] = {0};
    size_t argument_bytes = 0;
    bool live = argc > 0 && !strcmp(argv[0], "--live");
    if (live) {
        argc--;
        argv++;
    }
    if (live && gt_ext_runtime_support() != GT_OK) {
        return diagnostic("live multiplex runtime unsupported", 4);
    }
    if (argc < (live ? 2 : 6) || strcmp(argv[0], "--connection") || !native_id(argv[1]) ||
        (argc - 2) % 4 || (argc - 2) / 4 > (int)GT_EXT_MUX_INSTANCES_MAX) {
        return diagnostic(
            "multiplex requires a native connection identity and one to eight instances", 2);
    }
    size_t count = (size_t)(argc - 2) / 4;
    int parsed = 0;
    for (size_t i = 0; i < count; i++) {
        int at = 2 + (int)i * 4;
        if (strcmp(argv[at], "--instance") || !native_id(argv[at + 1]) ||
            !decimal(argv[at + 2], &instances[i].epoch)) {
            parsed = 2;
            break;
        }
        instances[i].id = argv[at + 1];
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(instances[i].id, instances[j].id) ||
                instances[i].epoch == instances[j].epoch) {
                parsed = 2;
                break;
            }
        }
        if (parsed) {
            break;
        }
        parsed = decode_profile(argv[at + 3], &instances[i]);
        if (parsed) {
            break;
        }
        for (int j = 0; j < instances[i].argc; j++) {
            size_t size = strlen(instances[i].argv[j]) + 1;
            if (size > 65536 - argument_bytes) {
                parsed = 2;
                break;
            }
            argument_bytes += size;
        }
        if (parsed) {
            break;
        }
        parsed = gterm_stdio_validate(instances[i].argc, instances[i].argv);
        if (parsed) {
            break;
        }
    }
    if (!parsed && (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO))) {
        parsed = 2;
    }
    if (parsed) {
        for (size_t i = 0; i < count; i++) {
            clear_profile(&instances[i]);
        }
        return diagnostic("multiplex profile preflight failed", parsed);
    }
    const char *temporary = getenv("TMPDIR");
    if (!temporary || temporary[0] != '/') {
        for (size_t i = 0; i < count; i++) {
            clear_profile(&instances[i]);
        }
        return diagnostic("multiplex resources require an explicit absolute TMPDIR", 4);
    }
    resource_owner_t shared = {0};
    stdio_job_t *jobs[GT_EXT_MUX_INSTANCES_MAX] = {0};
    gt_ext_channel_t *physical = NULL;
    gt_ext_mux_t *mux = NULL;
    int result = 5;
    size_t owners = 0;
    bool pending = false;
    if (!open_resource_owner(&shared, true)) {
        goto done;
    }
    /* Capture and check every fixed profile before starting any Session. No
     * module runs if a later profile fails Native preflight or digest checks. */
    for (size_t i = 0; i < count; i++) {
        jobs[i] = job_new(shared.store);
        if (!jobs[i]) {
            goto done;
        }
        owners++;
        memcpy(jobs[i]->instance, instances[i].id, 33);
        jobs[i]->epoch = instances[i].epoch;
        int status = job_prepare(jobs[i], instances[i].argc, instances[i].argv, cancelled);
        if (status) {
            result = status;
            jobs[i]->result = status;
            goto done;
        }
    }
    if (open_stdio_channel(&physical) != GT_OK) {
        goto done;
    }
    gt_ext_mux_config_t config = {
        .frame_bytes = live ? 8192 : 4096, .receive_frames = 64, .instances = 8};
    memcpy(config.connection, argv[1], 33);
    if (gt_ext_mux_open(&physical, &config, &mux) != GT_OK) {
        goto done;
    }
    if (live && gt_ext_mux_native_enable(mux, false) != GT_OK) {
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        gt_status_t status =
            gt_ext_mux_open_channel(mux, instances[i].id, instances[i].epoch, &jobs[i]->channel);
        if (status != GT_OK) {
            result = status == GT_ERR_INVALID_ARG ? 2 : status == GT_ERR_UNSUPPORTED ? 4 : 5;
            goto done;
        }
    }
    for (size_t i = 0; i < count; i++) {
        job_start(jobs[i]);
    }
    result = live ? jobs_drive_live(jobs, instances, mux, &shared, argv[1], cancelled)
                  : jobs_drive(jobs, count, mux, cancelled);
    if (live) {
        owners = GT_EXT_MUX_INSTANCES_MAX;
    }
done:
    for (size_t i = 0; i < owners; i++) {
        if (!jobs[i]) {
            continue;
        }
        if (!jobs[i]->released) {
            pending = true;
            if (!jobs[i]->finished) {
                jobs[i]->result = *cancelled ? 130 : result;
                jobs[i]->finished = true;
            }
        }
    }
    if (pending) {
        if (mux) {
            gt_ext_mux_stop(mux);
        }
        int closed = jobs_drive(jobs, owners, mux, cancelled);
        if (closed == 5) {
            result = 5;
        }
    }
    if (mux && (gt_ext_mux_close(mux, 0) != GT_EXT_RELEASED || gt_ext_mux_destroy(&mux) != GT_OK)) {
        result = diagnostic("physical multiplex owner remains unreleased", 5);
    }
    if (physical && (gt_ext_channel_close(physical, 0) != GT_EXT_RELEASED ||
                     gt_ext_channel_destroy(&physical) != GT_OK)) {
        result = 5;
    }
    for (size_t i = 0; i < owners; i++) {
        job_free(jobs[i]);
    }
    if (!close_resource_owner(&shared)) {
        result = diagnostic("shared resource cleanup failed", 5);
    }
    for (size_t i = 0; i < GT_EXT_MUX_INSTANCES_MAX; i++) {
        clear_profile(&instances[i]);
    }
    return result;
}
