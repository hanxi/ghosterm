/* Private I/O backend contract. Never included by platform bindings or UI. */
#ifndef GHOSTERM_IO_BACKEND_H
#define GHOSTERM_IO_BACKEND_H

#include "ghosterm.h"
#include "base/allocator.h"
#include "base/thread-options.h"

typedef struct gt_io_backend gt_io_backend_t;

/* Explicit exit observation; UNKNOWN never means successful exit. */
typedef enum {GT_IO_EXIT_UNKNOWN,GT_IO_EXIT_CODE,GT_IO_EXIT_SIGNAL} gt_io_exit_kind_t;
typedef struct {gt_io_exit_kind_t kind;int32_t code;} gt_io_exit_t;
/*
 * The caller exclusively owns backend access on its session thread. close
 * destroys the backend and its resources; attachment detach must not call it.
 * Read/write retain the existing bounded byte-stream contract during P1-b.
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
  /* Optional low-frequency observation. OK supplies CODE>=0 or SIGNAL>0;
   * NOT_FOUND = not observed yet. NULL means UNSUPPORTED, never code zero.
   * Legacy exited() may lack availability/signal information; do not infer it. */
  gt_status_t (*observe_exit)(const gt_io_backend_t *,gt_io_exit_t *);
  /* Optional owner-only read with a borrowed command wake fd. Readability
   * interrupts an otherwise idle wait with READ_TIMEOUT; ready output may
   * take precedence for fairness. Never consumes/closes the fd or
   * changes backend lifetime/EOF. Caller drains/rechecks its queue. NULL keeps
   * the ordinary bounded-read contract (e.g. SSH), without a wake guarantee. */
  int (*read_with_wake)(gt_io_backend_t *,uint8_t *,size_t,int,int);

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

#endif /* GHOSTERM_IO_BACKEND_H */
