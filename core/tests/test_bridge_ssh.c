#define _POSIX_C_SOURCE 200809L
#include "extension/backends/ssh_exec.h"
#include "extension/frame.h"
#include "ssh/connector.h"
#include "internal.h"
#include "base/secure-random.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void exact_write(int fd, const void *p, size_t n) {
    const uint8_t *bytes = p;
    while (n) {
        ssize_t k = write(fd, bytes, n > 17 ? 17 : n);
        if (k < 0 && errno == EINTR) {
            continue;
        }
        CHECK(k > 0);
        bytes += k;
        n -= (size_t)k;
    }
}

static bool exact_read(int fd, void *p, size_t n) {
    uint8_t *bytes = p;
    size_t used = 0;
    while (used < n) {
        ssize_t k = read(fd, bytes + used, n - used);
        if (k < 0 && errno == EINTR) {
            continue;
        }
        CHECK(k >= 0);
        if (!k) {
            CHECK(!used);
            return false;
        }
        used += (size_t)k;
    }
    return true;
}

static void executor(const char *mode) {
    CHECK(!isatty(0) && !isatty(1) && !isatty(2));
    if (!strcmp(mode, "pollution")) {
        exact_write(1, "Welcome", 7);
        return;
    }
    if (!strcmp(mode, "diagnostic") || !strcmp(mode, "flood")) {
        int count = !strcmp(mode, "flood") ? 20000 : 600;
        for (int i = 0; i < count; i++) {
            exact_write(2, "diagnostic\n", 11);
        }
    }
    const uint8_t ready[] = {0, 0, 0, 5, 'r', 'e', 'a', 'd', 'y'};
    exact_write(1, ready, sizeof(ready));
    if (!strcmp(mode, "truncated")) {
        const uint8_t tail[] = {0, 0, 0, 5, 'x', 'y'};
        exact_write(1, tail, sizeof(tail));
        return;
    }
    uint8_t buffer[65536], header[4];
    while (exact_read(0, header, 4)) {
        size_t n = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                   ((size_t)header[2] << 8) | header[3];
        CHECK(n && n <= sizeof(buffer) && exact_read(0, buffer, n));
        exact_write(1, header, 4);
        exact_write(1, buffer, n);
        if (!strcmp(mode, "exit")) {
            _exit(23);
        }
    }
}
#ifdef GT_HAVE_LIBSSH2
#include <libssh2.h>

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool clock_read(void *ctx, uint64_t *out) {
    (void)ctx;
    *out = now();
    return true;
}

static void delay(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t)) {
        CHECK(errno == EINTR);
    }
}

static char *private_key;

static gt_ssh_t *connect_to(char **argv) {
    gt_ssh_opts_t opts = {.host = "bridge-target.invalid",
                          .port = (uint16_t)atoi(argv[2]),
                          .username = argv[3],
                          .known_hosts_path = argv[4]};
    gt_ssh_t *ssh = gt_ssh_new(&opts);
    CHECK(ssh && gt_ssh_auth_private_key(ssh, private_key, NULL) == GT_OK);
    gt_ssh_connector_config_t c = {
        .family = 4, .address = {127, 0, 0, 1}, .timeout_ms = 5000, .clock = clock_read};
    gt_ssh_connector_t *connector = NULL;
    CHECK(gt__ssh_connector_open(&ssh, &c, NULL, &connector) == GT_OK && !ssh);
    uint64_t end = now() + 6000;
    for (;;) {
        gt_ssh_connector_phase_t phase = gt__ssh_connector_step(connector);
        if (phase == GT_SSH_CONNECT_READY) {
            break;
        }
        CHECK(phase != GT_SSH_CONNECT_FAILED && phase != GT_SSH_CONNECT_HOSTKEY && now() < end);
        delay(1);
    }
    CHECK(gt__ssh_connector_take(connector, &ssh) && ssh);
    gt__ssh_connector_close(connector);
    return ssh;
}

static gt_bridge_ssh_info_t info(gt_bridge_stream_t *s) {
    gt_bridge_ssh_info_t i;
    CHECK(gt__bridge_ssh_info(s, &i));
    return i;
}

