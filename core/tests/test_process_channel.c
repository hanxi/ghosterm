#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "ghosterm.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef GT_TEST_PLUGIN_JS
#include "base/sha256.h"
#endif

static void failure_state(void);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL process_channel %s:%d: %s\n", __func__, __LINE__, #x);           \
            failure_state();                                                                       \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static gt_ext_channel_t *channel;
static gt_ext_session_t *session;
static gt_ext_runtime_t *runtime;
static int directory = -1;
static char executable[4096], cwd[4096];

static void failure_state(void) {
    gt_ext_transport_process_info_t i;
    gt_status_t status = runtime   ? gt_ext_runtime_transport_process_info(runtime, &i)
                         : session ? gt_ext_session_process_info(session, &i)
                                   : gt_ext_channel_process_info(channel, &i);
    if (status == GT_OK) {
        fprintf(stderr,
                "STATE error=%d cleanup=%d reason=%d exited=%d code=%d signal=%d reaped=%d\n",
                i.error,
                i.cleanup_error,
                i.reason,
                i.child_exited,
                i.exit_code,
                i.term_signal,
                i.reaped);
        if (i.diagnostic_size) {
            fwrite(i.diagnostic, 1, i.diagnostic_size, stderr);
            fputc('\n', stderr);
        }
    }
}

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void pause_ms(void) {
    struct timespec t = {0, 1000000};
    (void)nanosleep(&t, NULL);
}

static gt_ext_transport_process_info_t info(void) {
    gt_ext_transport_process_info_t i;
    gt_status_t status = runtime   ? gt_ext_runtime_transport_process_info(runtime, &i)
                         : session ? gt_ext_session_process_info(session, &i)
                                   : gt_ext_channel_process_info(channel, &i);
    CHECK(status == GT_OK);
    return i;
}

static void release(void) {
    uint64_t end = now() + 2000;
    while (now() < end) {
        gt_ext_close_t r = runtime   ? gt_ext_runtime_close(runtime)
                           : session ? gt_ext_session_close(session, 1)
                                     : gt_ext_channel_close(channel, 1);
        CHECK(r != GT_EXT_CLOSE_ERROR);
        if (r == GT_EXT_RELEASED) {
            return;
        }
        pause_ms();
    }
    CHECK(false);
}

static void cleanup(void) {
    if (runtime || session || channel) {
        release();
        if (runtime) {
            CHECK(gt_ext_runtime_destroy(&runtime) == GT_OK);
        }
        if (session) {
            CHECK(gt_ext_session_destroy(&session) == GT_OK);
        }
        if (channel) {
            CHECK(gt_ext_channel_destroy(&channel) == GT_OK);
        }
    }
    if (directory >= 0) {
        CHECK(!close(directory));
        directory = -1;
    }
}

static gt_ext_process_channel_config_t config(const char *mode) {
    static const char *arguments[5];
    arguments[0] = "raw argv zero";
    arguments[1] = mode;
    static const char *environment[] = {
        "GT_SELECTED=only"
#if defined(__SANITIZE_ADDRESS__)
        ,
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0",
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:symbolize=0"
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
        ,
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0",
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:symbolize=0"
#endif
#endif
    };
    return (gt_ext_process_channel_config_t){.executable = executable,
                                             .arguments = arguments,
                                             .argument_count = 2,
                                             .cwd_fd = directory,
                                             .environment = environment,
                                             .environment_count =
                                                 sizeof(environment) / sizeof(environment[0]),
                                             .execution_authorized = true};
}

static void open_child(gt_ext_process_channel_config_t *c) {
    CHECK(gt_ext_channel_open_process(c, &channel) == GT_OK && channel);
    CHECK(gt_ext_channel_step(channel, 0) == GT_EXT_READY);
    CHECK(!info().child_exited && !info().reaped);
    CHECK(gt_ext_channel_destroy(&channel) == GT_ERR_INVALID_ARG && channel);
}

