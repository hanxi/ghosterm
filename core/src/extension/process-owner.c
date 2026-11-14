#define _POSIX_C_SOURCE 200809L
#include "extension/process-owner.h"
#include "extension/process-guard.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

struct gt_ext_process {
    pid_t pid;
    pid_t group;
    int control_fd, report_fd;
    gt_process_guard_message_t report;
    size_t report_size;
    bool observed, released, final;
    int group_error;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    bool exited;
    int exit_code;
    int term_signal;
};

static gt_ext_process_io_t
process_io(gt_ext_process_io_state_t state, size_t count, gt_status_t error) {
    return (gt_ext_process_io_t){state, count, error};
}

static int pipe_cloexec(int fds[2]) {
    if (pipe(fds)) {
        return -1;
    }
    for (size_t i = 0; i < 2; i++) {
        if (fds[i] < 3) {
            int copy = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
            if (copy < 0) {
                close(fds[0]);
                close(fds[1]);
                fds[0] = fds[1] = -1;
                return -1;
            }
            close(fds[i]);
            fds[i] = copy;
        }
        int flags = fcntl(fds[i], F_GETFD);
        if (flags < 0 || fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            int error = errno;
            close(fds[0]);
            close(fds[1]);
            fds[0] = fds[1] = -1;
            errno = error;
            return -1;
        }
    }
    return 0;
}

static void close_pipe(int fds[2]) {
    for (size_t i = 0; i < 2; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static gt_ext_process_io_t io_failure(int error) {
    (void)error;
    return process_io(GT_EXT_PROCESS_ERROR, 0, GT_ERR_IO);
}

static ssize_t write_without_sigpipe(int fd, const void *data, size_t size) {
    sigset_t block, previous, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &block, &previous)) {
        errno = EIO;
        return -1;
    }
    bool already_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
    ssize_t result = write(fd, data, size);
    int saved = errno;
    if (result < 0 && saved == EPIPE && !already_pending) {
        sigset_t pending_after;
        int generated = 0;
        if (sigpending(&pending_after) == 0 && sigismember(&pending_after, SIGPIPE) == 1) {
            (void)sigwait(&block, &generated);
        }
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
    errno = saved;
    return result;
}

static bool arguments_valid(const char *exe, const char *const *argv, size_t argc) {
    if (!exe || exe[0] != '/' || !argv || !argc || argc > GT_EXT_PROCESS_ARGUMENTS_MAX ||
        strnlen(exe, 4097) > 4096) {
        return false;
    }
    size_t total = 0;
    for (size_t i = 0; i < argc; i++) {
        if (!argv[i]) {
            return false;
        }
        size_t length = strnlen(argv[i], GT_EXT_PROCESS_ARGUMENT_MAX + 1u);
        if ((!length && i == 0) || length > GT_EXT_PROCESS_ARGUMENT_MAX ||
            total > GT_EXT_PROCESS_ARGUMENT_BYTES_MAX - (length + 1u)) {
            return false;
        }
        total += length + 1u;
    }
    return argv[0][0] != '\0';
}

bool gt__ext_process_environment_valid(const char *const *environment, size_t count) {
    if ((count && !environment) || count > GT_EXT_PROCESS_ENVIRONMENT_MAX) {
        return false;
    }
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        const char *entry = environment[i];
        if (!entry) {
            return false;
        }
        size_t size = strnlen(entry, GT_EXT_PROCESS_ENVIRONMENT_VALUE_MAX + 1u);
        const char *equal = memchr(entry, '=', size);
        if (!equal || equal == entry || size > GT_EXT_PROCESS_ENVIRONMENT_VALUE_MAX ||
            size + 1 > GT_EXT_PROCESS_ENVIRONMENT_BYTES_MAX - total) {
            return false;
        }
        size_t key_size = (size_t)(equal - entry);
        for (size_t j = 0; j < key_size; j++) {
            char c = entry[j];
            bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                         (j > 0 && c >= '0' && c <= '9');
            if (!valid) {
                return false;
            }
        }
        for (size_t j = 0; j < i; j++) {
            const char *previous_equal = strchr(environment[j], '=');
            if (previous_equal && (size_t)(previous_equal - environment[j]) == key_size &&
                !memcmp(environment[j], entry, key_size)) {
                return false;
            }
        }
        total += size + 1;
    }
    return true;
}

