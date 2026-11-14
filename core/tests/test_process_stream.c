#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "extension/process-stream.h"
#include "extension/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef __has_feature
#define __has_feature(x) 0
#endif

static void failure_state(void);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL process_stream %s:%d: %s\n", __func__, __LINE__, #x);            \
            failure_state();                                                                       \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static gt_bridge_stream_t *active;

static void failure_state(void) {
    int saved = errno;
    gt_process_stream_info_t i;
    if (active && gt__bridge_process_info(active, &i)) {
        fprintf(stderr,
                "STATE error=%d cleanup=%d reason=%d exited=%d code=%d signal=%d reaped=%d "
                "fenced=%d stdoutEOF=%d stderrEOF=%d out=%llu err=%llu errno=%d\n",
                i.error,
                i.cleanup_error,
                i.reason,
                i.child.exited,
                i.child.exit_code,
                i.child.term_signal,
                i.reaped,
                i.group_fenced,
                i.stdout_eof,
                i.stderr_eof,
                (unsigned long long)i.stdout_bytes,
                (unsigned long long)i.stderr_bytes,
                saved);
        if (i.diagnostic_size) {
            fwrite(i.diagnostic, 1, i.diagnostic_size, stderr);
            fputc('\n', stderr);
        }
    }
}

static char executable[4096], directory[4096];
static int cwd_fd = -1;

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void pause_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t)) {
        CHECK(errno == EINTR);
    }
}

static void cleanup(void) {
    if (active) {
        uint64_t end = now() + 9000;
        while (now() < end) {
            if (gt__bridge_stream_close(active, 10) == GT_BRIDGE_RELEASED) {
                (void)gt__bridge_stream_destroy(active);
                active = NULL;
                break;
            }
            pause_ms(1);
        }
    }
    if (cwd_fd >= 0) {
        close(cwd_fd);
    }
    if (directory[0]) {
        (void)rmdir(directory);
    }
}

static void exact(int fd, void *bytes, size_t size, bool writing) {
    size_t at = 0;
    while (at < size) {
        ssize_t n = writing ? write(fd, (char *)bytes + at, size - at)
                            : read(fd, (char *)bytes + at, size - at);
        CHECK(n > 0);
        at += (size_t)n;
    }
}

static volatile sig_atomic_t terminated;

static void term_handler(int n) {
    (void)n;
    terminated = 1;
}

