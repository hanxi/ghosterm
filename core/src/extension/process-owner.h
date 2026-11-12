/* Private POSIX child-process owner for explicitly authorized native services. */
#ifndef GT_EXTENSION_PROCESS_OWNER_H
#define GT_EXTENSION_PROCESS_OWNER_H
#include "ghosterm/types.h"
#include <signal.h>

#define GT_EXT_PROCESS_ARGUMENTS_MAX 64u
#define GT_EXT_PROCESS_ARGUMENT_MAX 4096u
#define GT_EXT_PROCESS_ARGUMENT_BYTES_MAX 16384u
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

/* Absolute executable, raw argv, no shell or PATH search. The caller is a
 * trusted native host that has already applied the process.spawn grant and
 * program allowlist. Process starts in the caller's current directory and
 * environment. Pipes are nonblocking and exclusively owned by this owner. */
gt_status_t gt__ext_process_spawn(const char *executable,
                                  const char *const *argv,
                                  size_t argc,
                                  gt_ext_process_t **out);
gt_ext_process_io_t gt__ext_process_write(gt_ext_process_t *, const void *, size_t);
gt_ext_process_io_t gt__ext_process_read_stdout(gt_ext_process_t *, void *, size_t);
gt_ext_process_io_t gt__ext_process_read_stderr(gt_ext_process_t *, void *, size_t);
gt_status_t gt__ext_process_close_stdin(gt_ext_process_t *);
/* Signals the process group created for this child; callers own grace periods. */
gt_status_t gt__ext_process_signal(gt_ext_process_t *, int signal_number);
/* Nonblocking waitpid observation. Unknown exit remains distinct from code 0. */
gt_status_t gt__ext_process_poll(gt_ext_process_t *, gt_ext_process_info_t *);
/* Refuses to free a running child. Caller must stop and reap before destroy. */
gt_status_t gt__ext_process_destroy(gt_ext_process_t **);
#endif
