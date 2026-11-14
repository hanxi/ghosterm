#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/process-runtime.h"
#include "extension/package-stage.h"
#include "extension/package-runtime.h"
#include "extension/process-rpc.h"
#include "extension/process-owner.h"
#include "extension/session-owner.h"
#include "extension/session.h"
#include "extension/sdk-identity.h"
#include "extension/frame.h"
#include "extension/memory.h"
#include "base/secure-random.h"
#include "base/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STARTUP_MAX 262144u

struct gt_process_runtime {
    atomic_bool stopped;
    gt_process_runtime_info_t info;
    gt_ext_session_t *session;
    gt_ext_lease_t lease;
    gt_package_checked_t *package;
    gt_package_snapshot_t *sdk_snapshot;
    gt_package_stage_t *package_stage, *sdk_stage;
    gt_process_rpc_t *rpc;
    gt_runtime_resources_t *resources;
    gt_process_jobs_t *jobs;
    gt_ext_process_t *child;
    gt_bridge_decoder_t *decoder;
    int parent, root, executable, startup;
    struct stat parent_identity, root_identity, executable_identity, startup_identity;
    char parent_path[4097], root_path[4097], root_name[48], executable_path[4097];
    char sdk_digest[65], executable_digest[65], startup_digest[65], runtime_version[129],
        entry[4097];
    char invocation[STARTUP_MAX + 1], startup_json[STARTUP_MAX + 1];
    size_t invocation_size, startup_size, startup_offset;
    char environment[16384];
    size_t environment_offsets[32], environment_count;
    char version_output[130];
    size_t version_size;
    uint8_t input[16384], output[8196];
    size_t input_at, input_size, output_at, output_size;
    uint64_t began, stop_at, activation_ms, lifetime_ms, grace_ms, stdout_limit, stderr_limit;
    bool node, root_created, probing, killed, stdout_eof, stderr_eof, eof_stop;
};

static bool runtime_now(uint64_t *out) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return false;
    }
    *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return true;
}

static bool runtime_same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mode == b->st_mode && a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime
#ifdef __APPLE__
           && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec
#else
           && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec
#endif
        ;
}

static bool runtime_sha(const char *s) {
    if (!s || strnlen(s, 65) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static gt_status_t runtime_digest(int fd, char digest[65], struct stat *identity) {
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        before.st_size > 512 * 1024 * 1024) {
        return GT_ERR_AUTH;
    }
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    uint8_t buffer[32768];
    off_t at = 0;
    for (;;) {
        ssize_t n = pread(fd, buffer, sizeof(buffer), at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return GT_ERR_IO;
        }
        if (!n) {
            break;
        }
        if (!gt__sha256_update(&hash, buffer, (size_t)n)) {
            return GT_ERR_IO;
        }
        at += n;
    }
    if (fstat(fd, &after) || !runtime_same(&before, &after) || !gt__sha256_final(&hash, digest)) {
        return GT_ERR_AUTH;
    }
    *identity = after;
    return GT_OK;
}

static bool runtime_path(char out[4097], const char *base, const char *name, const char *leaf) {
    int n = snprintf(out, 4097, "%s/%s%s%s", base, name, leaf ? "/" : "", leaf ? leaf : "");
    return n > 0 && n <= 4096;
}

static bool runtime_text(char *out, size_t *used, const char *text) {
    size_t n = strlen(text);
    if (n > STARTUP_MAX - *used) {
        return false;
    }
    memcpy(out + *used, text, n + 1);
    *used += n;
    return true;
}

static bool runtime_quote(char *out, size_t *used, const char *text) {
    if (!runtime_text(out, used, "\"")) {
        return false;
    }
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        char part[7] = {(char)*p, 0};
        if (*p == '"' || *p == '\\') {
            part[0] = '\\';
            part[1] = (char)*p;
            part[2] = 0;
        } else if (*p < 32) {
            snprintf(part, sizeof(part), "\\u%04x", *p);
        }
        if (!runtime_text(out, used, part)) {
            return false;
        }
    }
    return runtime_text(out, used, "\"");
}

static bool runtime_live(gt_process_runtime_t *r) {
    gt_ext_lease_t live;
    return !atomic_load_explicit(&r->stopped, memory_order_relaxed) &&
           gt_ext_session_lease(r->session, &live) && live.epoch == r->lease.epoch &&
           !strcmp(live.token, r->lease.token) && !strcmp(live.instance, r->lease.instance) &&
           !strcmp(live.connection, r->lease.connection);
}

static void runtime_signal(gt_process_runtime_t *r, int number) {
    if (gt__ext_process_signal(r->child, number) == GT_OK) {
        return;
    }
    int error = errno ? errno : EIO;
    gt_ext_process_info_t child = {0};
    if (gt__ext_process_observe_exit(r->child, &child) == GT_OK && child.exited) {
        /* Darwin reports EPERM for an all-zombie group. Preserve the actual
         * error; leader reap does not assert successful grandchild cleanup. */
        r->info.group_signal_error = error;
    } else {
        r->info.cleanup_error = GT_ERR_IO;
    }
}

