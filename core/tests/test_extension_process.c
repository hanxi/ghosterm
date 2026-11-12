#define _POSIX_C_SOURCE 200809L
#include "extension/process-owner.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "ASSERT: %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static gt_ext_process_t *active_process;
static void pause_ms(long milliseconds);

static void cleanup_process(void) {
    if (!active_process) {
        return;
    }
    (void)gt__ext_process_signal(active_process, SIGKILL);
    gt_ext_process_info_t info;
    for (int i = 0; i < 100; i++) {
        if (gt__ext_process_poll(active_process, &info) != GT_OK || info.exited) {
            break;
        }
        pause_ms(5);
    }
    (void)gt__ext_process_destroy(&active_process);
}

static gt_status_t
spawn(const char *executable, const char *const *args, size_t argc, gt_ext_process_t **out) {
    gt_status_t status = gt__ext_process_spawn(executable, args, argc, out);
    if (status == GT_OK) {
        active_process = *out;
    }
    return status;
}

static gt_status_t destroy(gt_ext_process_t **process) {
    gt_status_t status = gt__ext_process_destroy(process);
    if (status == GT_OK) {
        active_process = NULL;
    }
    return status;
}

static void pause_ms(long milliseconds) {
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {
    }
}

static int spawn_checks(void) {
    gt_ext_process_t *process = NULL;
    const char *args[] = {"cat"};
    CHECK(gt__ext_process_spawn("bin/cat", args, 1, &process) == GT_ERR_INVALID_ARG);
    CHECK(process == NULL);
    const char *too_many[GT_EXT_PROCESS_ARGUMENTS_MAX + 1];
    for (size_t i = 0; i < GT_EXT_PROCESS_ARGUMENTS_MAX + 1; i++) {
        too_many[i] = "x";
    }
    CHECK(gt__ext_process_spawn("/bin/cat", too_many, GT_EXT_PROCESS_ARGUMENTS_MAX + 1, &process) ==
          GT_ERR_INVALID_ARG);
    CHECK(process == NULL);
    CHECK(gt__ext_process_spawn("/ghosterm/codex-does-not-exist", args, 1, &process) ==
          GT_ERR_NOT_FOUND);
    CHECK(process == NULL);
    return 0;
}

static int roundtrip(void) {
    gt_ext_process_t *process = NULL;
    const char *args[] = {"cat"};
    CHECK(spawn("/bin/cat", args, 1, &process) == GT_OK);
    const char input[] = "app-server pipe round trip\n";
    gt_ext_process_io_t io;
    size_t sent = 0;
    for (int i = 0; i < 100 && sent < sizeof(input) - 1; i++) {
        io = gt__ext_process_write(process, input + sent, sizeof(input) - 1 - sent);
        if (io.state == GT_EXT_PROCESS_DATA) {
            sent += io.count;
        } else if (io.state == GT_EXT_PROCESS_WAIT) {
            pause_ms(5);
        } else {
            CHECK(false);
        }
    }
    CHECK(sent == sizeof(input) - 1);
    char output[128];
    size_t received = 0;
    for (int i = 0; i < 100 && received < sizeof(input) - 1; i++) {
        io = gt__ext_process_read_stdout(process, output + received, sizeof(output) - received);
        if (io.state == GT_EXT_PROCESS_DATA) {
            received += io.count;
        } else if (io.state == GT_EXT_PROCESS_WAIT) {
            pause_ms(5);
        } else {
            CHECK(false);
        }
    }
    CHECK(received == sizeof(input) - 1);
    CHECK(!memcmp(output, input, received));
    CHECK(gt__ext_process_close_stdin(process) == GT_OK);

    gt_ext_process_info_t info;
    for (int i = 0; i < 100; i++) {
        CHECK(gt__ext_process_poll(process, &info) == GT_OK);
        if (info.exited) {
            break;
        }
        pause_ms(5);
    }
    CHECK(info.exited && info.exit_code == 0 && info.term_signal == 0);
    io = gt__ext_process_read_stdout(process, output, sizeof(output));
    CHECK(io.state == GT_EXT_PROCESS_EOF);
    io = gt__ext_process_read_stderr(process, output, sizeof(output));
    CHECK(io.state == GT_EXT_PROCESS_EOF);
    CHECK(destroy(&process) == GT_OK && process == NULL);
    return 0;
}

static int broken_pipe(void) {
    gt_ext_process_t *process = NULL;
    const char *args[] = {"true"};
    CHECK(spawn("/usr/bin/true", args, 1, &process) == GT_OK);
    gt_ext_process_info_t info;
    for (int i = 0; i < 100; i++) {
        CHECK(gt__ext_process_poll(process, &info) == GT_OK);
        if (info.exited) {
            break;
        }
        pause_ms(5);
    }
    CHECK(info.exited && info.exit_code == 0);
    const char byte = 'x';
    gt_ext_process_io_t io = gt__ext_process_write(process, &byte, 1);
    CHECK(io.state == GT_EXT_PROCESS_ERROR && io.error == GT_ERR_IO);
    CHECK(destroy(&process) == GT_OK && process == NULL);
    return 0;
}

static int terminate(void) {
    gt_ext_process_t *process = NULL;
    const char *args[] = {"sleep", "30"};
    struct sigaction previous, ignored;
    memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    CHECK(sigaction(SIGTERM, &ignored, &previous) == 0);
    CHECK(spawn("/bin/sleep", args, 2, &process) == GT_OK);
    CHECK(sigaction(SIGTERM, &previous, NULL) == 0);
    CHECK(gt__ext_process_destroy(&process) == GT_ERR_IO);
    CHECK(gt__ext_process_close_stdin(process) == GT_OK);
    CHECK(gt__ext_process_signal(process, SIGTERM) == GT_OK);
    gt_ext_process_info_t info;
    for (int i = 0; i < 200; i++) {
        CHECK(gt__ext_process_poll(process, &info) == GT_OK);
        if (info.exited) {
            break;
        }
        pause_ms(5);
    }
    CHECK(info.exited && info.term_signal == SIGTERM);
    CHECK(destroy(&process) == GT_OK && process == NULL);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    (void)atexit(cleanup_process);
    if (!strcmp(argv[1], "spawn")) {
        return spawn_checks();
    }
    if (!strcmp(argv[1], "roundtrip")) {
        return roundtrip();
    }
    if (!strcmp(argv[1], "broken-pipe")) {
        return broken_pipe();
    }
    if (!strcmp(argv[1], "terminate")) {
        return terminate();
    }
    return 2;
}