gt_status_t gt__ext_process_spawn(const char *executable,
                                  const char *const *argv,
                                  size_t argc,
                                  int cwd_fd,
                                  const char *const *environment,
                                  size_t environment_count,
                                  gt_ext_process_t **out) {
    if (out) {
        *out = NULL;
    }
    struct stat cwd_stat;
    if (!out || !arguments_valid(executable, argv, argc) ||
        !gt__ext_process_environment_valid(environment, environment_count) || cwd_fd < 0 ||
        fstat(cwd_fd, &cwd_stat) || !S_ISDIR(cwd_stat.st_mode)) {
        return GT_ERR_INVALID_ARG;
    }
    int input[2] = {-1, -1};      /* child reads [0], parent writes [1] */
    int output[2] = {-1, -1};     /* child writes [1], parent reads [0] */
    int errors[2] = {-1, -1};     /* child stderr */
    int exec_error[2] = {-1, -1}; /* child writes exec errno; CLOEXEC EOF means exec */
    int control[2] = {-1, -1}, reports[2] = {-1, -1};
    if (pipe_cloexec(input) || pipe_cloexec(output) || pipe_cloexec(errors) ||
        pipe_cloexec(exec_error) || pipe_cloexec(control) || pipe_cloexec(reports)) {
        close_pipe(input);
        close_pipe(output);
        close_pipe(errors);
        close_pipe(exec_error);
        close_pipe(control);
        close_pipe(reports);
        return GT_ERR_IO;
    }

    long max_open_fd = sysconf(_SC_OPEN_MAX);
    if (max_open_fd < 0) {
        max_open_fd = 65536;
    }
    if (max_open_fd > INT_MAX) {
        max_open_fd = INT_MAX;
    }
    int max_fd = (int)max_open_fd;
    gt_ext_process_t *process = calloc(1, sizeof(*process));
    char **child_argv = calloc(argc + 1, sizeof(*child_argv));
    char **child_environment = calloc(environment_count + 1, sizeof(*child_environment));
    if (!process || !child_argv || !child_environment) {
        free(process);
        free(child_argv);
        free(child_environment);
        close_pipe(input);
        close_pipe(output);
        close_pipe(errors);
        close_pipe(exec_error);
        close_pipe(control);
        close_pipe(reports);
        return GT_ERR_IO;
    }
    process->stdin_fd = process->stdout_fd = process->stderr_fd = -1;
    process->control_fd = process->report_fd = -1;
    for (size_t i = 0; i < argc; i++) {
        child_argv[i] = (char *)argv[i];
    }
    for (size_t i = 0; i < environment_count; i++) {
        child_environment[i] = (char *)environment[i];
    }

    pid_t pid = fork();
    if (pid == 0) {
        gt__process_guard_run(executable,
                              child_argv,
                              child_environment,
                              cwd_fd,
                              input[0],
                              output[1],
                              errors[1],
                              exec_error[1],
                              control[0],
                              reports[1],
                              max_fd);
    }
    free(child_argv);
    free(child_environment);
    if (pid < 0) {
        close_pipe(input);
        close_pipe(output);
        close_pipe(errors);
        close_pipe(exec_error);
        close_pipe(control);
        close_pipe(reports);
        free(process);
        return GT_ERR_IO;
    }
    (void)setpgid(pid, pid);
    close(input[0]);
    input[0] = -1;
    close(output[1]);
    output[1] = -1;
    close(errors[1]);
    errors[1] = -1;
    close(exec_error[1]);
    exec_error[1] = -1;
    close(control[0]);
    control[0] = -1;
    close(reports[1]);
    reports[1] = -1;

    int child_error = 0;
    int handshake_error = 0;
    size_t got = 0;
    while (got < sizeof(child_error)) {
        ssize_t count = read(exec_error[0], (char *)&child_error + got, sizeof(child_error) - got);
        if (count > 0) {
            got += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0) {
            handshake_error = errno;
            break;
        } else {
            break;
        }
    }
    close(exec_error[0]);
    exec_error[0] = -1;
    gt_process_guard_message_t start = {0};
    if (!got && !handshake_error) {
        size_t size = 0;
        while (size < sizeof(start)) {
            ssize_t n = read(reports[0], (char *)&start + size, sizeof(start) - size);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                handshake_error = EIO;
                break;
            }
            size += (size_t)n;
        }
        if (start.kind != GT_PROCESS_GUARD_START || start.pid <= 0 || start.pid == pid) {
            handshake_error = EIO;
        }
    }
    if (got || handshake_error) {
        close_pipe(control);
        close_pipe(reports);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        close_pipe(input);
        close_pipe(output);
        close_pipe(errors);
        free(process);
        return child_error == ENOENT && !handshake_error ? GT_ERR_NOT_FOUND : GT_ERR_IO;
    }
    if (set_nonblocking(input[1]) || set_nonblocking(output[0]) || set_nonblocking(errors[0]) ||
        set_nonblocking(control[1]) || set_nonblocking(reports[0])) {
        int saved = errno;
        (void)kill(-start.pid, SIGKILL);
        close_pipe(control);
        close_pipe(reports);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        close_pipe(input);
        close_pipe(output);
        close_pipe(errors);
        free(process);
        errno = saved;
        return GT_ERR_IO;
    }
    process->pid = pid;
    process->group = (pid_t)start.pid;
    process->control_fd = control[1];
    process->report_fd = reports[0];
    process->stdin_fd = input[1];
    input[1] = -1;
    process->stdout_fd = output[0];
    output[0] = -1;
    process->stderr_fd = errors[0];
    errors[0] = -1;
    *out = process;
    return GT_OK;
}