static void runtime_fail(gt_process_runtime_t *r, gt_status_t error, uint64_t now) {
    if (r->info.phase >= GT_PROCESS_RUNTIME_STOPPING) {
        return;
    }
    if (r->info.error == GT_OK) {
        r->info.error = error;
    }
    gt_ext_session_info_t session;
    if (gt_ext_session_info(r->session, &session)) {
        r->info.session_end = session.end;
        r->info.session_error = session.error;
    }
    gt_ext_session_revoke(r->session);
    gt__runtime_resources_stop(r->resources);
    r->stop_at = now;
    r->info.phase = GT_PROCESS_RUNTIME_STOPPING;
    if (r->child && !r->killed) {
        runtime_signal(r, SIGTERM);
    }
}

static gt_status_t runtime_invocation(gt_process_runtime_t *r,
                                      const gt_ext_invocation_t *input,
                                      const char *workspace) {
    if (!input || !input->command || input->argc > 32 || (input->argc && !input->argv)) {
        return GT_ERR_INVALID_ARG;
    }
    char *decoded = gt__plugin_memory_alloc(NULL, STARTUP_MAX + 1);
    if (!decoded) {
        return GT_ERR_IO;
    }
    size_t size = 0;
    gt_status_t status = gt__package_decode_arguments(
        r->package, input->command, input->argv, input->argc, decoded, STARTUP_MAX, &size);
    if (status == GT_OK) {
        size_t at = 0;
        bool ok = runtime_text(r->invocation, &at, "{\"command\":") &&
                  runtime_quote(r->invocation, &at, input->command) &&
                  runtime_text(r->invocation, &at, ",\"arguments\":[");
        for (size_t i = 0; i < input->argc && ok; i++) {
            ok = (!i || runtime_text(r->invocation, &at, ",")) &&
                 runtime_quote(r->invocation, &at, input->argv[i]);
        }
        ok = ok && runtime_text(r->invocation, &at, "]");
        if (size) {
            decoded[size] = 0;
            ok = ok && runtime_text(r->invocation, &at, ",\"decodedArguments\":") &&
                 runtime_text(r->invocation, &at, decoded);
        }
        if (workspace) {
            ok = ok && runtime_text(r->invocation, &at, ",\"workspaceHandle\":{\"id\":") &&
                 runtime_quote(r->invocation, &at, workspace) &&
                 runtime_text(r->invocation, &at, "}");
        }
        ok = ok && runtime_text(r->invocation, &at, "}");
        if (!ok) {
            status = GT_ERR_INVALID_ARG;
        }
        r->invocation_size = at;
    }
    gt__plugin_memory_free(NULL, decoded, STARTUP_MAX + 1);
    return status;
}

static bool runtime_resource_clock(void *context, uint64_t *out) {
    (void)context;
    return runtime_now(out);
}

