/* Owned test Executor using the actual process protocol; no account or tools. */
#define main dev_peer_previous_main
#include "../../core/tests/process-runtime-peer.c"
#undef main

#ifndef GT_DEV_RESOURCE_TIMEOUT_MS
#define GT_DEV_RESOURCE_TIMEOUT_MS 1000
#endif

static void dev_stop(int signal_number) {
    (void)signal_number;
    _exit(0);
}

static bool dev_callback(uint64_t *sequence, const char *input, char value[4096]) {
    char fields[4096], reply[8193], handle[32];
    int n =
        snprintf(fields,
                 sizeof(fields),
                 "\"op\":\"request\",\"method\":\"dev.callback\",\"input\":%s,\"timeoutMs\":1000",
                 input);
    CHECK(n > 0 && (size_t)n < sizeof(fields));
    call(sequence, fields, reply);
    app_json_t *json = app_parse(reply);
    uint16_t ref = app_member(json, 1, "handle");
    CHECK(app_string(json, app_member(json, 1, "status"), "ok") && ref &&
          json->nodes[ref].kind == J_STRING && json->nodes[ref].size < sizeof(handle));
    snprintf(handle,
             sizeof(handle),
             "%.*s",
             (int)json->nodes[ref].size,
             json->strings + json->nodes[ref].text);
    release(NULL, json, sizeof(*json));
    bool success = false;
    for (;;) {
        snprintf(fields, sizeof(fields), "\"op\":\"result\",\"ref\":\"%s\"", handle);
        call(sequence, fields, reply);
        json = app_parse(reply);
        CHECK(app_string(json, app_member(json, 1, "status"), "ok"));
        uint16_t state = app_member(json, 1, "state");
        if (app_string(json, state, "pending") || app_string(json, state, "queued")) {
            release(NULL, json, sizeof(*json));
            continue;
        }
        success = app_string(json, state, "succeeded");
        if (success) {
            uint16_t outcome = app_member(json, 1, "outcome"),
                     body = app_member(json, outcome, "value"),
                     text = app_member(json, body, "text");
            CHECK(text && json->nodes[text].kind == J_STRING && json->nodes[text].bytes >= 2);
            /* Preserve the callback's escaped JSON string, adding only an ASCII prefix. */
            uint16_t resource = app_member(json, body, "resource");
            if (resource) {
                CHECK(json->nodes[resource].kind == J_OBJECT);
                n = snprintf(value,
                             4096,
                             "{\"text\":\"executor:%.*s,\"resource\":%.*s}",
                             (int)json->nodes[text].bytes - 1,
                             json->input + json->nodes[text].start + 1,
                             (int)json->nodes[resource].bytes,
                             json->input + json->nodes[resource].start);
            } else {
                n = snprintf(value,
                             4096,
                             "{\"text\":\"executor:%.*s}",
                             (int)json->nodes[text].bytes - 1,
                             json->input + json->nodes[text].start + 1);
            }
            CHECK(n > 0 && n < 4096);
        }
        release(NULL, json, sizeof(*json));
        break;
    }
    snprintf(fields, sizeof(fields), "\"op\":\"forget\",\"ref\":\"%s\"", handle);
    call(sequence, fields, reply);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    return success;
}

