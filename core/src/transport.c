/*
 * Transport facade. Platform I/O and process resources live in private
 * backends; this object owns only dispatch and stream EOF state.
 */
#include "ghosterm.h"
#include "internal.h"
#include "io/backend.h"

#include <stdlib.h>

struct gt_transport {
  gt_io_backend_t *backend;
  gt_transport_kind_t kind;
  bool eof;
};

static gt_transport_t *new_transport(gt_transport_kind_t kind) {
  gt_transport_t *t = calloc(1, sizeof *t);
  if (!t) {
    gt__set_error("transport: out of memory");
    return NULL;
  }
  t->kind = kind;
  return t;
}

gt_transport_t *gt_transport_open_pty(const gt_pty_opts_t *opts) {
  gt_transport_t *t = new_transport(GT_TRANSPORT_LOCAL_PTY);
  if (!t) return NULL;
  t->backend = gt__pty_open(opts);
  if (!t->backend) {
    free(t);
    return NULL;
  }
  return t;
}

gt_transport_t *gt_transport_open_ssh_shell(gt_ssh_t *ssh,
                                           const gt_shell_opts_t *opts) {
  if (!ssh) {
    gt__set_error("gt_transport_open_ssh_shell: ssh 会话为空");
    return NULL;
  }
  gt_transport_t *t = new_transport(GT_TRANSPORT_SSH_SHELL);
  if (!t) return NULL;
  t->backend = gt__ssh_backend_open(ssh, opts);
  if (!t->backend) {
    free(t);
    return NULL;
  }
  return t;
}

int gt_transport_write(gt_transport_t *t, const uint8_t *data, size_t len) {
  if (!t || (!data && len)) {
    gt__set_error("transport_write: null argument");
    return GT_ERR_INVALID_ARG;
  }
  return t->backend->ops->write(t->backend, data, len);
}

int gt_transport_resize(gt_transport_t *t, uint16_t cols, uint16_t rows,
                        uint32_t px_width, uint32_t px_height) {
  if (!t) return GT_ERR_INVALID_ARG;
  return t->backend->ops->resize(t->backend, cols, rows, px_width, px_height);
}

int gt_transport_read(gt_transport_t *t, uint8_t *buf, size_t cap,
                      int timeout_ms) {
  if (!t || !buf) {
    gt__set_error("transport_read: null argument");
    return GT_TRANSPORT_READ_ERROR;
  }
  if (cap == 0) {
    gt__set_error("transport_read: cap must be > 0");
    return GT_TRANSPORT_READ_ERROR;
  }
  if (t->eof) return GT_TRANSPORT_READ_EOF;
  int result = t->backend->ops->read(t->backend, buf, cap, timeout_ms);
  if (result == GT_TRANSPORT_READ_EOF) t->eof = true;
  return result;
}

void gt_transport_close(gt_transport_t *t) {
  if (!t) return;
  t->backend->ops->close(t->backend);
  free(t);
}

bool gt_transport_exited(const gt_transport_t *t, int *exit_code) {
  if (!t) {
    if (exit_code) *exit_code = 0;
    return false;
  }
  return t->backend->ops->exited(t->backend, exit_code);
}

gt_transport_child_t gt_transport_child_state(const gt_transport_t *t) {
  if (!t) return GT_TRANSPORT_CHILD_UNKNOWN;
  return t->backend->ops->child_state(t->backend);
}

bool gt_transport_child_cwd(const gt_transport_t *t, char *buf, size_t cap) {
  if (!t || !buf || cap < 2) {
    gt__set_error("child_cwd: 参数非法");
    return false;
  }
  return t->backend->ops->child_cwd(t->backend, buf, cap);
}

gt_transport_kind_t gt_transport_kind(const gt_transport_t *t) {
  return t ? t->kind : GT_TRANSPORT_LOCAL_PTY;
}

gt_transport_t *gt_transport_open_ssh_command(gt_ssh_t *ssh,const char *command,const gt_shell_opts_t *opts,
    uint32_t timeout,bool (*cancelled)(void *),void *context){
  if(!ssh||!command||!*command){gt__set_error("ssh command: invalid arguments");return NULL;}
  gt_transport_t *t=new_transport(GT_TRANSPORT_SSH_SHELL);if(!t)return NULL;
  t->backend=gt__ssh_backend_command(ssh,command,opts,timeout,cancelled,context);
  if(!t->backend){free(t);return NULL;}return t;
}