gt_status_t gt__process_runtime_open(gt_ext_session_t **session,
                                     gt_package_checked_t *package,
                                     gt_package_snapshot_t **sdk,
                                     const gt_process_runtime_config_t *c,
                                     gt_process_runtime_t **out) {
    if (out) {
        *out = NULL;
    }
    gt_package_check_info_t info;
    gt_ext_session_info_t si;
    gt_ext_lease_t lease;
    if (!out || !session || !*session || !c || !gt__package_checked_info(package, &info) ||
        !c->stage_parent_path || c->stage_parent_path[0] != '/' ||
        strnlen(c->stage_parent_path, 4097) > 4096 ||
        !gt__runtime_source_utf8(c->stage_parent_path, strlen(c->stage_parent_path)) ||
        !gt_ext_session_info(*session, &si) || si.phase != GT_EXT_SESSION_ACTIVE ||
        !gt_ext_session_lease(*session, &lease) || (*session)->sources || !(*session)->requests ||
        !gt__bridge_session_runtime_admissible((*session)->inner) || c->environment_count > 32 ||
        !gt__ext_process_environment_valid(c->environment, c->environment_count) ||
        c->activation_ms > 60000 || c->lifetime_ms > 3600000 || c->stop_grace_ms > 5000 ||
        c->stdout_bytes > 67108864 || c->stderr_bytes > 1048576 ||
        c->root_count > GT_RUNTIME_ROOTS_MAX || (c->root_count && !c->roots)) {
        return GT_ERR_INVALID_ARG;
    }
    if (!c->execution_authorized || (info.association_required && !c->command_associated)) {
        return GT_ERR_AUTH;
    }
    gt_bridge_role_t role;
    size_t method_count;
    (void)gt__bridge_session_methods((*session)->inner, &method_count, &role);
    if (role != GT_BRIDGE_EXECUTOR) {
        return GT_ERR_INVALID_ARG;
    }
    bool node = !strcmp(info.runtime, "node");
    if (!info.executor || (!node && strcmp(info.runtime, "exec")) ||
        strcmp(info.sdk_version, GT_PLUGIN_SDK_API_VERSION)) {
        return GT_ERR_UNSUPPORTED;
    }
    bool protocol = false;
    for (size_t i = 0; i < info.protocol_count; i++) {
        protocol |= info.protocols[i] == 1;
    }
    if (!protocol || !gt__bridge_session_package_matches((*session)->inner,
                                                         GT_BRIDGE_EXECUTOR,
                                                         info.id,
                                                         info.version,
                                                         info.package_digest,
                                                         info.sdk_version)) {
        return GT_ERR_PROTOCOL;
    }
    for (size_t i = 0; i < info.requires_count; i++) {
        const char *capability = info.requires[i];
        if (strcmp(capability, "session.rpc") && strcmp(capability, "resource.receive") &&
            strcmp(capability, "file.readScoped") && strcmp(capability, "process.spawn") &&
            strcmp(capability, "process.pipes")) {
            return GT_ERR_UNSUPPORTED;
        }
        bool process_cap =
            !strcmp(capability, "process.spawn") || !strcmp(capability, "process.pipes");
        if (process_cap &&
            (!c->process.program_count || !c->root_count ||
             gt_ext_session_access(*session, &lease, "process.spawn") != GT_EXT_ACCESS_ALLOWED ||
             gt_ext_session_access(*session, &lease, "process.pipes") != GT_EXT_ACCESS_ALLOWED)) {
            return GT_ERR_UNSUPPORTED;
        }
        if (!process_cap && strcmp(capability, "session.rpc") && !(*session)->store) {
            return GT_ERR_UNSUPPORTED;
        }
        if (!strcmp(capability, "file.readScoped") &&
            (!c->root_count || gt_ext_session_access(*session, &lease, "resource.receive") !=
                                   GT_EXT_ACCESS_ALLOWED)) {
            return GT_ERR_UNSUPPORTED;
        }
        if (gt_ext_session_access(*session, &lease, info.requires[i]) != GT_EXT_ACCESS_ALLOWED) {
            return GT_ERR_AUTH;
        }
    }
    char digest[65];
    gt_package_member_t member;
    const uint8_t *bytes;
    size_t size;
    if (node && (!sdk || !*sdk || !runtime_sha(c->sdk_catalog_sha256) ||
                 !gt__package_stage_catalog_digest(*sdk, digest) ||
                 strcmp(digest, c->sdk_catalog_sha256) || !c->node_executable ||
                 c->node_executable[0] != '/' || strnlen(c->node_executable, 4097) > 4096 ||
                 !runtime_sha(c->node_sha256) || !info.runtime_version[0])) {
        return GT_ERR_AUTH;
    }
    if (!gt__package_checked_member(package, info.entry, &member, &bytes, &size)) {
        return GT_ERR_NOT_FOUND;
    }
    if (node) {
        char canonical[4097];
        if (!realpath(c->node_executable, canonical) || strcmp(canonical, c->node_executable)) {
            return GT_ERR_AUTH;
        }
        size_t index;
        if (gt__package_snapshot_find(*sdk, "node/runtime/bootstrap.mjs", &index) !=
            GT_PACKAGE_OK) {
            return GT_ERR_NOT_FOUND;
        }
    }
    gt_process_runtime_t *r = gt__plugin_memory_calloc(1, sizeof(*r));
    if (!r) {
        return GT_ERR_IO;
    }
    r->parent = r->root = r->executable = r->startup = -1;
    atomic_init(&r->stopped, false);
    r->node = node;
    r->lease = lease;
    r->activation_ms = c->activation_ms ? c->activation_ms : 10000;
    r->lifetime_ms = c->lifetime_ms ? c->lifetime_ms : 60000;
    r->grace_ms = c->stop_grace_ms ? c->stop_grace_ms : 100;
    r->stdout_limit = c->stdout_bytes ? c->stdout_bytes : 16777216;
    r->stderr_limit = c->stderr_bytes ? c->stderr_bytes : 65536;
    gt_status_t status = GT_ERR_IO;
    int fd = open(c->stage_parent_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat path_identity;
    if (fd < 0 || fstat(fd, &path_identity) || fstat(c->stage_parent_fd, &r->parent_identity) ||
        !S_ISDIR(path_identity.st_mode) || path_identity.st_dev != r->parent_identity.st_dev ||
        path_identity.st_ino != r->parent_identity.st_ino || path_identity.st_uid != geteuid() ||
        (path_identity.st_mode & 0077)) {
        if (fd >= 0) {
            (void)close(fd);
        }
        status = GT_ERR_AUTH;
        goto fail;
    }
    r->parent = fd;
    strcpy(r->parent_path, c->stage_parent_path);
    strcpy(r->entry, info.entry);
    if (!gt__package_checked_retain(package, &r->package)) {
        goto fail;
    }
    status = runtime_invocation(
        r, c->invocation, c->process.program_count && c->root_count ? c->roots[0].id : NULL);
    if (status != GT_OK) {
        goto fail;
    }
    size_t used = 0;
    for (size_t i = 0; i < c->environment_count; i++) {
        const char *text = c->environment[i];
        if (!text) {
            status = GT_ERR_INVALID_ARG;
            goto fail;
        }
        size_t n = strnlen(text, 4097);
        if (n > 4096 || n + 1 > sizeof(r->environment) - used || !strchr(text, '=') ||
            !strncmp(text, "NODE_OPTIONS=", 13) || !strncmp(text, "NODE_PATH=", 10) ||
            !strncmp(text, "LD_", 3) || !strncmp(text, "DYLD_", 5)) {
            status = GT_ERR_INVALID_ARG;
            goto fail;
        }
        r->environment_offsets[i] = used;
        memcpy(r->environment + used, text, n + 1);
        used += n + 1;
    }
    r->environment_count = c->environment_count;
    if (node) {
        strcpy(r->executable_path, c->node_executable);
        strcpy(r->executable_digest, c->node_sha256);
        strcpy(r->sdk_digest, c->sdk_catalog_sha256);
        strcpy(r->runtime_version, info.runtime_version);
        r->executable = open(r->executable_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        status = r->executable < 0 ? GT_ERR_AUTH
                                   : runtime_digest(r->executable, digest, &r->executable_identity);
        if (status == GT_OK &&
            (strcmp(digest, r->executable_digest) || !(r->executable_identity.st_mode & 0111))) {
            status = GT_ERR_AUTH;
        }
        if (status != GT_OK) {
            goto fail;
        }
    }
    status = gt__bridge_decoder_new(8192, NULL, &r->decoder);
    if (status != GT_OK) {
        goto fail;
    }
    if (!runtime_now(&r->began)) {
        status = GT_ERR_IO;
        goto fail;
    }
    if (c->root_count && !(*session)->store && !c->process.program_count) {
        status = GT_ERR_UNSUPPORTED;
        goto fail;
    }
    if ((*session)->store || (c->root_count && c->process.program_count)) {
        status = gt__runtime_resources_open(*session,
                                            (*session)->store,
                                            &lease,
                                            c->roots,
                                            c->root_count,
                                            NULL,
                                            runtime_resource_clock,
                                            &r->resources);
        if (status != GT_OK) {
            goto fail;
        }
    }
    if (c->process.program_count) {
        status = gt__process_jobs_open(*session, r->resources, package, &c->process, &r->jobs);
        if (status != GT_OK) {
            goto fail;
        }
    }
    status = gt__process_rpc_open_services(*session, r->resources, r->jobs, NULL, &r->rpc);
    if (status != GT_OK) {
        goto fail;
    }
    r->session = *session;
    *session = NULL;
    if (node) {
        r->sdk_snapshot = *sdk;
        *sdk = NULL;
    }
    *out = r;
    return GT_OK;
fail:
    if (r->jobs) {
        (void)gt__process_jobs_destroy(&r->jobs);
    }
    if (r->resources) {
        (void)gt__runtime_resources_destroy(&r->resources);
    }
    if (r->parent >= 0) {
        (void)close(r->parent);
    }
    if (r->executable >= 0) {
        (void)close(r->executable);
    }
    if (r->package) {
        (void)gt__package_checked_destroy(&r->package);
    }
    gt__bridge_decoder_free(r->decoder);
    gt__plugin_memory_free(NULL, r, sizeof(*r));
    return status;
}

static gt_status_t runtime_stage_error(gt_package_stage_status_t status) {
    switch (status) {
        case GT_STAGE_CANCELLED:
            return GT_ERR_CANCELLED;
        case GT_STAGE_UNSUPPORTED:
            return GT_ERR_UNSUPPORTED;
        case GT_STAGE_CHANGED:
            return GT_ERR_AUTH;
        case GT_STAGE_INVALID:
        case GT_STAGE_LIMIT:
            return GT_ERR_INVALID_ARG;
        default:
            return GT_ERR_IO;
    }
}

static gt_status_t runtime_root(gt_process_runtime_t *r) {
    uint8_t random[16];
    gt_status_t status = gt__secure_random(random, sizeof(random));
    if (status != GT_OK) {
        return status;
    }
    strcpy(r->root_name, "executor-");
    for (size_t i = 0; i < 16; i++) {
        r->root_name[9 + i * 2] = "0123456789abcdef"[random[i] >> 4];
        r->root_name[10 + i * 2] = "0123456789abcdef"[random[i] & 15];
    }
    r->root_name[41] = 0;
    gt__secure_erase(random, sizeof(random));
    if (!runtime_path(r->root_path, r->parent_path, r->root_name, NULL) ||
        mkdirat(r->parent, r->root_name, 0700)) {
        return GT_ERR_IO;
    }
    r->root_created = true;
    r->root = openat(r->parent, r->root_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (r->root < 0 || fstat(r->root, &r->root_identity)) {
        return GT_ERR_IO;
    }
    gt_package_stage_status_t stage =
        gt__package_stage_open(r->package, r->root, 67108864, &r->stopped, NULL, &r->package_stage);
    if (stage != GT_STAGE_OK) {
        return runtime_stage_error(stage);
    }
    if (r->node) {
        stage = gt__package_stage_open_host(
            &r->sdk_snapshot, r->sdk_digest, r->root, &r->stopped, NULL, &r->sdk_stage);
        if (stage != GT_STAGE_OK) {
            return runtime_stage_error(stage);
        }
    }
    return GT_OK;
}

static bool runtime_bound(gt_process_runtime_t *r) {
    struct stat a, b;
    return !stat(r->parent_path, &a) && a.st_dev == r->parent_identity.st_dev &&
           a.st_ino == r->parent_identity.st_ino && !lstat(r->root_path, &b) &&
           S_ISDIR(b.st_mode) && b.st_dev == r->root_identity.st_dev &&
           b.st_ino == r->root_identity.st_ino;
}

static gt_status_t runtime_spawn(gt_process_runtime_t *r, bool probe) {
    if (!runtime_live(r) || !runtime_bound(r)) {
        return GT_ERR_AUTH;
    }
    char digest[65];
    struct stat identity, current;
    if (r->node &&
        (stat(r->executable_path, &current) || !runtime_same(&r->executable_identity, &current) ||
         runtime_digest(r->executable, digest, &identity) != GT_OK ||
         !runtime_same(&r->executable_identity, &identity) ||
         strcmp(digest, r->executable_digest))) {
        return GT_ERR_AUTH;
    }
    const char *argv[5];
    size_t argc = 0;
    int directory;
    const char *name, *entry;
    if (gt__package_stage_view(r->package_stage, &directory, &name, &entry) != GT_STAGE_OK) {
        return GT_ERR_IO;
    }
    char package_root[4097], plugin[4097], bootstrap[4097], startup[4097];
    if (!runtime_path(package_root, r->root_path, name, NULL) ||
        !runtime_path(plugin, r->root_path, name, entry)) {
        return GT_ERR_INVALID_ARG;
    }
    if (probe) {
        argv[0] = r->executable_path;
        argv[1] = "--version";
        argc = 2;
    } else {
        if (!runtime_path(startup, r->root_path, "startup.json", NULL)) {
            return GT_ERR_INVALID_ARG;
        }
        if (r->node) {
            const char *sdk_name, *unused;
            int sdk_fd;
            if (gt__package_stage_view(r->sdk_stage, &sdk_fd, &sdk_name, &unused) != GT_STAGE_OK ||
                !runtime_path(bootstrap, r->root_path, sdk_name, "node/runtime/bootstrap.mjs")) {
                return GT_ERR_IO;
            }
            argv[0] = r->executable_path;
            argv[1] = "--";
            argv[2] = bootstrap;
            argv[3] = startup;
            argv[4] = r->startup_digest;
            argc = 5;
        } else {
            gt_package_member_t member;
            const uint8_t *bytes;
            size_t size;
            int file = openat(directory, entry, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (file < 0) {
                return GT_ERR_IO;
            }
            gt_status_t status = runtime_digest(file, digest, &identity);
            int closed = close(file);
            if (status != GT_OK || closed ||
                !gt__package_checked_member(r->package, entry, &member, &bytes, &size) ||
                strcmp(digest, member.sha256)) {
                return GT_ERR_AUTH;
            }
            argv[0] = plugin;
            argv[1] = "--ghosterm-startup";
            argv[2] = startup;
            argv[3] = r->startup_digest;
            argc = 4;
        }
    }
    const char *environment[32];
    for (size_t i = 0; i < r->environment_count; i++) {
        environment[i] = r->environment + r->environment_offsets[i];
    }
    gt_status_t status = gt__ext_process_spawn(argv[0],
                                               argv,
                                               argc,
                                               directory,
                                               probe ? NULL : environment,
                                               probe ? 0 : r->environment_count,
                                               &r->child);
    if (status == GT_OK) {
        r->probing = probe;
        r->stdout_eof = false;
        r->stderr_eof = false;
        r->killed = false;
    }
    return status;
}

static gt_status_t runtime_startup(gt_process_runtime_t *r) {
    int fd;
    const char *name, *entry;
    if (gt__package_stage_view(r->package_stage, &fd, &name, &entry) != GT_STAGE_OK) {
        return GT_ERR_IO;
    }
    char package_root[4097], plugin[4097], epoch[32];
    if (!runtime_path(package_root, r->root_path, name, NULL) ||
        !runtime_path(plugin, r->root_path, name, entry)) {
        return GT_ERR_INVALID_ARG;
    }
    gt_ext_session_info_t info;
    if (!gt_ext_session_info(r->session, &info)) {
        return GT_ERR_IO;
    }
    snprintf(epoch, sizeof(epoch), "%" PRIu64, info.epoch);
    size_t at = 0;
    bool ok =
        runtime_text(r->startup_json, &at, "{\"v\":1,\"sdkVersion\":\"1.0.0\",\"instanceId\":") &&
        runtime_quote(r->startup_json, &at, info.instance) &&
        runtime_text(r->startup_json, &at, ",\"epoch\":") &&
        runtime_quote(r->startup_json, &at, epoch) &&
        runtime_text(r->startup_json, &at, ",\"packageRoot\":") &&
        runtime_quote(r->startup_json, &at, package_root) &&
        runtime_text(r->startup_json, &at, ",\"entry\":") &&
        runtime_quote(r->startup_json, &at, plugin) &&
        runtime_text(r->startup_json, &at, ",\"invocation\":") &&
        runtime_text(r->startup_json, &at, r->invocation) &&
        runtime_text(r->startup_json, &at, "}");
    if (!ok) {
        return GT_ERR_INVALID_ARG;
    }
    r->startup_size = at;
    r->startup =
        openat(r->root, "startup.json", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (r->startup < 0 || fstat(r->startup, &r->startup_identity)) {
        return GT_ERR_IO;
    }
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    if (!gt__sha256_update(&hash, r->startup_json, at) ||
        !gt__sha256_final(&hash, r->startup_digest)) {
        return GT_ERR_IO;
    }
    return GT_OK;
}

static gt_status_t runtime_io(gt_process_runtime_t *r) {
    if (r->output_at < r->output_size) {
        gt_ext_process_io_t io = gt__ext_process_write(
            r->child, r->output + r->output_at, r->output_size - r->output_at);
        if (io.state != GT_EXT_PROCESS_DATA && io.state != GT_EXT_PROCESS_WAIT) {
            return GT_ERR_IO;
        }
        r->output_at += io.count;
        if (r->output_at == r->output_size) {
            if (gt__process_rpc_consume(r->rpc) != GT_PROCESS_RPC_OK) {
                return GT_ERR_PROTOCOL;
            }
            r->output_at = r->output_size = 0;
        }
    }
    if (r->input_at == r->input_size && !r->stdout_eof) {
        gt_ext_process_io_t io = gt__ext_process_read_stdout(r->child, r->input, sizeof(r->input));
        if (io.state == GT_EXT_PROCESS_ERROR || io.state == GT_EXT_PROCESS_CLOSED) {
            return GT_ERR_IO;
        }
        if (io.state == GT_EXT_PROCESS_EOF) {
            r->stdout_eof = true;
            if (gt__bridge_decoder_finish(r->decoder) != GT_OK) {
                return GT_ERR_PROTOCOL;
            }
        }
        if (io.count > r->stdout_limit - r->info.stdout_bytes) {
            return GT_ERR_IO;
        }
        r->info.stdout_bytes += io.count;
        r->input_at = 0;
        r->input_size = io.count;
    }
    if (!r->output_size && r->input_at < r->input_size) {
        size_t consumed;
        const uint8_t *bytes;
        size_t size;
        if (gt__bridge_decoder_feed(
                r->decoder, r->input + r->input_at, r->input_size - r->input_at, &consumed) !=
            GT_OK) {
            return GT_ERR_PROTOCOL;
        }
        r->input_at += consumed;
        if (gt__bridge_decoder_view(r->decoder, &bytes, &size)) {
            if (gt__process_rpc_call(r->rpc, bytes, size) != GT_PROCESS_RPC_OK ||
                !gt__process_rpc_reply(r->rpc, &bytes, &size)) {
                return GT_ERR_PROTOCOL;
            }
            r->output[0] = (uint8_t)(size >> 24);
            r->output[1] = (uint8_t)(size >> 16);
            r->output[2] = (uint8_t)(size >> 8);
            r->output[3] = (uint8_t)size;
            memcpy(r->output + 4, bytes, size);
            r->output_size = size + 4;
            if (gt__bridge_decoder_consume(r->decoder) != GT_OK) {
                return GT_ERR_PROTOCOL;
            }
        }
    }
    return GT_OK;
}

static void runtime_cleanup(gt_process_runtime_t *r) {
    if (r->rpc && gt__process_rpc_destroy(&r->rpc) != GT_OK) {
        r->info.cleanup_error = GT_ERR_IO;
    }
    if (r->jobs) {
        gt__process_jobs_stop(r->jobs);
        if (gt__process_jobs_step(r->jobs) != GT_OK) {
            r->info.cleanup_error = GT_ERR_IO;
        }
        if (!r->info.group_signal_error) {
            r->info.group_signal_error = gt__process_jobs_group_error(r->jobs);
        }
        gt_status_t status = gt__process_jobs_destroy(&r->jobs);
        if (status == GT_PROCESS_JOBS_WAIT) {
            return;
        }
        if (status != GT_OK) {
            r->info.cleanup_error = status;
            return;
        }
    }
    if (r->package_stage && gt__package_stage_destroy(&r->package_stage) != GT_STAGE_OK) {
        r->info.cleanup_error = GT_ERR_IO;
    }
    if (r->sdk_stage && gt__package_stage_destroy(&r->sdk_stage) != GT_STAGE_OK) {
        r->info.cleanup_error = GT_ERR_IO;
    }
    if (r->startup >= 0) {
        if (close(r->startup)) {
            r->info.cleanup_error = GT_ERR_IO;
        }
        r->startup = -1;
    }
    if (r->root >= 0) {
        struct stat st;
        if (r->startup_identity.st_ino) {
            if (fstatat(r->root, "startup.json", &st, AT_SYMLINK_NOFOLLOW) ||
                st.st_dev != r->startup_identity.st_dev ||
                st.st_ino != r->startup_identity.st_ino || !S_ISREG(st.st_mode) ||
                unlinkat(r->root, "startup.json", 0)) {
                r->info.cleanup_error = GT_ERR_IO;
            }
        }
        if (close(r->root)) {
            r->info.cleanup_error = GT_ERR_IO;
        }
        r->root = -1;
    }
    if (r->root_created) {
        struct stat st;
        if (fstatat(r->parent, r->root_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode) ||
            st.st_dev != r->root_identity.st_dev || st.st_ino != r->root_identity.st_ino ||
            unlinkat(r->parent, r->root_name, AT_REMOVEDIR)) {
            r->info.cleanup_error = GT_ERR_IO;
        }
        r->root_created = false;
    }
    if (r->session) {
        gt_ext_close_t closed = gt_ext_session_close(r->session, 0);
        if (r->resources) {
            gt_runtime_resource_status_t status = gt__runtime_resources_destroy(&r->resources);
            if (status != GT_RUNTIME_RESOURCE_OK) {
                if (status != GT_RUNTIME_RESOURCE_WAIT) {
                    r->info.cleanup_error = GT_ERR_IO;
                }
                return;
            }
            closed = gt_ext_session_close(r->session, 0);
        }
        if (closed != GT_EXT_RELEASED) {
            return;
        }
        gt_ext_session_info_t info;
        if (gt_ext_session_info(r->session, &info) && r->info.session_end == GT_EXT_END_NONE) {
            r->info.session_end = info.end;
            r->info.session_error = info.error;
        }
        if (gt_ext_session_destroy(&r->session) != GT_OK) {
            r->info.cleanup_error = GT_ERR_IO;
            return;
        }
    }
    r->info.phase = GT_PROCESS_RUNTIME_CLOSED;
}

gt_process_runtime_phase_t gt__process_runtime_step(gt_process_runtime_t *r) {
    if (!r) {
        return GT_PROCESS_RUNTIME_CLOSED;
    }
    if (r->info.phase == GT_PROCESS_RUNTIME_CLOSED) {
        return r->info.phase;
    }
    uint64_t now;
    if (!runtime_now(&now)) {
        now = r->began;
        runtime_fail(r, GT_ERR_IO, now);
        if (r->child) {
            runtime_signal(r, SIGKILL);
            r->killed = true;
        }
    }
    if (r->info.phase < GT_PROCESS_RUNTIME_STOPPING) {
        if (atomic_load_explicit(&r->stopped, memory_order_relaxed) ||
            now - r->began >= r->lifetime_ms ||
            (!r->info.activated && now - r->began >= r->activation_ms)) {
            runtime_fail(r, GT_ERR_CANCELLED, now);
        } else {
            gt_ext_session_step(r->session, 0);
            gt__runtime_resources_step(r->resources);
            if (!runtime_live(r)) {
                runtime_fail(r, GT_ERR_CANCELLED, now);
            }
        }
    }
    if (gt__process_jobs_step(r->jobs) != GT_OK) {
        runtime_fail(r, GT_ERR_IO, now);
    }
    if (r->info.phase == GT_PROCESS_RUNTIME_CREATED) {
        gt_status_t status = runtime_root(r);
        if (status != GT_OK) {
            runtime_fail(r, status, now);
        } else {
            r->info.phase = GT_PROCESS_RUNTIME_STAGING;
        }
    } else if (r->info.phase == GT_PROCESS_RUNTIME_STAGING) {
        gt_package_stage_status_t a = gt__package_stage_step(r->package_stage),
                                  b = r->sdk_stage ? gt__package_stage_step(r->sdk_stage)
                                                   : GT_STAGE_OK;
        if ((a != GT_STAGE_OK && a != GT_STAGE_WAIT) || (b != GT_STAGE_OK && b != GT_STAGE_WAIT)) {
            runtime_fail(
                r, runtime_stage_error(a != GT_STAGE_OK && a != GT_STAGE_WAIT ? a : b), now);
        } else if (a == GT_STAGE_OK && b == GT_STAGE_OK) {
            gt_status_t status = GT_OK;
            if (status == GT_OK && r->node) {
                status = runtime_spawn(r, true);
                if (status == GT_OK) {
                    r->info.phase = GT_PROCESS_RUNTIME_PROBING;
                }
            } else if (status == GT_OK) {
                r->info.phase = GT_PROCESS_RUNTIME_STARTING;
            }
            if (status != GT_OK) {
                runtime_fail(r, status, now);
            }
        }
    }
    if (r->info.phase == GT_PROCESS_RUNTIME_STARTING && !r->child) {
        gt_status_t status = GT_OK;
        if (r->startup < 0 && !r->startup_size) {
            status = runtime_startup(r);
        }
        if (status == GT_OK && r->startup >= 0) {
            size_t n = r->startup_size - r->startup_offset;
            if (n > 16384) {
                n = 16384;
            }
            ssize_t written = write(r->startup, r->startup_json + r->startup_offset, n);
            if (written < 0 && errno == EINTR) {
                return r->info.phase;
            }
            if (written <= 0) {
                status = GT_ERR_IO;
            } else {
                r->startup_offset += (size_t)written;
            }
            if (status == GT_OK && r->startup_offset == r->startup_size) {
                int changed = fchmod(r->startup, 0400), closed = close(r->startup);
                r->startup = -1;
                if (changed || closed) {
                    status = GT_ERR_IO;
                } else {
                    status = runtime_spawn(r, false);
                }
            }
        }
        if (status != GT_OK) {
            runtime_fail(r, status, now);
        }
    }
    if (r->child) {
        uint8_t errors[16384];
        gt_ext_process_io_t error_io = {GT_EXT_PROCESS_EOF, 0, GT_OK};
        if (!r->stderr_eof) {
            error_io = gt__ext_process_read_stderr(r->child, errors, sizeof(errors));
        }
        if (error_io.state == GT_EXT_PROCESS_EOF) {
            r->stderr_eof = true;
        }
        if (error_io.count > r->stderr_limit - r->info.stderr_bytes ||
            error_io.state == GT_EXT_PROCESS_ERROR) {
            runtime_fail(r, GT_ERR_IO, now);
        } else {
            r->info.stderr_bytes += error_io.count;
        }
        if (r->probing) {
            gt_ext_process_io_t io = {GT_EXT_PROCESS_EOF, 0, GT_OK};
            if (!r->stdout_eof) {
                io = gt__ext_process_read_stdout(r->child, errors, sizeof(errors));
            }
            if (io.state == GT_EXT_PROCESS_EOF) {
                r->stdout_eof = true;
            }
            if (io.count >= sizeof(r->version_output) - r->version_size ||
                io.state == GT_EXT_PROCESS_ERROR) {
                runtime_fail(r, GT_ERR_AUTH, now);
            } else {
                memcpy(r->version_output + r->version_size, errors, io.count);
                r->version_size += io.count;
            }
        } else {
            gt_status_t status = runtime_io(r);
            if (status != GT_OK) {
                runtime_fail(r, status, now);
            }
            if (r->stdout_eof && r->info.phase < GT_PROCESS_RUNTIME_STOPPING) {
                /* A clean pipe EOF starts shutdown, but its exit classification
                 * is provisional until the actual child status is reaped. */
                r->eof_stop = true;
                runtime_fail(r, GT_ERR_IO, now);
            }
            if (gt__process_rpc_activated(r->rpc) && r->info.phase < GT_PROCESS_RUNTIME_STOPPING) {
                r->info.activated = true;
                r->info.phase = GT_PROCESS_RUNTIME_ACTIVE;
            }
        }
        gt_ext_process_info_t process = {0};
        if (gt__ext_process_observe_exit(r->child, &process) != GT_OK) {
            runtime_fail(r, GT_ERR_IO, now);
        } else if (process.exited && !r->killed) {
            /* Hold the zombie leader's PID until the group has been signalled.
             * Never signal a potentially reused pgid after waitpid reaps it. */
            runtime_signal(r, SIGKILL);
            r->killed = true;
        }
        if (process.exited && (r->stdout_eof || r->info.phase == GT_PROCESS_RUNTIME_STOPPING)) {
            if (gt__ext_process_poll(r->child, &process) != GT_OK || !process.exited) {
                runtime_fail(r, GT_ERR_IO, now);
                return r->info.phase;
            }
            if (gt__ext_process_destroy(&r->child) != GT_OK) {
                r->info.cleanup_error = GT_ERR_IO;
            }
            if (r->probing && r->info.phase < GT_PROCESS_RUNTIME_STOPPING) {
                char expected[132];
                snprintf(expected, sizeof(expected), "v%s\n", r->runtime_version);
                if (process.exit_code || process.term_signal || r->info.stderr_bytes ||
                    strcmp(expected, r->version_output)) {
                    runtime_fail(r, GT_ERR_AUTH, now);
                } else {
                    r->info.probe_verified = true;
                    r->probing = false;
                    r->info.phase = GT_PROCESS_RUNTIME_STARTING;
                }
            } else {
                r->info.child_exited = true;
                r->info.exit_code = process.exit_code;
                r->info.term_signal = process.term_signal;
                if (r->eof_stop && process.exit_code == 77 && !process.term_signal &&
                    r->info.error == GT_ERR_IO) {
                    r->info.error = GT_ERR_UNSUPPORTED;
                }
                runtime_fail(r, process.exit_code == 77 ? GT_ERR_UNSUPPORTED : GT_ERR_IO, now);
            }
        }
    }
    if (r->info.phase == GT_PROCESS_RUNTIME_STOPPING) {
        if (r->child && !r->killed && now - r->stop_at >= r->grace_ms) {
            runtime_signal(r, SIGKILL);
            r->killed = true;
        }
        if (!r->child) {
            runtime_cleanup(r);
        }
    }
    return r->info.phase;
}

void gt__process_runtime_stop(gt_process_runtime_t *r) {
    if (r) {
        atomic_store_explicit(&r->stopped, true, memory_order_relaxed);
    }
}

bool gt__process_runtime_info(const gt_process_runtime_t *r, gt_process_runtime_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!r || !out) {
        return false;
    }
    *out = r->info;
    return true;
}

gt_ext_close_t gt__process_runtime_close(gt_process_runtime_t *r) {
    if (!r) {
        return GT_EXT_CLOSE_ERROR;
    }
    gt__process_runtime_stop(r);
    return gt__process_runtime_step(r) == GT_PROCESS_RUNTIME_CLOSED ? GT_EXT_RELEASED
                                                                    : GT_EXT_DRAINING;
}

gt_status_t gt__process_runtime_destroy(gt_process_runtime_t **out) {
    if (!out || !*out || (*out)->info.phase != GT_PROCESS_RUNTIME_CLOSED) {
        return GT_ERR_INVALID_ARG;
    }
    gt_process_runtime_t *r = *out;
    *out = NULL;
    gt_status_t status = r->info.cleanup_error;
    if (r->executable >= 0 && close(r->executable)) {
        status = GT_ERR_IO;
    }
    if (r->parent >= 0 && close(r->parent)) {
        status = GT_ERR_IO;
    }
    if (r->sdk_snapshot && gt__package_snapshot_destroy(&r->sdk_snapshot) != GT_PACKAGE_OK) {
        status = GT_ERR_IO;
    }
    if (gt__package_checked_destroy(&r->package) != GT_PACKAGE_OK) {
        status = GT_ERR_IO;
    }
    gt__bridge_decoder_free(r->decoder);
    gt__plugin_memory_free(NULL, r, sizeof(*r));
    return status;
}
