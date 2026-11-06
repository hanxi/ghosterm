/* Private scoped regular-file producer. One non-UI IO owner, no implicit cwd,
 * tilde/env/glob/shell expansion or symlink fallback. Caller supplies a root fd
 * selected/authorized by its native UI/CLI, not by a remote hostname or payload.
 * Native Session file.readScoped and resource.receive are additional gates.
 * Borrow Session through source destroy; drive its step separately, close all
 * sources before destroying Session. No fs snapshot/OS sandbox is promised. */
#ifndef GT_BRIDGE_SOURCE_H
#define GT_BRIDGE_SOURCE_H
#include "extension/transfer.h"
#define GT_BRIDGE_SOURCE_DEPTH 32u
#define GT_BRIDGE_SOURCE_PATH 4096u
#define GT_BRIDGE_SOURCE_QUANTUM 16384u
typedef struct gt_bridge_source gt_bridge_source_t;

typedef enum {
    GT_BRIDGE_SOURCE_OK,
    GT_BRIDGE_SOURCE_WAIT,
    GT_BRIDGE_SOURCE_INVALID,
    GT_BRIDGE_SOURCE_DENIED,
    GT_BRIDGE_SOURCE_UNSUPPORTED,
    GT_BRIDGE_SOURCE_IO,
    GT_BRIDGE_SOURCE_LIMIT,
    GT_BRIDGE_SOURCE_CHANGED,
    GT_BRIDGE_SOURCE_CANCELLED,
    GT_BRIDGE_SOURCE_BUSY,
    GT_BRIDGE_SOURCE_UNKNOWN,
    GT_BRIDGE_SOURCE_CLOSED,
    GT_BRIDGE_SOURCE_NOT_FOUND
} gt_bridge_source_status_t;

typedef enum {
    GT_BRIDGE_SOURCE_HASHING,
    GT_BRIDGE_SOURCE_READY,
    GT_BRIDGE_SOURCE_SENDING,
    GT_BRIDGE_SOURCE_RECEIPT,
    GT_BRIDGE_SOURCE_CANCELLING,
    GT_BRIDGE_SOURCE_SUCCEEDED,
    GT_BRIDGE_SOURCE_FAILED,
    GT_BRIDGE_SOURCE_STOPPED,
    GT_BRIDGE_SOURCE_UNCERTAIN
} gt_bridge_source_phase_t;

typedef struct {
    gt_bridge_session_t *session;
    gt_bridge_lease_t lease;
    int root_fd;
    const char *relative_path, *mime;
    uint64_t byte_limit;
} gt_bridge_source_config_t;

typedef struct {
    gt_bridge_source_phase_t phase;
    gt_bridge_source_status_t error;
    bool closed;
    uint64_t hashed, read_bytes, accepted, transfer_handle;
    gt_bridge_resource_descriptor_t descriptor;
    gt_bridge_transfer_result_t transfer;
} gt_bridge_source_info_t;

/* Copy path/metadata and retain only owned CLOEXEC descriptors on success.
 * Pure validation/auth errors do not read files. Relative ./ is supported;
 * absolute paths, .., non-UTF-8/control names and >32 components are rejected.
 * Regular file is inspected before/after nonblocking NOFOLLOW open; a rename
 * race is checked against retained ancestors. Root grant follows its opened
 * inode, not its former absolute name. Trusted caller owns ACL/mount policy.
 * Hard links reachable under the root are allowed; this is not a malicious
 * same-uid process sandbox. Fixed workspace; allocator context outlives destroy. */
gt_bridge_source_status_t gt__bridge_source_open(const gt_bridge_source_config_t *,
                                                 const gt_allocator_t *,
                                                 gt_bridge_source_t **);
/* One <=16 KiB pread or one bounded native producer write per step, with
 * post-IO authorization/stat checks. Not a hard disk latency deadline.
 * First pass requires real EOF and unchanged metadata before READY; send is
 * explicit. Second pass checks EOF/stat before finishing and uses the existing
 * producer digest check. Late changes after completion do not rewrite a copy. */
gt_bridge_source_phase_t gt__bridge_source_step(gt_bridge_source_t *);
gt_bridge_source_status_t gt__bridge_source_send(gt_bridge_source_t *);
void gt__bridge_source_cancel(gt_bridge_source_t *);
bool gt__bridge_source_info(const gt_bridge_source_t *, gt_bridge_source_info_t *);
/* Stops local reads and closes owned fds once. Pending remote cancellation
 * returns WAIT; caller continues Session step or explicitly stops the Session.
 * Close IO is reported once with the object retained, never by retrying a
 * consumed numeric fd. A subsequent close/destroy can finish bookkeeping.
 * destroy forgets the terminal transfer observation; WAIT/IO retain the source. */
gt_bridge_source_status_t gt__bridge_source_close(gt_bridge_source_t *);
gt_bridge_source_status_t gt__bridge_source_destroy(gt_bridge_source_t *);
#endif