static void dev_business(uint64_t *sequence, const char *kind, char outcome[3073]) {
    char fields[8192], reply[8193], handle[32];
    const char *mode = !strcmp(kind, "stderr")                              ? "stderr"
                       : (!strcmp(kind, "cancel") || !strcmp(kind, "wait")) ? "wait"
                                                                            : "roundtrip";
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"process\",\"action\":\"spawn\",\"program\":\"%s\",\"root\":\"%s\","
                   "\"argv\":[\"business\",\"quoted ' \\\" $() 中文\",\"%s\"],"
                   "\"envPolicy\":\"selected-user-environment\"",
                   !strcmp(kind, "missing") ? "missing" : "business",
                   !strcmp(kind, "outside") ? "outside" : "workspace",
                   mode) > 0);
    call(sequence, fields, reply);
    uint16_t value;
    app_json_t *json = app_proc_value(reply, &value);
    if (!strcmp(kind, "missing") || !strcmp(kind, "outside")) {
        CHECK(app_string(json, app_member(json, value, "status"), "denied"));
        release(NULL, json, sizeof(*json));
        strcpy(outcome, "{\"text\":\"DENIED\"}");
        return;
    }
    uint16_t ref = app_member(json, value, "ref");
    CHECK(app_string(json, app_member(json, value, "status"), "ok") &&
          app_string(json, app_member(json, value, "version"), "1.0.0") && ref &&
          json->nodes[ref].kind == J_STRING && json->nodes[ref].size < sizeof(handle));
    snprintf(handle,
             sizeof(handle),
             "%.*s",
             (int)json->nodes[ref].size,
             json->strings + json->nodes[ref].text);
    release(NULL, json, sizeof(*json));
    if (!strcmp(kind, "wait")) {
        for (;;) {
            pause();
        }
    }
    if (!strcmp(mode, "roundtrip")) {
        size_t sent = 0;
        while (sent < 2048) {
            char hex[2049];
            size_t count = 2048 - sent;
            if (count > 1024) {
                count = 1024;
            }
            for (size_t i = 0; i < count; i++) {
                snprintf(hex + i * 2, 3, "%02x", (unsigned)((sent + i) % 251));
            }
            snprintf(fields,
                     sizeof(fields),
                     "\"op\":\"process\",\"action\":\"write\",\"ref\":\"%s\",\"hex\":\"%s\"",
                     handle,
                     hex);
            call(sequence, fields, reply);
            json = app_proc_value(reply, &value);
            uint64_t wrote;
            CHECK(app_string(json, app_member(json, value, "status"), "ok") &&
                  node_uint(json, app_member(json, value, "count"), &wrote) && wrote <= count);
            CHECK(app_string(json, app_member(json, value, "state"), "data") ||
                  app_string(json, app_member(json, value, "state"), "wait"));
            sent += (size_t)wrote;
            release(NULL, json, sizeof(*json));
        }
    }
    app_proc_command(sequence, reply, handle, "closeStdin", "");
    app_proc_ok(reply);
    bool cancel = !strcmp(kind, "cancel");
    if (cancel) {
        size_t ready = 0;
        while (ready < 5) {
            uint8_t bytes[3072];
            size_t count;
            CHECK(app_proc_read(sequence, reply, handle, false, bytes, &count) != 2 &&
                  count <= 5 - ready);
            CHECK(!memcmp(bytes, &"ready"[ready], count));
            ready += count;
        }
        app_proc_command(sequence, reply, handle, "signal", ",\"signal\":\"terminate\"");
        app_proc_ok(reply);
    }
    uint8_t output[8193], errors[8193];
    size_t count = app_proc_collect(sequence, reply, handle, false, output);
    size_t error_count = app_proc_collect(sequence, reply, handle, true, errors);
    uint64_t code, signal_number;
    for (;;) {
        app_proc_command(sequence, reply, handle, "poll", "");
        json = app_proc_value(reply, &value);
        bool exited = app_string(json, app_member(json, value, "status"), "ok") &&
                      json->nodes[app_member(json, value, "exited")].kind == J_TRUE;
        if (exited) {
            CHECK(node_uint(json, app_member(json, value, "code"), &code) &&
                  node_uint(json, app_member(json, value, "signal"), &signal_number));
        }
        release(NULL, json, sizeof(*json));
        if (exited) {
            break;
        }
    }
    if (cancel) {
        CHECK(!code && signal_number == SIGTERM && !count && !error_count);
    } else if (!strcmp(mode, "stderr")) {
        CHECK(code == 7 && !signal_number && !count && error_count == 12 &&
              !memcmp(errors, "stderr-proof", 12));
    } else {
        CHECK(!code && !signal_number && !error_count && count == 2048);
        for (size_t i = 0; i < count; i++) {
            CHECK(output[i] == i % 251);
        }
    }
    app_proc_command(sequence, reply, handle, "release", "");
    app_proc_ok(reply);
    app_proc_command(sequence, reply, handle, "poll", "");
    json = app_proc_value(reply, &value);
    CHECK(app_string(json, app_member(json, value, "status"), "invalid"));
    release(NULL, json, sizeof(*json));
    if (cancel) {
        strcpy(outcome, "{\"text\":\"process:signal:15\"}");
    } else {
        snprintf(outcome, 3073, "{\"text\":\"process:%zu:%llu\"}", count, (unsigned long long)code);
    }
}

static void dev_publish(uint64_t *sequence) {
    char reply[8193], fields[512], handle[32];
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"resource\",\"action\":\"publish\",\"root\":\"workspace\",\"path64\":"
                   "\"ZmlsZS5iaW4=\",\"mime\":\"application/"
                   "octet-stream\",\"limit\":\"262144\",\"timeoutMs\":%u",
                   (unsigned)GT_DEV_RESOURCE_TIMEOUT_MS) > 0);
    call(sequence, fields, reply);
    app_json_t *json = app_parse(reply);
    uint16_t value = app_member(json, 1, "value"), ref = app_member(json, value, "handle");
    CHECK(app_string(json, app_member(json, value, "status"), "ok") && ref &&
          json->nodes[ref].kind == J_STRING && json->nodes[ref].size < sizeof(handle));
    snprintf(handle,
             sizeof(handle),
             "%.*s",
             (int)json->nodes[ref].size,
             json->strings + json->nodes[ref].text);
    release(NULL, json, sizeof(*json));
    for (;;) {
        snprintf(fields,
                 sizeof(fields),
                 "\"op\":\"resource\",\"action\":\"publication\",\"ref\":\"%s\"",
                 handle);
        call(sequence, fields, reply);
        json = app_parse(reply);
        value = app_member(json, 1, "value");
        uint16_t pub = app_member(json, value, "publication");
        CHECK(app_string(json, app_member(json, value, "status"), "ok"));
        if (app_string(json, app_member(json, pub, "phase"), "succeeded")) {
            uint16_t confirmed = app_member(json, pub, "confirmed");
            CHECK(confirmed && json->nodes[confirmed].kind == J_TRUE &&
                  app_string(json, app_member(json, pub, "transferState"), "succeeded"));
            release(NULL, json, sizeof(*json));
            break;
        }
        CHECK(!app_string(json, app_member(json, pub, "phase"), "failed"));
        release(NULL, json, sizeof(*json));
    }
    snprintf(
        fields, sizeof(fields), "\"op\":\"resource\",\"action\":\"forget\",\"ref\":\"%s\"", handle);
    call(sequence, fields, reply);
    CHECK(strstr(reply, "\"value\":{\"status\":\"ok\""));
}

