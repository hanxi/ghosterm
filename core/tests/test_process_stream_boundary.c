#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "extension/process-stream.h"
#include "extension/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* Compile the exact provider against controlled private syscall seams. The
 * process owner and common stream are the same archive as configuration control. */
static gt_status_t controlled_signal(gt_ext_process_t *, int);
static gt_status_t controlled_poll(gt_ext_process_t *, gt_ext_process_info_t *);
static gt_ext_process_io_t controlled_stderr(gt_ext_process_t *, void *, size_t);
static gt_status_t controlled_wait(gt_ext_process_t *, unsigned, int, int);
#define gt__ext_process_signal controlled_signal
#define gt__ext_process_poll controlled_poll
#define gt__ext_process_read_stderr controlled_stderr
#define gt__ext_process_wait controlled_wait
#include "../src/extension/process-stream.c"
#undef gt__ext_process_signal
#undef gt__ext_process_poll
#undef gt__ext_process_read_stderr
#undef gt__ext_process_wait

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL process_stream_boundary %s:%d: %s\n", __func__, __LINE__, #x);   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static gt_bridge_stream_t *active;
static bool deny_live, fail_poll, fail_stderr, fail_wait;
static int group_error;
static unsigned signal_calls;

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void delay(void) {
    struct timespec t = {0, 1000000};
    (void)nanosleep(&t, NULL);
}

static gt_status_t controlled_signal(gt_ext_process_t *p, int number) {
    signal_calls++;
    if (deny_live && (number == SIGTERM || number == SIGKILL)) {
        errno = EPERM;
        return GT_ERR_IO;
    }
    if (group_error && number == SIGKILL) {
        gt_ext_process_info_t i;
        CHECK(gt__ext_process_observe_exit(p, &i) == GT_OK && i.exited);
        errno = group_error;
        group_error = 0;
        return GT_ERR_IO;
    }
    return gt__ext_process_signal(p, number);
}

static gt_status_t controlled_poll(gt_ext_process_t *p, gt_ext_process_info_t *out) {
    if (fail_poll) {
        fail_poll = false;
        errno = EIO;
        return GT_ERR_IO;
    }
    return gt__ext_process_poll(p, out);
}

static gt_ext_process_io_t controlled_stderr(gt_ext_process_t *p, void *bytes, size_t size) {
    if (fail_stderr) {
        fail_stderr = false;
        return (gt_ext_process_io_t){GT_EXT_PROCESS_ERROR, 0, GT_ERR_IO};
    }
    return gt__ext_process_read_stderr(p, bytes, size);
}

static gt_status_t controlled_wait(gt_ext_process_t *p, unsigned interests, int wake_fd, int ms) {
    if (fail_wait) {
        fail_wait = false;
        return GT_ERR_IO;
    }
    return gt__ext_process_wait(p, interests, wake_fd, ms);
}

static void cleanup(void) {
    deny_live = fail_poll = fail_stderr = fail_wait = false;
    group_error = 0;
    if (active) {
        uint64_t end = now() + 9000;
        while (now() < end) {
            if (gt__bridge_stream_close(active, 1) == GT_BRIDGE_RELEASED) {
                (void)gt__bridge_stream_destroy(active);
                active = NULL;
                break;
            }
            delay();
        }
    }
}

static gt_process_stream_info_t snapshot(void) {
    gt_process_stream_info_t i;
    CHECK(gt__bridge_process_info(active, &i));
    return i;
}

static gt_bridge_io_t read_one(int ms) {
    char byte;
    return gt__bridge_stream_read(active, &byte, 1, ms);
}

