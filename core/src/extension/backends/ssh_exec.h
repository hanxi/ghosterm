/* Private dedicated SSH provider; never borrowed from terminal/SFTP owners. */
#ifndef GT_BRIDGE_SSH_EXEC_H
#define GT_BRIDGE_SSH_EXEC_H
#include "extension/stream.h"
#include "ghosterm/ssh.h"
#define GT_BRIDGE_SSH_COMMAND_MAX 1024u
#define GT_BRIDGE_SSH_DIAGNOSTIC_MAX 4096u
#define GT_BRIDGE_SSH_STDERR_MAX 1048576u

typedef enum {
  GT_BRIDGE_SSH_CHANNEL, GT_BRIDGE_SSH_EXEC, GT_BRIDGE_SSH_READY,
  GT_BRIDGE_SSH_FAILED, GT_BRIDGE_SSH_STOPPED, GT_BRIDGE_SSH_CLOSED
} gt_bridge_ssh_phase_t;
typedef struct { uint32_t admission_ms, stderr_limit; } gt_bridge_ssh_options_t;
typedef struct {
  gt_bridge_ssh_phase_t phase;
  gt_bridge_ssh_phase_t failure_phase; /* meaningful when error != GT_OK */
  gt_status_t error;
  bool admission_expired, remote_eof;
  /* libssh2 1.11.1's getter has no presence bit: always unknown, including 0.
   * A process exit is never used as an application response/operation result. */
  bool exit_status_known;
  int exit_status;
  uint64_t stderr_bytes;
  size_t diagnostic_size;
  bool diagnostic_truncated;
  uint8_t diagnostic[GT_BRIDGE_SSH_DIAGNOSTIC_MAX]; /* raw, not UI text */
} gt_bridge_ssh_info_t;
/* Success takes *owned and sets it NULL before any channel admission. Caller
 * supplies a fresh authenticated connector result, no channels or aliases.
 * Command is a trusted fixed bootstrap, NOT plugin arguments/cwd or user input.
 * It is copied; dynamic data goes in the framed protocol after authorization.
 * Failure before ownership transfer leaves *owned usable. No PTY/shell request.
 * NULL options: 5s total admission, 256 KiB lifetime stderr budget. */
gt_status_t gt__bridge_ssh_open(gt_ssh_t **owned, const char *command,
  const gt_bridge_ssh_options_t *, const gt_allocator_t *, gt_bridge_stream_t **);
/* Same exclusive-session contract, RFC4254 direct-tcpip instead of exec.
 * Destination is explicit managed route data, never a remote offer. */
gt_status_t gt__bridge_ssh_forward_open(gt_ssh_t **,const char *host,uint16_t port,
  const gt_bridge_ssh_options_t *,const gt_allocator_t *,gt_bridge_stream_t **);
/* Owner-only nested wire support; never treat a channel as a native fd. */
int gt__bridge_ssh_descriptor(const gt_bridge_stream_t *);
bool gt__bridge_ssh_forward_ready(const gt_bridge_stream_t *);
unsigned gt__bridge_ssh_depth(const gt_bridge_stream_t *);
int gt__bridge_ssh_wait(gt_bridge_stream_t *,short,int,const int *,size_t);
gt_status_t gt__bridge_ssh_attach_stop(gt_bridge_stream_t *,const atomic_bool *,int wake_fd);
/* Explicit owner-driven admission, finite ms >=0. Overall deadline includes
 * time between steps. Stop is safe from another thread through stream_stop.
 * Failed/partially admitted owners must close; never retry on another route. */
gt_bridge_ssh_phase_t gt__bridge_ssh_step(gt_bridge_stream_t *, int ms);
bool gt__bridge_ssh_info(const gt_bridge_stream_t *, gt_bridge_ssh_info_t *);
#endif
