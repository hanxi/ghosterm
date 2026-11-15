/* Owned test Executor using the actual process protocol; no account or tools. */
#define main dev_peer_previous_main
#include "../../core/tests/process-runtime-peer.c"
#undef main

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

static void dev_publish(uint64_t *sequence) {
    char reply[8193], fields[512], handle[32];
    call(sequence,
         "\"op\":\"resource\",\"action\":\"publish\",\"root\":\"workspace\",\"path64\":"
         "\"ZmlsZS5iaW4=\",\"mime\":\"application/"
         "octet-stream\",\"limit\":\"262144\",\"timeoutMs\":1000",
         reply);
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
        release(NULL, json, sizeof(*json));
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