static size_t read_all(uint8_t *bytes, size_t capacity, bool expect_error) {
    size_t size = 0;
    uint64_t end = now() + 2000;
    while (now() < end) {
        uint8_t buffer[16384];
        gt_ext_io_t r = gt_ext_channel_read(channel, buffer, sizeof(buffer), 5);
        if (r.state == GT_EXT_DATA) {
            CHECK(size + r.count <= capacity);
            memcpy(bytes + size, buffer, r.count);
            size += r.count;
        } else if (r.state == GT_EXT_IO_ERROR) {
            CHECK(expect_error && r.error != GT_OK);
            return size;
        } else if (r.state == GT_EXT_EOF && info().reaped) {
            CHECK(!expect_error);
            return size;
        } else {
            CHECK(r.state == GT_EXT_WAIT || r.state == GT_EXT_EOF);
        }
    }
    CHECK(false);
    return 0;
}

static void diagnostic(void) {
    static const uint8_t expected[] = {'D', 0, 255, '\n'};
    gt_ext_transport_process_info_t p = info();
    CHECK(p.diagnostic_size == 4 && p.stderr_bytes == 4 && !p.diagnostic_truncated);
    CHECK(!memcmp(p.diagnostic, expected, 4));
}

static gt_ext_session_config_t session_config(void) {
    gt_ext_session_config_t c = {.required_count = 1,
                                 .available_count = 1,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 5000,
                                 .peer = {GT_EXT_PEER_LOCAL, "process-fixture"},
                                 .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(c.plugin_id, "org.ghosterm.process-fixture");
    strcpy(c.plugin_version, "0.1.0");
    memset(c.client_digest, 'd', 64);
    memset(c.executor_digest, 'e', 64);
    strcpy(c.host_version, "0.1.0");
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.required[0], "ui.form");
    strcpy(c.available[0], "ui.form");
    return c;
}

static void adopt(bool handshake) {
    gt_ext_session_config_t c = session_config();
    gt_ext_rpc_method_t method = {.receiver = GT_EXT_EXECUTOR,
                                  .input_shape = GT_EXT_JSON_OBJECT,
                                  .output_shape = GT_EXT_JSON_OBJECT};
    strcpy(method.method, "fixture.echo");
    strcpy(method.capability, "ui.form");
    c.peer.kind = GT_EXT_PEER_SSH;
    CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &c, &method, 1, &session) ==
              GT_ERR_INVALID_ARG &&
          channel && !session);
    c.peer.kind = GT_EXT_PEER_LOCAL;
    CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &c, &method, 1, &session) == GT_OK &&
          !channel && session);
    CHECK(!info().child_exited);
    if (!handshake) {
        return;
    }
    uint64_t end = now() + 2000;
    bool consent = false;
    while (now() < end) {
        gt_ext_session_phase_t phase = gt_ext_session_step(session, 1);
        gt_ext_session_info_t i;
        CHECK(gt_ext_session_info(session, &i));
        CHECK(i.peer.kind == GT_EXT_PEER_LOCAL);
        if (phase == GT_EXT_SESSION_CONSENT) {
            CHECK(i.decision == GT_EXT_DECISION_UNKNOWN);
            consent = true;
            CHECK(gt_ext_session_decide(session, i.consent_token, GT_EXT_DECISION_ALLOW) == GT_OK);
        } else if (phase == GT_EXT_SESSION_ACTIVE) {
            CHECK(consent);
            diagnostic();
            return;
        } else {
            CHECK(phase < GT_EXT_SESSION_ENDING);
        }
    }
    CHECK(false);
}

static void *stop_worker(void *p) {
    struct timespec t = {0, 20000000};
    (void)nanosleep(&t, NULL);
    gt_ext_channel_stop(p);
    return NULL;
}