static gt_bridge_stream_t *open_stream(char **argv, uint32_t admission, uint32_t limit) {
    gt_ssh_t *ssh = connect_to(argv);
    gt_bridge_stream_t *s = NULL;
    gt_bridge_ssh_options_t options = {admission, limit};
    CHECK(gt__bridge_ssh_open(&ssh, argv[5], &options, NULL, &s) == GT_OK && !ssh && s);
    return s;
}

static void admitted(gt_bridge_stream_t *s) {
    uint64_t end = now() + 6000;
    for (;;) {
        gt_bridge_ssh_phase_t p = gt__bridge_ssh_step(s, 25);
        if (p == GT_BRIDGE_SSH_READY) {
            break;
        }
        CHECK(p < GT_BRIDGE_SSH_READY && now() < end);
    }
}

static void close_stream(gt_bridge_stream_t *s) {
    uint64_t start = now();
    for (;;) {
        gt_bridge_close_t r = gt__bridge_stream_close(s, 0);
        if (r == GT_BRIDGE_RELEASED) {
            break;
        }
        CHECK(r == GT_BRIDGE_DRAINING && now() - start < 1000);
        delay(1);
    }
    CHECK(gt__bridge_stream_close(s, 0) == GT_BRIDGE_RELEASED &&
          info(s).phase == GT_BRIDGE_SSH_CLOSED);
    CHECK(gt__bridge_stream_write(s, "late", 4, 0).state == GT_BRIDGE_CLOSED);
    CHECK(gt__bridge_stream_destroy(s) == GT_OK && now() - start < 1000);
}

static void read_frame(gt_bridge_stream_t *s, const uint8_t *expected, size_t size) {
    gt_bridge_decoder_t *d;
    CHECK(gt__bridge_decoder_new(65536, NULL, &d) == GT_OK);
    uint64_t end = now() + 5000;
    size_t total = 0;
    const uint8_t *view;
    size_t n;
    while (!gt__bridge_decoder_view(d, &view, &n)) {
        uint8_t bytes[137];
        size_t capacity = size + 4 - total;
        if (capacity > sizeof(bytes)) {
            capacity = sizeof(bytes);
        }
        gt_bridge_io_t r = gt__bridge_stream_read(s, bytes, capacity, 25);
        CHECK(now() < end && (r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT));
        if (r.state == GT_BRIDGE_WAIT) {
            continue;
        }
        size_t used;
        CHECK(gt__bridge_decoder_feed(d, bytes, r.count, &used) == GT_OK && used == r.count);
        total += used;
    }
    CHECK(n == size && total == size + 4 && !memcmp(view, expected, size));
    gt__bridge_decoder_free(d);
}

static void ready(gt_bridge_stream_t *s) {
    read_frame(s, (const uint8_t *)"ready", 5);
}

static void send_frame(gt_bridge_stream_t *s, const uint8_t *p, size_t n) {
    gt_bridge_writer_t *w;
    CHECK(gt__bridge_writer_new(65536, NULL, &w) == GT_OK &&
          gt__bridge_writer_queue(w, p, n) == GT_OK);
    uint64_t end = now() + 5000;
    while (gt__bridge_writer_pending(w)) {
        gt_bridge_io_t r = gt__bridge_writer_step(w, s, 25);
        CHECK(now() < end && (r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT));
    }
    gt__bridge_writer_free(w);
}

static void roundtrip(gt_bridge_stream_t *s, size_t n) {
    uint8_t *p = malloc(n);
    CHECK(p);
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(i * 13u);
    }
    send_frame(s, p, n);
    read_frame(s, p, n);
    free(p);
}

static void *stop_later(void *arg) {
    delay(30);
    gt__bridge_stream_stop(arg);
    return NULL;
}

