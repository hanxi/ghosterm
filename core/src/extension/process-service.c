#define _POSIX_C_SOURCE 200809L
#include "extension/process-service.h"
#include <stdlib.h>
#include <string.h>

struct gt_ext_process_service {
    gt_ext_session_t *session;
    gt_ext_lease_t lease;
    size_t program_count;
    char programs[GT_EXT_PROCESS_PROGRAMS_MAX][129];
    size_t mapping_count;
    char mapping_names[GT_EXT_PROCESS_PROGRAMS_MAX][129];
    char executables[GT_EXT_PROCESS_PROGRAMS_MAX][4097];
    char verified_versions[GT_EXT_PROCESS_PROGRAMS_MAX][129];
};

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
                sizeof(service->verified_versions[0])) {
            free(service);
            return GT_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(mappings[i].name, mappings[j].name)) {
                free(service);
                return GT_ERR_INVALID_ARG;
            }
        }
        strcpy(service->executables[i], mappings[i].executable);
        strcpy(service->verified_versions[i], mappings[i].verified_version);
        /* Mapping order is retained; names share the same indexed table. */
    }
    /* Reuse the program table tail for mapped names, preserving separate
     * package scope by requiring every mapping to be in that scope below. */
    service->mapping_count = mapping_count;
    for (size_t i = 0; i < mapping_count; i++) {
        strcpy(service->mapping_names[i], mappings[i].name);
    }
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
        mappings[i] = (gt_ext_process_program_mapping_t){
            service->mapping_names[i], service->executables[i], service->verified_versions[i]};
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
    free(*service);
    *service = NULL;
    return GT_OK;
}
