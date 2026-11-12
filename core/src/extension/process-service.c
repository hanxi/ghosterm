#define _POSIX_C_SOURCE 200809L
#include "extension/process-service.h"
#include "base/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct gt_ext_process_service {
    gt_ext_session_t *session;
    gt_ext_lease_t lease;
    size_t program_count;
    char programs[GT_EXT_PROCESS_PROGRAMS_MAX][129];
    size_t mapping_count;
    char mapping_names[GT_EXT_PROCESS_PROGRAMS_MAX][129];
    char executables[GT_EXT_PROCESS_PROGRAMS_MAX][4097];
    char verified_versions[GT_EXT_PROCESS_PROGRAMS_MAX][129];
    char verified_sha256[GT_EXT_PROCESS_PROGRAMS_MAX][65];
    struct stat executable_identity[GT_EXT_PROCESS_PROGRAMS_MAX];
    int executable_fds[GT_EXT_PROCESS_PROGRAMS_MAX];
};

static bool stat_identity_equal(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_mtimensec == b->st_mtimensec && a->st_ctime == b->st_ctime &&
           a->st_ctimensec == b->st_ctimensec;
#else
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static bool valid_sha256(const char *digest) {
    if (!digest || strnlen(digest, 65) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        char c = digest[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static gt_status_t executable_digest(int fd, char digest[65], struct stat *identity) {
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || !(before.st_mode & 0111)) {
        return GT_ERR_AUTH;
    }
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    unsigned char buffer[32768];
    off_t offset = 0;
    for (;;) {
        ssize_t count = pread(fd, buffer, sizeof(buffer), offset);
        if (count > 0) {
            if (!gt__sha256_update(&hash, buffer, (size_t)count) ||
                offset > (off_t)(INT64_MAX - count)) {
                return GT_ERR_IO;
            }
            offset += count;
        } else if (!count) {
            break;
        } else if (errno != EINTR) {
            return GT_ERR_IO;
        }
    }
    if (fstat(fd, &after) || !stat_identity_equal(&before, &after) ||
        !gt__sha256_final(&hash, digest)) {
        return GT_ERR_AUTH;
    }
    if (identity) {
        *identity = after;
    }
    return GT_OK;
}

static bool valid_program_name(const char *name) {
    if (!name) {
        return false;
    }
    size_t n = strnlen(name, 129);
    if (!n || n > 128) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

gt_status_t gt__ext_process_service_resolve(const gt_package_check_info_t *info,
                                            const gt_ext_process_program_mapping_t *mappings,
                                            size_t mapping_count,
                                            gt_ext_access_t access,
                                            const char *program,
                                            const char **executable) {
    if (executable) {
        *executable = NULL;
    }
    if (!info || (mapping_count && !mappings) || !executable || !program ||
        mapping_count > GT_EXT_PROCESS_PROGRAMS_MAX) {
        return GT_ERR_INVALID_ARG;
    }
    if (access != GT_EXT_ACCESS_ALLOWED) {
        return GT_ERR_AUTH;
    }
    bool declared = false;
    for (size_t i = 0; i < info->process_program_count; i++) {
        if (!strcmp(program, info->process_programs[i])) {
            declared = true;
            break;
        }
    }
    if (!declared) {
        return GT_ERR_AUTH;
    }
    for (size_t i = 0; i < mapping_count; i++) {
        if (!strcmp(program, mappings[i].name)) {
            if (!mappings[i].executable || mappings[i].executable[0] != '/') {
                return GT_ERR_INVALID_ARG;
            }
            *executable = mappings[i].executable;
            return GT_OK;
        }
    }
    return GT_ERR_UNSUPPORTED;
}

gt_status_t gt__ext_process_service_open(gt_ext_session_t *session,
                                         const gt_ext_lease_t *lease,
                                         const gt_package_checked_t *package,
                                         const gt_ext_process_program_mapping_t *mappings,
                                         size_t mapping_count,
                                         gt_ext_process_service_t **out) {
    if (out) {
        *out = NULL;
    }
    gt_package_check_info_t info;
    if (!out || !session || !lease || !package || (mapping_count && !mappings) ||
        mapping_count > GT_EXT_PROCESS_PROGRAMS_MAX || !gt__package_checked_info(package, &info)) {
        return GT_ERR_INVALID_ARG;
    }
    if (!info.executor || !info.process_program_count ||
        info.process_program_count > GT_EXT_PROCESS_PROGRAMS_MAX) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_ext_process_service_t *service = calloc(1, sizeof(*service));
    if (!service) {
        return GT_ERR_IO;
    }
    service->session = session;
    service->lease = *lease;
    for (size_t i = 0; i < GT_EXT_PROCESS_PROGRAMS_MAX; i++) {
        service->executable_fds[i] = -1;
    }
    for (size_t i = 0; i < info.process_program_count; i++) {
        if (!valid_program_name(info.process_programs[i])) {
            free(service);
            return GT_ERR_PROTOCOL;
        }
        strcpy(service->programs[i], info.process_programs[i]);
    }
    service->program_count = info.process_program_count;
    for (size_t i = 0; i < mapping_count; i++) {
        if (!valid_program_name(mappings[i].name) || !mappings[i].executable ||
            mappings[i].executable[0] != '/' ||
            strnlen(mappings[i].executable, sizeof(service->executables[0])) >=
                sizeof(service->executables[0]) ||
            !mappings[i].verified_version || !mappings[i].verified_version[0] ||
            strnlen(mappings[i].verified_version, sizeof(service->verified_versions[0])) >=
                sizeof(service->verified_versions[0]) ||
            !valid_sha256(mappings[i].verified_sha256)) {
            gt__ext_process_service_destroy(&service);
            return GT_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(mappings[i].name, mappings[j].name)) {
                gt__ext_process_service_destroy(&service);
                return GT_ERR_INVALID_ARG;
            }
        }
        service->mapping_count = i + 1;
        strcpy(service->mapping_names[i], mappings[i].name);
        strcpy(service->executables[i], mappings[i].executable);
        strcpy(service->verified_versions[i], mappings[i].verified_version);
        strcpy(service->verified_sha256[i], mappings[i].verified_sha256);
        service->executable_fds[i] = open(mappings[i].executable, O_RDONLY | O_CLOEXEC);
        if (service->executable_fds[i] < 0) {
            gt__ext_process_service_destroy(&service);
            return GT_ERR_AUTH;
        }
        char actual_sha256[65];
        gt_status_t digest_status = executable_digest(
            service->executable_fds[i], actual_sha256, &service->executable_identity[i]);
        if (digest_status != GT_OK || strcmp(actual_sha256, mappings[i].verified_sha256)) {
            gt__ext_process_service_destroy(&service);
            return digest_status == GT_OK ? GT_ERR_AUTH : digest_status;
        }
        /* Mapping order is retained; names share the same indexed table. */
    }
    /* Reuse the program table tail for mapped names, preserving separate
     * package scope by requiring every mapping to be in that scope below. */
    *out = service;
    return GT_OK;
}

gt_status_t gt__ext_process_service_spawn(gt_ext_process_service_t *service,
                                          const char *program,
                                          const char *const *argv,
                                          size_t argc,
                                          int cwd_fd,
                                          const char *const *environment,
                                          size_t environment_count,
                                          gt_ext_process_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!service || !program || !out) {
        return GT_ERR_INVALID_ARG;
    }
    gt_package_check_info_t info = {0};
    info.executor = true;
    info.process_program_count = service->program_count;
    memcpy(info.process_programs, service->programs, sizeof(service->programs));
    gt_ext_process_program_mapping_t mappings[GT_EXT_PROCESS_PROGRAMS_MAX];
    for (size_t i = 0; i < service->mapping_count; i++) {
        mappings[i] = (gt_ext_process_program_mapping_t){service->mapping_names[i],
                                                         service->executables[i],
                                                         service->verified_versions[i],
                                                         service->verified_sha256[i]};
    }
    const char *executable = NULL;
    gt_status_t resolved = gt__ext_process_service_resolve(
        &info,
        mappings,
        service->mapping_count,
        gt_ext_session_access(service->session, &service->lease, "process.spawn"),
        program,
        &executable);
    if (resolved != GT_OK) {
        return resolved;
    }
    for (size_t i = 0; i < service->mapping_count; i++) {
        if (!strcmp(program, service->mapping_names[i])) {
            struct stat path_identity, fd_identity;
            if (stat(service->executables[i], &path_identity) ||
                fstat(service->executable_fds[i], &fd_identity) ||
                !stat_identity_equal(&service->executable_identity[i], &path_identity) ||
                !stat_identity_equal(&service->executable_identity[i], &fd_identity)) {
                return GT_ERR_AUTH;
            }
            return gt__ext_process_spawn(
                executable, argv, argc, cwd_fd, environment, environment_count, out);
        }
    }
    return GT_ERR_UNSUPPORTED;
}

gt_status_t gt__ext_process_service_destroy(gt_ext_process_service_t **service) {
    if (!service || !*service) {
        return GT_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < (*service)->mapping_count; i++) {
        if ((*service)->executable_fds[i] >= 0) {
            close((*service)->executable_fds[i]);
        }
    }
    free(*service);
    *service = NULL;
    return GT_OK;
}

const char *gt__ext_process_service_version(const gt_ext_process_service_t *service,
                                            const char *program) {
    if (!service || !program) {
        return NULL;
    }
    for (size_t i = 0; i < service->mapping_count; i++) {
        if (!strcmp(program, service->mapping_names[i])) {
            return service->verified_versions[i];
        }
    }
    return NULL;
}
