/* Trusted native runtime worker. Only stop is cross-thread; no caller may race
 * adoption/destroy. Module bytes are initialization data, never terminal IO.
 * Platform bindings enter through ghosterm.h. Not an OS/same-user sandbox. */
#ifndef GHOSTERM_EXTENSION_RUNTIME_H
#define GHOSTERM_EXTENSION_RUNTIME_H
#include "ghosterm/extension_rpc.h"
#include "ghosterm/extension_package.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_RUNTIME_MODULES_MAX 64u
#define GT_EXT_RUNTIME_MODULE_MAX 131072u
#define GT_EXT_RUNTIME_SOURCE_MAX 1048576u
#define GT_EXT_RUNTIME_ROOTS_MAX 8u
#define GT_EXT_RUNTIME_PROCESS_PROGRAMS_MAX 16u
#define GT_EXT_RUNTIME_PROCESS_ENV_MAX 32u
#define GT_EXT_RUNTIME_PROCESS_PREFIX_MAX 16u
#define GT_EXT_RUNTIME_PROCESS_PREFIX_BYTES_MAX 16384u
#define GT_EXT_RUNTIME_PROCESS_LIFETIME_MS_MAX 3600000u
#define GT_EXT_RUNTIME_PROCESS_OUTPUT_BYTES_MAX (256u * 1024u * 1024u)
typedef struct gt_ext_runtime gt_ext_runtime_t;

typedef enum {
    GT_EXT_RUNTIME_CREATED,
    GT_EXT_RUNTIME_STARTING,
    GT_EXT_RUNTIME_ACTIVE,
    GT_EXT_RUNTIME_FAILED,
    GT_EXT_RUNTIME_STOPPED,
    GT_EXT_RUNTIME_CLOSED
} gt_ext_runtime_phase_t;

typedef enum {
    GT_EXT_RUNTIME_NONE,
    GT_EXT_RUNTIME_SCRIPT,
    GT_EXT_RUNTIME_MEMORY,
    GT_EXT_RUNTIME_CPU,
    GT_EXT_RUNTIME_CANCELLED,
    GT_EXT_RUNTIME_JOBS,
    GT_EXT_RUNTIME_PROTOCOL,
    GT_EXT_RUNTIME_IO,
    GT_EXT_RUNTIME_EXPIRED
} gt_ext_runtime_error_t;

typedef struct {
    const char *name;
    const void *source;
    size_t size;
    char sha256[65];
} gt_ext_runtime_module_t;

/* Native-authorized directory scope. IDs are plugin-visible labels, not paths.
 * Caller validates ACL/mount policy. Core copies ID and CLOEXEC fd duplicate;
 * does not consume the caller fd. Limit 1..16 MiB; <=8 unique ASCII IDs. */
typedef struct {
    const char *id;
    int directory_fd;
    uint64_t byte_limit;
} gt_ext_runtime_root_t;

/* Raw invocation data, not shell or decoded/schema-validated options. Native
 * caller supplies the selected canonical command. Copied before adoption;
 * <=32 UTF-8 arguments, <=4096 bytes each, <=16384 bytes including NULs. */
typedef struct {
    const char *command;
    const char *const *argv;
    size_t argc;
} gt_ext_invocation_t;

/* Trusted Host resolution for a manifest-scoped process program. The Host
 * supplies its verified version and SHA-256; Core verifies the file digest and
 * rechecks file identity before spawn. No PATH lookup. Optional Host prefix
 * arguments are copied and placed after caller argv[0], before caller argv[1..].
 * <=16 arguments, <=4096 bytes each, <=16384 bytes including NULs; the final
 * combined argv must also fit the existing process argument count/byte limits.
 * Prefix values are trusted Host configuration, never plugin-supplied authority.
 * Executable pinning does not validate scripts/dependencies named in arguments. */
typedef struct {
    const char *name;
    const char *executable;
    const char *verified_version;
    const char *verified_sha256;
    const char *const *prefix_argv;
    size_t prefix_argc;
} gt_ext_process_program_mapping_t;

/* Explicit environment entry selected by the Host. Values are copied by the
 * Runtime and passed only to children whose Session grant is live. */
typedef struct {
    const char *name;
    const char *value;
} gt_ext_process_environment_t;

/* Explicit native approval/configuration for a checked Node/Exec Executor.
 * No automatic runtime discovery/download or same-user OS sandbox. Parent fd
 * and absolute path must identify the same private (0700) directory.
 * Node requires an installed pinned executable and a native-approved Host SDK
 * directory plus independently trusted catalog digest. Core captures that tree
 * read-only before adoption; <=1024 files / 4096 entries / 64 MiB, no plugin code
 * executes during construction. Caller retains all input fds/strings. Only the
 * explicitly selected environment reaches the main Node/Exec process; business
 * children use the separate process_environment below. Zero budgets choose
 * finite defaults. This does not enforce heap/stack/CPU budgets for OS processes.
 * SDK fields are Node-only; Exec sets node_sdk_directory_fd=-1. */
typedef struct {
    int stage_parent_fd;
    const char *stage_parent_path;
    bool execution_authorized;
    const char *node_executable, *node_sha256;
    int node_sdk_directory_fd;
    const char *node_sdk_catalog_sha256;
    const gt_ext_process_environment_t *environment;
    size_t environment_count;
    uint32_t lifetime_ms, stop_grace_ms;
    uint64_t stdout_bytes, stderr_bytes;
} gt_ext_runtime_execution_t;

