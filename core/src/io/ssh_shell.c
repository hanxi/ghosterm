/* SSH channel adapter. libssh2 session ownership stays in ssh.c for now. */
#include "backend.h"
#include "internal.h"

#include <stdlib.h>

typedef struct {
  gt_io_backend_t base;
  void *channel;
} gt_ssh_backend_t;

static int ssh_read(gt_io_backend_t *backend, uint8_t *buf, size_t cap,
                    int timeout_ms) {
  gt_ssh_backend_t *ssh = (gt_ssh_backend_t *)backend;
  return gt__ssh_shell_read(ssh->channel, buf, cap, timeout_ms);
}

static int ssh_write(gt_io_backend_t *backend, const uint8_t *data, size_t len) {
  gt_ssh_backend_t *ssh = (gt_ssh_backend_t *)backend;
  return gt__ssh_shell_write(ssh->channel, data, len);
}

static int ssh_resize(gt_io_backend_t *backend, uint16_t cols, uint16_t rows,
                      uint32_t width, uint32_t height) {
  gt_ssh_backend_t *ssh = (gt_ssh_backend_t *)backend;
  return gt__ssh_shell_resize(ssh->channel, cols, rows, width, height);
}

static bool ssh_exited(const gt_io_backend_t *backend, int *code) {
  const gt_ssh_backend_t *ssh = (const gt_ssh_backend_t *)backend;
  return gt__ssh_shell_exited(ssh->channel, code);
}

static gt_transport_child_t ssh_child_state(const gt_io_backend_t *backend) {
  (void)backend;
  return GT_TRANSPORT_CHILD_UNKNOWN;
}

static bool ssh_child_cwd(const gt_io_backend_t *backend, char *buf, size_t cap) {
  (void)backend;
  (void)buf;
  (void)cap;
  gt__set_error("child_cwd: 仅本地 PTY 有内核 cwd（远端走 OSC 7）");
  return false;
}

static void ssh_close(gt_io_backend_t *backend) {
  gt_ssh_backend_t *ssh = (gt_ssh_backend_t *)backend;
  gt__ssh_shell_close(ssh->channel);
  free(ssh);
}

static const gt_io_ops_t ssh_ops = {
  .read = ssh_read,
  .write = ssh_write,
  .resize = ssh_resize,
  .exited = ssh_exited,
  .child_state = ssh_child_state,
  .child_cwd = ssh_child_cwd,
  .close = ssh_close,
};

gt_io_backend_t *gt__ssh_backend_open(gt_ssh_t *ssh, const gt_shell_opts_t *opts) {
  static const gt_shell_opts_t zero;
  const gt_shell_opts_t *o = opts ? opts : &zero;
  if (!ssh) {
    gt__set_error("gt_transport_open_ssh_shell: ssh 会话为空");
    return NULL;
  }
  gt_ssh_backend_t *backend = calloc(1, sizeof *backend);
  if (!backend) {
    gt__set_error("ssh_shell: out of memory");
    return NULL;
  }
  backend->channel = gt__ssh_shell_open(ssh, o->term, o->cols, o->rows,
                                       o->px_width, o->px_height);
  if (!backend->channel) {
    free(backend);
    return NULL;
  }
  backend->base.ops = &ssh_ops;
  return &backend->base;
}

gt_io_backend_t *gt__ssh_backend_command(gt_ssh_t *ssh,const char *command,const gt_shell_opts_t *opts,
    uint32_t timeout,bool (*cancelled)(void *),void *context,gt_status_t *out_status){
  static const gt_shell_opts_t zero;
  gt_ssh_backend_t *backend=calloc(1,sizeof *backend);if(!backend){if(out_status)*out_status=GT_ERR_INTERNAL;return NULL;}
  backend->channel=gt__ssh_command_open(ssh,command,opts?opts:&zero,timeout,cancelled,context,out_status);
  if(!backend->channel){free(backend);return NULL;}
  backend->base.ops=&ssh_ops;return &backend->base;
}