gt_ext_process_io_t
gt__ext_process_write(gt_ext_process_t *process, const void *data, size_t size) {
    if (!process || (size && !data) || size > GT_EXT_PROCESS_IO_MAX) {
        return process_io(GT_EXT_PROCESS_ERROR, 0, GT_ERR_INVALID_ARG);
    }
    if (process->stdin_fd < 0) {
        return process_io(GT_EXT_PROCESS_CLOSED, 0, GT_ERR_IO);
    }
    if (!size) {
        return process_io(GT_EXT_PROCESS_DATA, 0, GT_OK);
    }
    ssize_t count = write_without_sigpipe(process->stdin_fd, data, size);
    if (count > 0) {
        return process_io(GT_EXT_PROCESS_DATA, (size_t)count, GT_OK);
    }
    if (count == 0) {
        return process_io(GT_EXT_PROCESS_WAIT, 0, GT_OK);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return process_io(GT_EXT_PROCESS_WAIT, 0, GT_OK);
    }
    return io_failure(errno);
}

static gt_ext_process_io_t process_read(int *fd, void *buffer, size_t size) {
    if (!fd || !buffer || !size || size > GT_EXT_PROCESS_IO_MAX) {
        return process_io(GT_EXT_PROCESS_ERROR, 0, GT_ERR_INVALID_ARG);
    }
    if (*fd < 0) {
        return process_io(GT_EXT_PROCESS_EOF, 0, GT_OK);
    }
    ssize_t count = read(*fd, buffer, size);
    if (count > 0) {
        return process_io(GT_EXT_PROCESS_DATA, (size_t)count, GT_OK);
    }
    if (!count) {
        close(*fd);
        *fd = -1;
        return process_io(GT_EXT_PROCESS_EOF, 0, GT_OK);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return process_io(GT_EXT_PROCESS_WAIT, 0, GT_OK);
    }
    return io_failure(errno);
}

gt_ext_process_io_t
gt__ext_process_read_stdout(gt_ext_process_t *process, void *buffer, size_t size) {
    return process ? process_read(&process->stdout_fd, buffer, size)
                   : process_io(GT_EXT_PROCESS_ERROR, 0, GT_ERR_INVALID_ARG);
}

gt_ext_process_io_t
gt__ext_process_read_stderr(gt_ext_process_t *process, void *buffer, size_t size) {
    return process ? process_read(&process->stderr_fd, buffer, size)
                   : process_io(GT_EXT_PROCESS_ERROR, 0, GT_ERR_INVALID_ARG);
}

gt_status_t
gt__ext_process_wait(gt_ext_process_t *process, unsigned interests, int wake_fd, int ms) {
    if (!process || (interests & ~7u) || wake_fd < -1 || ms < 0) {
        return GT_ERR_INVALID_ARG;
    }
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return GT_ERR_IO;
    }
    uint64_t end = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000 + (unsigned)ms;
    for (;;) {
        struct pollfd fds[] = {
            {(interests & GT_EXT_PROCESS_WAIT_INPUT) ? process->stdin_fd : -1, POLLOUT, 0},
            {(interests & GT_EXT_PROCESS_WAIT_OUTPUT) ? process->stdout_fd : -1, POLLIN, 0},
            {(interests & GT_EXT_PROCESS_WAIT_ERROR) ? process->stderr_fd : -1, POLLIN, 0},
            {process->report_fd, POLLIN, 0},
            {wake_fd, POLLIN, 0}};
        if (clock_gettime(CLOCK_MONOTONIC, &t)) {
            return GT_ERR_IO;
        }
        uint64_t now = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
        int ready = poll(fds, 5, now < end ? (int)(end - now) : 0);
        if (ready < 0 && errno == EINTR) {
            if (now >= end) {
                return GT_OK;
            }
            continue;
        }
        if (ready < 0) {
            return GT_ERR_IO;
        }
        for (size_t i = 0; i < 5; i++) {
            if (fds[i].revents & POLLNVAL) {
                return GT_ERR_IO;
            }
        }
        return GT_OK;
    }
}

gt_status_t gt__ext_process_close_stdin(gt_ext_process_t *process) {
    if (!process) {
        return GT_ERR_INVALID_ARG;
    }
    if (process->stdin_fd >= 0) {
        close(process->stdin_fd);
        process->stdin_fd = -1;
    }
    return GT_OK;
}

