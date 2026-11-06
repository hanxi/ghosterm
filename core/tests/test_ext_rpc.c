/* Public headers/calls only; reuse the fd/admitted-SSH fixture utilities. */
#define main ext_rpc_session_fixture_main
#include "test_ext_session.c"
#undef main

typedef struct {
    ext_pair_t pair;
    gt_ext_lease_t h, e;
} rpc_pair_t;

static size_t rpc_methods(gt_ext_rpc_method_t *m) {
    memset(m, 0, 4 * sizeof(*m));
    const char *names[] = {"fixture.echo", "fixture.ask", "fixture.done", "fixture.denied"};
    for (size_t i = 0; i < 4; i++) {
        strcpy(m[i].method, names[i]);
        strcpy(m[i].capability, i == 3 ? "ui.chat" : "ui.form");
        m[i].receiver = i == 0 || i == 3 ? GT_EXT_EXECUTOR : GT_EXT_HOST;
        m[i].input_shape = m[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    return 4;
}

static gt_ext_session_t *(*rpc_adopt_hook)(gt_ext_channel_t **,
                                           gt_ext_role_t,
                                           gt_ext_session_config_t *,
                                           const gt_ext_rpc_method_t *,
                                           size_t);
static bool (*rpc_progress_hook)(gt_ext_session_t *, const gt_ext_lease_t *);

static gt_ext_session_t *rpc_adopt(gt_ext_channel_t **c,
                                   gt_ext_role_t role,
                                   gt_ext_session_config_t *config,
                                   const gt_ext_rpc_method_t *methods,
                                   size_t count) {
    if (rpc_adopt_hook) {
        return rpc_adopt_hook(c, role, config, methods, count);
    }
    gt_ext_session_t *s = NULL;
    CHECK(gt_ext_session_open_rpc(c, role, config, methods, count, &s) == GT_OK && s && !*c);
    return s;
}

static void rpc_open(rpc_pair_t *f, const char *mode) {
    memset(f, 0, sizeof(*f));
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_channel_t *h = ext_channel(fd[0]), *e = ext_channel(fd[1]);
    gt_ext_session_config_t c = ext_config();
    c.features = GT_EXT_FEATURE_REQUESTS;
    if (strstr(mode, "timeout")) {
        f->pair.clock = 1000;
        c.clock = ext_clock;
        c.clock_context = &f->pair.clock;
    }
    gt_ext_rpc_method_t m[4];
    size_t count = rpc_methods(m);
    f->pair.h = rpc_adopt(&h, GT_EXT_HOST, &c, m, count);
    if (!strcmp(mode, "remote-invalid")) {
        m[0].input_shape = GT_EXT_JSON_ARRAY;
    }
    if (!strcmp(mode, "remote-unknown")) {
        count = 0;
    }
    f->pair.e = rpc_adopt(&e, GT_EXT_EXECUTOR, &c, m, count);
    memset(m, 0, sizeof(m));
    ext_allow(&f->pair);
    CHECK(gt_ext_session_lease(f->pair.h, &f->h) && gt_ext_session_lease(f->pair.e, &f->e));
}

static void rpc_pump(rpc_pair_t *f) {
    gt_ext_session_step(f->pair.h, 0);
    gt_ext_session_step(f->pair.e, 0);
}

static void rpc_drop(rpc_pair_t *f) {
    ext_drop(&f->pair.h);
    ext_drop(&f->pair.e);
}

static gt_ext_rpc_result_t rpc_result(gt_ext_session_t *s, const gt_ext_lease_t *l, uint64_t h) {
    gt_ext_rpc_result_t r;
    CHECK(gt_ext_session_request_result(s, l, h, &r) == GT_EXT_RPC_OK);
    return r;
}

static uint64_t rpc_start(rpc_pair_t *f, bool reverse, const char *text, uint32_t timeout) {
    uint64_t handle = 0;
    CHECK(gt_ext_session_request(reverse ? f->pair.e : f->pair.h,
                                 reverse ? &f->e : &f->h,
                                 reverse ? "fixture.ask" : "fixture.echo",
                                 text,
                                 strlen(text),
                                 timeout,
                                 &handle) == GT_EXT_RPC_OK &&
          handle);
    return handle;
}

static gt_ext_rpc_request_t rpc_take(rpc_pair_t *f, bool reverse) {
    gt_ext_rpc_request_t q;
    for (unsigned i = 0; i < 2000; i++) {
        rpc_pump(f);
        gt_ext_rpc_status_t status = gt_ext_session_take_request(
            reverse ? f->pair.h : f->pair.e, reverse ? &f->h : &f->e, &q);
        CHECK(status == GT_EXT_RPC_WAIT || status == GT_EXT_RPC_OK);
        if (status == GT_EXT_RPC_OK) {
            return q;
        }
    }
    CHECK(false);
    return q;
}

static void rpc_reply(gt_ext_session_t *s, const gt_ext_lease_t *l, const gt_ext_rpc_request_t *q) {
    CHECK(gt_ext_session_check_request(s, l, q->ticket) == GT_EXT_RPC_OK);
    char out[GT_EXT_RPC_BODY_MAX + 1];
    int n = snprintf(
        out, sizeof(out), "{\"kind\":\"success\",\"value\":%.*s}", (int)q->input_size, q->input);
    CHECK(n > 0 && (size_t)n < sizeof(out));
    CHECK(gt_ext_session_respond(s, l, q->ticket, out, (size_t)n) == GT_EXT_RPC_OK);
}

static gt_ext_rpc_result_t rpc_complete(rpc_pair_t *f, bool reverse, uint64_t handle) {
    for (unsigned i = 0; i < 2000; i++) {
        rpc_pump(f);
        gt_ext_rpc_result_t r =
            rpc_result(reverse ? f->pair.e : f->pair.h, reverse ? &f->e : &f->h, handle);
        if (r.confirmed) {
            return r;
        }
    }
    CHECK(false);
    return (gt_ext_rpc_result_t){0};
}

static void rpc_shapes(void) {
    const char *values[] = {
        "{\"any\":true}", "{\"object\":17}", "[1,2]", "\"中文\"", "false", "-12.25e3", "null"};
    for (unsigned shape = 0; shape < 7; shape++) {
        rpc_pair_t f = {0};
        int fd[2];
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
        gt_ext_channel_t *h = ext_channel(fd[0]), *e = ext_channel(fd[1]);
        gt_ext_session_config_t c = ext_config();
        c.features = GT_EXT_FEATURE_REQUESTS;
        gt_ext_rpc_method_t m = {.receiver = GT_EXT_EXECUTOR,
                                 .input_shape = (gt_ext_json_shape_t)shape,
                                 .output_shape = (gt_ext_json_shape_t)shape};
        strcpy(m.method, "shape.echo");
        strcpy(m.capability, "ui.form");
        f.pair.h = rpc_adopt(&h, GT_EXT_HOST, &c, &m, 1);
        f.pair.e = rpc_adopt(&e, GT_EXT_EXECUTOR, &c, &m, 1);
        memset(&m, 0, sizeof(m));
        ext_allow(&f.pair);
        CHECK(gt_ext_session_lease(f.pair.h, &f.h) && gt_ext_session_lease(f.pair.e, &f.e));
        uint64_t handle = 0;
        if (shape) {
            const char *bad = shape == 1 ? "[]" : "{}";
            CHECK(gt_ext_session_request(
                      f.pair.h, &f.h, "shape.echo", bad, strlen(bad), 1000, &handle) ==
                      GT_EXT_RPC_INVALID &&
                  !handle);
        }
        CHECK(gt_ext_session_request(f.pair.h,
                                     &f.h,
                                     "shape.echo",
                                     values[shape],
                                     strlen(values[shape]),
                                     1000,
                                     &handle) == GT_EXT_RPC_OK &&
              handle);
        gt_ext_rpc_request_t q = rpc_take(&f, false);
        rpc_reply(f.pair.e, &f.e, &q);
        gt_ext_rpc_result_t r = rpc_complete(&f, false, handle);
        CHECK(r.state == GT_EXT_RPC_SUCCEEDED && r.confirmed && strstr(r.outcome, values[shape]));
        rpc_drop(&f);
    }
}

static void rpc_native(const char *mode) {
    if (!strcmp(mode, "shapes")) {
        rpc_shapes();
        return;
    }
    if (!strcmp(mode, "registry") || !strcmp(mode, "invalid") || !strcmp(mode, "feature")) {
        int fd[2];
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
        gt_ext_channel_t *c = ext_channel(fd[0]);
        gt_ext_session_config_t cfg = ext_config();
        gt_ext_rpc_method_t m[4];
        rpc_methods(m);
        gt_ext_session_t *s = (gt_ext_session_t *)1;
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, m, 4, &s) == GT_ERR_UNSUPPORTED &&
              !s && c);
        cfg.features = 2;
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, m, 4, &s) == GT_ERR_UNSUPPORTED &&
              !s && c);
        cfg.features = GT_EXT_FEATURE_REQUESTS;
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, NULL, 1, &s) == GT_ERR_INVALID_ARG &&
              !s && c);
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, m, 17, &s) == GT_ERR_INVALID_ARG &&
              !s && c);
        m[0].input_shape = (gt_ext_json_shape_t)7;
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, m, 4, &s) == GT_ERR_UNSUPPORTED &&
              !s && c);
        rpc_methods(m);
        m[0].receiver = (gt_ext_role_t)256;
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, m, 4, &s) == GT_ERR_INVALID_ARG &&
              !s && c);
        rpc_methods(m);
        strcpy(m[1].method, m[0].method);
        CHECK(gt_ext_session_open_rpc(&c, GT_EXT_HOST, &cfg, m, 4, &s) == GT_ERR_INVALID_ARG &&
              !s && c);
        rpc_methods(m);
        gt_ext_rpc_request_t q;
        gt_ext_rpc_result_t r;
        gt_ext_lease_t l = {0};
        uint64_t handle = 99;
        CHECK(gt_ext_session_request(NULL, &l, "fixture.echo", "{}", 2, 1000, &handle) ==
                  GT_EXT_RPC_INVALID &&
              !handle);
        memset(&q, 1, sizeof(q));
        memset(&r, 1, sizeof(r));
        CHECK(gt_ext_session_take_request(NULL, &l, &q) == GT_EXT_RPC_INVALID && !q.ticket &&
              !q.input_size && !q.input[0]);
        CHECK(gt_ext_session_request_result(NULL, &l, 1, &r) == GT_EXT_RPC_INVALID &&
              !r.confirmed && !r.handle && !r.outcome[0]);
        if (!strcmp(mode, "feature")) {
            cfg.features = 0;
            s = ext_adopt(&c, GT_EXT_HOST, &cfg);
            CHECK(gt_ext_session_request(s, &l, "fixture.echo", "{}", 2, 1000, &handle) ==
                      GT_EXT_RPC_UNSUPPORTED &&
                  !handle);
            CHECK(gt_ext_session_take_request(s, &l, &q) == GT_EXT_RPC_UNSUPPORTED);
            CHECK(gt_ext_session_request_result(s, &l, 1, &r) == GT_EXT_RPC_UNSUPPORTED);
        } else {
            s = rpc_adopt(&c, GT_EXT_HOST, &cfg, m, 4);
        }
        ext_drop(&s);
        CHECK(!close(fd[1]));
        return;
    }
    rpc_pair_t f;
    rpc_open(&f, mode);
    gt_ext_rpc_request_t q;
    uint64_t handle = 0;
    if (!strcmp(mode, "scope")) {
        gt_ext_lease_t bad = f.h;
        bad.epoch++;
        CHECK(gt_ext_session_request(f.pair.h, &bad, "fixture.echo", "{}", 2, 1000, &handle) ==
                  GT_EXT_RPC_DENIED &&
              !handle);
        CHECK(gt_ext_session_take_request(f.pair.h, &bad, &q) == GT_EXT_RPC_DENIED && !q.ticket);
        gt_ext_rpc_result_t r;
        CHECK(gt_ext_session_request_result(f.pair.h, &bad, 1, &r) == GT_EXT_RPC_DENIED &&
              !r.handle);
        CHECK(gt_ext_session_cancel_request(f.pair.h, &bad, 1) == GT_EXT_RPC_DENIED &&
              gt_ext_session_forget_request(f.pair.h, &bad, 1) == GT_EXT_RPC_DENIED &&
              gt_ext_session_check_request(f.pair.h, &bad, 1) == GT_EXT_RPC_DENIED &&
              gt_ext_session_respond(f.pair.h, &bad, 1, "{}", 2) == GT_EXT_RPC_DENIED);
    } else if (!strcmp(mode, "policy")) {
        CHECK(gt_ext_session_request(f.pair.h, &f.h, "fixture.ask", "{}", 2, 1000, &handle) ==
                  GT_EXT_RPC_UNSUPPORTED &&
              !handle);
        CHECK(gt_ext_session_request(f.pair.h, &f.h, "fixture.unknown", "{}", 2, 1000, &handle) ==
                  GT_EXT_RPC_UNSUPPORTED &&
              !handle);
        CHECK(gt_ext_session_request(f.pair.h, &f.h, "fixture.denied", "{}", 2, 1000, &handle) ==
                  GT_EXT_RPC_DENIED &&
              !handle);
    } else if (!strcmp(mode, "validation")) {
        const char *invalid[] = {
            "[]", "null", "{\"x\":1,\"x\":2}", "{} trailing", "{\"x\":\"\xc0\x80\"}"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
            CHECK(gt_ext_session_request(f.pair.h,
                                         &f.h,
                                         "fixture.echo",
                                         invalid[i],
                                         strlen(invalid[i]),
                                         1000,
                                         &handle) == GT_EXT_RPC_INVALID &&
                  !handle);
        }
        CHECK(gt_ext_session_request(f.pair.h, &f.h, "fixture.echo", "{}\0x", 4, 1000, &handle) ==
                  GT_EXT_RPC_INVALID &&
              !handle);
        char large[GT_EXT_RPC_BODY_MAX + 1];
        memset(large, ' ', sizeof(large));
        CHECK(gt_ext_session_request(
                  f.pair.h, &f.h, "fixture.echo", large, sizeof(large), 1000, &handle) ==
                  GT_EXT_RPC_INVALID &&
              !handle);
        handle = rpc_start(&f, false, "{}", 1000);
        q = rpc_take(&f, false);
        const char *bad = "{\"kind\":\"success\",\"value\":[]}";
        CHECK(gt_ext_session_respond(f.pair.e, &f.e, q.ticket, bad, strlen(bad)) ==
              GT_EXT_RPC_INVALID);
        rpc_reply(f.pair.e, &f.e, &q);
        CHECK(rpc_complete(&f, false, handle).state == GT_EXT_RPC_SUCCEEDED);
    } else if (!strcmp(mode, "full")) {
        uint64_t handles[64];
        for (unsigned i = 0; i < 64; i++) {
            handles[i] = rpc_start(&f, false, "{}", 60000);
        }
        CHECK(gt_ext_session_request(f.pair.h, &f.h, "fixture.echo", "{}", 2, 1000, &handle) ==
                  GT_EXT_RPC_WAIT &&
              !handle);
        for (unsigned i = 0; i < 64; i++) {
            CHECK(gt_ext_session_cancel_request(f.pair.h, &f.h, handles[i]) == GT_EXT_RPC_OK &&
                  gt_ext_session_forget_request(f.pair.h, &f.h, handles[i]) == GT_EXT_RPC_OK);
        }
    } else if (!strcmp(mode, "queued-cancel") || !strcmp(mode, "queued-timeout")) {
        handle = rpc_start(&f, false, "{}", 20);
        bool cancel = !strcmp(mode, "queued-cancel");
        if (cancel) {
            CHECK(gt_ext_session_cancel_request(f.pair.h, &f.h, handle) == GT_EXT_RPC_OK);
        } else {
            f.pair.clock += 20;
        }
        gt_ext_rpc_result_t r = rpc_result(f.pair.h, &f.h, handle);
        CHECK(r.state == (cancel ? GT_EXT_RPC_CANCELLED : GT_EXT_RPC_FAILED) && !r.confirmed &&
              r.cancel_requested == cancel && r.deadline_elapsed == !cancel && !r.request_id[0] &&
              strstr(r.outcome, "none"));
        for (unsigned i = 0; i < 40; i++) {
            rpc_pump(&f);
        }
        CHECK(gt_ext_session_take_request(f.pair.e, &f.e, &q) == GT_EXT_RPC_WAIT);
    } else if (!strcmp(mode, "remote-invalid") || !strcmp(mode, "remote-unknown")) {
        handle = rpc_start(&f, false, "{}", 1000);
        gt_ext_rpc_result_t r = rpc_complete(&f, false, handle);
        CHECK(r.confirmed && r.state == GT_EXT_RPC_FAILED &&
              strstr(r.outcome, !strcmp(mode, "remote-invalid") ? "INVALID_INPUT" : "UNSUPPORTED"));
        CHECK(gt_ext_session_take_request(f.pair.e, &f.e, &q) == GT_EXT_RPC_WAIT);
    } else {
        bool reverse = !strcmp(mode, "reverse");
        handle = rpc_start(&f, reverse, "{\"text\":\"中文\",\"number\":17}", 1000);
        q = rpc_take(&f, reverse);
        gt_ext_session_t *sender = reverse ? f.pair.e : f.pair.h,
                         *receiver = reverse ? f.pair.h : f.pair.e;
        gt_ext_lease_t *sl = reverse ? &f.e : &f.h, *rl = reverse ? &f.h : &f.e;
        CHECK(q.input_size == strlen(q.input) && strstr(q.input, "中文"));
        gt_ext_rpc_request_t saved = q;
        gt_ext_rpc_result_t r;
        CHECK(gt_ext_session_check_request(receiver, rl, q.ticket + 1) == GT_EXT_RPC_INVALID);
        if (!strcmp(mode, "lost") || !strcmp(mode, "sent-timeout")) {
            if (!strcmp(mode, "sent-timeout")) {
                f.pair.clock += 1000;
            } else {
                gt_ext_session_stop(sender);
            }
            r = rpc_result(sender, sl, handle);
            CHECK(r.state == GT_EXT_RPC_UNKNOWN && !r.confirmed && r.request_id[0] &&
                  strstr(r.outcome, "unknown"));
            if (!strcmp(mode, "sent-timeout")) {
                CHECK(r.deadline_elapsed && r.cancel_requested);
            }
        } else if (!strcmp(mode, "sent-cancel")) {
            CHECK(gt_ext_session_cancel_request(sender, sl, handle) == GT_EXT_RPC_OK);
            for (unsigned i = 0; i < 40; i++) {
                rpc_pump(&f);
            }
            CHECK(gt_ext_session_check_request(receiver, rl, q.ticket) == GT_EXT_RPC_CLOSED);
            const char *unknown = "{\"kind\":\"unknown\",\"reason\":\"effect not established\"}";
            CHECK(gt_ext_session_respond(receiver, rl, q.ticket, unknown, strlen(unknown)) ==
                  GT_EXT_RPC_OK);
            r = rpc_complete(&f, reverse, handle);
            CHECK(r.state == GT_EXT_RPC_UNKNOWN && r.confirmed && r.cancel_requested);
        } else {
            rpc_reply(receiver, rl, &q);
            r = rpc_complete(&f, reverse, handle);
            CHECK(r.state == GT_EXT_RPC_SUCCEEDED && r.confirmed && strstr(r.outcome, "中文"));
            CHECK(gt_ext_session_respond(receiver, rl, q.ticket, r.outcome, r.outcome_size) ==
                  GT_EXT_RPC_INVALID);
            if (!strcmp(mode, "confirmed-stop")) {
                gt_ext_session_stop(sender);
                gt_ext_rpc_result_t after = rpc_result(sender, sl, handle);
                CHECK(after.state == GT_EXT_RPC_SUCCEEDED && after.confirmed &&
                      !strcmp(after.outcome, r.outcome));
            }
        }
        gt_ext_rpc_result_t snapshot = r;
        CHECK(gt_ext_session_forget_request(sender, sl, handle) == GT_EXT_RPC_OK);
        CHECK(gt_ext_session_request_result(sender, sl, handle, &r) == GT_EXT_RPC_INVALID &&
              !r.handle && !r.outcome_size && !r.outcome[0]);
        CHECK(!memcmp(&q, &saved, sizeof(q)) && snapshot.outcome_size &&
              snapshot.outcome[snapshot.outcome_size] == 0);
        if (!strcmp(mode, "copy")) {
            rpc_drop(&f);
            CHECK(strstr(snapshot.outcome, "中文") && strstr(saved.input, "中文"));
            return;
        }
    }
    rpc_drop(&f);
}

