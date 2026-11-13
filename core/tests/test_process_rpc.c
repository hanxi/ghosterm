#define EXT_RPC_MAIN process_rpc_fixture_main
#include "test_ext_rpc.c"
#include "extension/process-rpc.h"
#include "extension/process-owner.h"
#include "extension/session-owner.h"
#include "extension/memory.h"
#include "extension/frame.h"
#include <inttypes.h>
#include <signal.h>

typedef struct {
    gt_process_rpc_t *broker;
    uint64_t sequence;
    char reply[GT_PROCESS_RPC_MESSAGE_MAX + 1];
} broker_fixture_t;

static const char *broker_call(broker_fixture_t *b, const char *fields) {
    char input[GT_PROCESS_RPC_MESSAGE_MAX + 1];
    int n = snprintf(
        input, sizeof(input), "{\"v\":1,\"id\":\"%" PRIu64 "\",%s}", ++b->sequence, fields);
    CHECK(n > 0 && (size_t)n < sizeof(input));
    CHECK(gt__process_rpc_call(b->broker, input, (size_t)n) == GT_PROCESS_RPC_OK);
    const uint8_t *bytes;
    size_t size;
    CHECK(gt__process_rpc_reply(b->broker, &bytes, &size) && size < sizeof(b->reply));
    memcpy(b->reply, bytes, size);
    b->reply[size] = 0;
    char id[64];
    CHECK(snprintf(id, sizeof(id), "\"id\":\"%" PRIu64 "\"", b->sequence) > 0 &&
          strstr(b->reply, id));
    CHECK(gt__process_rpc_consume(b->broker) == GT_PROCESS_RPC_OK);
    CHECK(!gt__process_rpc_reply(b->broker, &bytes, &size) && !bytes && !size);
    return b->reply;
}

static uint64_t broker_ref(const char *reply, const char *key) {
    char needle[32];
    CHECK(snprintf(needle, sizeof(needle), "\"%s\":\"", key) > 0);
    const char *at = strstr(reply, needle);
    CHECK(at);
    return strtoull(at + strlen(needle), NULL, 10);
}

static const char *broker_reference(broker_fixture_t *b, const char *op, uint64_t ref) {
    char input[128];
    CHECK(snprintf(input, sizeof(input), "\"op\":\"%s\",\"ref\":\"%" PRIu64 "\"", op, ref) > 0);
    return broker_call(b, input);
}

static uint64_t broker_request(broker_fixture_t *b, uint32_t timeout) {
    char fields[256];
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"request\",\"method\":\"fixture.ask\","
                   "\"input\":{\"text\":\"中文\"},\"timeoutMs\":%u",
                   timeout) > 0);
    const char *reply = broker_call(b, fields);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    return broker_ref(reply, "handle");
}

static uint64_t broker_take(broker_fixture_t *b, rpc_pair_t *f) {
    for (size_t i = 0; i < 2000; i++) {
        rpc_pump(f);
        const char *reply = broker_call(b, "\"op\":\"take\",\"methods\":[\"fixture.echo\"]");
        if (strstr(reply, "\"status\":\"ok\"")) {
            CHECK(strstr(reply, "\"method\":\"fixture.echo\"") &&
                  strstr(reply, "\"input\":{\"x\":1}"));
            return broker_ref(reply, "ticket");
        }
        CHECK(strstr(reply, "\"status\":\"wait\""));
    }
    CHECK(false);
    return 0;
}

static void broker_respond(broker_fixture_t *b, uint64_t ticket, bool interrupted) {
    char fields[256];
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"respond\",\"ref\":\"%" PRIu64 "\","
                   "\"outcome\":{\"kind\":\"success\",\"value\":{\"x\":1}}",
                   ticket) > 0);
    const char *reply = broker_call(b, fields);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    CHECK((strstr(reply, "\"interrupted\":true") != NULL) == interrupted);
}

