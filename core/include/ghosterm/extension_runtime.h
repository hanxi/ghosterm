/* Trusted native runtime worker. Only stop is cross-thread; no caller may race
 * adoption/destroy. Module bytes are initialization data, never terminal IO.
 * Platform bindings enter through ghosterm.h. Not an OS/same-user sandbox. */
#ifndef GHOSTERM_EXTENSION_RUNTIME_H
#define GHOSTERM_EXTENSION_RUNTIME_H
#include "ghosterm/extension_rpc.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_RUNTIME_MODULES_MAX 64u
#define GT_EXT_RUNTIME_MODULE_MAX 131072u
#define GT_EXT_RUNTIME_SOURCE_MAX 1048576u
#define GT_EXT_RUNTIME_ROOTS_MAX 8u
typedef struct gt_ext_runtime gt_ext_runtime_t;
typedef enum {GT_EXT_RUNTIME_CREATED,GT_EXT_RUNTIME_STARTING,GT_EXT_RUNTIME_ACTIVE,GT_EXT_RUNTIME_FAILED,GT_EXT_RUNTIME_STOPPED,GT_EXT_RUNTIME_CLOSED} gt_ext_runtime_phase_t;
typedef enum {GT_EXT_RUNTIME_NONE,GT_EXT_RUNTIME_SCRIPT,GT_EXT_RUNTIME_MEMORY,GT_EXT_RUNTIME_CPU,GT_EXT_RUNTIME_CANCELLED,GT_EXT_RUNTIME_JOBS,GT_EXT_RUNTIME_PROTOCOL,GT_EXT_RUNTIME_IO,GT_EXT_RUNTIME_EXPIRED} gt_ext_runtime_error_t;
typedef struct {const char *name;const void *source;size_t size;char sha256[65];} gt_ext_runtime_module_t;
/* Native-authorized directory scope. IDs are plugin-visible labels, not paths.
 * Caller validates ACL/mount policy. Core copies ID and CLOEXEC fd duplicate;
 * does not consume the caller fd. Limit 1..16 MiB; <=8 unique ASCII IDs. */
typedef struct {const char *id;int directory_fd;uint64_t byte_limit;} gt_ext_runtime_root_t;
typedef struct {
 const gt_ext_runtime_module_t *modules;size_t module_count;const char *entry;
 gt_ext_role_t role;uint64_t memory_bytes,stack_bytes;uint32_t cpu_ms,jobs_per_step,activation_ms;
 const gt_ext_runtime_root_t *roots;size_t root_count;
} gt_ext_runtime_config_t;
typedef struct {
 gt_ext_runtime_phase_t phase;gt_ext_runtime_error_t error;gt_status_t cleanup_error;
 gt_ext_session_end_t session_end;gt_status_t session_error;bool dispose_error,engine_released;
 uint64_t steps,jobs,module_bytes;
} gt_ext_runtime_info_t;
/* Local build fact, not package compatibility/permission or activation success.
 * OFF is UNSUPPORTED. No implicit backend, install, network or gateway fallback. */
gt_status_t gt_ext_runtime_support(void);
/* Copies all modules/config and consumes *session only on success. Requires
 * ACTIVE requests-enabled same-role exclusive Session, no live Source, outgoing
 * request observations, taken unfinished tickets or resource producer handles.
 * Untaken peer requests remain owned by Session and may arrive before adoption.
 * Caller verifies package/SDK snapshot matching negotiated digests and required
 * services. Module digests prove consistency, not publisher identity/grants.
 * ASCII plugin:/ / sdk:/ module IDs, canonical in-root relative imports; JS text
 * only. Constructor executes no plugin. Context exposes RPC, IDs, side,
 * cancellation and, for a resource Session, a bounded native ResourceBroker.
 * Roots authorize only scoped source publication, never ambient filesystem IO.
 * No roots means receive-only resources; no Store means no resource service.
 * Process/UI services and App installation follow.
 * 0 limits select 16 MiB heap / 256 KiB stack / 25ms CPU / 32 jobs / 5s activation.
 * Maximum 128 MiB / 1 MiB / 1s / 128 jobs / 60s. Heap excludes source/native IO. */
gt_status_t gt_ext_runtime_open(gt_ext_session_t **session,const gt_ext_runtime_config_t *,gt_ext_runtime_t **);
/* Owner drives zero-wait native IO and bounded JS work; never call on UI thread.
 * Business outcomes remain separate from runtime lifecycle/error observations. */
gt_ext_runtime_phase_t gt_ext_runtime_step(gt_ext_runtime_t *);
bool gt_ext_runtime_info(const gt_ext_runtime_t *,gt_ext_runtime_info_t *);
/* Only sets cancellation; caller schedules/wakes owner. Stop may not race destroy. */
void gt_ext_runtime_stop(gt_ext_runtime_t *);
/* Revokes first, bounds cooperative dispose, destroys engine before native close.
 * DRAINING/error retain owner; retry on same worker. Destroy only after RELEASED.
 * Info copies contain no exception text or borrowed handles; failure zeros out. */
gt_ext_close_t gt_ext_runtime_close(gt_ext_runtime_t *);
gt_status_t gt_ext_runtime_destroy(gt_ext_runtime_t **);
#ifdef __cplusplus
}
#endif
#endif
