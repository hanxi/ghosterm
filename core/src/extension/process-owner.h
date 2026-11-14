/* Private POSIX child-process owner for explicitly authorized native services. */
#ifndef GT_EXTENSION_PROCESS_OWNER_H
#define GT_EXTENSION_PROCESS_OWNER_H
#include "ghosterm/extension.h"
#include <signal.h>

#define GT_EXT_PROCESS_IO_MAX 16384u

typedef struct gt_ext_process gt_ext_process_t;

typedef enum {
    GT_EXT_PROCESS_DATA,
    GT_EXT_PROCESS_WAIT,
    GT_EXT_PROCESS_EOF,
    GT_EXT_PROCESS_CLOSED,
    GT_EXT_PROCESS_ERROR
} gt_ext_process_io_state_t;

typedef struct {
    gt_ext_process_io_state_t state;
    size_t count;
    gt_status_t error;
} gt_ext_process_io_t;

typedef struct {
    bool exited;
    int exit_code;
    int term_signal;
} gt_ext_process_info_t;

/* The same bounded name/value and duplicate checks used before fork. */
bool gt__ext_process_environment_valid(const char *const *, size_t);

/* Absolute executable, raw argv, no shell or PATH search. The caller is a
 * trusted native host that has already applied the process.spawn grant and
 * program allowlist. cwd_fd must name an open directory; only the explicit
 * environment entries are inherited. No ambient caller cwd/environment leaks.
 * Pipes are nonblocking and exclusively owned by this owner. */
gt_status_t gt__ext_process_spawn(const char *executable,
                                  const char *const *argv,
                                  size_t argc,
                                  int cwd_fd,
                                  const char *const *environment,
                                  size_t environment_count,
                                  gt_ext_process_t **out);
gt_ext_process_io_t gt__ext_process_write(gt_ext_process_t *, const void *, size_t);
gt_ext_process_io_t gt__ext_process_read_stdout(gt_ext_process_t *, void *, size_t);
gt_ext_process_io_t gt__ext_process_read_stderr(gt_ext_process_t *, void *, size_t);
/* Private readiness only; descriptors stay exclusively owned here. HUP/ERR
 * wake the caller to perform the actual read/write, never stand in for EOF.
 * An optional borrowed wake fd interrupts the finite monotonic wait. */
#define GT_EXT_PROCESS_WAIT_INPUT 1u
#define GT_EXT_PROCESS_WAIT_OUTPUT 2u
#define GT_EXT_PROCESS_WAIT_ERROR 4u
gt_status_t gt__ext_process_wait(gt_ext_process_t *, unsigned interests, int wake_fd, int ms);
gt_status_t gt__ext_process_close_stdin(gt_ext_process_t *);
/* Signals the process group created for this child; callers own grace periods. */
gt_status_t gt__ext_process_signal(gt_ext_process_t *, int signal_number);
/* Nonblocking waitpid observation. Unknown exit remains distinct from code 0. */
gt_status_t gt__ext_process_poll(gt_ext_process_t *, gt_ext_process_info_t *);
/* Observe without reaping (POSIX waitid/WNOWAIT). A controller can fence/kill
 * the owned process group while its leader PID is still reserved, then poll to
 * reap. This does not claim that grandchildren have been reaped by this owner. */
gt_status_t gt__ext_process_observe_exit(gt_ext_process_t *, gt_ext_process_info_t *);
/* Refuses to free a running child. Caller must stop and reap before destroy. */
gt_status_t gt__ext_process_destroy(gt_ext_process_t **);
#endif