/* Fixed-size lifecycle records only; terminal/process bytes bypass this pipe. */
static gt_status_t process_reports(gt_ext_process_t *process) {
    while (process->report_fd >= 0) {
        ssize_t n = read(process->report_fd,
                         (char *)&process->report + process->report_size,
                         sizeof(process->report) - process->report_size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return GT_OK;
        }
        if (n == 0 && process->final && !process->report_size) {
            close(process->report_fd);
            process->report_fd = -1;
            return GT_OK;
        }
        if (n <= 0) {
            return GT_ERR_IO;
        }
        process->report_size += (size_t)n;
        if (process->report_size != sizeof(process->report)) {
            continue;
        }
        gt_process_guard_message_t *m = &process->report;
        if (m->pid != process->group || m->exit_code < 0 || m->exit_code > 255 ||
            m->term_signal < 0 || (m->exit_code && m->term_signal) || m->group_error < 0) {
            return GT_ERR_IO;
        }
        if (m->kind == GT_PROCESS_GUARD_EXIT && !process->observed && !process->released) {
            process->observed = true;
            process->exit_code = m->exit_code;
            process->term_signal = m->term_signal;
        } else if (m->kind == GT_PROCESS_GUARD_FINAL && process->observed && process->released &&
                   !process->final && m->exit_code == process->exit_code &&
                   m->term_signal == process->term_signal) {
            process->final = true;
            process->group_error = m->group_error;
        } else {
            return GT_ERR_IO;
        }
        process->report_size = 0;
    }
    return GT_OK;
}

bool gt__ext_process_reaping(const gt_ext_process_t *process) {
    return process && process->released;
}

int gt__ext_process_group_error(const gt_ext_process_t *process) {
    return process ? process->group_error : 0;
}

gt_status_t gt__ext_process_signal(gt_ext_process_t *process, int signal_number) {
    struct sigaction action;
    if (!process || signal_number <= 0 ||
        (signal_number != SIGKILL && signal_number != SIGSTOP &&
         sigaction(signal_number, NULL, &action))) {
        return GT_ERR_INVALID_ARG;
    }
    if (process->exited || process->released) {
        errno = ESRCH;
        return GT_ERR_NOT_FOUND;
    }
    if (process_reports(process) != GT_OK) {
        return GT_ERR_IO;
    }
    /* The monitor keeps the executable unreaped until our release command.
     * Refuse known monitor failure rather than signal an unreserved group. */
    siginfo_t monitor;
    memset(&monitor, 0, sizeof(monitor));
    if (waitid(P_PID, (id_t)process->pid, &monitor, WEXITED | WNOHANG | WNOWAIT) ||
        monitor.si_pid) {
        return GT_ERR_IO;
    }
    if (kill(-process->group, signal_number) == 0 || errno == ESRCH) {
        return GT_OK;
    }
    return GT_ERR_IO;
}

gt_status_t gt__ext_process_poll(gt_ext_process_t *process, gt_ext_process_info_t *out) {
    if (!process || !out) {
        return GT_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (process_reports(process) != GT_OK) {
        return GT_ERR_IO;
    }
    if (process->observed && !process->released) {
        const char command = 1;
        process->released = true;
        if (write_without_sigpipe(process->control_fd, &command, 1) != 1) {
            return GT_ERR_IO;
        }
    }
    if (!process->exited && process->final) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(process->pid, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 || (waited > 0 && (!WIFEXITED(status) || WEXITSTATUS(status)))) {
            return GT_ERR_IO;
        }
        process->exited = waited == process->pid;
    }
    *out = (gt_ext_process_info_t){process->exited, process->exit_code, process->term_signal};
    return GT_OK;
}

gt_status_t gt__ext_process_observe_exit(gt_ext_process_t *process, gt_ext_process_info_t *out) {
    if (!process || !out) {
        return GT_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (process_reports(process) != GT_OK) {
        return GT_ERR_IO;
    }
    *out = (gt_ext_process_info_t){process->observed, process->exit_code, process->term_signal};
    return GT_OK;
}

gt_status_t gt__ext_process_destroy(gt_ext_process_t **owner) {
    if (!owner || !*owner) {
        return GT_ERR_INVALID_ARG;
    }
    gt_ext_process_t *process = *owner;
    if (!process->exited) {
        return GT_ERR_IO;
    }
    (void)gt__ext_process_close_stdin(process);
    if (process->stdout_fd >= 0) {
        close(process->stdout_fd);
    }
    if (process->stderr_fd >= 0) {
        close(process->stderr_fd);
    }
    if (process->control_fd >= 0) {
        close(process->control_fd);
    }
    if (process->report_fd >= 0) {
        close(process->report_fd);
    }
    memset(process, 0, sizeof(*process));
    free(process);
    *owner = NULL;
    return GT_OK;
}
