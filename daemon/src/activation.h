#ifndef GT_DAEMON_ACTIVATION_H
#define GT_DAEMON_ACTIVATION_H
#include "ipc/bootstrap-wire.h"
#include "ghosterm/types.h"
#include <sys/types.h>
#include "ipc/proxy-status.h"
typedef struct {
  const char *directory,*helper;gt_protocol_hello_t hello;uint32_t timeout_ms;
  void *context;bool (*cancelled)(void *);
} gt_activation_config_t;
typedef struct {int fd;pid_t child;bool committed;} gt_activation_result_t;
/* Startup only, off UI: one monotonic deadline, nonblocking socket attempts and
 * bounded polling. No application commands, engine or SSH session calls.
 * Starts only MANAGEMENT with unspecified instance, after startup lock/recheck.
 * Only an owned child that has received no readiness ACK may be killed on
 * failure; before ACK the child cannot process CREATE. Once ACK bytes enter the
 * stream, never terminate it on cancellation or protocol/client failures.
 * out.fd transfers on OK. out.child is an owned child PID (if any) to reap with
 * waitpid, never a PID read from a file. A committed child may outlive caller. */
gt_activation_rc_t gt__activation_connect(const gt_activation_config_t *,gt_activation_result_t *);
/* WNOHANG only; clears child after observed exit/ECHILD, never kills a child. */
void gt__activation_reap(gt_activation_result_t *);
#endif
