/* Private transport for managed SSH nesting. All calls on one IO owner. */
#ifndef GT_SSH_WIRE_H
#define GT_SSH_WIRE_H
#include "ghosterm/ssh.h"
#include <sys/types.h>
#include <stddef.h>
ssize_t gt__ssh_wire_read(gt_ssh_t *, void *, size_t, int flags);
ssize_t gt__ssh_wire_write(gt_ssh_t *, const void *, size_t, int flags);
/* Returns negative errno from read/write. wait uses POSIX POLLIN/POLLOUT,
 * returns -1 error / 0 no progress / 1 readiness. Wake fds are borrowed. */
int gt__ssh_wire_wait(gt_ssh_t *, short, int ms, const int *wake_fds, size_t wake_count);
int gt__ssh_wire_descriptor(const gt_ssh_t *);
unsigned gt__ssh_wire_depth(const gt_ssh_t *);
bool gt__ssh_route_fresh(const gt_ssh_t *);
uint16_t gt__ssh_route_port(const gt_ssh_t *);
#endif
