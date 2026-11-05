/* Private, single IO owner. Never expose this vtable through platform bindings. */
#ifndef GT_BRIDGE_STREAM_H
#define GT_BRIDGE_STREAM_H
#include "ghosterm/types.h"
#include "base/allocator.h"
#include <stdatomic.h>

#define GT_BRIDGE_IO_QUANTUM 16384u

typedef enum {
  GT_BRIDGE_DATA, GT_BRIDGE_WAIT, GT_BRIDGE_EOF,
  GT_BRIDGE_CLOSED, GT_BRIDGE_ERROR
} gt_bridge_io_state_t;
typedef struct {
  gt_bridge_io_state_t state;
  size_t count; /* DATA only: local acceptance, never remote completion. */
  gt_status_t error;
} gt_bridge_io_t;
typedef enum { GT_BRIDGE_DRAINING, GT_BRIDGE_RELEASED, GT_BRIDGE_CLOSE_ERROR } gt_bridge_close_t;
typedef struct gt_bridge_stream gt_bridge_stream_t;
typedef struct {
  gt_bridge_io_t (*read)(gt_bridge_stream_t *, void *, size_t, int);
  gt_bridge_io_t (*write)(gt_bridge_stream_t *, const void *, size_t, int);
  /* Thread-safe wake only. Resource release belongs to the IO owner. */
  void (*wake)(gt_bridge_stream_t *);
  gt_bridge_close_t (*close_step)(gt_bridge_stream_t *, int);
  void (*destroy)(gt_bridge_stream_t *);
} gt_bridge_stream_ops_t;
struct gt_bridge_stream {
  const gt_bridge_stream_ops_t *ops;
  atomic_bool stopping;
  bool closed; /* owner only */
};

/* Budgets are finite milliseconds >= 0; zero means try without waiting.
 * One owner calls read/write/close/destroy. Only request_stop may run elsewhere.
 * On WAIT a write's bytes/address/length stay stable across continuations until
 * acceptance or completed close, including EAGAIN in a future SSH provider.
 * request_stop alone does not release a retained buffer. No provider may access
 * caller buffers after completed close. No automatic reconnect or replay.
 * stop may race close, but all stop callers must finish before destroy. */
gt_status_t gt__bridge_stream_init(gt_bridge_stream_t *, const gt_bridge_stream_ops_t *);
gt_bridge_io_t gt__bridge_stream_read(gt_bridge_stream_t *, void *, size_t, int);
gt_bridge_io_t gt__bridge_stream_write(gt_bridge_stream_t *, const void *, size_t, int);
void gt__bridge_stream_stop(gt_bridge_stream_t *);
gt_bridge_close_t gt__bridge_stream_close(gt_bridge_stream_t *, int);
/* Refuses to free a live/draining owner. Idempotence applies to close, not destroy. */
gt_status_t gt__bridge_stream_destroy(gt_bridge_stream_t *);

/* Adopts connected pipe ends or a stream socket (read_fd == write_fd allowed).
 * Success consumes the fds, marks them NONBLOCK/CLOEXEC; failure retains them
 * and restores original flags. Exclusive ownership of their open descriptions
 * is required (dup aliases share NONBLOCK). Datagram/file fds and sockets with
 * SO_LINGER enabled are rejected. No implicit authentication/IPC trust.
 * Allocator context must outlive destroy. NULL selects malloc/free. */
gt_status_t gt__bridge_fd_open(int read_fd, int write_fd,
                               const gt_allocator_t *, gt_bridge_stream_t **);
#endif
