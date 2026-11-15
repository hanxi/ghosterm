/* Owned test Executor using the actual process protocol; no account or tools. */
#define main dev_peer_previous_main
#include "../../core/tests/process-runtime-peer.c"
#undef main

static void dev_stop(int signal_number) {
    (void)signal_number;
    _exit(0);
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
        call(&sequence, "\"op\":\"take\",\"methods\":[\"fixture.echo\"]", reply);
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
        char fields[4096];
        bool fail = app_string(json, app_member(json, input, "text"), "throw");
        int n = fail ? snprintf(fields,
                                sizeof(fields),
                                "\"op\":\"respond\",\"ref\":\"%.*s\",\"outcome\":{\"kind\":"
                                "\"unknown\",\"reason\":\"EXEC_FIXTURE\"}",
                                (int)json->nodes[ticket].size,
                                json->strings + json->nodes[ticket].text)
                     : snprintf(fields,
                                sizeof(fields),
                                "\"op\":\"respond\",\"ref\":\"%.*s\",\"outcome\":{\"kind\":"
                                "\"success\",\"value\":%.*s}",
                                (int)json->nodes[ticket].size,
                                json->strings + json->nodes[ticket].text,
                                (int)json->nodes[input].bytes,
                                json->input + json->nodes[input].start);
        CHECK(n > 0 && (size_t)n < sizeof(fields));
        release(NULL, json, sizeof(*json));
        call(&sequence, fields, reply);
        CHECK(strstr(reply, "\"status\":\"ok\""));
    }
}