static void cancelled(gt_bridge_stream_t *s, bool admission) {
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, stop_later, s));
    uint64_t start = now();
    if (admission) {
        CHECK(gt__bridge_ssh_step(s, 5000) == GT_BRIDGE_SSH_STOPPED);
    } else {
        uint8_t b;
        gt_bridge_io_t r = gt__bridge_stream_read(s, &b, 1, 5000);
        CHECK(r.state == GT_BRIDGE_CLOSED && r.error == GT_ERR_CANCELLED);
    }
    CHECK(!pthread_join(thread, NULL) && now() - start < 1000);
    close_stream(s);
}

static void expect_end(gt_bridge_stream_t *s, bool clean) {
    uint64_t end = now() + 5000;
    uint8_t bytes[128];
    for (;;) {
        gt_bridge_io_t r = gt__bridge_stream_read(s, bytes, sizeof(bytes), 25);
        if (r.state == GT_BRIDGE_WAIT) {
            CHECK(now() < end);
            continue;
        }
        CHECK(clean ? r.state == GT_BRIDGE_EOF
                    : (r.state == GT_BRIDGE_ERROR && r.error == GT_ERR_IO));
        break;
    }
    CHECK(!info(s).exit_status_known); /* Getter's default 0 is never success. */
}

static void terminal_echo(gt_transport_t *t, const char *marker) {
    char command[512] = "printf '";
    size_t at = strlen(command);
    for (size_t i = 0; marker[i]; i++) {
        int k = snprintf(command + at, sizeof(command) - at, "\\%03o", (unsigned char)marker[i]);
        CHECK(k == 4);
        at += 4;
    }
    CHECK(at + 6 < sizeof(command));
    memcpy(command + at, "\\n'\n", 5);
    int n = (int)(at + 4);
    CHECK(gt_transport_write(t, (const uint8_t *)command, (size_t)n) == GT_OK);
    char output[4096] = {0};
    size_t used = 0;
    uint64_t end = now() + 5000;
    while (!strstr(output, marker)) {
        int r = gt_transport_read(t, (uint8_t *)output + used, sizeof(output) - used - 1, 25);
        CHECK(r > 0 || r == GT_TRANSPORT_READ_TIMEOUT);
        CHECK(now() < end && used < sizeof(output) - 1);
        if (r > 0) {
            used += (size_t)r;
        }
    }
}

static void *refuse(void *ctx, size_t n) {
    (void)ctx;
    (void)n;
    return NULL;
}

static void unused_release(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)p;
    (void)n;
    CHECK(false);
}

static void *owner_allocate(void *ctx, size_t n) {
    size_t *live = ctx;
    CHECK(!*live);
    void *p = malloc(n);
    CHECK(p);
    *live = n;
    return p;
}

static void owner_release(void *ctx, void *p, size_t n) {
    size_t *live = ctx;
    CHECK(*live == n);
    *live = 0;
    free(p);
}

