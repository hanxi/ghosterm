/* Unix connected-stream adapter. Authentication remains the endpoint's job. */
#ifndef GT_IPC_SOCKET_STREAM_H
#define GT_IPC_SOCKET_STREAM_H
#include "connection.h"
#include "ghosterm/types.h"
typedef struct {int fd;} gt_socket_stream_t;
/* Fresh storage; connected Unix SOCK_STREAM only. Success adopts fd, configures
 * NONBLOCK/CLOEXEC/NOSIGPIPE, returns ops with context=storage. Failure owns no
 * fd and restores flags. If subsequent connection_open fails, caller closes
 * this adapter explicitly; otherwise connection owns close. Storage outlives
 * connection. No readers/writers/aliases of fd may remain. Read gives HUP/error
 * priority over queued data, matching attachment-peer's no-late-input policy. */
gt_status_t gt__socket_stream_adopt(int fd,gt_socket_stream_t *,gt_stream_ops_t *);
void gt__socket_stream_close(gt_socket_stream_t *);
#endif
