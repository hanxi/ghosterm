/* Private Node/Exec prerequisite: fixed checked bytes -> owned runtime files.
 * C11 + POSIX, single non-UI owner, no launch/installation/authorization grant.
 * This is not a sandbox against a same-UID process: Node/Exec require a separate
 * trusted-runtime policy. Never expose a partially written tree to a child. */
#ifndef GT_PRIVATE_PACKAGE_STAGE_H
#define GT_PRIVATE_PACKAGE_STAGE_H
#include "extension/package-check.h"

typedef struct gt_package_stage gt_package_stage_t;

typedef enum {
    GT_STAGE_OK,
    GT_STAGE_WAIT,
    GT_STAGE_INVALID,
    GT_STAGE_UNSUPPORTED,
    GT_STAGE_LIMIT,
    GT_STAGE_NOMEM,
    GT_STAGE_IO,
    GT_STAGE_CHANGED,
    GT_STAGE_CANCELLED
} gt_package_stage_status_t;

/* parent_fd is a borrowed, explicitly selected staging directory owned by the
 * effective UID with no group/other permissions. Retained fd, never ambient cwd
 * or a path reopened later. byte_limit is explicit, 1..64 MiB; no overwrite,
 * links or existing-tree adoption. Only checked executor node/exec is accepted.
 * Retains checked and duplicates parent. Allocator/stop contexts outlive destroy.
 * Failed open clears out and has no filesystem side effects. */
gt_package_stage_status_t gt__package_stage_open(gt_package_checked_t *,
                                                 int parent_fd,
                                                 uint64_t byte_limit,
                                                 const atomic_bool *stop,
                                                 const gt_allocator_t *,
                                                 gt_package_stage_t **out);
/* Host-owned SDK tree: canonical length/digest-framed physical catalog hash,
 * including the build manifest. Caller supplies an independently trusted hash.
 * This is content identity, not publisher authentication or an execution grant.
 * Successful open alone consumes *snapshot. No plugin metadata is fabricated. */
bool gt__package_stage_catalog_digest(const gt_package_snapshot_t *, char digest[65]);
gt_package_stage_status_t gt__package_stage_open_host(gt_package_snapshot_t **,
                                                      const char *trusted_digest,
                                                      int parent_fd,
                                                      const atomic_bool *stop,
                                                      const gt_allocator_t *,
                                                      gt_package_stage_t **out);
/* At most one 16 KiB write or one final directory chmod per step, plus a bounded
 * path walk. Errors/cancellation sticky; destroy cleans partial files. No hard
 * filesystem latency guarantee. Files 0400, selected Exec entry 0500, directories
 * 0500 on ready. Complete physical catalog including metadata/licenses is copied;
 * no implicit SDK injection, dependency fetch, runtime selection or execution. */
gt_package_stage_status_t gt__package_stage_step(gt_package_stage_t *);
/* Ready only: borrowed directory fd and root basename under retained parent.
 * Strings borrowed until destroy. Output cleared on failure. Does not prove a
 * hostile child left contents unchanged; callers own execution identity checks. */
gt_package_stage_status_t gt__package_stage_view(gt_package_stage_t *,
                                                 int *directory_fd,
                                                 const char **name,
                                                 const char **entry);
/* Caller must stop/reap all children first. Identity-checked recorded entries
 * only; no recursive traversal/deletion of unknown names or replacements. On
 * cleanup failure consumes owner but explicitly reports IO/CHANGED, possibly
 * leaving files for inspection. Does not retry close or unlink foreign objects. */
gt_package_stage_status_t gt__package_stage_destroy(gt_package_stage_t **);
#endif
