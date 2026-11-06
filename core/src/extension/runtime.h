/* Private trusted-plugin runtime, original serial non-UI owner. Only stop may
 * race step/close; no caller may race adoption/destroy. Not an OS sandbox. */
#ifndef GT_BRIDGE_RUNTIME_H
#define GT_BRIDGE_RUNTIME_H
#include "ghosterm/extension_runtime.h"
#include "extension/runtime-resource.h"
#include "extension/package-check.h"
typedef struct gt_bridge_runtime gt_bridge_runtime_t;
typedef enum {GT_RUNTIME_CREATED,GT_RUNTIME_STARTING,GT_RUNTIME_ACTIVE,GT_RUNTIME_FAILED,GT_RUNTIME_STOPPED,GT_RUNTIME_CLOSED} gt_runtime_phase_t;
typedef enum {GT_RUNTIME_NONE,GT_RUNTIME_SCRIPT,GT_RUNTIME_MEMORY,GT_RUNTIME_CPU,GT_RUNTIME_CANCELLED,GT_RUNTIME_JOBS,GT_RUNTIME_PROTOCOL,GT_RUNTIME_IO,GT_RUNTIME_EXPIRED} gt_runtime_error_t;
typedef struct {const char *name;const void *source;size_t size;char sha256[65];} gt_runtime_module_t;
typedef struct {
 const gt_runtime_module_t *modules;size_t module_count;const char *entry;
 gt_ext_role_t role;size_t memory_bytes,stack_bytes;uint32_t cpu_ms,jobs_per_step,activation_ms;
 const gt_runtime_root_t *roots;size_t root_count;
 const gt_ext_invocation_t *invocation;
} gt_runtime_config_t;
typedef struct {gt_runtime_phase_t phase;gt_runtime_error_t error;gt_status_t cleanup_error;gt_ext_session_end_t session_end;gt_status_t session_error;bool dispose_error,engine_released;uint64_t steps,jobs;size_t module_bytes;} gt_runtime_info_t;
/* Requires ACTIVE, requests-enabled, exclusive Session with no source or prior
 * request owners. Trusted caller supplies an already verified package snapshot
 * matching negotiated package/side and copied module content digests. Digests
 * prove content consistency, not publisher identity/authorization. Only JS text
 * modules, 1..64, each <=128 KiB, total <=1 MiB. No filesystem/dynamic FFI loader.
 * Construction copies everything and executes no plugin. Success alone clears
 * *session. Default build has no engine and returns UNSUPPORTED, never success. */
gt_status_t gt__bridge_runtime_open(gt_ext_session_t **session,const gt_runtime_config_t *,gt_bridge_runtime_t **);
/* Package path: modules/module_count/entry must be empty. Retains fixed checked
 * bytes, uses only the embedded host SDK, binds side/id/version/digest and SDK
 * version to the admitted Session and verifies currently implemented required
 * services/scopes. Association is an explicit native command selection, not a
 * publisher/source grant. Constructor runs no plugin. Caller keeps its package
 * reference; success alone consumes *session. No filesystem loader or cache. */
gt_status_t gt__bridge_runtime_open_package(gt_ext_session_t **session,gt_package_checked_t *,
 const gt_runtime_config_t *,bool command_associated,gt_bridge_runtime_t **);
/* Native IO uses zero wait; execute/evaluate and each job share one CPU budget.
 * A nonempty queue at jobs_per_step is JOBS failure, not successful draining.
 * Limits govern JS; blocking native services still need independent budgets. */
gt_runtime_phase_t gt__bridge_runtime_step(gt_bridge_runtime_t *);
bool gt__bridge_runtime_info(const gt_bridge_runtime_t *,gt_runtime_info_t *);
void gt__bridge_runtime_stop(gt_bridge_runtime_t *);
/* Revoke before any optional JS dispose; do not wait for cooperative cleanup.
 * IO/DRAINING retain owner. Info never includes exception text/plugin data. */
gt_ext_close_t gt__bridge_runtime_close(gt_bridge_runtime_t *);
gt_status_t gt__bridge_runtime_destroy(gt_bridge_runtime_t **);
#endif