static void broker_roundtrip(broker_fixture_t *b, rpc_pair_t *f, const char *mode) {
    uint64_t host = rpc_start(f, false, "{\"x\":1}", 1000);
    uint64_t ticket = broker_take(b, f);
    CHECK(strstr(broker_reference(b, "check", ticket), "\"status\":\"ok\""));
    if (!strcmp(mode, "incoming-cancel")) {
        CHECK(gt_ext_session_cancel_request(f->pair.h, &f->h, host) == GT_EXT_RPC_OK);
        for (size_t i = 0; i < 50; i++) {
            rpc_pump(f);
        }
        CHECK(strstr(broker_reference(b, "check", ticket), "\"status\":\"closed\""));
    }
    broker_respond(b, ticket, !strcmp(mode, "incoming-cancel"));
    gt_ext_rpc_result_t result = rpc_complete(f, false, host);
    CHECK(result.state ==
          (!strcmp(mode, "incoming-cancel") ? GT_EXT_RPC_UNKNOWN : GT_EXT_RPC_SUCCEEDED));
    CHECK(gt_ext_session_forget_request(f->pair.h, &f->h, host) == GT_EXT_RPC_OK);
    CHECK(strstr(broker_reference(b, "check", ticket), "\"status\":\"invalid\""));
    uint64_t outgoing = broker_request(b, 1000);
    CHECK(outgoing != ticket);
    CHECK(strstr(broker_reference(b, "result", ticket), "\"status\":\"invalid\""));
    CHECK(strstr(broker_reference(b, "check", outgoing), "\"status\":\"invalid\""));
    gt_ext_rpc_request_t q = rpc_take(f, true);
    rpc_reply(f->pair.h, &f->h, &q);
    bool complete = false;
    for (size_t i = 0; i < 2000 && !complete; i++) {
        rpc_pump(f);
        const char *reply = broker_reference(b, "result", outgoing);
        complete = strstr(reply, "\"state\":\"succeeded\"") != NULL;
        if (complete) {
            CHECK(strstr(reply, "\"confirmed\":true") && strstr(reply, "\"text\":\"中文\""));
        }
    }
    CHECK(complete);
    CHECK(strstr(broker_reference(b, "forget", outgoing), "\"status\":\"ok\""));
    CHECK(strstr(broker_reference(b, "result", outgoing), "\"status\":\"invalid\""));
}

static bool peer_read(void *bytes, size_t size) {
    size_t at = 0;
    while (at < size) {
        ssize_t n = read(STDIN_FILENO, (char *)bytes + at, size - at);
        if (n <= 0) {
            return false;
        }
        at += (size_t)n;
    }
    return true;
}

static void peer_call(uint64_t *sequence, const char *fields, char *reply) {
    char input[1024];
    int n =
        snprintf(input, sizeof(input), "{\"v\":1,\"id\":\"%" PRIu64 "\",%s}", ++*sequence, fields);
    CHECK(n > 0 && (size_t)n < sizeof(input));
    unsigned char header[4] = {(unsigned char)((unsigned)n >> 24),
                               (unsigned char)((unsigned)n >> 16),
                               (unsigned char)((unsigned)n >> 8),
                               (unsigned char)n};
    CHECK(write(STDOUT_FILENO, header, 4) == 4 && write(STDOUT_FILENO, input, (size_t)n) == n);
    CHECK(peer_read(header, 4));
    size_t size = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) | ((size_t)header[2] << 8) |
                  header[3];
    CHECK(size && size <= GT_PROCESS_RPC_MESSAGE_MAX && peer_read(reply, size));
    reply[size] = 0;
}

