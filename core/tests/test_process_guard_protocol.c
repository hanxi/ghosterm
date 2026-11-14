/* Validate the private lifecycle parser without creating/signalling a PID. */
#include "../src/extension/process-owner.c"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    int fds[2];
    if (pipe_cloexec(fds) || set_nonblocking(fds[0])) {
        return 2;
    }
    gt_ext_process_t process = {.pid = -1,
                                .group = 4242,
                                .stdin_fd = -1,
                                .stdout_fd = -1,
                                .stderr_fd = -1,
                                .control_fd = -1,
                                .report_fd = fds[0]};
    gt_process_guard_message_t message = {
        .kind = GT_PROCESS_GUARD_EXIT, .pid = 4242, .exit_code = 37};
    size_t size = sizeof(message);
    bool good = false;
    if (!strcmp(argv[1], "kind")) {
        message.kind = GT_PROCESS_GUARD_START;
    } else if (!strcmp(argv[1], "pid")) {
        message.pid++;
    } else if (!strcmp(argv[1], "range")) {
        message.exit_code = 256;
    } else if (!strcmp(argv[1], "conflict")) {
        message.term_signal = SIGTERM;
    } else if (!strcmp(argv[1], "truncated")) {
        size--;
    } else if (!strcmp(argv[1], "eof")) {
        size = 0;
    } else if (!strcmp(argv[1], "early-final")) {
        message.kind = GT_PROCESS_GUARD_FINAL;
    } else if (!strcmp(argv[1], "final")) {
        process.observed = process.released = true;
        process.exit_code = 37;
        message.kind = GT_PROCESS_GUARD_FINAL;
        good = true;
    } else {
        return 2;
    }
    bool ended = !strcmp(argv[1], "truncated") || !strcmp(argv[1], "eof");
    if (write(fds[1], &message, size) != (ssize_t)size || (ended && close(fds[1]))) {
        close(fds[0]);
        return 2;
    }
    gt_ext_process_info_t info;
    gt_status_t status = gt__ext_process_observe_exit(&process, &info);
    if (!ended) {
        close(fds[1]);
    }
    bool accepted = good ? status == GT_OK && info.exited && info.exit_code == 37 &&
                               !info.term_signal && process.final
                         : status == GT_ERR_IO && !info.exited;
    if (process.report_fd >= 0) {
        close(process.report_fd);
    }
    if (!accepted) {
        fputs("ASSERT: lifecycle record must preserve identity/order/unknown status\n", stderr);
        return 1;
    }
    return 0;
}