int main(int argc, char **argv) {
    CHECK(argc == 4);
    CHECK(!atexit(cleanup));
    bool running = !strcmp(argv[1], "running-denied");
    bool holding = running || !strcmp(argv[1], "stderr-error") || !strcmp(argv[1], "wait-error") ||
                   !strcmp(argv[1], "first-error");
    char executable[4096];
    CHECK(realpath(argv[2], executable));
    const char *arguments[] = {executable,
                               "--child",
                               running   ? "ignore"
                               : holding ? "hold"
                                         : "release-exit",
                               "",
                               "中文😀'$(literal)"};
    char cwd[4096], entry[4200];
    CHECK(realpath(argv[3], cwd));
    CHECK(snprintf(entry, sizeof(entry), "GT_PROCESS_STREAM_CWD=%s", cwd) > 0);
    const char *environment[] = {
        "GT_PROCESS_STREAM_SELECTED=approved",
        entry,
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0",
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:symbolize=0",
#endif
    };
    int fd = open(cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    gt_process_stream_config_t c = {.executable = executable,
                                    .arguments = arguments,
                                    .argument_count = 5,
                                    .cwd_fd = fd,
                                    .environment = environment,
                                    .environment_count =
                                        sizeof(environment) / sizeof(environment[0]),
                                    .execution_authorized = true,
                                    .stop_grace_ms = 50,
                                    .lifetime_ms = 2000};
    CHECK(gt__bridge_process_open(&c, &active) == GT_OK);
    close(fd);
    CHECK(read_one(1000).state == GT_BRIDGE_DATA);
    if (running) {
        deny_live = true;
        CHECK(gt__bridge_stream_close(active, 0) == GT_BRIDGE_DRAINING);
        CHECK(!snapshot().reaped && !snapshot().group_fenced &&
              snapshot().group_signal_error == EPERM);
        uint64_t end = now() + 1000;
        gt_bridge_close_t status;
        do {
            status = gt__bridge_stream_close(active, 0);
            if (status == GT_BRIDGE_DRAINING) {
                delay();
            }
        } while (status == GT_BRIDGE_DRAINING && now() < end);
        CHECK(status == GT_BRIDGE_CLOSE_ERROR);
        CHECK(snapshot().cleanup_error == GT_ERR_IO && !snapshot().reaped);
        CHECK(gt__plugin_memory_info().used > 0 &&
              gt__bridge_stream_destroy(active) == GT_ERR_INVALID_ARG);
        deny_live = false;
    } else {
        if (!strcmp(argv[1], "dead-group-unknown")) {
            group_error = EPERM;
        } else if (!strcmp(argv[1], "dead-group-io")) {
            group_error = EIO;
        } else if (!strcmp(argv[1], "poll-retry")) {
            fail_poll = true;
        } else if (!strcmp(argv[1], "stderr-error") || !strcmp(argv[1], "first-error")) {
            fail_stderr = true;
        } else if (!strcmp(argv[1], "wait-error")) {
            fail_wait = true;
        } else {
            CHECK(false);
        }
        if (!holding) {
            CHECK(gt__bridge_stream_write(active, "X", 1, 1000).state == GT_BRIDGE_DATA);
        }
        uint64_t end = now() + 1000;
        gt_bridge_io_t result;
        do {
            result = read_one(10);
            CHECK(now() < end);
        } while (result.state == GT_BRIDGE_WAIT ||
                 (result.state == GT_BRIDGE_EOF && !snapshot().reaped));
        if (!strcmp(argv[1], "dead-group-unknown")) {
            CHECK(!group_error);
            CHECK(result.state == GT_BRIDGE_EOF && snapshot().reaped && !snapshot().group_fenced);
            CHECK(snapshot().group_signal_error == EPERM && snapshot().error == GT_OK &&
                  snapshot().cleanup_error == GT_OK);
        } else {
            CHECK(!group_error && !fail_poll && !fail_stderr && !fail_wait);
            CHECK(result.state == GT_BRIDGE_ERROR && snapshot().error == GT_ERR_IO);
            CHECK(gt__bridge_stream_destroy(active) == GT_ERR_INVALID_ARG &&
                  gt__plugin_memory_info().used > 0);
            if (!holding) {
                CHECK(!snapshot().reaped && snapshot().cleanup_error == GT_ERR_IO);
            }
            if (!strcmp(argv[1], "first-error")) {
                uint64_t until = now() + 2100;
                while (now() < until) {
                    delay();
                }
                CHECK(read_one(0).state == GT_BRIDGE_ERROR &&
                      snapshot().reason == GT_PROCESS_STREAM_NONE);
            }
        }
    }
    uint64_t end = now() + 2000;
    gt_bridge_close_t closed;
    do {
        closed = gt__bridge_stream_close(active, 0);
        CHECK(closed != GT_BRIDGE_CLOSE_ERROR);
        if (closed == GT_BRIDGE_DRAINING) {
            delay();
        }
    } while (closed == GT_BRIDGE_DRAINING && now() < end);
    CHECK(closed == GT_BRIDGE_RELEASED && snapshot().reaped && snapshot().cleanup_error == GT_OK);
    unsigned calls = signal_calls;
    CHECK(gt__bridge_stream_close(active, 0) == GT_BRIDGE_RELEASED && signal_calls == calls);
    CHECK(gt__bridge_stream_destroy(active) == GT_OK);
    active = NULL;
    CHECK(gt__plugin_memory_info().used == 0);
    printf("PASS process_stream_boundary_%s\n", argv[1]);
    return 0;
}
