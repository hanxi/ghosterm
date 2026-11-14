#define _POSIX_C_SOURCE 200809L
#include "extension/process-guard.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t guard_wakeup = -1;

static void guard_child_event(int number) {
    (void)number;
    int saved = errno;
    const char byte = 1;
    if (guard_wakeup >= 0) {
        (void)write((int)guard_wakeup, &byte, 1);
    }
    errno = saved;
}

static bool guard_report(int fd, const gt_process_guard_message_t *message) {
    const char *bytes = (const char *)message;
    size_t offset = 0;
    while (offset < sizeof(*message)) {
        ssize_t n = write(fd, bytes + offset, sizeof(*message) - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        offset += (size_t)n;
    }
    return true;
}

static uint64_t guard_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void guard_action(int signal_number, void (*handler)(int)) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = handler;
    (void)sigaction(signal_number, &action, NULL);
}

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
                                     int max_fd) {
    sigset_t empty;
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    guard_action(SIGCHLD, SIG_DFL);
    guard_action(SIGPIPE, SIG_IGN);
    guard_action(SIGTERM, SIG_IGN);
    guard_action(SIGINT, SIG_IGN);
    guard_action(SIGHUP, SIG_IGN);
    guard_action(SIGQUIT, SIG_IGN);
    if (setpgid(0, 0) || fchdir(cwd)) {
        int error = errno;
        (void)write(exec_error, &error, sizeof(error));
        _exit(126);
    }
    // Do not inherit other owners' liveness writers into this monitor.
    for (int fd = 3; fd < max_fd; fd++) {
        if (fd != input && fd != output && fd != errors && fd != exec_error && fd != control &&
            fd != reports) {
            (void)close(fd);
        }
    }
    int wake[2];
    if (pipe(wake)) {
        int error = errno;
        (void)write(exec_error, &error, sizeof(error));
        _exit(126);
    }
    for (size_t i = 0; i < 2; i++) {
        if (wake[i] < 3) {
            int copy = fcntl(wake[i], F_DUPFD_CLOEXEC, 3);
            if (copy < 0) {
                int error = errno;
                (void)write(exec_error, &error, sizeof(error));
                _exit(126);
            }
            close(wake[i]);
            wake[i] = copy;
        }
    }
    if (fcntl(wake[0], F_SETFD, FD_CLOEXEC) || fcntl(wake[1], F_SETFD, FD_CLOEXEC) ||
        fcntl(wake[0], F_SETFL, O_NONBLOCK) || fcntl(wake[1], F_SETFL, O_NONBLOCK)) {
        int error = errno;
        (void)write(exec_error, &error, sizeof(error));
        _exit(126);
    }
    guard_wakeup = wake[1];
    guard_action(SIGCHLD, guard_child_event);
    pid_t child = fork();
    if (child == 0) {
        guard_action(SIGCHLD, SIG_DFL);
        guard_action(SIGPIPE, SIG_DFL);
        guard_action(SIGTERM, SIG_DFL);
        guard_action(SIGINT, SIG_DFL);
        guard_action(SIGHUP, SIG_DFL);
        guard_action(SIGQUIT, SIG_DFL);
        if (setpgid(0, 0) || dup2(input, STDIN_FILENO) < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0) {
            int error = errno;
            (void)write(exec_error, &error, sizeof(error));
            _exit(126);
        }
        for (int fd = 3; fd < max_fd; fd++) {
            if (fd != exec_error) {
                (void)close(fd);
            }
        }
        execve(executable, argv, environment);
        int error = errno;
        (void)write(exec_error, &error, sizeof(error));
        _exit(127);
    }
    if (child < 0) {
        int error = errno;
        (void)write(exec_error, &error, sizeof(error));
        _exit(126);
    }
    (void)setpgid(child, child);
    gt_process_guard_message_t message = {.kind = GT_PROCESS_GUARD_START, .pid = child};
    bool connected = guard_report(reports, &message);
    close(exec_error);
    close(input);
    close(output);
    close(errors);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    bool observed = false, stopping = false, killed = false, released = false;
    uint64_t stop_at = 0;
    for (;;) {
        siginfo_t state;
        memset(&state, 0, sizeof(state));
        int result = waitid(P_PID, (id_t)child, &state, WEXITED | WNOHANG | WNOWAIT);
        if (result && errno != EINTR) {
            // Never signal after losing the child's wait/identity ownership.
            _exit(125);
        }
        if (!result && state.si_pid && !observed) {
            observed = true;
            message.kind = GT_PROCESS_GUARD_EXIT;
            if (state.si_code == CLD_EXITED) {
                message.exit_code = state.si_status;
            } else if (state.si_code == CLD_KILLED || state.si_code == CLD_DUMPED) {
                message.term_signal = state.si_status;
            } else {
                _exit(125);
            }
            connected = connected && guard_report(reports, &message);
        }
        if (!connected && !stopping) {
            stopping = true;
            stop_at = guard_now();
            (void)kill(-child, SIGTERM);
        }
        uint64_t current = guard_now();
        int timeout = -1;
        if (stopping && !killed) {
            timeout = !current || !stop_at || current - stop_at >= 100
                          ? 0
                          : (int)(100 - (current - stop_at));
        }
        struct pollfd requests[] = {{connected ? control : -1, POLLIN, 0}, {wake[0], POLLIN, 0}};
        int ready = poll(requests, 2, timeout);
        if (ready < 0 && errno != EINTR) {
            connected = false;
        }
        if (ready > 0 && requests[1].revents) {
            char bytes[64];
            while (read(wake[0], bytes, sizeof(bytes)) > 0) {
            }
        }
        if (ready > 0 && requests[0].revents) {
            char command;
            ssize_t n = read(control, &command, 1);
            if (n == 1 && command == 1 && observed) {
                released = true;
            } else if (n == 0 || (n < 0 && errno != EINTR)) {
                connected = false;
            } else if (n == 1) {
                connected = false;
            }
        }
        if (!connected && !stopping) {
            stopping = true;
            stop_at = guard_now();
            (void)kill(-child, SIGTERM);
        }
        current = guard_now();
        if (released || observed ||
            (stopping && (!current || !stop_at || current - stop_at >= 100))) {
            if ((released || !connected) && !killed) {
                if (kill(-child, SIGKILL) && errno != ESRCH) {
                    message.group_error = errno;
                }
                killed = true;
            }
        }
        if (observed && (released || !connected)) {
            int status;
            pid_t waited;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited != child) {
                _exit(125);
            }
            message.kind = GT_PROCESS_GUARD_FINAL;
            if (connected) {
                (void)guard_report(reports, &message);
            }
            close(control);
            close(reports);
            close(wake[0]);
            close(wake[1]);
            _exit(0);
        }
    }
}