int main(int argc, char **argv) {
    CHECK((argc == 4 || argc == 5) && realpath(argv[2], executable) && realpath(argv[3], cwd));
    directory = open(cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 0);
    CHECK(!atexit(cleanup));
    const char *mode = argv[1];
    gt_ext_process_channel_config_t c = config(mode);
    if (!strcmp(mode, "invalid") || !strcmp(mode, "disabled")) {
        gt_ext_transport_process_info_t i;
        memset(&i, 255, sizeof(i));
        CHECK(gt_ext_channel_process_info(NULL, &i) == GT_ERR_INVALID_ARG && !i.child_exited &&
              !i.diagnostic_size);
        CHECK(gt_ext_session_process_info(NULL, &i) == GT_ERR_INVALID_ARG);
        CHECK(gt_ext_runtime_transport_process_info(NULL, &i) == GT_ERR_INVALID_ARG);
        CHECK(gt_ext_channel_open_process(NULL, &channel) == GT_ERR_INVALID_ARG && !channel);
        if (!strcmp(mode, "disabled")) {
            CHECK(gt_ext_channel_open_process(&c, &channel) == GT_ERR_UNSUPPORTED && !channel);
        } else {
            c.execution_authorized = false;
            for (size_t n = 0; n < GT_EXT_CHANNEL_LIMIT * 2; n++) {
                CHECK(gt_ext_channel_open_process(&c, &channel) == GT_ERR_AUTH && !channel);
            }
            c.execution_authorized = true;
            c.executable = "relative";
            CHECK(gt_ext_channel_open_process(&c, &channel) == GT_ERR_INVALID_ARG && !channel);
            int f[2];
            CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
            CHECK(gt_ext_channel_open_fd(f[0], f[0], &channel) == GT_OK);
            CHECK(gt_ext_channel_process_info(channel, &i) == GT_ERR_UNSUPPORTED &&
                  !i.child_exited && !i.diagnostic_size);
            gt_ext_session_config_t sc = session_config();
            sc.features = 0;
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &sc, &session) == GT_OK && !channel &&
                  session);
            memset(&i, 255, sizeof(i));
            CHECK(gt_ext_session_process_info(session, &i) == GT_ERR_UNSUPPORTED &&
                  !i.child_exited && !i.diagnostic_size);
            CHECK(!close(f[1]));
        }
        CHECK(fcntl(directory, F_GETFD) >= 0);
    } else if (!strcmp(mode, "duplex")) {
        c = config("echo");
        open_child(&c);
        uint8_t ready;
        gt_ext_io_t r;
        do {
            r = gt_ext_channel_read(channel, &ready, 1, 5);
        } while (r.state == GT_EXT_WAIT);
        CHECK(r.state == GT_EXT_DATA && ready == 'R');
        uint8_t input[16384], output[16384];
        size_t received = 0;
        for (size_t block = 0; block < 16; block++) {
            for (size_t j = 0; j < sizeof(input); j++) {
                input[j] = (uint8_t)((j + block) % 251);
            }
            CHECK(gt_ext_channel_write(channel, input, sizeof(input)).count == sizeof(input));
            size_t at = 0;
            uint64_t end = now() + 2000;
            while (at < sizeof(input) && now() < end) {
                CHECK(gt_ext_channel_step(channel, 1) == GT_EXT_READY);
                r = gt_ext_channel_read(channel, output, sizeof(output), 1);
                CHECK(r.state == GT_EXT_DATA || r.state == GT_EXT_WAIT);
                CHECK(at + r.count <= sizeof(input) && !memcmp(output, input + at, r.count));
                at += r.count;
            }
            CHECK(at == sizeof(input));
            received += at;
        }
        CHECK(info().stdout_bytes == received + 1);
        diagnostic();
        release();
        CHECK(info().reaped);
    } else if (!strcmp(mode, "adopt") || !strcmp(mode, "capacity") || !strcmp(mode, "runtime")) {
        c = config(!strcmp(mode, "capacity") ? "hold" : "session");
        open_child(&c);
        adopt(strcmp(mode, "capacity") != 0);
#ifdef GT_TEST_PLUGIN_JS
        if (!strcmp(mode, "runtime")) {
            const char *source = "export function activate(){return {dispose(){}}}";
            CHECK(argc == 5);
            const char *names[] = {"index.js",
                                   "codec.js",
                                   "session.js",
                                   "wire-contract.js",
                                   "table.js",
                                   "diff.js",
                                   "task.js"};
            const char *ids[] = {"sdk:/index.js",
                                 "sdk:/codec.js",
                                 "sdk:/session.js",
                                 "sdk:/wire-contract.js",
                                 "sdk:/table.js",
                                 "sdk:/diff.js",
                                 "sdk:/task.js"};

            enum {
                SDK_COUNT = sizeof(names) / sizeof(names[0])
            };

            gt_ext_runtime_module_t modules[SDK_COUNT + 1] = {{0}};
            char *texts[SDK_COUNT];
            for (size_t n = 0; n < SDK_COUNT; n++) {
                char path[4096];
                CHECK(snprintf(path, sizeof(path), "%s/%s", argv[4], names[n]) > 0);
                FILE *file = fopen(path, "rb");
                CHECK(file && !fseek(file, 0, SEEK_END));
                long size = ftell(file);
                CHECK(size > 0 && size <= 131072 && !fseek(file, 0, SEEK_SET));
                texts[n] = calloc(1, (size_t)size + 1);
                CHECK(texts[n] && fread(texts[n], 1, (size_t)size, file) == (size_t)size &&
                      !fclose(file));
                modules[n] = (gt_ext_runtime_module_t){
                    .name = ids[n], .source = texts[n], .size = (size_t)size};
                gt_sha256_t digest;
                gt__sha256_init(&digest);
                CHECK(gt__sha256_update(&digest, texts[n], (size_t)size) &&
                      gt__sha256_final(&digest, modules[n].sha256));
            }
            modules[SDK_COUNT] = (gt_ext_runtime_module_t){
                .name = "plugin:/entry.js", .source = source, .size = strlen(source)};
            gt_sha256_t hash;
            gt__sha256_init(&hash);
            CHECK(gt__sha256_update(&hash, source, strlen(source)) &&
                  gt__sha256_final(&hash, modules[SDK_COUNT].sha256));
            gt_ext_runtime_config_t rc = {.modules = modules,
                                          .module_count = SDK_COUNT + 1,
                                          .entry = "plugin:/entry.js",
                                          .role = GT_EXT_HOST};
            CHECK(gt_ext_runtime_open(&session, &rc, &runtime) == GT_OK && !session && runtime);
            for (size_t n = 0; n < SDK_COUNT; n++) {
                free(texts[n]);
            }
            CHECK(!info().child_exited);
            CHECK(gt_ext_runtime_step(runtime) == GT_EXT_RUNTIME_ACTIVE);
        }
#endif
        if (!strcmp(mode, "capacity")) {
            gt_ext_channel_t *extra[15] = {0};
            int peers[15];
            for (size_t n = 0; n < 15; n++) {
                int f[2];
                CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
                peers[n] = f[1];
                CHECK(gt_ext_channel_open_fd(f[0], f[0], &extra[n]) == GT_OK);
            }
            gt_ext_channel_t *rejected = NULL;
            CHECK(gt_ext_channel_open_process(&c, &rejected) == GT_ERR_IO && !rejected);
            release();
            CHECK(info().reaped);
            CHECK(gt_ext_channel_open_process(&c, &rejected) == GT_ERR_IO && !rejected);
            CHECK(gt_ext_session_destroy(&session) == GT_OK && !session);
            CHECK(gt_ext_channel_open_process(&c, &channel) == GT_OK && channel);
            for (size_t n = 0; n < 15; n++) {
                CHECK(gt_ext_channel_close(extra[n], 0) == GT_EXT_RELEASED);
                CHECK(gt_ext_channel_destroy(&extra[n]) == GT_OK);
                CHECK(!close(peers[n]));
            }
        } else {
            release();
            gt_ext_transport_process_info_t i = info();
            CHECK(i.child_exited && i.reaped && i.exit_code == 37 && !i.term_signal);
            diagnostic();
        }
    } else {
        const char *context_arguments[] = {
            "raw argv zero", "context", "", "目录⚠️", "$(touch nope); ' \\"};
        if (!strcmp(mode, "context")) {
            c.arguments = context_arguments;
            c.argument_count = 5;
        }
        if (!strcmp(mode, "stdout-limit")) {
            c = config("tail");
            c.stdout_limit = 2;
        }
        if (!strcmp(mode, "stderr-limit")) {
            c = config("stderr");
            c.stderr_limit = 3;
        }
        if (!strcmp(mode, "deadline")) {
            c = config("hold");
            c.lifetime_ms = 30;
        }
        if (!strcmp(mode, "wake")) {
            c = config("hold");
        }
        open_child(&c);
        uint8_t bytes[32768];
        bool error = !strcmp(mode, "stdout-limit") || !strcmp(mode, "stderr-limit") ||
                     !strcmp(mode, "deadline");
        if (!strcmp(mode, "eof-hold")) {
            gt_ext_io_t r;
            uint64_t end = now() + 2000;
            do {
                r = gt_ext_channel_read(channel, bytes, sizeof(bytes), 5);
            } while (r.state == GT_EXT_WAIT && now() < end);
            CHECK(r.state == GT_EXT_EOF && !info().child_exited && !info().reaped);
            release();
            CHECK(info().reaped);
        } else if (!strcmp(mode, "wake")) {
            gt_ext_io_t r;
            do {
                r = gt_ext_channel_read(channel, bytes, sizeof(bytes), 5);
            } while (r.state == GT_EXT_WAIT);
            CHECK(r.state == GT_EXT_DATA && r.count == 1);
            pthread_t thread;
            CHECK(!pthread_create(&thread, NULL, stop_worker, channel));
            uint64_t start = now();
            r = gt_ext_channel_read(channel, bytes, sizeof(bytes), 1000);
            CHECK(!pthread_join(thread, NULL));
            CHECK(r.state == GT_EXT_IO_CLOSED && now() - start < 500 && info().stopping);
            release();
            CHECK(info().reaped);
        } else {
            size_t size = read_all(bytes, sizeof(bytes), error);
            gt_ext_transport_process_info_t i = info();
            if (error) {
                CHECK(i.error != GT_OK);
                CHECK(i.reason ==
                      (!strcmp(mode, "stdout-limit")   ? GT_EXT_PROCESS_TRANSPORT_STDOUT_LIMIT
                       : !strcmp(mode, "stderr-limit") ? GT_EXT_PROCESS_TRANSPORT_STDERR_LIMIT
                                                       : GT_EXT_PROCESS_TRANSPORT_DEADLINE));
            } else {
                CHECK(i.child_exited && i.reaped);
                if (!strcmp(mode, "signal")) {
                    CHECK(i.term_signal == SIGUSR1);
                } else {
                    CHECK(i.exit_code == 23 && !i.term_signal);
                }
                if (!strcmp(mode, "tail")) {
                    CHECK(size == 6 && !memcmp(bytes, "tail\0\377", 6));
                    diagnostic();
                }
                if (!strcmp(mode, "context")) {
                    size_t at = 0;
                    for (size_t n = 0; n < 5; n++) {
                        size_t length = strlen(context_arguments[n]) + 1;
                        CHECK(at + length <= size &&
                              !memcmp(bytes + at, context_arguments[n], length));
                        at += length;
                    }
                    size_t length = strlen(cwd) + 1;
                    CHECK(at + length <= size && !memcmp(bytes + at, cwd, length));
                    at += length;
                    CHECK(size - at == 21 && !memcmp(bytes + at, "only\0no-home\0no-path\0", 21));
                    diagnostic();
                }
            }
            release();
            CHECK(info().reaped && info().reason == i.reason);
        }
    }
    cleanup();
    puts("PASS process_channel");
    return 0;
}
