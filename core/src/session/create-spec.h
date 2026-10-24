/* Private creation request, shared by local IPC and SSH management. */
#ifndef GT_CREATE_SPEC_H
#define GT_CREATE_SPEC_H
#include "ghosterm.h"
enum {GT_CREATE_NONCE_BYTES=16,GT_CREATE_MAX_BYTES=8192,GT_CREATE_MAX_ARGS=32,
  GT_CREATE_MAX_ENV=64,GT_CREATE_HOST_ENV=1,GT_CREATE_PREFIX_BYTES=48};
typedef struct {
  uint8_t nonce[GT_CREATE_NONCE_BYTES];
  gt_pty_cmd_kind_t command;uint16_t cols,rows;uint32_t px_width,px_height,environment;
  const char *executable,*cwd,*term;
  uint32_t argc,envc;
  const char *argv[GT_CREATE_MAX_ARGS],*env[GT_CREATE_MAX_ENV];
} gt_create_spec_t;
/* Exact big-endian schema. Text is valid UTF-8 without NUL. Absolute POSIX
 * executable/cwd, explicit shell/direct, positive cells, HOST_ENV only currently.
 * env keys are unique ASCII identifiers; TERM comes solely from term field.
 * Decoder borrows caller storage until reused; never copy spec then free storage.
 * PROTOCOL is malformed. UNSUPPORTED means syntactically valid unsupported
 * command/environment policy; output and scratch are cleared on either error.
 * Strings/args/env never enter diagnostics. No allocations or OS calls. */
gt_status_t gt__create_spec_decode(const void *,size_t,gt_create_spec_t *,char *,size_t);
gt_status_t gt__create_spec_encode(const gt_create_spec_t *,uint8_t *,size_t,size_t *);
/* Produces borrowed PTY options. Caller retains spec AND these pointer arrays
 * until backend has copied/finished using them. Pixel values must fit POSIX
 * winsize, otherwise UNSUPPORTED instead of silent truncation. */
gt_status_t gt__create_spec_pty(const gt_create_spec_t *,gt_pty_opts_t *,
  const char *args[GT_CREATE_MAX_ARGS+1],const char *env[GT_CREATE_MAX_ENV+1]);
#endif