static int child(const char *mode, int argc, char **argv) {
    alarm(8); /* Fault-injected test children never become persistent services. */
    CHECK(!isatty(0) && !isatty(1) && !isatty(2));
    CHECK(!getenv("GT_PROCESS_STREAM_AMBIENT"));
    CHECK(getenv("GT_PROCESS_STREAM_SELECTED") &&
          !strcmp(getenv("GT_PROCESS_STREAM_SELECTED"), "approved"));
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof(cwd)) && getenv("GT_PROCESS_STREAM_CWD") &&
          !strcmp(cwd, getenv("GT_PROCESS_STREAM_CWD")));
    if (!strcmp(mode, "ignore")) {
        CHECK(signal(SIGTERM, SIG_IGN) != SIG_ERR);
    }
    if (!strcmp(mode, "term")) {
        CHECK(signal(SIGTERM, term_handler) != SIG_ERR);
    }
    if (!strcmp(mode, "broken")) {
        CHECK(!close(0));
    }
    if (!strcmp(mode, "context")) {
        CHECK(argc == 5 && !strcmp(argv[3], "") && !strcmp(argv[4], "中文😀'$(literal)"));
    }
    exact(1, "R", 1, true);
    if (!strcmp(mode, "context")) {
        return 0;
    }
    if (!strcmp(mode, "release-exit")) {
        char byte;
        exact(0, &byte, 1, false);
        CHECK(byte == 'X');
        return 0;
    }
    if (!strcmp(mode, "tail")) {
        unsigned char bytes[8192];
        for (size_t i = 0; i < sizeof(bytes); i++) {
            bytes[i] = (unsigned char)(i % 251);
        }
        exact(2, "diagnostic\n", 11, true);
        exact(1, bytes, sizeof(bytes), true);
        return 23;
    }
    if (!strcmp(mode, "signal")) {
        raise(SIGUSR1);
        return 99;
    }
    if (!strcmp(mode, "stderr")) {
        char bytes[9000];
        memset(bytes, 's', sizeof(bytes));
        exact(2, bytes, sizeof(bytes), true);
        exact(1, "K", 1, true);
        return 0;
    }
    if (!strcmp(mode, "stdout-limit") || !strcmp(mode, "stderr-limit")) {
        char bytes[4096];
        memset(bytes, 'f', sizeof(bytes));
        for (;;) {
            exact(!strcmp(mode, "stdout-limit") ? 1 : 2, bytes, sizeof(bytes), true);
        }
    }
    if (!strcmp(mode, "orphan")) {
        int ready[2];
        CHECK(!pipe(ready));
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            close(ready[0]);
            CHECK(signal(SIGTERM, SIG_IGN) != SIG_ERR);
            alarm(8);
            exact(ready[1], "G", 1, true);
            close(ready[1]);
            for (;;) {
                pause();
            }
        }
        close(ready[1]);
        char byte;
        exact(ready[0], &byte, 1, false);
        close(ready[0]);
        exact(1, &pid, sizeof(pid), true);
        return 0;
    }
    if (!strcmp(mode, "eof-live")) {
        CHECK(!close(1));
    }
    if (!strcmp(mode, "hold") || !strcmp(mode, "ignore") || !strcmp(mode, "broken") ||
        !strcmp(mode, "eof-live")) {
        for (;;) {
            pause();
        }
    }
    if (!strcmp(mode, "term")) {
        while (!terminated) {
            pause_ms(1);
        }
        return 37;
    }
    CHECK(!strcmp(mode, "echo"));
    unsigned char bytes[127];
    size_t total = 0;
    while (total < 262144) {
        size_t n = 262144 - total;
        if (n > sizeof(bytes)) {
            n = sizeof(bytes);
        }
        exact(0, bytes, n, false);
        exact(1, bytes, n, true);
        total += n;
    }
    return 0;
}

static gt_process_stream_config_t config(const char *mode) {
    static const char *arguments[5];
    static const char *environment[] = {
        NULL,
        NULL,
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0",
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:symbolize=0",
#endif
    };
    static char cwd_env[4200];
    arguments[0] = executable;
    arguments[1] = "--child";
    arguments[2] = mode;
    arguments[3] = "";
    arguments[4] = "中文😀'$(literal)";
    CHECK(snprintf(cwd_env, sizeof(cwd_env), "GT_PROCESS_STREAM_CWD=%s", directory) > 0);
    environment[0] = "GT_PROCESS_STREAM_SELECTED=approved";
    environment[1] = cwd_env;
    return (gt_process_stream_config_t){.executable = executable,
                                        .arguments = arguments,
                                        .argument_count = 5,
                                        .cwd_fd = cwd_fd,
                                        .environment = environment,
                                        .environment_count =
                                            sizeof(environment) / sizeof(environment[0]),
                                        .execution_authorized = true};
}

static void start(gt_process_stream_config_t c) {
    CHECK(!active);
    CHECK(gt__bridge_process_open(&c, &active) == GT_OK && active);
}

static gt_process_stream_info_t info(void) {
    gt_process_stream_info_t value;
    CHECK(gt__bridge_process_info(active, &value));
    return value;
}

static void ready(void) {
    char byte = 0;
    gt_bridge_io_t r = gt__bridge_stream_read(active, &byte, 1, 1000);
    CHECK(r.state == GT_BRIDGE_DATA && r.count == 1 && byte == 'R');
}

