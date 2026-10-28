/* Private I/O backend contract. Never included by platform bindings or UI. */
#ifndef GHOSTERM_IO_BACKEND_H
#define GHOSTERM_IO_BACKEND_H

#include "ghosterm.h"
#include "base/allocator.h"
#include "base/thread-options.h"

typedef struct gt_io_backend gt_io_backend_t;

/*
 * The caller exclusively owns backend access on its session thread. close
 * destroys the backend and its resources. Read/write use bounded byte-stream
 * operations; no UI or session-manager representation enters this interface.
 * No backend may depend on a surface, platform UI, or the public transport's
 * representation. Every backend embeds this base as its first member.
 */
typedef struct {
  int (*read)(gt_io_backend_t *, uint8_t *, size_t, int);
  int (*write)(gt_io_backend_t *, const uint8_t *, size_t);
  int (*resize)(gt_io_backend_t *, uint16_t, uint16_t, uint32_t, uint32_t);
  bool (*exited)(const gt_io_backend_t *, int *);
  gt_transport_child_t (*child_state)(const gt_io_backend_t *);
  bool (*child_cwd)(const gt_io_backend_t *, char *, size_t);
  void (*close)(gt_io_backend_t *);

} gt_io_ops_t;

struct gt_io_backend {
  const gt_io_ops_t *ops;
};

gt_io_backend_t *gt__pty_open(const gt_pty_opts_t *opts);
/* Domain covers parent-side handle, launch arrays/strings and input backlog.
 * NULL selects libc. Every allocation is freed on its session owner except
 * libc internals; context outlives close. Replacing backlog charges old+new.
 * OS descriptors, waiter stack and libc passwd lookup internals are separate. */
gt_io_backend_t *gt__pty_open_with_allocator(const gt_pty_opts_t *,const gt_allocator_t *);
/* Explicit waiter stack/guard; host admits capacity before open and retains
 * it until close has joined waiter. Invalid attributes reject before fork. */
gt_io_backend_t *gt__pty_open_with_options(const gt_pty_opts_t *,const gt_allocator_t *,const gt_thread_options_t *);
/* Native C resource inspection only, never a platform binding. Borrowed thread
 * ID is valid until backend close; querying does not join or transfer ownership.
 * Non-PTY backend reports UNSUPPORTED; failure zeroes output. Owner serializes. */
gt_status_t gt__pty_waiter_thread(const gt_io_backend_t *,pthread_t *);
gt_io_backend_t *gt__ssh_backend_open(gt_ssh_t *ssh, const gt_shell_opts_t *opts);

gt_io_backend_t *gt__ssh_backend_command(gt_ssh_t *,const char *,const gt_shell_opts_t *,uint32_t,bool (*)(void *),void *,gt_status_t *);
#endif /* GHOSTERM_IO_BACKEND_H */