int main(int argc, char **argv) {
    CHECK(signal(SIGTERM, dev_stop) != SIG_ERR);
    CHECK(argc == 4 && !strcmp(argv[1], "--ghosterm-startup"));
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    CHECK(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
          st.st_size <= 262144);
    char *startup = malloc((size_t)st.st_size + 1);
    CHECK(startup);
    exact(fd, startup, (size_t)st.st_size, 0);
    startup[st.st_size] = 0;
    CHECK(!close(fd));
    gt_sha256_t hash;
    char digest[65];
    gt__sha256_init(&hash);
    CHECK(gt__sha256_update(&hash, startup, (size_t)st.st_size) &&
          gt__sha256_final(&hash, digest) && !strcmp(digest, argv[3]));
    free(startup);
    uint64_t sequence = 0;
    char reply[8193];
    call(&sequence, "\"op\":\"activated\"", reply);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    for (;;) {
        call(&sequence, "\"op\":\"take\",\"methods\":[\"fixture.echo\",\"dev.exchange\"]", reply);
        app_json_t *json = app_parse(reply);
        uint16_t ticket = app_member(json, 1, "ticket"), input = app_member(json, 1, "input");
        if (!ticket) {
            release(NULL, json, sizeof(*json));
            continue;
        }
        CHECK(input && json->nodes[ticket].kind == J_STRING && json->nodes[input].kind == J_OBJECT);
        if (app_string(json, app_member(json, input, "text"), "wait")) {
            release(NULL, json, sizeof(*json));
            const char *root = getenv("TMPDIR");
            CHECK(root);
            app_mark(root, "entered", true);
            for (;;) {
                pause();
            }
        }
        char fields[4096], ticket_text[32], input_text[3073], value[4096];
        CHECK(json->nodes[ticket].size < sizeof(ticket_text) &&
              json->nodes[input].bytes < sizeof(input_text));
        snprintf(ticket_text,
                 sizeof(ticket_text),
                 "%.*s",
                 (int)json->nodes[ticket].size,
                 json->strings + json->nodes[ticket].text);
        snprintf(input_text,
                 sizeof(input_text),
                 "%.*s",
                 (int)json->nodes[input].bytes,
                 json->input + json->nodes[input].start);
        bool exchange = app_string(json, app_member(json, 1, "method"), "dev.exchange");
        bool fail = app_string(json, app_member(json, input, "text"), "throw");
        bool publish = app_string(json, app_member(json, input, "text"), "resource");
        const char *business_kind = NULL;
        const char *business_modes[] = {
            "business", "stderr", "cancel", "missing", "outside", "business-wait"};
        for (size_t i = 0; i < 6; i++) {
            if (app_string(json, app_member(json, input, "text"), business_modes[i])) {
                business_kind = i == 5 ? "wait" : business_modes[i];
            }
        }
        release(NULL, json, sizeof(*json));
        if (business_kind) {
            dev_business(&sequence, business_kind, input_text);
        }
        if (publish) {
            dev_publish(&sequence);
        }
        if (exchange && !fail) {
            fail = !dev_callback(&sequence, input_text, value);
        }
        int n = fail ? snprintf(fields,
                                sizeof(fields),
                                "\"op\":\"respond\",\"ref\":\"%.*s\",\"outcome\":{\"kind\":"
                                "\"unknown\",\"reason\":\"EXEC_FIXTURE\"}",
                                (int)strlen(ticket_text),
                                ticket_text)
                     : snprintf(fields,
                                sizeof(fields),
                                "\"op\":\"respond\",\"ref\":\"%.*s\",\"outcome\":{\"kind\":"
                                "\"success\",\"value\":%.*s}",
                                (int)strlen(ticket_text),
                                ticket_text,
                                (int)strlen(exchange ? value : input_text),
                                exchange ? value : input_text);
        CHECK(n > 0 && (size_t)n < sizeof(fields));
        call(&sequence, fields, reply);
        CHECK(strstr(reply, "\"status\":\"ok\""));
    }
}