static gt_process_stream_info_t dispose(void) {
    uint64_t end = now() + 2000;
    gt_bridge_close_t r;
    do {
        r = gt__bridge_stream_close(active, 0);
        CHECK(r != GT_BRIDGE_CLOSE_ERROR);
        if (r == GT_BRIDGE_DRAINING) {
            pause_ms(1);
        }
    } while (r == GT_BRIDGE_DRAINING && now() < end);
    CHECK(r == GT_BRIDGE_RELEASED);
    CHECK(info().reaped && info().child.exited);
    CHECK(info().group_fenced || info().group_signal_error == EPERM);
    CHECK(gt__bridge_stream_close(active, 0) == GT_BRIDGE_RELEASED);
    gt_process_stream_info_t final = info();
    CHECK(gt__bridge_stream_destroy(active) == GT_OK);
    active = NULL;
    return final;
}

static void wait_eof(unsigned char *bytes, size_t capacity, size_t *total) {
    uint64_t end = now() + 2000;
    *total = 0;
    for (;;) {
        unsigned char chunk[997];
        gt_bridge_io_t r = gt__bridge_stream_read(active, chunk, sizeof(chunk), 10);
        if (r.state == GT_BRIDGE_DATA) {
            CHECK(r.count <= capacity - *total);
            memcpy(bytes + *total, chunk, r.count);
            *total += r.count;
        } else if (r.state == GT_BRIDGE_EOF) {
            break;
        } else {
            CHECK(r.state == GT_BRIDGE_WAIT);
        }
        CHECK(now() < end);
    }
    while (!info().reaped && now() < end) {
        unsigned char byte;
        CHECK(gt__bridge_stream_read(active, &byte, 1, 0).state == GT_BRIDGE_EOF);
        pause_ms(1);
    }
    CHECK(info().reaped);
}

static void validation(void) {
    gt_process_stream_config_t c = config("context");
    gt_bridge_stream_t *p = (void *)1;
    CHECK(gt__bridge_process_open(NULL, &p) == GT_ERR_INVALID_ARG && !p);
    CHECK(gt__bridge_process_open(&c, NULL) == GT_ERR_INVALID_ARG);
    c.lifetime_ms = 3600001;
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_INVALID_ARG && !p);
    c = config("context");
    c.stop_grace_ms = 5001;
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_INVALID_ARG && !p);
    c = config("context");
    c.stdout_limit = 67108865;
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_INVALID_ARG && !p);
    c = config("context");
    c.stderr_limit = 1048577;
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_INVALID_ARG && !p);
    c = config("context");
    c.cwd_fd = -1;
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_INVALID_ARG && !p);
    c = config("context");
    c.executable = "relative";
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_INVALID_ARG && !p);
    c = config("context");
    c.executable = "/ghosterm/nonexistent-process-stream-fixture";
    CHECK(gt__bridge_process_open(&c, &p) == GT_ERR_NOT_FOUND && !p);
    CHECK(fcntl(cwd_fd, F_GETFD) >= 0);
    CHECK(gt__plugin_memory_info().used == 0);
    gt_process_stream_info_t i;
    memset(&i, 0xff, sizeof(i));
    CHECK(!gt__bridge_process_info(NULL, &i) && i.error == GT_OK && !i.child.exited);
}

static void duplex(void) {
    start(config("echo"));
    ready();
    size_t sent = 0, received = 0;
    uint64_t end = now() + 2000;
    unsigned char bytes[16384], out[997];
    while (received < 262144) {
        if (sent < 262144) {
            size_t n = 262144 - sent;
            if (n > sizeof(bytes)) {
                n = sizeof(bytes);
            }
            for (size_t i = 0; i < n; i++) {
                bytes[i] = (unsigned char)((sent + i) * 7 % 251);
            }
            gt_bridge_io_t r = gt__bridge_stream_write(active, bytes, n, 0);
            CHECK(r.state == GT_BRIDGE_WAIT || r.state == GT_BRIDGE_DATA);
            if (r.state == GT_BRIDGE_DATA) {
                sent += r.count;
            }
        }
        gt_bridge_io_t r = gt__bridge_stream_read(active, out, sizeof(out), 1);
        CHECK(r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT);
        if (r.state == GT_BRIDGE_DATA) {
            for (size_t i = 0; i < r.count; i++) {
                CHECK(out[i] == (unsigned char)((received + i) * 7 % 251));
            }
            received += r.count;
        }
        CHECK(now() < end);
    }
    CHECK(sent == 262144 && received == 262144);
    dispose();
}