static void rpc_executor(const char *proof_root) {
    CHECK(!isatty(0) && !isatty(1));
    gt_ext_channel_t *c = NULL;
    CHECK(gt_ext_channel_open_fd(0, 1, &c) == GT_OK);
    gt_ext_session_config_t cfg = ext_config();
    cfg.features = GT_EXT_FEATURE_REQUESTS;
    gt_ext_rpc_method_t m[4];
    rpc_methods(m);
    gt_ext_session_t *s = rpc_adopt(&c, GT_EXT_EXECUTOR, &cfg, m, 4);
    gt_ext_lease_t l = {0};
    unsigned completed = 0, handled = 0;
    uint64_t pending = 0, end = ext_now() + 25000;
    bool done = false;
    while (ext_info(s).phase < GT_EXT_SESSION_ENDED) {
        gt_ext_session_step(s, 5);
        CHECK(ext_now() < end);
        if (ext_info(s).phase != GT_EXT_SESSION_ACTIVE) {
            continue;
        }
        CHECK(gt_ext_session_lease(s, &l));
        bool extra_ready = !rpc_progress_hook || rpc_progress_hook(s, &l);
        if (!pending && (completed < 80 || (handled == 80 && extra_ready))) {
            const char *method = completed < 80 ? "fixture.ask" : "fixture.done";
            CHECK(gt_ext_session_request(
                      s, &l, method, "{\"text\":\"bridge\"}", 17, 10000, &pending) ==
                      GT_EXT_RPC_OK &&
                  pending);
        }
        gt_ext_rpc_request_t q;
        gt_ext_rpc_status_t status = gt_ext_session_take_request(s, &l, &q);
        CHECK(status == GT_EXT_RPC_WAIT || status == GT_EXT_RPC_OK);
        if (status == GT_EXT_RPC_OK) {
            CHECK(!strcmp(q.method, "fixture.echo"));
            rpc_reply(s, &l, &q);
            handled++;
        }
        if (pending) {
            gt_ext_rpc_result_t r = rpc_result(s, &l, pending);
            if (r.confirmed) {
                CHECK(r.state == GT_EXT_RPC_SUCCEEDED);
                CHECK(gt_ext_session_forget_request(s, &l, pending) == GT_EXT_RPC_OK);
                pending = 0;
                if (completed == 80) {
                    done = true;
                    break;
                }
                completed++;
            }
        }
    }
    if (ext_info(s).end == GT_EXT_END_DENIED) {
        CHECK(!completed && !handled);
        ext_drop(&s);
        return;
    }
    CHECK(done && completed == 80 && handled == 80);
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/rpc-proof-XXXXXX", proof_root) > 0);
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char *proof = "{\"completed\":80,\"handled\":80,\"done\":true}\n";
    CHECK(write(fd, proof, strlen(proof)) == (ssize_t)strlen(proof) && !close(fd));
    ext_drop(&s);
}
#ifdef GT_HAVE_LIBSSH2
static void rpc_network(int argc, char **argv) {
    unsigned count = (unsigned)atoi(argv[2]);
    CHECK(count >= 2 && count <= 3 && argc == (int)(6 + count));
    gt_ext_test_port = (uint16_t)atoi(argv[6]);
    uint8_t prefix[4];
    CHECK(read(0, prefix, 4) == 4);
    size_t size = ((size_t)prefix[0] << 24) | ((size_t)prefix[1] << 16) | ((size_t)prefix[2] << 8) |
                  prefix[3];
    CHECK(size && size < 8192);
    char *key = calloc(1, size + 1);
    CHECK(key);
    for (size_t at = 0; at < size;) {
        ssize_t n = read(0, key + at, size - at);
        CHECK(n > 0);
        at += (size_t)n;
    }
    gt_ssh_t *owned[GT_EXT_MAX_HOPS] = {0};
    gt_ext_ssh_hop_t hops[GT_EXT_MAX_HOPS] = {0};
    for (unsigned i = 0; i < count; i++) {
        char host[64];
        CHECK(snprintf(host, sizeof(host), "route-hop-%u.invalid", i) > 0);
        gt_ssh_opts_t o = {.host = host,
                           .port = (uint16_t)atoi(argv[6 + i]),
                           .username = argv[3],
                           .known_hosts_path = argv[4]};
        owned[i] = gt_ssh_new(&o);
        CHECK(owned[i] && gt_ssh_auth_private_key(owned[i], key, NULL) == GT_OK);
        hops[i].authentication = (gt_ssh_auth_options_t){
            .family = i ? 0 : 4, .address = {127, 0, 0, 1}, .timeout_ms = 5000};
        if (i) {
            hops[i].via_host = "127.0.0.1";
        }
    }
    memset(key, 0, size);
    free(key);
    gt_ext_channel_t *c = NULL;
    CHECK(gt_ext_channel_open_ssh(owned, hops, count, argv[5], NULL, &c) == GT_OK);
    uint64_t end = ext_now() + 25000;
    while (gt_ext_channel_step(c, 5) != GT_EXT_READY) {
        gt_ext_channel_info_t i;
        CHECK(gt_ext_channel_info(c, &i) && i.phase == GT_EXT_CONNECTING && ext_now() < end);
    }
    gt_ext_session_config_t cfg = ext_config();
    cfg.features = GT_EXT_FEATURE_REQUESTS;
    cfg.peer.kind = GT_EXT_PEER_SSH;
    gt_ext_rpc_method_t m[4];
    rpc_methods(m);
    gt_ext_session_t *s = rpc_adopt(&c, GT_EXT_HOST, &cfg, m, 4);
    gt_ext_lease_t l = {0};
    unsigned completed = 0, handled = 0;
    uint64_t pending = 0;
    gt_ext_rpc_request_t done_request = {0};
    bool decided = false, done = false, answered = false, deny = !strcmp(argv[1], "ssh-deny");
    while (ext_info(s).phase < GT_EXT_SESSION_ENDED) {
        gt_ext_session_step(s, 5);
        CHECK(ext_now() < end);
        gt_ext_session_info_t i = ext_info(s);
        if (i.phase == GT_EXT_SESSION_CONSENT) {
            CHECK(!decided && gt_ext_session_decide(s,
                                                    i.consent_token,
                                                    deny ? GT_EXT_DECISION_DENY
                                                         : GT_EXT_DECISION_ALLOW) == GT_OK);
            decided = true;
        }
        if (i.phase != GT_EXT_SESSION_ACTIVE) {
            continue;
        }
        CHECK(gt_ext_session_lease(s, &l));
        if (rpc_progress_hook) {
            (void)rpc_progress_hook(s, &l);
        }
        if (completed < 80 && !pending) {
            CHECK(gt_ext_session_request(
                      s, &l, "fixture.echo", "{\"text\":\"bridge\"}", 17, 10000, &pending) ==
                      GT_EXT_RPC_OK &&
                  pending);
        }
        gt_ext_rpc_request_t q;
        gt_ext_rpc_status_t status = gt_ext_session_take_request(s, &l, &q);
        CHECK(status == GT_EXT_RPC_WAIT || status == GT_EXT_RPC_OK);
        if (status == GT_EXT_RPC_OK) {
            if (!strcmp(q.method, "fixture.done")) {
                CHECK(!done);
                done = true;
                done_request = q;
            } else {
                CHECK(!strcmp(q.method, "fixture.ask"));
                handled++;
                rpc_reply(s, &l, &q);
            }
        }
        if (pending) {
            gt_ext_rpc_result_t r = rpc_result(s, &l, pending);
            if (r.confirmed) {
                CHECK(r.state == GT_EXT_RPC_SUCCEEDED && strstr(r.outcome, "bridge"));
                CHECK(gt_ext_session_forget_request(s, &l, pending) == GT_EXT_RPC_OK);
                pending = 0;
                completed++;
            }
        }
        if (done && !answered && completed == 80 && handled == 80 &&
            (!rpc_progress_hook || rpc_progress_hook(s, &l))) {
            rpc_reply(s, &l, &done_request);
            answered = true;
        }
    }
    CHECK(decided && gt_ext_test_dials == 1);
    if (deny) {
        CHECK(!completed && !handled && ext_info(s).end == GT_EXT_END_DENIED);
    } else {
        CHECK(completed == 80 && handled == 80 && done && answered &&
              ext_info(s).end == GT_EXT_END_LOST);
    }
    ext_drop(&s);
    printf("PASS ext_rpc_%s_%u\n", argv[1], count);
}
#endif
#ifndef EXT_RPC_MAIN
#define EXT_RPC_MAIN main
#endif
int EXT_RPC_MAIN(int argc, char **argv) {
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "--executor")) {
        CHECK(argc == 3);
        rpc_executor(argv[2]);
        return 0;
    }
#ifdef GT_HAVE_LIBSSH2
    if (argc > 2) {
        rpc_network(argc, argv);
        return 0;
    }
#endif
    size_t before = ext_fds();
    rpc_native(argv[1]);
    CHECK(ext_fds() == before);
    printf("PASS ext_rpc_%s\n", argv[1]);
    return 0;
}
