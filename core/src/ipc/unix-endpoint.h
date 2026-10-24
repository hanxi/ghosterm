/* Private Unix endpoint shared by local clients and remote SSH proxies. */
#ifndef GT_UNIX_ENDPOINT_H
#define GT_UNIX_ENDPOINT_H
#include <stdbool.h>
#include <sys/types.h>
#include "base/allocator.h"
typedef struct gt_unix_endpoint gt_unix_endpoint_t;
typedef enum {
  GT_ENDPOINT_OK,GT_ENDPOINT_WAIT,GT_ENDPOINT_NOT_FOUND,GT_ENDPOINT_BUSY,
  GT_ENDPOINT_INVALID,GT_ENDPOINT_IO,GT_ENDPOINT_AUTH,GT_ENDPOINT_NO_MEMORY,
  GT_ENDPOINT_UNSUPPORTED,GT_ENDPOINT_REJECTED
} gt_endpoint_rc_t;
/* Single non-UI owner. Absolute path with no symlink/dot/empty components.
 * Ancestors must be root/current-user owned and not group/world writable;
 * leaf is current-user owned, exactly 0700. With create, missing components
 * are made 0700 then revalidated; existing nodes are never chmodded. Runtime
 * directory/lock nodes persist; endpoint close never recursively deletes them.
 * One fixed allocation; allocator context outlives close. Failure clears out. */
gt_endpoint_rc_t gt__unix_endpoint_open(const char *directory,bool create,
  const gt_allocator_t *,gt_unix_endpoint_t **out);
/* Nonblocking client/proxy startup lock. Not the daemon's lifetime lock.
 * Caller connects again after acquiring it, launches if still absent, and
 * releases after bounded readiness. Repeated lock/unlock is idempotent.
 * Never unlink lock files, which would split the lock namespace. */
gt_endpoint_rc_t gt__unix_endpoint_startup_lock(gt_unix_endpoint_t *);
gt_endpoint_rc_t gt__unix_endpoint_startup_unlock(gt_unix_endpoint_t *);
/* Acquires a separate daemon lifetime lock; holds it until close. Does not
 * start a process. Existing live/backed-up endpoint is BUSY, never removed.
 * Stale removal requires owned 0600 socket, lifetime lock, refused connection,
 * and unchanged inode. Regular files, symlinks or foreign nodes are errors.
 * Successful socket is 0600, nonblocking/CLOEXEC. Backlog 1..128. */
gt_endpoint_rc_t gt__unix_endpoint_listen(gt_unix_endpoint_t *,int backlog);
/* Borrowed poll fd; caller MUST NOT close/duplicate it. -1 before listen. */
int gt__unix_endpoint_listener(const gt_unix_endpoint_t *);
/* One accept attempt; WAIT on EAGAIN/EINTR, BUSY on OS descriptor/memory limits.
 * REJECTED means an accepted peer failed configuration/credential validation;
 * its fd was closed, listener remains usable. AUTH concerns endpoint identity.
 * Missing credential capability remains UNSUPPORTED. Successful fd transfers
 * to caller, is nonblocking/CLOEXEC, UID checked. Clears out to -1. */
gt_endpoint_rc_t gt__unix_endpoint_accept(gt_unix_endpoint_t *,int *out);
/* timeout 0..1000ms, one monotonic deadline across EINTR. Failure closes its
 * temporary socket; WAIT does not preserve a pending connect. Success verifies
 * server UID and transfers fd; it does NOT authenticate a protocol/session.
 * NOT_FOUND means absent/refused, not arbitrary IO/auth errors. */
gt_endpoint_rc_t gt__unix_endpoint_connect(gt_unix_endpoint_t *,int timeout_ms,int *out);
/* Connected Unix stream only. Kernel credential query failure is an error,
 * never inferred from socket pathname permissions. Does not adopt/close fd. */
gt_endpoint_rc_t gt__unix_peer_check(int fd,uid_t expected);
/* Always closes owned fds/frees object. Only unlink this listener's unchanged
 * inode under its unchanged lifetime lock; a replacement is left untouched.
 * Other cleanup errors are reported. No worker/session/attachment calls. */
gt_endpoint_rc_t gt__unix_endpoint_close(gt_unix_endpoint_t *);
#endif