static int peer_main(void) {
    CHECK(!getenv("HOME") && !getenv("NODE_OPTIONS"));
    uint64_t sequence = 0;
    char reply[GT_PROCESS_RPC_MESSAGE_MAX + 1];
    peer_call(&sequence,
              "\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":{\"peer\":true},"
              "\"timeoutMs\":10000",
              reply);
    CHECK(strstr(reply, "\"handle\":\"1\""));
    do {
        peer_call(&sequence, "\"op\":\"result\",\"ref\":\"1\"", reply);
    } while (!strstr(reply, "\"state\":\"succeeded\""));
    CHECK(strstr(reply, "\"confirmed\":true") && strstr(reply, "\"peer\":true"));
    peer_call(&sequence, "\"op\":\"forget\",\"ref\":\"1\"", reply);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    do {
        peer_call(&sequence, "\"op\":\"take\",\"methods\":[\"fixture.echo\"]", reply);
    } while (strstr(reply, "\"status\":\"wait\""));
    CHECK(strstr(reply, "\"ticket\":\"2\""));
    peer_call(&sequence, "\"op\":\"check\",\"ref\":\"2\"", reply);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    peer_call(&sequence,
              "\"op\":\"respond\",\"ref\":\"2\",\"outcome\":{\"kind\":\"success\",\"value\":{"
              "\"peer\":true}}",
              reply);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    return 0;
}

static void broker_pipe(broker_fixture_t *b, rpc_pair_t *f, int argc, char **argv) {
    bool node = !strncmp(argv[1], "pipe-node", 9);
    bool sdk = !strncmp(argv[1], "pipe-node-sdk", 13);
    bool cancel = !strcmp(argv[1], "pipe-node-sdk-cancel");
    CHECK(argc >= 3 && (!node || argc == 5));
    char executable[4097];
    CHECK(realpath(node ? argv[3] : argv[0], executable));
    const char *args[] = {executable, node ? argv[4] : "--peer"};
    int cwd = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cwd >= 0);
    gt_ext_process_t *child = NULL;
    /* Explicit test-tool environment, never the parent's HOME/NODE_OPTIONS.
     * Instrumented peers need a symbolizer PATH even before user main(). */
    const char *environment[] = {"PATH=/usr/bin:/bin",
                                 "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1",
                                 "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"};
    CHECK(gt__ext_process_spawn(executable, args, 2, cwd, environment, 3, &child) == GT_OK &&
          child && !close(cwd));
    gt_bridge_decoder_t *decoder = NULL;
    CHECK(gt__bridge_decoder_new(GT_PROCESS_RPC_MESSAGE_MAX, NULL, &decoder) == GT_OK);
    uint64_t handle = rpc_start(f, false, "{\"peer\":true}", 10000);
    uint8_t input[16384], output[GT_PROCESS_RPC_MESSAGE_MAX + 4];
    size_t input_at = 0, input_size = 0, output_at = 0, output_size = 0;
    gt_ext_process_info_t state = {0};
    bool replied = false, done = false, cancelled = false;
    uint64_t deadline = ext_now() + 20000;
    while (!state.exited && ext_now() < deadline) {
        rpc_pump(f);
        gt_ext_rpc_request_t q;
        gt_ext_rpc_status_t taken = gt_ext_session_take_request(f->pair.h, &f->h, &q);
        if (taken == GT_EXT_RPC_OK) {
            CHECK(!strcmp(q.input, "{\"peer\":true}"));
            if (!strcmp(q.method, "fixture.ask")) {
                CHECK(!replied);
                replied = true;
            } else {
                CHECK(sdk && !done && !strcmp(q.method, "fixture.done"));
                gt_ext_rpc_result_t result = rpc_result(f->pair.h, &f->h, handle);
                CHECK(result.confirmed &&
                      result.state == (cancel ? GT_EXT_RPC_UNKNOWN : GT_EXT_RPC_SUCCEEDED));
                done = true;
            }
            rpc_reply(f->pair.h, &f->h, &q);
        } else {
            CHECK(taken == GT_EXT_RPC_WAIT || (sdk && done && taken == GT_EXT_RPC_CLOSED));
        }
        if (output_at < output_size) {
            gt_ext_process_io_t io =
                gt__ext_process_write(child, output + output_at, output_size - output_at);
            CHECK(io.state == GT_EXT_PROCESS_DATA || io.state == GT_EXT_PROCESS_WAIT);
            output_at += io.count;
            if (output_at == output_size) {
                CHECK(gt__process_rpc_consume(b->broker) == GT_PROCESS_RPC_OK);
                output_at = output_size = 0;
            }
        }
        if (input_at == input_size) {
            gt_ext_process_io_t io = gt__ext_process_read_stdout(child, input, sizeof(input));
            CHECK(io.state == GT_EXT_PROCESS_DATA || io.state == GT_EXT_PROCESS_WAIT ||
                  io.state == GT_EXT_PROCESS_EOF);
            input_at = 0;
            input_size = io.count;
        }
        if (!output_size && input_at < input_size) {
            size_t consumed;
            CHECK(gt__bridge_decoder_feed(
                      decoder, input + input_at, input_size - input_at, &consumed) == GT_OK);
            input_at += consumed;
            const uint8_t *bytes;
            size_t size;
            if (gt__bridge_decoder_view(decoder, &bytes, &size)) {
                char message[GT_PROCESS_RPC_MESSAGE_MAX + 1];
                memcpy(message, bytes, size);
                message[size] = 0;
                if (cancel && !cancelled && strstr(message, "\"op\":\"respond\"")) {
                    CHECK(gt_ext_session_cancel_request(f->pair.h, &f->h, handle) == GT_EXT_RPC_OK);
                    for (size_t i = 0; i < 50; i++) {
                        rpc_pump(f);
                    }
                    cancelled = true;
                }
                CHECK(gt__process_rpc_call(b->broker, bytes, size) == GT_PROCESS_RPC_OK);
                CHECK(gt__process_rpc_reply(b->broker, &bytes, &size));
                output[0] = (uint8_t)(size >> 24);
                output[1] = (uint8_t)(size >> 16);
                output[2] = (uint8_t)(size >> 8);
                output[3] = (uint8_t)size;
                memcpy(output + 4, bytes, size);
                output_size = size + 4;
                CHECK(gt__bridge_decoder_consume(decoder) == GT_OK);
            }
        }
        char errors[1024];
        gt_ext_process_io_t io = gt__ext_process_read_stderr(child, errors, sizeof(errors));
        if (io.count) {
            fprintf(stderr, "child: %.*s\n", (int)io.count, errors);
        }
        CHECK(!io.count);
        CHECK(gt__ext_process_poll(child, &state) == GT_OK);
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    if (!state.exited) {
        CHECK(gt__ext_process_signal(child, SIGKILL) == GT_OK);
        for (size_t i = 0; i < 1000 && !state.exited; i++) {
            CHECK(gt__ext_process_poll(child, &state) == GT_OK);
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CHECK(state.exited && !state.term_signal && !state.exit_code && replied && (!sdk || done) &&
          cancelled == cancel);
    CHECK(rpc_complete(f, false, handle).state ==
          (cancel ? GT_EXT_RPC_UNKNOWN : GT_EXT_RPC_SUCCEEDED));
    CHECK(gt_ext_session_forget_request(f->pair.h, &f->h, handle) == GT_EXT_RPC_OK);
    CHECK(!output_size && gt__ext_process_destroy(&child) == GT_OK && !child);
    gt__bridge_decoder_free(decoder);
}

static const char *broker_bad(const char *mode) {
    if (!strcmp(mode, "version")) {
        return "{\"v\":2,\"id\":\"1\",\"op\":\"revoke\"}";
    }
    if (!strcmp(mode, "numeric-id")) {
        return "{\"v\":1,\"id\":1,\"op\":\"revoke\"}";
    }
    if (!strcmp(mode, "leading-zero")) {
        return "{\"v\":1,\"id\":\"01\",\"op\":\"revoke\"}";
    }
    if (!strcmp(mode, "overflow")) {
        return "{\"v\":1,\"id\":\"18446744073709551616\",\"op\":\"revoke\"}";
    }
    if (!strcmp(mode, "sequence")) {
        return "{\"v\":1,\"id\":\"2\",\"op\":\"revoke\"}";
    }
    if (!strcmp(mode, "duplicate")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"revoke\",\"op\":\"take\"}";
    }
    if (!strcmp(mode, "escaped-key")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"revoke\",\"\\u006f\\u0070\":\"take\"}";
    }
    if (!strcmp(mode, "unknown-field")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"revoke\",\"token\":\"forged\"}";
    }
    if (!strcmp(mode, "unknown-op")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"shell\"}";
    }
    if (!strcmp(mode, "missing")) {
        return "{\"v\":1,\"op\":\"revoke\"}";
    }
    if (!strcmp(mode, "utf8")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"\xc0\x80\"}";
    }
    if (!strcmp(mode, "nul")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"rev\\u0000oke\"}";
    }
    if (!strcmp(mode, "nested-duplicate")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":{"
               "\"x\":1,\"x\":2},\"timeoutMs\":1}";
    }
    if (!strcmp(mode, "timeout-fraction")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":{},"
               "\"timeoutMs\":1.5}";
    }
    if (!strcmp(mode, "duplicate-methods")) {
        return "{\"v\":1,\"id\":\"1\",\"op\":\"take\",\"methods\":[\"fixture.echo\",\"fixture."
               "echo\"]}";
    }
    return NULL;
}

typedef struct {
    size_t calls, fail, live;
    bool wiped;
} broker_memory_t;

static void *broker_allocate(void *ctx, size_t n) {
    broker_memory_t *m = ctx;
    if (++m->calls == m->fail) {
        return NULL;
    }
    void *p = malloc(n);
    if (p) {
        m->live++;
        memset(p, 0xa5, n);
    }
    return p;
}

static void broker_release(void *ctx, void *p, size_t n) {
    broker_memory_t *m = ctx;
    for (size_t i = 0; i < n; i++) {
        if (((uint8_t *)p)[i]) {
            m->wiped = false;
        }
    }
    m->live--;
    free(p);
}

int main(int argc, char **argv) {
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "--peer")) {
        return peer_main();
    }
    const char *mode = argv[1];
    rpc_pair_t f;
    rpc_open(&f, strstr(mode, "timeout") ? "timeout" : "roundtrip");
    broker_fixture_t b = {0};
    if (!strcmp(mode, "allocation")) {
        for (size_t fail = 1; fail <= 3; fail++) {
            broker_memory_t memory = {.fail = fail, .wiped = true};
            gt_allocator_t a = {&memory, broker_allocate, broker_release};
            gt_status_t status = gt__process_rpc_open(f.pair.e, &a, &b.broker);
            CHECK(status == (fail < 3 ? GT_ERR_IO : GT_OK));
            if (b.broker) {
                CHECK(gt__process_rpc_destroy(&b.broker) == GT_OK);
            }
            CHECK(!memory.live && memory.wiped && !f.pair.e->sources);
        }
    } else if (!strcmp(mode, "adoption")) {
        uint64_t handle = rpc_start(&f, true, "{}", 1000);
        CHECK(gt__process_rpc_open(f.pair.e, NULL, &b.broker) == GT_ERR_INVALID_ARG && !b.broker);
        CHECK(gt_ext_session_cancel_request(f.pair.e, &f.e, handle) == GT_EXT_RPC_OK &&
              gt_ext_session_forget_request(f.pair.e, &f.e, handle) == GT_EXT_RPC_OK);
        CHECK(gt__process_rpc_open(f.pair.e, NULL, &b.broker) == GT_OK && b.broker);
        CHECK(gt__process_rpc_destroy(&b.broker) == GT_OK);
    } else if (!strcmp(mode, "global-memory")) {
        gt_plugin_memory_info_t budget = gt__plugin_memory_info();
        size_t size = budget.limit - budget.used - 1;
        void *reservation = gt__plugin_memory_alloc(NULL, size);
        CHECK(reservation);
        CHECK(gt__process_rpc_open(f.pair.e, NULL, &b.broker) == GT_ERR_IO && !b.broker &&
              !f.pair.e->sources);
        gt__plugin_memory_free(NULL, reservation, size);
    } else if (!strcmp(mode, "invalid")) {
        CHECK(gt__process_rpc_open(NULL, NULL, &b.broker) == GT_ERR_INVALID_ARG && !b.broker);
        CHECK(gt__process_rpc_open(f.pair.h, NULL, &b.broker) == GT_ERR_UNSUPPORTED && !b.broker);
        CHECK(gt__process_rpc_open(f.pair.e, NULL, NULL) == GT_ERR_INVALID_ARG);
        CHECK(gt__process_rpc_call(NULL, NULL, 0) == GT_PROCESS_RPC_INVALID);
        CHECK(gt__process_rpc_destroy(NULL) == GT_ERR_INVALID_ARG);
    } else {
        CHECK(gt__process_rpc_open(f.pair.e, NULL, &b.broker) == GT_OK && b.broker);
        const char *bad = broker_bad(mode);
        if (bad || !strcmp(mode, "replay")) {
            if (!bad) {
                broker_call(&b, "\"op\":\"take\",\"methods\":[]");
                bad = "{\"v\":1,\"id\":\"1\",\"op\":\"take\",\"methods\":[]}";
            }
            CHECK(gt__process_rpc_call(b.broker, bad, strlen(bad)) == GT_PROCESS_RPC_PROTOCOL);
            CHECK(gt__process_rpc_call(b.broker, "{}", 2) == GT_PROCESS_RPC_PROTOCOL);
            CHECK(gt_ext_session_access(f.pair.e, &f.e, "ui.form") == GT_EXT_ACCESS_REVOKED);
        } else if (!strcmp(mode, "oversize")) {
            char bytes[GT_PROCESS_RPC_MESSAGE_MAX + 1];
            memset(bytes, ' ', sizeof(bytes));
            const char *header = "{\"v\":1,\"id\":\"1\",\"op\":\"take\",\"methods\":[]}";
            memcpy(bytes, header, strlen(header));
            CHECK(gt__process_rpc_call(b.broker, bytes, sizeof(bytes)) == GT_PROCESS_RPC_PROTOCOL);
        } else if (!strcmp(mode, "message-boundary")) {
            char bytes[GT_PROCESS_RPC_MESSAGE_MAX];
            const char *header = "{\"v\":1,\"id\":\"1\",\"op\":\"take\",\"methods\":[]}";
            memset(bytes, ' ', sizeof(bytes));
            memcpy(bytes, header, strlen(header));
            CHECK(gt__process_rpc_call(b.broker, bytes, sizeof(bytes)) == GT_PROCESS_RPC_OK);
            CHECK(gt__process_rpc_consume(b.broker) == GT_PROCESS_RPC_OK);
        } else if (!strcmp(mode, "body-boundary") || !strcmp(mode, "body-limit")) {
            char bytes[GT_PROCESS_RPC_MESSAGE_MAX];
            const char *prefix =
                "{\"v\":1,\"id\":\"1\",\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":";
            strcpy(bytes, prefix);
            char *body = bytes + strlen(bytes);
            size_t size = GT_EXT_RPC_BODY_MAX + (!strcmp(mode, "body-limit") ? 1 : 0);
            memcpy(body, "{\"text\":\"", 9);
            memset(body + 9, 'x', size - 11);
            memcpy(body + size - 2, "\"}", 2);
            strcpy(body + size, ",\"timeoutMs\":1000}");
            CHECK(gt__process_rpc_call(b.broker, bytes, strlen(bytes)) ==
                  (!strcmp(mode, "body-limit") ? GT_PROCESS_RPC_PROTOCOL : GT_PROCESS_RPC_OK));
        } else if (!strcmp(mode, "depth")) {
            char bytes[512] =
                "{\"v\":1,\"id\":\"1\",\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":";
            for (size_t i = 0; i < 33; i++) {
                strcat(bytes, "[");
            }
            strcat(bytes, "0");
            for (size_t i = 0; i < 33; i++) {
                strcat(bytes, "]");
            }
            strcat(bytes, ",\"timeoutMs\":1000}");
            CHECK(gt__process_rpc_call(b.broker, bytes, strlen(bytes)) == GT_PROCESS_RPC_PROTOCOL);
        } else if (!strcmp(mode, "semantic-rejection")) {
            CHECK(strstr(broker_call(&b,
                                     "\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":[],"
                                     "\"timeoutMs\":1000"),
                         "\"status\":\"invalid\""));
            CHECK(strstr(broker_call(&b,
                                     "\"op\":\"request\",\"method\":\"fixture.missing\",\"input\":{"
                                     "},\"timeoutMs\":1000"),
                         "\"status\":\"unsupported\""));
            CHECK(strstr(broker_call(&b,
                                     "\"op\":\"request\",\"method\":\"fixture.echo\",\"input\":{},"
                                     "\"timeoutMs\":1000"),
                         "\"status\":\"unsupported\""));
        } else if (!strcmp(mode, "method-filter")) {
            uint64_t handle = rpc_start(&f, false, "{\"x\":1}", 1000);
            for (size_t i = 0; i < 50; i++) {
                rpc_pump(&f);
            }
            CHECK(strstr(broker_call(&b, "\"op\":\"take\",\"methods\":[]"), "\"status\":\"wait\""));
            CHECK(strstr(broker_call(&b, "\"op\":\"take\",\"methods\":[\"fixture.ask\"]"),
                         "\"status\":\"wait\""));
            uint64_t ticket = broker_take(&b, &f);
            char invalid[128];
            CHECK(snprintf(invalid,
                           sizeof(invalid),
                           "\"op\":\"respond\",\"ref\":\"%" PRIu64 "\",\"outcome\":{}",
                           ticket) > 0);
            CHECK(strstr(broker_call(&b, invalid), "\"status\":\"invalid\""));
            CHECK(strstr(broker_reference(&b, "check", ticket), "\"status\":\"ok\""));
            broker_respond(&b, ticket, false);
            CHECK(rpc_complete(&f, false, handle).state == GT_EXT_RPC_SUCCEEDED);
            CHECK(gt_ext_session_forget_request(f.pair.h, &f.h, handle) == GT_EXT_RPC_OK);
        } else if (!strcmp(mode, "methods-limit")) {
            char bytes[1024] = "{\"v\":1,\"id\":\"1\",\"op\":\"take\",\"methods\":[";
            for (size_t i = 0; i < 17; i++) {
                char item[32];
                CHECK(snprintf(item, sizeof(item), "%s\"fixture.m%zu\"", i ? "," : "", i) > 0);
                strcat(bytes, item);
            }
            strcat(bytes, "]}");
            CHECK(gt__process_rpc_call(b.broker, bytes, strlen(bytes)) == GT_PROCESS_RPC_PROTOCOL);
        } else if (!strcmp(mode, "roundtrip") || !strcmp(mode, "incoming-cancel")) {
            broker_roundtrip(&b, &f, mode);
        } else if (!strcmp(mode, "backpressure")) {
            const char *one = "{\"v\":1,\"id\":\"1\",\"op\":\"request\",\"method\":\"fixture.ask\","
                              "\"input\":{},\"timeoutMs\":1000}";
            CHECK(gt__process_rpc_call(b.broker, one, strlen(one)) == GT_PROCESS_RPC_OK);
            CHECK(gt__process_rpc_call(b.broker, one, strlen(one)) == GT_PROCESS_RPC_WAIT);
            const uint8_t *bytes;
            size_t n;
            CHECK(gt__process_rpc_reply(b.broker, &bytes, &n) &&
                  strstr((const char *)bytes, "\"handle\":\"1\""));
            CHECK(gt__process_rpc_consume(b.broker) == GT_PROCESS_RPC_OK);
            b.sequence = 1;
            CHECK(strstr(broker_reference(&b, "result", 2), "\"status\":\"invalid\""));
        } else if (!strcmp(mode, "cancel")) {
            uint64_t ref = broker_request(&b, 1000);
            CHECK(strstr(broker_reference(&b, "cancel", ref), "\"status\":\"ok\""));
            const char *reply = broker_reference(&b, "result", ref);
            CHECK(strstr(reply, "\"state\":\"cancelled\"") && strstr(reply, "\"confirmed\":false"));
            CHECK(strstr(broker_reference(&b, "forget", ref), "\"status\":\"ok\""));
        } else if (!strcmp(mode, "timeout") || !strcmp(mode, "late-reply-timeout") ||
                   !strcmp(mode, "revoke")) {
            uint64_t ref = broker_request(&b, strstr(mode, "timeout") ? 1 : 1000);
            gt_ext_rpc_request_t request = rpc_take(&f, true);
            if (!strcmp(mode, "late-reply-timeout")) {
                rpc_reply(f.pair.h, &f.h, &request);
            }
            if (strstr(mode, "timeout")) {
                f.pair.clock += 2;
            } else {
                CHECK(strstr(broker_call(&b, "\"op\":\"revoke\""), "\"status\":\"ok\""));
            }
            const char *reply = broker_reference(&b, "result", ref);
            CHECK(strstr(reply, "\"state\":\"unknown\"") && strstr(reply, "\"confirmed\":false"));
            if (!strcmp(mode, "late-reply-timeout")) {
                bool confirmed = false;
                for (size_t i = 0; i < 2000 && !confirmed; i++) {
                    rpc_pump(&f);
                    reply = broker_reference(&b, "result", ref);
                    confirmed = strstr(reply, "\"confirmed\":true") != NULL;
                }
                CHECK(confirmed && strstr(reply, "\"state\":\"succeeded\""));
            }
            CHECK(strstr(broker_reference(&b, "forget", ref), "\"status\":\"ok\""));
        } else if (!strcmp(mode, "reuse")) {
            for (size_t i = 0; i < 65; i++) {
                uint64_t ref = broker_request(&b, 1000);
                CHECK(ref == i + 1);
                CHECK(strstr(broker_reference(&b, "cancel", ref), "\"status\":\"ok\""));
                CHECK(strstr(broker_reference(&b, "forget", ref), "\"status\":\"ok\""));
            }
        } else if (!strcmp(mode, "capacity")) {
            for (size_t i = 0; i < 64; i++) {
                CHECK(broker_request(&b, 1000) == i + 1);
            }
            CHECK(strstr(broker_call(&b,
                                     "\"op\":\"request\",\"method\":\"fixture.ask\",\"input\":{},"
                                     "\"timeoutMs\":1000"),
                         "\"status\":\"wait\""));
        } else if (!strcmp(mode, "access")) {
            CHECK(strstr(broker_call(&b, "\"op\":\"access\",\"capability\":\"ui.form\""),
                         "\"access\":\"allowed\""));
            CHECK(strstr(broker_call(&b, "\"op\":\"access\",\"capability\":\"ui.chat\""),
                         "\"access\":\"unsupported\""));
            CHECK(strstr(broker_call(&b, "\"op\":\"access\",\"capability\":\"process.spawn\""),
                         "\"access\":\"unsupported\""));
            gt__process_rpc_revoke(b.broker);
            CHECK(strstr(broker_call(&b, "\"op\":\"access\",\"capability\":\"ui.form\""),
                         "\"access\":\"revoked\""));
        } else if (!strcmp(mode, "lifetime")) {
            gt_process_rpc_t *second = NULL;
            CHECK(gt__process_rpc_open(f.pair.e, NULL, &second) == GT_ERR_INVALID_ARG && !second);
            CHECK(gt_ext_session_close(f.pair.e, 0) == GT_EXT_DRAINING);
            CHECK(gt_ext_session_destroy(&f.pair.e) == GT_ERR_INVALID_ARG && f.pair.e);
        } else if (!strncmp(mode, "pipe-", 5)) {
            broker_pipe(&b, &f, argc, argv);
        } else {
            CHECK(false);
        }
        CHECK(gt__process_rpc_destroy(&b.broker) == GT_OK && !b.broker);
    }
    rpc_drop(&f);
    CHECK(!gt__plugin_memory_info().used);
    return 0;
}