typedef struct {
    const gt_ext_runtime_module_t *modules;
    size_t module_count;
    const char *entry;
    gt_ext_role_t role;
    uint64_t memory_bytes, stack_bytes;
    uint32_t cpu_ms, jobs_per_step, activation_ms;
    const gt_ext_runtime_root_t *roots;
    size_t root_count;
    const gt_ext_process_program_mapping_t *process_programs;
    size_t process_program_count;
    const gt_ext_process_environment_t *process_environment;
    size_t process_environment_count;
    /* Per-child wall-clock maximum and aggregate bytes returned by child stdout
     * plus stderr for this Runtime. Embedded defaults: 10 minutes / 64 MiB,
     * maximum 256 MiB. External defaults: 60 seconds / 1 MiB, maximum 64 MiB.
     * Total execution/transport budgets are separate. Nonzero values are Host
     * policy, not plugin-controlled. */
    uint32_t process_lifetime_ms;
    uint64_t process_output_bytes;
    const gt_ext_invocation_t *invocation;
    /* Package-only, Executor-only. NULL keeps the embedded Portable runtime.
     * Required for Node/Exec; never silently changes backend. Nonzero embedded
     * heap/stack/CPU/jobs budgets with this config are UNSUPPORTED. */
    const gt_ext_runtime_execution_t *execution;
} gt_ext_runtime_config_t;

typedef struct {
    gt_ext_runtime_phase_t phase;
    gt_ext_runtime_error_t error;
    gt_status_t cleanup_error;
    gt_ext_session_end_t session_end;
    gt_status_t session_error;
    bool dispose_error, engine_released;
    /* Embedded engine counters; process mode has no embedded jobs/modules.
     * Zero here does not claim zero OS process/module memory. */
    uint64_t steps, jobs, module_bytes;
    /* Zero for Portable. Child exit is an OS fact, not a business receipt.
     * group_signal_error is a retained errno, distinct from actual leader reap. */
    bool external_process, activated, child_exited, probe_verified;
    gt_status_t execution_error;
    int exit_code, term_signal, group_signal_error;
    uint64_t stdout_bytes, stderr_bytes;
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
gt_status_t gt_ext_runtime_open(gt_ext_session_t **session,
                                const gt_ext_runtime_config_t *,
                                gt_ext_runtime_t **);
/* Uses CHECKED fixed bytes and the built-in SDK. No caller module/entry override.
 * Verifies negotiated package identity and required grants. associated is a
 * trusted native command association decision, not plugin-controlled consent.
 * Retains package bytes, consumes Session only on success. Stopped owners refuse.
 * Node/Exec additionally require execution configuration, explicit launch
 * approval, actual runtime/SDK identity and a live same-package Executor grant.
 * Caller still enforces publisher/source policy before executing any package. */
gt_status_t gt_ext_runtime_open_package(gt_ext_session_t **session,
                                        gt_ext_package_t *,
                                        const gt_ext_runtime_config_t *,
                                        bool associated,
                                        gt_ext_runtime_t **);
/* Owner drives zero-wait native IO and bounded JS work; never call on UI thread.
 * Business outcomes remain separate from runtime lifecycle/error observations. */
gt_ext_runtime_phase_t gt_ext_runtime_step(gt_ext_runtime_t *);
bool gt_ext_runtime_info(const gt_ext_runtime_t *, gt_ext_runtime_info_t *);
/* Explicit native view-registration service, not proof of rendering. Enable
 * once before the first step, Host only; table=1, diff=2, task=4. Package mode
 * also requires the selected invocation's declared views. Live Session grants
 * remain mandatory. No process/Executor UI service or implicit fallback.
 * <=8 live registrations. Handles are opaque and never reused by this owner.
 * APIs run on the same non-UI Runtime worker. JS-off returns UNSUPPORTED. */
gt_status_t gt_ext_runtime_views_enable(gt_ext_runtime_t *, uint32_t kinds);
/* Borrowed UTF-8 JSON <=8192 bytes: {instanceId,epoch,revision,views:[
 * {handle,viewId,kind,methods:{describe,read[,cancel]}}]}. Counters are uint64
 * decimal strings. Copy before any mutating call/step/close. Failure zeroes
 * outputs; stopped/revoked owners never expose stale view registrations. */
gt_status_t gt_ext_runtime_views_snapshot(gt_ext_runtime_t *, const char **json, size_t *size);
/* A Native close invalidates the corresponding JS handle. NOT_FOUND includes
 * stale/foreign handles; closing a view does not cancel Executor business. */
gt_status_t gt_ext_runtime_view_close(gt_ext_runtime_t *, const char *handle);
/* The transport child adopted with Session, independent of any Executor or
 * business process reported by runtime_info. Final snapshot survives internal
 * Session destruction at close, until Runtime destroy. Snapshot only, no IO;
 * non-process transport returns UNSUPPORTED and zeroes *out. */
gt_status_t gt_ext_runtime_transport_process_info(const gt_ext_runtime_t *,
                                                  gt_ext_transport_process_info_t *);
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
