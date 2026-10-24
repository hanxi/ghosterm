/* Dedicated terminal SSH connection. One serial non-UI owner; no SFTP aliases. */
#ifndef GT_IPC_SSH_LINK_H
#define GT_IPC_SSH_LINK_H
#include "connection.h"
#include "ghosterm/types.h"
typedef struct gt_ssh gt_ssh_t;
typedef struct gt_ssh_link gt_ssh_link_t;
/* Shared POSIX absolute-path quoting for exec and installation commands. */
bool gt__ssh_posix_path(char *,size_t,size_t *,const char *);
typedef enum {GT_SSH_LINK_IDLE,GT_SSH_LINK_OPENING,GT_SSH_LINK_READY,
  GT_SSH_LINK_CLOSING,GT_SSH_LINK_ENDED,GT_SSH_LINK_FAILED} gt_ssh_link_phase_t;
typedef struct {
  gt_ssh_link_phase_t phase;gt_status_t error;uint64_t stderr_bytes;
  /* libssh2 cannot distinguish absent exit-status from exit 0. Preserve unknown. */
  bool has_exit_code;int exit_code;
} gt_ssh_link_info_t;
/* Takes a dedicated authenticated SSH connection on success only, clears *ssh.
 * Caller relinquishes ALL aliases/handles; all subsequent calls use one owner.
 * Two fixed 64KiB queues per stream + stable pending packet, charged up front. */
gt_status_t gt__ssh_link_open(gt_ssh_t **,const gt_allocator_t *,gt_ssh_link_t **);
/* Slots 0/1: management/data. An ended slot may be reused only after stream close.
 * Absolute paths quoted for POSIX sh; control characters/oversize are rejected.
 * No PTY request. Success returns stable ops usable immediately (WAIT until exec).
 * ops context remains owned by link, which must outlive connection destruction. */
gt_status_t gt__ssh_link_stream(gt_ssh_link_t *,unsigned slot,const char *proxy,
  const char *runtime,const char *daemon,gt_stream_ops_t *);
/* Bounded nonblocking attempts; pumps both stderr/stdout fairly. Retry an
 * outstanding libssh2 packet with identical owned arguments before new writes. */
gt_status_t gt__ssh_link_step(gt_ssh_link_t *);
bool gt__ssh_link_info(const gt_ssh_link_t *,unsigned,gt_ssh_link_info_t *);
/* Cancel closes only SSH/exec streams, never daemon/session. No waiting on peer.
 * Existing stream contexts stay valid (ERROR) until link_close. */
void gt__ssh_link_cancel(gt_ssh_link_t *);
void gt__ssh_link_close(gt_ssh_link_t *);
#endif