typedef struct {
    atomic_bool started, done;
    gt_bridge_io_t result;
} reader_t;

static void *reader(void *context) {
    reader_t *r = context;
    char byte;
    atomic_store(&r->started, true);
    r->result = gt__bridge_stream_read(active, &byte, 1, 3000);
    atomic_store(&r->done, true);
    return NULL;
}

static void test(const char *mode) {
    if (!strcmp(mode, "validation")) {
        validation();
        return;
    }
    if (!strcmp(mode, "denied")) {
        gt_process_stream_config_t c = config("context");
        c.execution_authorized = false;
        CHECK(gt__bridge_process_open(&c, &active) == GT_ERR_AUTH && !active);
        return;
    }
    if (!strcmp(mode, "memory")) {
        CHECK(gt__plugin_memory_limit(1));
        gt_process_stream_config_t c = config("context");
        CHECK(gt__bridge_process_open(&c, &active) == GT_ERR_IO && !active);
        CHECK(gt__plugin_memory_info().used == 0);
        CHECK(gt__plugin_memory_limit(GT_PLUGIN_MEMORY_DEFAULT));
        return;
    }
    if (!strcmp(mode, "duplex")) {
        duplex();
        return;
    }
    const char *child_mode =
        !strcmp(mode, "wake") || !strcmp(mode, "deadline") || !strcmp(mode, "wait") ? "hold" : mode;
    gt_process_stream_config_t c = config(child_mode);
    if (!strcmp(mode, "stdout-limit")) {
        c.stdout_limit = 256;
    }
    if (!strcmp(mode, "stderr-limit")) {
        c.stderr_limit = 256;
    }
    if (!strcmp(mode, "deadline")) {
        c.lifetime_ms = 2000;
    }
    if (!strcmp(mode, "ignore")) {
        c.stop_grace_ms = 50;
    }
    uint64_t began = now();
    start(c);
    uint64_t open_cost = now() - began;
    if (!strstr(mode, "-limit")) {
        ready();
    }
    if (!strcmp(mode, "context") || !strcmp(mode, "signal") || !strcmp(mode, "tail") ||
        !strcmp(mode, "stderr") || !strcmp(mode, "orphan")) {
        unsigned char bytes[10000];
        size_t total;
        wait_eof(bytes, sizeof(bytes), &total);
        gt_process_stream_info_t i = info();
        CHECK(i.error == GT_OK && i.cleanup_error == GT_OK);
        if (!strcmp(mode, "tail")) {
            CHECK(total == 8192 && i.stdout_bytes == 8193 && i.child.exit_code == 23 &&
                  !i.child.term_signal);
            for (size_t n = 0; n < total; n++) {
                CHECK(bytes[n] == n % 251);
            }
            CHECK(i.stderr_bytes == 11 && i.diagnostic_size == 11 &&
                  !memcmp(i.diagnostic, "diagnostic\n", 11));
        } else if (!strcmp(mode, "stderr")) {
            CHECK(total == 1 && i.stdout_bytes == 2 && bytes[0] == 'K' && i.stderr_bytes == 9000 &&
                  i.diagnostic_size == 4096 && i.diagnostic_truncated);
            for (size_t n = 0; n < i.diagnostic_size; n++) {
                CHECK(i.diagnostic[n] == 's');
            }
        } else if (!strcmp(mode, "signal")) {
            CHECK(total == 0 && i.child.term_signal == SIGUSR1);
        } else if (!strcmp(mode, "orphan")) {
            CHECK(total == sizeof(pid_t));
            pid_t pid;
            memcpy(&pid, bytes, sizeof(pid));
            CHECK(pid > 0);
            uint64_t end = now() + 1000;
            while (kill(pid, 0) == 0 && now() < end) {
                pause_ms(1);
            }
            CHECK(kill(pid, 0) < 0 && errno == ESRCH);
        } else {
            CHECK(total == 0 && i.child.exit_code == 0 && !i.child.term_signal);
        }
        dispose();
        return;
    }
    if (!strcmp(mode, "eof-live")) {
        char byte;
        CHECK(gt__bridge_stream_read(active, &byte, 1, 1000).state == GT_BRIDGE_EOF);
        CHECK(!info().child.exited && !info().reaped);
        dispose();
        return;
    }
    if (!strcmp(mode, "broken")) {
        char byte = 'x';
        CHECK(gt__bridge_stream_write(active, &byte, 1, 1000).state == GT_BRIDGE_ERROR);
        CHECK(info().error == GT_ERR_IO);
        dispose();
        return;
    }
    if (!strcmp(mode, "wait")) {
        char byte;
        CHECK(gt__bridge_stream_read(active, &byte, 1, 0).state == GT_BRIDGE_WAIT);
        CHECK(gt__bridge_stream_read(active, &byte, 1, 20).state == GT_BRIDGE_WAIT);
        CHECK(!info().stdout_eof && !info().child.exited && info().error == GT_OK);
        dispose();
        return;
    }
    if (strstr(mode, "-limit")) {
        uint64_t end = now() + 1000;
        gt_bridge_io_t r;
        do {
            char bytes[1024];
            r = gt__bridge_stream_read(active, bytes, sizeof(bytes), 10);
            CHECK(r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT ||
                  r.state == GT_BRIDGE_ERROR);
            CHECK(now() < end);
        } while (r.state != GT_BRIDGE_ERROR);
        CHECK(info().reason == (!strcmp(mode, "stdout-limit") ? GT_PROCESS_STREAM_STDOUT_LIMIT
                                                              : GT_PROCESS_STREAM_STDERR_LIMIT));
        dispose();
        return;
    }
    if (!strcmp(mode, "deadline")) {
        CHECK(open_cost < c.lifetime_ms / 2);
        char byte;
        CHECK(gt__bridge_stream_read(active, &byte, 1, 3000).state == GT_BRIDGE_ERROR);
        CHECK(info().reason == GT_PROCESS_STREAM_DEADLINE && now() - began >= 1990 &&
              now() - began < 3000);
        dispose();
        return;
    }
    if (!strcmp(mode, "wake")) {
        reader_t r;
        memset(&r, 0, sizeof(r));
        atomic_init(&r.started, false);
        atomic_init(&r.done, false);
        pthread_t thread;
        CHECK(!pthread_create(&thread, NULL, reader, &r));
        while (!atomic_load(&r.started)) {
            pause_ms(1);
        }
        pause_ms(20);
        CHECK(!atomic_load(&r.done));
        uint64_t started = now();
        gt__bridge_stream_stop(active);
        CHECK(!pthread_join(thread, NULL));
        CHECK(now() - started < 100 && r.result.state == GT_BRIDGE_CLOSED);
        CHECK(info().stopping);
        dispose();
        return;
    }
    CHECK(!strcmp(mode, "term") || !strcmp(mode, "ignore"));
    uint64_t started = now();
    gt_process_stream_info_t final = dispose();
    CHECK(now() - started < 1000);
    if (!strcmp(mode, "ignore")) {
        CHECK(final.child.term_signal == SIGKILL && now() - started >= 40);
    } else {
        CHECK(final.child.exit_code == 37 && !final.child.term_signal);
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--child")) {
        return child(argv[2], argc, argv);
    }
    CHECK(argc == 3 && argv[2][0] == '/' && realpath(argv[0], executable));
    CHECK(snprintf(directory, sizeof(directory), "%s/process-stream-XXXXXX", argv[2]) > 0 &&
          mkdtemp(directory));
    cwd_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cwd_fd >= 0);
    CHECK(!atexit(cleanup));
    CHECK(!setenv("GT_PROCESS_STREAM_AMBIENT", "must-not-inherit", 1));
    test(argv[1]);
    CHECK(!active && gt__plugin_memory_info().used == 0);
    printf("PASS process_stream_%s\n", argv[1]);
    return 0;
}
