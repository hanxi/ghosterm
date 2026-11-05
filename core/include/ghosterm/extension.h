/* Native plugin transport boundary. No terminal PTY, plugin permission grant,
 * protocol handshake, runtime, automatic reconnect or gateway fallback. Only
 * trusted native transport owners call this API; never expose it to plugins.
 * All calls except stop belong to one serial non-UI IO owner. Stop may race IO
 * and close, but every stop caller must finish before destroy. */
#ifndef GHOSTERM_EXTENSION_H
#define GHOSTERM_EXTENSION_H
#include "ghosterm/ssh_auth.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_MAX_HOPS 8u
#define GT_EXT_CHANNEL_LIMIT 16u
#define GT_EXT_IO_MAX 16384u
#define GT_EXT_DIAGNOSTIC_MAX 4096u
typedef struct gt_ext_channel gt_ext_channel_t;
typedef enum {GT_EXT_CONNECTING,GT_EXT_READY,GT_EXT_FAILED,GT_EXT_STOPPED,GT_EXT_CLOSED} gt_ext_phase_t;
typedef enum {GT_EXT_STAGE_NONE,GT_EXT_STAGE_AUTH,GT_EXT_STAGE_FORWARD,GT_EXT_STAGE_EXEC,GT_EXT_STAGE_STREAM} gt_ext_stage_t;
typedef enum {GT_EXT_DATA,GT_EXT_WAIT,GT_EXT_EOF,GT_EXT_IO_CLOSED,GT_EXT_IO_ERROR} gt_ext_io_state_t;
typedef struct {gt_ext_io_state_t state;size_t count;gt_status_t error;} gt_ext_io_t;
typedef enum {GT_EXT_DRAINING,GT_EXT_RELEASED,GT_EXT_CLOSE_ERROR} gt_ext_close_t;
typedef struct {
  /* First: resolved binary address, via_host=NULL. Inner: family=0, explicit
   * via_host as seen from the previous hop. Each SSH handle retains its own
   * host/user/port/known_hosts identity. NULL clocks select Core monotonic time;
   * custom clocks/context must be identical and outlive channel destruction. */
  gt_ssh_auth_options_t authentication;
  const char *via_host;
} gt_ext_ssh_hop_t;
typedef struct {uint32_t setup_ms,admission_ms,stderr_limit;} gt_ext_ssh_options_t;
typedef struct {
  gt_ext_phase_t phase;gt_ext_stage_t stage,failure_stage;gt_status_t error,cleanup_error;
  uint32_t hop,hops;bool setup_expired;
  /* Nonzero only for the current hostkey/credential wait. Replies require this
   * locally issued token AND hop. Duplicate/stale replies never reach SSH. */
  uint64_t challenge;
  gt_ssh_auth_info_t authentication[GT_EXT_MAX_HOPS];
  gt_hostkey_decision_t decisions[GT_EXT_MAX_HOPS];
  /* Local copied bytes / underlying accepted writes / underlying reads. These
   * observations survive concurrent stop; none proves protocol completion. */
  uint64_t accepted,sent,received,stderr_bytes;
  uint32_t buffered,diagnostic_size;
  bool remote_eof,exit_status_known,diagnostic_truncated;
  int32_t exit_status;
  uint8_t diagnostic[GT_EXT_DIAGNOSTIC_MAX]; /* raw bytes; not UI text */
} gt_ext_channel_info_t;
/* Adopts exclusive pipe ends/connected stream sockets on success only, marks
 * NONBLOCK/CLOEXEC; equal fds allowed. Failure retains fds/restores flags.
 * No implicit peer authentication. File/datagram/linger fds are INVALID_ARG.
 * The public channel build option may explicitly return UNSUPPORTED. */
gt_status_t gt_ext_channel_open_fd(int read_fd,int write_fd,gt_ext_channel_t **);
/* Takes/clears ALL fresh SSH handles on success only. No IO during wrapper
 * construction beyond native connector setup. Fixed trusted bootstrap command
 * is copied (1..1024 bytes); dynamic plugin data/cwd/paths must use the later
 * framed protocol, never shell interpolation. No PTY or shell relay request.
 * NULL options: 30s setup, 5s admission, 256 KiB stderr; each hop timeout is
 * explicit 1..60000ms. One total setup budget includes native approval waits.
 * At most 16 public channels, including failed/draining owners until destroy. */
gt_status_t gt_ext_channel_open_ssh(gt_ssh_t **,const gt_ext_ssh_hop_t *,size_t,
  const char *bootstrap,const gt_ext_ssh_options_t *,gt_ext_channel_t **);
/* Finite ms >=0. Advances admission OR one queued write. Does not read protocol
 * bytes or busy-loop waiting for a decision. Read has its own finite budget. */
gt_ext_phase_t gt_ext_channel_step(gt_ext_channel_t *,int ms);
bool gt_ext_channel_info(const gt_ext_channel_t *,gt_ext_channel_info_t *);
gt_status_t gt_ext_channel_decide(gt_ext_channel_t *,uint32_t hop,uint64_t challenge,gt_hostkey_decision_t);
gt_status_t gt_ext_channel_supply(gt_ext_channel_t *,uint32_t hop,uint64_t challenge,gt_ssh_t **);
gt_ext_io_t gt_ext_channel_read(gt_ext_channel_t *,void *,size_t,int ms);
/* Copies <=16 KiB into native stable storage, no network IO. DATA count means
 * local acceptance only, never delivery/completion. WAIT means queue occupied;
 * caller memory is never retained, even on WAIT. step drains the queue with
 * partial writes. Stop rejects new IO; borrowed storage remains charged and
 * unchanged until underlying acceptance or completed close. Close discards
 * remaining bytes after provider release, never claims delivery. */
gt_ext_io_t gt_ext_channel_write(gt_ext_channel_t *,const void *,size_t);
void gt_ext_channel_stop(gt_ext_channel_t *);
/* Close is idempotent and retains ownership on DRAINING/ERROR. No network
 * success/replay promise. Destroy only after RELEASED; success clears *owner,
 * failure keeps it. Calls on NULL/malformed arguments explicitly fail. */
gt_ext_close_t gt_ext_channel_close(gt_ext_channel_t *,int ms);
gt_status_t gt_ext_channel_destroy(gt_ext_channel_t **);
#ifdef __cplusplus
}
#endif
#endif
