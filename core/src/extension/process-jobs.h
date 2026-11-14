/* Private serial owner of bounded, explicitly mapped business processes. */
#ifndef GT_PROCESS_JOBS_H
#define GT_PROCESS_JOBS_H
#include "extension/process-service.h"
#include "extension/runtime-resource.h"
/* Private pending status; never returned across the public C ABI. */
#define GT_PROCESS_JOBS_WAIT ((gt_status_t) - 1)
#define GT_PROCESS_JOBS_MAX 8u
#define GT_PROCESS_JOBS_IO_MAX 3072u
typedef struct gt_process_jobs gt_process_jobs_t;

typedef struct {
    const gt_ext_process_program_mapping_t *programs;
    size_t program_count;
    const char *const *environment;
    size_t environment_count;
    uint32_t lifetime_ms, grace_ms;
    uint64_t output_bytes;
} gt_process_jobs_config_t;

typedef struct {
    gt_ext_process_info_t process;
    const char *reason; /* none / deadline / output-limit */
} gt_process_job_info_t;

/* Borrows unused roots/Session; caller keeps them until destroy. No spawn.
 * Copies native mappings and selected environment, never child-provided env. */
gt_status_t gt__process_jobs_open(gt_ext_session_t *,
                                  gt_runtime_resources_t *,
                                  const gt_package_checked_t *,
                                  const gt_process_jobs_config_t *,
                                  gt_process_jobs_t **);
bool gt__process_jobs_matches(const gt_process_jobs_t *, gt_ext_session_t *);
gt_status_t gt__process_jobs_spawn(gt_process_jobs_t *,
                                   const char *,
                                   const char *,
                                   const char *const *,
                                   size_t,
                                   uint64_t *,
                                   const char **);
gt_ext_process_io_t gt__process_jobs_read(gt_process_jobs_t *, uint64_t, bool, void *, size_t);
gt_ext_process_io_t gt__process_jobs_write(gt_process_jobs_t *, uint64_t, const void *, size_t);
gt_status_t gt__process_jobs_input_close(gt_process_jobs_t *, uint64_t);
gt_status_t gt__process_jobs_signal(gt_process_jobs_t *, uint64_t, bool);
gt_status_t gt__process_jobs_poll(gt_process_jobs_t *, uint64_t, gt_process_job_info_t *);
/* WAIT while running; no implied signal or success before actual waitpid. */
gt_status_t gt__process_jobs_release(gt_process_jobs_t *, uint64_t);
gt_status_t gt__process_jobs_step(gt_process_jobs_t *);
void gt__process_jobs_stop(gt_process_jobs_t *);
/* First group-signal errno after a known leader exit; distinct from reap. */
int gt__process_jobs_group_error(const gt_process_jobs_t *);
/* Refuses live processes; stop/step/reap first. */
gt_status_t gt__process_jobs_destroy(gt_process_jobs_t **);
#endif
