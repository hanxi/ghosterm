/* Private POSIX Node/Exec controller. Original non-UI owner; only stop may race
 * step. Trusted native launch approval is separate from business process.spawn.
 * No App enable, installation, runtime download, OS sandbox or recovery replay. */
#ifndef GT_PRIVATE_PROCESS_RUNTIME_H
#define GT_PRIVATE_PROCESS_RUNTIME_H
#include "extension/package-check.h"
#include "ghosterm/extension_runtime.h"
#include "extension/runtime-resource.h"
#include "extension/process-jobs.h"
typedef struct gt_process_runtime gt_process_runtime_t;

typedef enum {
    GT_PROCESS_RUNTIME_CREATED,
    GT_PROCESS_RUNTIME_STAGING,
    GT_PROCESS_RUNTIME_PROBING,
    GT_PROCESS_RUNTIME_STARTING,
    GT_PROCESS_RUNTIME_ACTIVE,
    GT_PROCESS_RUNTIME_STOPPING,
    GT_PROCESS_RUNTIME_CLOSED
} gt_process_runtime_phase_t;

typedef struct {
    int stage_parent_fd;
    const char *stage_parent_path; /* absolute, must match fd; private effective UID */
    bool execution_authorized, command_associated;
    const char *node_executable, *node_sha256; /* canonical absolute regular file */
    const char *sdk_catalog_sha256;            /* independently trusted host SDK bytes */
    const gt_ext_invocation_t *invocation;
    const char *const *environment;
    size_t environment_count;
    const gt_runtime_root_t *roots; /* explicit native authorization; copied fds */
    size_t root_count;
    gt_process_jobs_config_t process; /* native selected mappings/environment/budgets */
    uint32_t activation_ms, lifetime_ms, stop_grace_ms;
    uint64_t stdout_bytes, stderr_bytes;
} gt_process_runtime_config_t;

typedef struct {
    gt_process_runtime_phase_t phase;
    gt_status_t error, cleanup_error;
    gt_ext_session_end_t session_end;
    bool activated, child_exited, probe_verified;
    int exit_code, term_signal;
    int group_signal_error; /* first errno after known executor/business leader exit */
    uint64_t stdout_bytes, stderr_bytes;
} gt_process_runtime_info_t;

/* Success consumes Session and (Node only) SDK physical snapshot; retains fixed
 * checked package. Constructor validates identity/grants/command arguments and
 * copies config, opens trusted executable but starts no process/writes no files.
 * Failure retains caller ownership and issues no execution/association grant.
 * SDK snapshot must contain node/runtime/bootstrap.mjs. Exec needs no SDK.
 * Environment is explicit, no inherited ambient variables; loader injection
 * variables are rejected. Version probe always receives an empty environment. */
gt_status_t gt__process_runtime_open(gt_ext_session_t **,
                                     gt_package_checked_t *,
                                     gt_package_snapshot_t **sdk,
                                     const gt_process_runtime_config_t *,
                                     gt_process_runtime_t **);
/* Bounded staging/stdio quantum and zero-wait Session IO. Filesystem hashing /
 * fork/exec can still incur native latency; no UI-thread guarantee. Activation,
 * total lifetime and output have finite budgets. Revoke before TERM/KILL;
 * resources/staging survive until actual child exit/reap. Known exit code and
 * signal stay separate from unknown business effects and cleanup errors. */
gt_process_runtime_phase_t gt__process_runtime_step(gt_process_runtime_t *);
void gt__process_runtime_stop(gt_process_runtime_t *);
bool gt__process_runtime_info(const gt_process_runtime_t *, gt_process_runtime_info_t *);
gt_ext_close_t gt__process_runtime_close(gt_process_runtime_t *);
gt_status_t gt__process_runtime_destroy(gt_process_runtime_t **);
#endif