static void legacy(char **argv) {
    const char *mode = argv[1];
    gt_ssh_t *ssh = connect_to(argv);
    gt_status_t status;
    gt_ssh_exec_t *e = gt_ssh_exec(ssh, "fixed-legacy-probe", &status);
    CHECK(e && status == GT_OK);
    void *raw = NULL;
    int fd = -1;
    CHECK(gt__ssh_borrow(ssh, &raw, &fd) == GT_OK);
    long timeout = libssh2_session_get_timeout(raw);
    int blocking = libssh2_session_get_blocking(raw);
    char out[64] = {0}, err[64] = {0};
    size_t used = 0, errors = 0;
    bool ended = false;
    uint64_t end = now() + 5000;
    while (used < 11 || errors < 11) {
        uint8_t bytes[3];
        bool fromerr = false;
        int n = gt_ssh_exec_read(e, bytes, sizeof(bytes), 25, &fromerr);
        CHECK(now() < end && (n > 0 || n == GT_EXEC_READ_TIMEOUT));
        CHECK(libssh2_session_get_timeout(raw) == timeout &&
              libssh2_session_get_blocking(raw) == blocking);
        if (n > 0) {
            char *p = fromerr ? err : out;
            size_t *size = fromerr ? &errors : &used;
            CHECK(*size + (size_t)n < 64);
            memcpy(p + *size, bytes, (size_t)n);
            *size += (size_t)n;
        }
    }
    CHECK(used == 11 && errors == 11 && !memcmp(out, "stdout-tail", 11) &&
          !memcmp(err, "stderr-tail", 11));
    bool live = !strcmp(mode, "legacy-live"), late = !strcmp(mode, "legacy-late"),
         reset = !strcmp(mode, "legacy-reset");
    if (!live && !reset) {
        uint8_t b;
        bool iserr = false;
        while (!ended) {
            int n = gt_ssh_exec_read(e, &b, 1, 25, &iserr);
            CHECK(now() < end && (n == GT_EXEC_READ_TIMEOUT || n == GT_EXEC_READ_EOF));
            ended = n == GT_EXEC_READ_EOF;
        }
    }
    int value = 987654;
    uint64_t start = now();
    bool known = gt_ssh_exec_exit_status(e, &value);
    CHECK(now() - start < 200 && libssh2_session_get_timeout(raw) == timeout &&
          libssh2_session_get_blocking(raw) == blocking);
    bool expected = !strcmp(mode, "legacy-zero") || !strcmp(mode, "legacy-23") ||
                    !strcmp(mode, "legacy-duplicate");
    if (expected) {
        CHECK(known && value == (!strcmp(mode, "legacy-zero") ? 0 : 23));
    } else {
        CHECK(!known && value == 987654);
    }
    if (live || late || reset) {
        puts("LEGACY_QUERY_OBSERVED");
        fflush(stdout); /* explicit peer-release barrier */
        bool done = false;
        while (!done) {
            uint8_t bytes[7];
            bool fromerr = false;
            int n = gt_ssh_exec_read(e, bytes, sizeof(bytes), 25, &fromerr);
            CHECK(now() < end);
            if (reset) {
                CHECK(n == GT_EXEC_READ_TIMEOUT || n == GT_EXEC_READ_ERROR);
                done = n == GT_EXEC_READ_ERROR;
            } else {
                CHECK(n > 0 || n == GT_EXEC_READ_TIMEOUT || n == GT_EXEC_READ_EOF);
                if (n > 0) {
                    CHECK(live && !fromerr && used + (size_t)n < 64);
                    memcpy(out + used, bytes, (size_t)n);
                    used += (size_t)n;
                }
                value = 987654;
                known = gt_ssh_exec_exit_status(e, &value);
                if (known) {
                    CHECK(value == (late ? 23 : 0));
                    done = true;
                } else {
                    CHECK(value == 987654);
                }
            }
        }
        if (live) {
            CHECK(used == 15 && !memcmp(out, "stdout-tailLATE", 15));
        }
        if (reset) {
            value = 987654;
            CHECK(!gt_ssh_exec_exit_status(e, &value) && value == 987654);
        }
    }
    /* Dedicated test connection: abort before cleanup, never wait on a faulty peer. */
    gt__ssh_abort(ssh);
    gt_ssh_exec_close(e);
    gt_ssh_close(ssh);
}

