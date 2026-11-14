/* Owned App -> SSH -> Exec -> native business ProcessService conformance.
 * Included by process-runtime-peer.c; no production hooks or user processes. */
static app_json_t *app_proc_value(const char *reply, uint16_t *v) {
    app_json_t *d = app_parse(reply);
    CHECK(app_string(d, app_member(d, 1, "status"), "ok"));
    *v = app_member(d, 1, "value");
    CHECK(*v && d->nodes[*v].kind == J_OBJECT);
    return d;
}

static void app_proc_command(
    uint64_t *seq, char reply[8193], const char *id, const char *action, const char *tail) {
    char fields[512];
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"process\",\"action\":\"%s\",\"ref\":\"%s\"%s",
                   action,
                   id,
                   tail) > 0);
    call(seq, fields, reply);
}

static void app_proc_ok(const char *reply) {
    uint16_t v;
    app_json_t *d = app_proc_value(reply, &v);
    CHECK(app_string(d, app_member(d, v, "status"), "ok"));
    release(NULL, d, sizeof(*d));
}

/* 0=WAIT, 1=DATA, 2=EOF. Neither error nor CLOSED is an EOF receipt. */
static int app_proc_read(uint64_t *seq,
                         char reply[8193],
                         const char *id,
                         bool stderr_pipe,
                         uint8_t bytes[3072],
                         size_t *n) {
    app_proc_command(seq,
                     reply,
                     id,
                     "read",
                     stderr_pipe ? ",\"stderr\":true,\"maxBytes\":16384"
                                 : ",\"stderr\":false,\"maxBytes\":16384");
    uint16_t v;
    app_json_t *d = app_proc_value(reply, &v);
    uint64_t count, error;
    CHECK(app_string(d, app_member(d, v, "status"), "ok") &&
          node_uint(d, app_member(d, v, "count"), &count) && count <= 3072 &&
          node_uint(d, app_member(d, v, "error"), &error) && !error);
    uint16_t state = app_member(d, v, "state"), hex = app_member(d, v, "hex");
    int result = app_string(d, state, "data") ? 1 : app_string(d, state, "eof") ? 2 : 0;
    CHECK(result || app_string(d, state, "wait"));
    CHECK((result == 1 ? count > 0 : count == 0) && hex && d->nodes[hex].kind == J_STRING &&
          d->nodes[hex].size == count * 2);
    for (size_t i = 0; i < count; i++) {
        char part[3] = {d->strings[d->nodes[hex].text + i * 2],
                        d->strings[d->nodes[hex].text + i * 2 + 1],
                        0},
             *end;
        unsigned long value = strtoul(part, &end, 16);
        CHECK(end == part + 2 && value <= 255);
        bytes[i] = (uint8_t)value;
    }
    *n = (size_t)count;
    release(NULL, d, sizeof(*d));
    return result;
}

static size_t
app_proc_collect(uint64_t *seq, char reply[8193], const char *id, bool errors, uint8_t out[8193]) {
    size_t used = 0;
    for (;;) {
        uint8_t bytes[3072];
        size_t n = 0;
        int state = app_proc_read(seq, reply, id, errors, bytes, &n);
        CHECK(n <= 8192 - used);
        memcpy(out + used, bytes, n);
        used += n;
        if (state == 2) {
            out[used] = 0;
            return used;
        }
    }
}

