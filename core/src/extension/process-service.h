/* Private policy layer between checked package metadata and POSIX children. */
#ifndef GT_EXTENSION_PROCESS_SERVICE_H
#define GT_EXTENSION_PROCESS_SERVICE_H
#include "extension/package-check.h"
#include "extension/process-owner.h"
#include "ghosterm/extension_runtime.h"

#define GT_EXT_PROCESS_PROGRAMS_MAX 16u

typedef struct {
    const char *name;
    const char *executable;
} gt_ext_process_program_mapping_t;

typedef struct gt_ext_process_service gt_ext_process_service_t;

/* Shared policy decision used by the live Session path and focused tests. */
gt_status_t gt__ext_process_service_resolve(const gt_package_check_info_t *,
                                            const gt_ext_process_program_mapping_t *,
                                            size_t,
                                            gt_ext_access_t,
                                            const char *,
                                            const char **);

/* Copies the checked package's Executor program scope and trusted Host
 * executable mappings. Construction does not spawn or authorize a child. */
gt_status_t gt__ext_process_service_open(gt_ext_session_t *,
                                         const gt_ext_lease_t *,
                                         const gt_package_checked_t *,
                                         const gt_ext_process_program_mapping_t *,
                                         size_t,
                                         gt_ext_process_service_t **);
/* Every spawn rechecks the live Session grant and package program scope. The
 * caller supplies an already authorized cwd fd and explicit environment. */
gt_status_t gt__ext_process_service_spawn(gt_ext_process_service_t *,
                                          const char *,
                                          const char *const *,
                                          size_t,
                                          int,
                                          const char *const *,
                                          size_t,
                                          gt_ext_process_t **);
gt_status_t gt__ext_process_service_destroy(gt_ext_process_service_t **);
#endif
