/* Optional tmux management. One serial non-UI owner per handle. No registry,
 * persistence, automatic retries or shell input injection. SSH is borrowed and
 * dedicated to management; attach takes a separate authenticated owner. */
#ifndef GHOSTERM_TMUX_H
#define GHOSTERM_TMUX_H
#include "ghosterm/transport.h"
#include "ghosterm/types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_TMUX_NAME_MAX 1024
#define GT_TMUX_PATH_MAX 4096
#define GT_TMUX_SESSION_MAX 256
typedef struct gt_tmux gt_tmux_t;
typedef struct {
  const char *executable; /* absolute path or NULL: login-shell command -v */
  const char
      *socket_path; /* absolute path or NULL: tmux default, then locked */
  const char *login_shell; /* local only, NULL uses passwd shell */
  uint32_t timeout_ms;     /* 0 = 5000, per high-level operation */
  bool (*cancelled)(void *);
  void *context;
} gt_tmux_opts_t;
typedef struct {
  uint64_t server_pid, server_started, id, created;
  uint32_t windows, attached;
  /* -1 = unknown. destroy: 0 off, 1 on, 2 keep-last, 3 keep-group. */
  int32_t destroy_unattached, exit_unattached;
  char name[GT_TMUX_NAME_MAX];
  char socket_path[GT_TMUX_PATH_MAX];
} gt_tmux_session_t;
typedef enum { GT_TMUX_SERVER_RUNNING, GT_TMUX_SERVER_ABSENT } gt_tmux_server_t;
typedef enum {
  GT_TMUX_CREATE,
  GT_TMUX_RENAME,
  GT_TMUX_TERMINATE
} gt_tmux_operation_t;
typedef enum {
  GT_TMUX_CONFIRMED,
  GT_TMUX_REJECTED,
  GT_TMUX_UNKNOWN
} gt_tmux_result_t;
/* ssh=NULL selects local. Resolves path and verifies tmux >=3.6a. */
gt_tmux_t *gt_tmux_open(gt_ssh_t *ssh, const gt_tmux_opts_t *opts);
void gt_tmux_close(gt_tmux_t *tmux);
/* All-or-error snapshot. A failed refresh clears count; caller discards old UI
 * selection. ABSENT is distinct from a successfully parsed empty list. */
int gt_tmux_list(gt_tmux_t *, gt_tmux_session_t *, size_t capacity,
                 size_t *count, gt_tmux_server_t *server);
/* Target is mandatory except CREATE; exact ID + server/creation/name/socket
 * revalidated before use. Non-OK may still have UNKNOWN result: never replay.
 */
int gt_tmux_change(gt_tmux_t *, gt_tmux_operation_t,
                   const gt_tmux_session_t *target, const char *name,
                   gt_tmux_result_t *result);
/* Creates an ordinary interactive transport after fresh target validation.
 * -N attach-session only. Closing it closes the client, never kills session.
 * Local: attach_ssh=NULL; remote: dedicated authenticated attach_ssh required.
 */
gt_transport_t *gt_tmux_attach(gt_tmux_t *, const gt_tmux_session_t *,
                               gt_ssh_t *attach_ssh,
                               const gt_shell_opts_t *size);
#ifdef __cplusplus
}
#endif
#endif
