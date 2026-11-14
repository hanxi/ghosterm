/* Private, fork-local process monitor. No executable/helper payload or service.
 * The real child stays unreaped until the owner releases its process-group lease.
 * Only POSIX async-signal-safe operations run after fork. */
#ifndef GT_EXTENSION_PROCESS_GUARD_H
#define GT_EXTENSION_PROCESS_GUARD_H
#include <sys/types.h>

enum {
    GT_PROCESS_GUARD_START = 1,
    GT_PROCESS_GUARD_EXIT,
    GT_PROCESS_GUARD_FINAL
};

typedef struct {
    int kind, pid, exit_code, term_signal, group_error;
} gt_process_guard_message_t;

_Noreturn void gt__process_guard_run(const char *executable,
                                     char *const *argv,
                                     char *const *environment,
                                     int cwd,
                                     int input,
                                     int output,
                                     int errors,
                                     int exec_error,
                                     int control,
                                     int reports,
                                     int max_fd);
#endif