static void client(int argc, char **argv) {
    CHECK(argc == 6);
    uint8_t header[4];
    CHECK(exact_read(0, header, 4));
    size_t keylen = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                    ((size_t)header[2] << 8) | header[3];
    CHECK(keylen && keylen < 8192);
    private_key = calloc(1, keylen + 1);
    CHECK(private_key && exact_read(0, private_key, keylen));
    const char *mode = argv[1];
    if (!strncmp(mode, "legacy-", 7)) {
        legacy(argv);
    } else if (!strcmp(mode, "allocation")) {
        gt_ssh_t *ssh = connect_to(argv);
        gt_bridge_stream_t *s = NULL;
        gt_allocator_t allocator = {NULL, refuse, unused_release};
        CHECK(gt__bridge_ssh_open(&ssh, argv[5], NULL, &allocator, &s) == GT_ERR_IO && ssh && !s);
        gt_bridge_ssh_options_t bad = {0, 1};
        CHECK(gt__bridge_ssh_open(&ssh, argv[5], &bad, NULL, &s) == GT_ERR_INVALID_ARG && ssh &&
              !s);
        gt_ssh_t *alias = ssh;
        void *raw = NULL;
        int fd = -1;
        CHECK(gt__ssh_stream_claim(ssh, &raw, &fd, false) == GT_OK);
        size_t live = 0;
        allocator = (gt_allocator_t){&live, owner_allocate, owner_release};
        CHECK(gt__bridge_ssh_open(&ssh, argv[5], NULL, &allocator, &s) == GT_OK && !ssh && live);
        gt_status_t rejected;
        gt_shell_opts_t shell = {.cols = 80, .rows = 24};
        CHECK(!gt_ssh_exec(alias, "true", &rejected) && rejected == GT_ERR_INVALID_ARG);
        CHECK(!gt_transport_open_ssh_shell(alias, &shell) && !gt_ssh_sftp(alias));
        void *borrowed = NULL;
        int other = -1;
        CHECK(gt__ssh_borrow(alias, &borrowed, &other) == GT_ERR_INVALID_ARG &&
              libssh2_session_get_blocking(raw) == 0);
        admitted(s);
        ready(s);
        roundtrip(s, 32);
        close_stream(s);
        CHECK(!live);
    } else {
        bool deadline = !strcmp(mode, "admission-deadline");
        gt_bridge_stream_t *s =
            open_stream(argv, deadline ? 100 : 5000, !strcmp(mode, "flood") ? 8192 : 262144);
        if (!strcmp(mode, "before-admission")) {
            gt__bridge_stream_stop(s);
            close_stream(s);
        } else if (!strcmp(mode, "deny") || !strcmp(mode, "channel-deny") || deadline ||
                   !strcmp(mode, "admission-cancel")) {
            uint64_t end = now() + 3000;
            if (!strcmp(mode, "admission-cancel")) {
                while (info(s).phase != GT_BRIDGE_SSH_EXEC) {
                    CHECK(gt__bridge_ssh_step(s, 10) < GT_BRIDGE_SSH_READY && now() < end);
                }
                cancelled(s, true);
            } else {
                while (gt__bridge_ssh_step(s, 25) != GT_BRIDGE_SSH_FAILED) {
                    CHECK(now() < end);
                }
                bool channel_denied = !strcmp(mode, "channel-deny");
                CHECK(info(s).error ==
                      ((deadline || channel_denied) ? GT_ERR_IO : GT_ERR_UNSUPPORTED));
                CHECK(info(s).failure_phase ==
                      (channel_denied ? GT_BRIDGE_SSH_CHANNEL : GT_BRIDGE_SSH_EXEC));
                CHECK(info(s).admission_expired == deadline);
                close_stream(s);
            }
        } else {
            admitted(s);
            if (!strcmp(mode, "flood")) {
                uint8_t bytes[32];
                uint64_t end = now() + 5000;
                for (;;) {
                    gt_bridge_io_t r = gt__bridge_stream_read(s, bytes, sizeof(bytes), 25);
                    if (r.state == GT_BRIDGE_ERROR) {
                        CHECK(r.error == GT_ERR_PROTOCOL);
                        break;
                    }
                    CHECK(r.state == GT_BRIDGE_WAIT && now() < end);
                }
                CHECK(info(s).stderr_bytes > 8192 && info(s).stderr_bytes <= 9216 &&
                      info(s).diagnostic_size == 4096 && info(s).diagnostic_truncated);
                close_stream(s);
            } else if (!strcmp(mode, "pollution")) {
                uint8_t bytes[32];
                size_t used;
                gt_bridge_decoder_t *d;
                CHECK(gt__bridge_decoder_new(65536, NULL, &d) == GT_OK);
                gt_bridge_io_t r = gt__bridge_stream_read(s, bytes, sizeof(bytes), 1000);
                CHECK(r.state == GT_BRIDGE_DATA &&
                      gt__bridge_decoder_feed(d, bytes, r.count, &used) == GT_ERR_PROTOCOL);
                gt__bridge_decoder_free(d);
                close_stream(s);
            } else {
                ready(s);
                if (!strcmp(mode, "cancel")) {
                    cancelled(s, false);
                } else if (!strcmp(mode, "reset")) {
                    send_frame(s, (const uint8_t *)"reset-ack", 9);
                    expect_end(s, false);
                    close_stream(s);
                } else if (!strcmp(mode, "truncated")) {
                    gt_bridge_decoder_t *d;
                    CHECK(gt__bridge_decoder_new(65536, NULL, &d) == GT_OK);
                    uint8_t bytes[32];
                    uint64_t end = now() + 5000;
                    for (;;) {
                        gt_bridge_io_t r = gt__bridge_stream_read(s, bytes, sizeof(bytes), 25);
                        if (r.state == GT_BRIDGE_EOF) {
                            break;
                        }
                        CHECK(now() < end &&
                              (r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT));
                        if (r.state == GT_BRIDGE_DATA) {
                            size_t used;
                            CHECK(gt__bridge_decoder_feed(d, bytes, r.count, &used) == GT_OK &&
                                  used == r.count);
                        }
                    }
                    CHECK(gt__bridge_decoder_finish(d) == GT_ERR_PROTOCOL &&
                          !info(s).exit_status_known);
                    gt__bridge_decoder_free(d);
                    close_stream(s);
                } else {
                    if (!strcmp(mode, "isolation")) {
                        gt_ssh_t *terminal = connect_to(argv);
                        gt_shell_opts_t o = {.term = "xterm", .cols = 80, .rows = 24};
                        gt_transport_t *t = gt_transport_open_ssh_shell(terminal, &o);
                        CHECK(t);
                        terminal_echo(t, "terminal-before-plugin-close");
                        gt_bridge_stream_t *reused = NULL;
                        CHECK(gt__bridge_ssh_open(&terminal, argv[5], NULL, NULL, &reused) ==
                                  GT_ERR_INVALID_ARG &&
                              terminal && !reused);
                        roundtrip(s, 65536);
                        close_stream(s);
                        terminal_echo(t, "terminal-after-plugin-close");
                        gt_transport_close(t);
                        gt_ssh_close(terminal);
                        s = open_stream(argv, 5000, 262144);
                        admitted(s);
                        ready(s);
                        terminal = connect_to(argv);
                        t = gt_transport_open_ssh_shell(terminal, &o);
                        CHECK(t);
                        terminal_echo(t, "terminal-before-terminal-close");
                        gt_transport_close(t);
                        gt_ssh_close(terminal);
                        roundtrip(s, 65536);
                    } else {
                        roundtrip(s, 65536);
                    }
                    if (!strcmp(mode, "diagnostic")) {
                        CHECK(info(s).stderr_bytes == 6600 && info(s).diagnostic_size == 4096 &&
                              info(s).diagnostic_truncated);
                    }
                    if (!strcmp(mode, "exit") || !strcmp(mode, "missing-exit")) {
                        expect_end(s, true);
                    }
                    CHECK(!info(s).exit_status_known);
                    close_stream(s);
                }
            }
        }
    }
    gt__secure_erase(private_key, keylen);
    free(private_key);
    private_key = NULL;
    printf("PASS bridge_ssh_%s\n", mode);
}
#endif
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--executor")) {
        executor(argv[2]);
        return 0;
    }
#ifdef GT_HAVE_LIBSSH2
    client(argc, argv);
#else
    CHECK(argc == 2 && !strcmp(argv[1], "unsupported"));
    gt_ssh_t *owned = (gt_ssh_t *)1;
    gt_bridge_stream_t *s = (gt_bridge_stream_t *)1;
    CHECK(gt__bridge_ssh_open(&owned, "bootstrap", NULL, NULL, &s) == GT_ERR_UNSUPPORTED &&
          owned == (gt_ssh_t *)1 && !s);
    gt_bridge_ssh_info_t i;
    CHECK(!gt__bridge_ssh_info(NULL, &i));
    puts("PASS bridge_ssh_unsupported");
    int value = 987654;
    CHECK(!gt_ssh_exec_exit_status(NULL, &value) && value == 987654);
#endif
    return 0;
}