static int app_proc_compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void app_process(
    uint64_t *seq, char reply[8193], const char *root, const char *kind, char outcome[4096]) {
    bool root_denied = !strcmp(kind, "PROCESS_ROOT"),
         program_denied = !strcmp(kind, "PROCESS_PROGRAM"),
         errors = !strcmp(kind, "PROCESS_STDERR"), cancelled = !strcmp(kind, "PROCESS_CANCEL"),
         roundtrip = !strcmp(kind, "PROCESS");
    char fields[3072], marker[64];
    CHECK(snprintf(marker, sizeof(marker), "ghosterm-business-%ld", (long)getpid()) > 0);
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"process\",\"action\":\"spawn\",\"program\":\"%s\",\"root\":\"%s\","
                   "\"argv\":[\"%s\"%s],\"envPolicy\":\"selected-user-environment\"",
                   program_denied ? "missing" : "cat",
                   root_denied ? "outside" : "workspace",
                   marker,
                   errors ? ",\"-!\"" : "") > 0);
    call(seq, fields, reply);
    uint16_t v;
    app_json_t *d = app_proc_value(reply, &v);
    uint16_t ref = app_member(d, v, "ref");
    if (root_denied || program_denied) {
        CHECK(app_string(d, app_member(d, v, "status"), "denied") && !ref);
        release(NULL, d, sizeof(*d));
        app_text(root, "process-proof", "{\"rejected\":true}");
        strcpy(outcome, "{\"text\":\"DENIED\",\"count\":0}");
        return;
    }
    CHECK(app_string(d, app_member(d, v, "status"), "ok") &&
          app_string(d, app_member(d, v, "version"), "fixture") && ref &&
          d->nodes[ref].kind == J_STRING && d->nodes[ref].size < 32);
    char id[32];
    CHECK(
        snprintf(id, sizeof(id), "%.*s", (int)d->nodes[ref].size, d->strings + d->nodes[ref].text) >
        0);
    release(NULL, d, sizeof(*d));
    app_text(root, "business-started", "started");
    if (!strcmp(kind, "PROCESS_STOP") || !strcmp(kind, "PROCESS_LOSS")) {
        app_mark(root, "handler", false);
        for (;;) {
            pause();
        }
    }
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    size_t sent = 0, received = 0;
    if (cancelled) {
        app_proc_command(seq, reply, id, "signal", ",\"signal\":\"terminate\"");
        app_proc_ok(reply);
    } else if (roundtrip) {
        bool closed = false;
        while (received < 262144) {
            if (sent < 262144) {
                size_t count = 262144 - sent;
                if (count > 1024) {
                    count = 1024;
                }
                char hex[2049];
                for (size_t i = 0; i < count; i++) {
                    CHECK(snprintf(hex + i * 2, 3, "%02x", (unsigned)((sent + i) % 251)) == 2);
                }
                CHECK(snprintf(
                          fields,
                          sizeof(fields),
                          "\"op\":\"process\",\"action\":\"write\",\"ref\":\"%s\",\"hex\":\"%s\"",
                          id,
                          hex) > 0);
                call(seq, fields, reply);
                d = app_proc_value(reply, &v);
                uint64_t accepted;
                CHECK(app_string(d, app_member(d, v, "status"), "ok") &&
                      node_uint(d, app_member(d, v, "count"), &accepted) && accepted <= count);
                CHECK(accepted ? app_string(d, app_member(d, v, "state"), "data")
                               : app_string(d, app_member(d, v, "state"), "wait"));
                sent += (size_t)accepted;
                release(NULL, d, sizeof(*d));
            }
            if (sent == 262144 && !closed) {
                app_proc_command(seq, reply, id, "closeStdin", "");
                app_proc_ok(reply);
                closed = true;
            }
            uint8_t bytes[3072];
            size_t n;
            int state = app_proc_read(seq, reply, id, false, bytes, &n);
            CHECK(state != 2 && n <= 262144 - received);
            for (size_t i = 0; i < n; i++) {
                CHECK(bytes[i] == (uint8_t)((received + i) % 251));
            }
            CHECK(gt__sha256_update(&hash, bytes, n));
            received += n;
        }
        CHECK(sent == 262144);
    } else {
        app_proc_command(seq, reply, id, "closeStdin", "");
        app_proc_ok(reply);
    }
    uint8_t out[8193], err[8193];
    size_t out_n = 0, err_n = 0;
    if (!cancelled) {
        out_n = app_proc_collect(seq, reply, id, false, out);
        err_n = app_proc_collect(seq, reply, id, true, err);
    }
    uint64_t code, signal;
    for (;;) {
        app_proc_command(seq, reply, id, "poll", "");
        d = app_proc_value(reply, &v);
        CHECK(app_string(d, app_member(d, v, "status"), "ok"));
        uint16_t exited = app_member(d, v, "exited");
        CHECK(exited);
        if (d->nodes[exited].kind == J_TRUE) {
            CHECK(node_uint(d, app_member(d, v, "code"), &code) &&
                  node_uint(d, app_member(d, v, "signal"), &signal) &&
                  app_string(d, app_member(d, v, "reason"), "none"));
            release(NULL, d, sizeof(*d));
            break;
        }
        CHECK(d->nodes[exited].kind == J_FALSE);
        release(NULL, d, sizeof(*d));
    }
    if (cancelled) {
        CHECK(!code && signal == SIGTERM);
    } else if (errors) {
        CHECK(code && !signal && !out_n && err_n);
        received = err_n;
        CHECK(gt__sha256_update(&hash, err, err_n));
    } else {
        CHECK(!code && !signal && !err_n);
        if (roundtrip) {
            CHECK(!out_n);
        } else {
            received = out_n;
            if (!strcmp(kind, "PROCESS_ENVIRONMENT")) {
                CHECK(out_n && out[out_n - 1] == '\n' && !memchr(out, 0, out_n));
                char *lines[32], *save;
                size_t count = 0;
                for (char *s = strtok_r((char *)out, "\n", &save); s;
                     s = strtok_r(NULL, "\n", &save)) {
                    CHECK(count < 32);
                    lines[count++] = s;
                }
                qsort(lines, count, sizeof(lines[0]), app_proc_compare);
                for (size_t i = 0; i < count; i++) {
                    CHECK(gt__sha256_update(&hash, lines[i], strlen(lines[i])) &&
                          gt__sha256_update(&hash, "\n", 1));
                }
            } else {
                CHECK(gt__sha256_update(&hash, out, out_n));
            }
        }
    }
    app_proc_command(seq, reply, id, "release", "");
    app_proc_ok(reply);
    app_proc_command(seq, reply, id, "poll", "");
    d = app_proc_value(reply, &v);
    CHECK(app_string(d, app_member(d, v, "status"), "invalid"));
    release(NULL, d, sizeof(*d));
    char proof[256], digest[65];
    CHECK(gt__sha256_final(&hash, digest));
    if (cancelled) {
        CHECK(snprintf(outcome, 4096, "{\"text\":\"cancelled\",\"count\":%d}", SIGTERM) > 0);
        CHECK(snprintf(proof, sizeof(proof), "{\"signal\":%d,\"reaped\":true}", SIGTERM) > 0);
    } else {
        CHECK(snprintf(outcome, 4096, "{\"text\":\"%s\",\"count\":%zu}", digest, received) > 0);
        CHECK(
            snprintf(proof,
                     sizeof(proof),
                     "{\"bytes\":%zu,\"sha256\":\"%s\",\"code\":%llu,\"signal\":0,\"reaped\":true}",
                     received,
                     digest,
                     (unsigned long long)code) > 0);
    }
    app_text(root, "process-proof", proof);
}
