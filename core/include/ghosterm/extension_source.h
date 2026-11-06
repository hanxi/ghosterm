/* Trusted native scoped regular-file producer. One serial non-UI owner; only
 * Session stop is cross-thread. Caller authorizes an opened root fd, including
 * ACL/mount policy. No implicit cwd, shell/tilde/env expansion or symlink
 * fallback. This is not an OS/same-uid sandbox or atomic filesystem snapshot. */
#ifndef GHOSTERM_EXTENSION_SOURCE_H
#define GHOSTERM_EXTENSION_SOURCE_H
#include "ghosterm/extension_resource.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_SOURCE_LIMIT 16u
typedef struct gt_ext_source gt_ext_source_t;

typedef enum {
    GT_EXT_SOURCE_OK,
    GT_EXT_SOURCE_WAIT,
    GT_EXT_SOURCE_INVALID,
    GT_EXT_SOURCE_DENIED,
    GT_EXT_SOURCE_UNSUPPORTED,
    GT_EXT_SOURCE_IO,
    GT_EXT_SOURCE_LIMIT_REACHED,
    GT_EXT_SOURCE_CHANGED,
    GT_EXT_SOURCE_CANCELLED,
    GT_EXT_SOURCE_BUSY,
    GT_EXT_SOURCE_UNKNOWN,
    GT_EXT_SOURCE_CLOSED,
    GT_EXT_SOURCE_NOT_FOUND
} gt_ext_source_status_t;

typedef enum {
    GT_EXT_SOURCE_HASHING,
    GT_EXT_SOURCE_READY,
    GT_EXT_SOURCE_SENDING,
    GT_EXT_SOURCE_RECEIPT,
    GT_EXT_SOURCE_CANCELLING,
    GT_EXT_SOURCE_SUCCEEDED,
    GT_EXT_SOURCE_FAILED,
    GT_EXT_SOURCE_STOPPED,
    GT_EXT_SOURCE_UNCERTAIN
} gt_ext_source_phase_t;

typedef struct {
    int root_fd;
    const char *relative_path, *mime;
    uint64_t byte_limit;
} gt_ext_source_config_t;

/* No transfer handle escapes: Source owns its resource result/forget lifecycle.
 * Wire IDs are correlation metadata, not a native permission or owning ref. */
typedef struct {
    gt_ext_source_phase_t phase;
    gt_ext_source_status_t error, cleanup_error;
    bool closed, has_transfer;
    uint64_t hashed, read_bytes, accepted;
    gt_ext_resource_descriptor_t descriptor;
    gt_ext_transfer_state_t transfer_state;
    bool confirmed, cancel_requested;
    uint64_t transfer_accepted, generation;
    char resource_id[33], code[33];
} gt_ext_source_info_t;

/* Requires resources-enabled Session, original lease, file.readScoped and
 * resource.receive grants. Invalid/unsupported/denied construction performs no
 * file reads. On success copies path/metadata and owns CLOEXEC fd duplicates,
 * with at most 16 sources per Session. Failure clears output, keeps caller fd.
 * Relative ./ is allowed; .., absolute/control/non-UTF8 names, >32 components
 * and special files are rejected. Reachable hard links are allowed.
 * Each source pins Session through successful destroy, including after close.
 * Session close may release transport but remains DRAINING until sources die. */
gt_ext_source_status_t gt_ext_source_open(gt_ext_session_t *,
                                          const gt_ext_lease_t *,
                                          const gt_ext_source_config_t *,
                                          gt_ext_source_t **);
/* One <=16 KiB pread or bounded producer write per step. Drive Session step
 * separately on the same owner; ordinary disk IO has no hard latency promise.
 * First pass checks real EOF/metadata before READY; send is explicit. Second
 * pass checks EOF/metadata/digest. Success needs receiver publication receipt. */
gt_ext_source_phase_t gt_ext_source_step(gt_ext_source_t *);
gt_ext_source_status_t gt_ext_source_send(gt_ext_source_t *);
void gt_ext_source_cancel(gt_ext_source_t *);
bool gt_ext_source_info(const gt_ext_source_t *, gt_ext_source_info_t *);
/* Close stops reads and closes each owned fd once. WAIT retains the source:
 * continue Session step, or explicitly stop/close Session to settle UNKNOWN.
 * IO is reported with owner retained, never retrying a consumed numeric fd.
 * cleanup_error is separate from original source/transfer failure facts.
 * Destroy closes, forgets its terminal observation, consumes *owner only on OK
 * and finally unpins Session. Source info POD remains a caller-owned copy. */
gt_ext_source_status_t gt_ext_source_close(gt_ext_source_t *);
gt_ext_source_status_t gt_ext_source_destroy(gt_ext_source_t **);
#ifdef __cplusplus
}
#endif
#endif
