#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint64_t ext_now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool ext_clock(void *ctx, uint64_t *out) {
    *out = *(uint64_t *)ctx;
    return true;
}

static gt_ext_session_config_t ext_config(void) {
    gt_ext_session_config_t c = {.required_count = 1,
                                 .available_count = 2,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 60000,
                                 .peer = {GT_EXT_PEER_LOCAL, "fixture-local-owner"}};
    strcpy(c.plugin_id, "org.ghosterm.preview");
    strcpy(c.plugin_version, "0.1.0");
    memset(c.client_digest, 'd', 64);
    memset(c.executor_digest, 'e', 64);
    strcpy(c.host_version, "0.1.0");
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.required[0], "ui.form");
    strcpy(c.available[0], "ui.form");
    strcpy(c.available[1], "ui.chat");
    return c;
}

static gt_ext_channel_t *ext_channel(int fd) {
    gt_ext_channel_t *c = NULL;
    CHECK(gt_ext_channel_open_fd(fd, fd, &c) == GT_OK && c);
    return c;
}

static gt_ext_session_t *
ext_adopt(gt_ext_channel_t **c, gt_ext_role_t r, gt_ext_session_config_t *config) {
    gt_ext_session_t *s = NULL;
    CHECK(gt_ext_session_open(c, r, config, &s) == GT_OK && s && !*c);
    return s;
}

static gt_ext_session_info_t ext_info(gt_ext_session_t *s) {
    gt_ext_session_info_t i;
    CHECK(gt_ext_session_info(s, &i));
    return i;
}

static void ext_drop_channel(gt_ext_channel_t **c) {
    for (unsigned i = 0; i < 100; i++) {
        if (gt_ext_channel_close(*c, 0) == GT_EXT_RELEASED) {
            CHECK(gt_ext_channel_destroy(c) == GT_OK && !*c);
            return;
        }
    }
    CHECK(false);
}

static void ext_drop(gt_ext_session_t **s) {
    for (unsigned i = 0; i < 100; i++) {
        if (gt_ext_session_close(*s, 0) == GT_EXT_RELEASED) {
            CHECK(gt_ext_session_close(*s, 0) == GT_EXT_RELEASED &&
                  ext_info(*s).phase == GT_EXT_SESSION_CLOSED);
            CHECK(gt_ext_session_destroy(s) == GT_OK && !*s);
            return;
        }
    }
    CHECK(false);
}

typedef struct {
    gt_ext_session_t *h, *e;
    uint64_t clock;
} ext_pair_t;

static void ext_pair(ext_pair_t *p, bool timed) {
    memset(p, 0, sizeof(*p));
    int f[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
    gt_ext_channel_t *h = ext_channel(f[0]), *e = ext_channel(f[1]);
    gt_ext_session_config_t c = ext_config();
    if (timed) {
        p->clock = 1000;
        c.clock = ext_clock;
        c.clock_context = &p->clock;
    }
    p->h = ext_adopt(&h, GT_EXT_HOST, &c);
    p->e = ext_adopt(&e, GT_EXT_EXECUTOR, &c);
    memset(&c, 0, sizeof(c));
}

static void ext_wait(ext_pair_t *p, gt_ext_session_phase_t phase) {
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(p->h, 0);
        gt_ext_session_step(p->e, 0);
        if (ext_info(p->h).phase == phase) {
            return;
        }
    }
    CHECK(false);
}

static gt_ext_lease_t ext_allow(ext_pair_t *p) {
    ext_wait(p, GT_EXT_SESSION_CONSENT);
    gt_ext_session_info_t i = ext_info(p->h);
    CHECK(gt_ext_session_decide(p->h, i.consent_token, GT_EXT_DECISION_ALLOW) == GT_OK);
    ext_wait(p, GT_EXT_SESSION_ACTIVE);
    gt_ext_lease_t l;
    CHECK(gt_ext_session_lease(p->h, &l));
    return l;
}

static void *ext_stop_later(void *ctx) {
    struct timespec t = {0, 20000000};
    while (nanosleep(&t, &t)) {
        CHECK(errno == EINTR);
    }
    gt_ext_session_stop(ctx);
    return NULL;
}

static size_t ext_fds(void) {
    DIR *d = opendir("/dev/fd");
    CHECK(d);
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            n++;
        }
    }
    CHECK(!closedir(d));
    return n;
}

static void ext_native(const char *mode) {
    if (!strcmp(mode, "capacity")) {
        gt_ext_session_t *all[GT_EXT_CHANNEL_LIMIT] = {0};
        int peers[GT_EXT_CHANNEL_LIMIT];
        gt_ext_session_config_t c = ext_config();
        for (unsigned i = 0; i < GT_EXT_CHANNEL_LIMIT; i++) {
            int f[2];
            CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
            gt_ext_channel_t *channel = ext_channel(f[0]);
            peers[i] = f[1];
            all[i] = ext_adopt(&channel, GT_EXT_HOST, &c);
        }
        int f[2];
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
        gt_ext_channel_t *extra = NULL;
        CHECK(gt_ext_channel_open_fd(f[0], f[0], &extra) == GT_ERR_IO && !extra);
        CHECK(gt_ext_session_close(all[0], 0) == GT_EXT_RELEASED &&
              gt_ext_channel_open_fd(f[0], f[0], &extra) == GT_ERR_IO && !extra);
        CHECK(gt_ext_session_destroy(&all[0]) == GT_OK && !all[0]);
        extra = ext_channel(f[0]);
        ext_drop_channel(&extra);
        CHECK(!close(f[1]));
        for (unsigned i = 0; i < GT_EXT_CHANNEL_LIMIT; i++) {
            if (all[i]) {
                ext_drop(&all[i]);
            }
            CHECK(!close(peers[i]));
        }
        return;
    }
    if (!strcmp(mode, "invalid") || !strcmp(mode, "config") || !strcmp(mode, "dirty-write") ||
        !strcmp(mode, "dirty-read") || !strcmp(mode, "stopped") || !strcmp(mode, "closed")) {
        int f[2];
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
        gt_ext_channel_t *channel = ext_channel(f[0]);
        gt_ext_session_config_t c = ext_config();
        gt_ext_session_t *s = (gt_ext_session_t *)1;
        if (!strcmp(mode, "dirty-write")) {
            CHECK(gt_ext_channel_write(channel, "x", 1).count == 1 &&
                  gt_ext_channel_step(channel, 0) == GT_EXT_READY);
        } else if (!strcmp(mode, "dirty-read")) {
            CHECK(write(f[1], "x", 1) == 1);
            char b;
            CHECK(gt_ext_channel_read(channel, &b, 1, 10).count == 1);
        } else if (!strcmp(mode, "stopped")) {
            gt_ext_channel_stop(channel);
        } else if (!strcmp(mode, "closed")) {
            CHECK(gt_ext_channel_close(channel, 0) == GT_EXT_RELEASED);
        } else if (!strcmp(mode, "config")) {
            gt_ext_session_config_t bad = c;
            bad.features = 1;
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &bad, &s) == GT_ERR_UNSUPPORTED &&
                  !s && channel);
            bad = c;
            bad.peer.kind = GT_EXT_PEER_SSH;
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &bad, &s) == GT_ERR_INVALID_ARG &&
                  !s && channel);
            bad = c;
            bad.required_count = 17;
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &bad, &s) == GT_ERR_INVALID_ARG &&
                  !s && channel);
            bad = c;
            memset(bad.plugin_id, 'x', sizeof(bad.plugin_id));
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &bad, &s) == GT_ERR_INVALID_ARG &&
                  !s && channel);
            bad = c;
            strcpy(bad.sdk_version, "2.0.0");
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &bad, &s) == GT_ERR_UNSUPPORTED &&
                  !s && channel);
            bad = c;
            strcpy(bad.required[0], "process.spawn");
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &bad, &s) == GT_ERR_UNSUPPORTED &&
                  !s && channel);
        } else {
            CHECK(gt_ext_session_open(NULL, GT_EXT_HOST, &c, &s) == GT_ERR_INVALID_ARG && !s);
            CHECK(gt_ext_session_open(&channel, (gt_ext_role_t)2, &c, &s) == GT_ERR_INVALID_ARG &&
                  !s && channel);
            gt_ext_session_info_t i;
            memset(&i, 1, sizeof(i));
            CHECK(!gt_ext_session_info(NULL, &i) && !i.epoch && !i.consent_token[0]);
            gt_ext_lease_t l;
            memset(&l, 1, sizeof(l));
            CHECK(!gt_ext_session_lease(NULL, &l) && !l.epoch && !l.token[0]);
            CHECK(gt_ext_session_step(NULL, 0) == GT_EXT_SESSION_ENDED &&
                  gt_ext_session_decide(NULL, "x", GT_EXT_DECISION_ALLOW) == GT_ERR_INVALID_ARG &&
                  gt_ext_session_access(NULL, &l, "ui.form") == GT_EXT_ACCESS_DENIED &&
                  gt_ext_session_close(NULL, 0) == GT_EXT_CLOSE_ERROR &&
                  gt_ext_session_destroy(NULL) == GT_ERR_INVALID_ARG);
            gt_ext_session_stop(NULL);
            gt_ext_session_revoke(NULL);
        }
        if (!strcmp(mode, "invalid") || !strcmp(mode, "config")) {
            gt_ext_channel_info_t i;
            CHECK(gt_ext_channel_info(channel, &i) && !i.accepted && !i.received);
            char b;
            CHECK(recv(f[1], &b, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN);
            s = ext_adopt(&channel, GT_EXT_HOST, &c);
            CHECK(gt_ext_session_destroy(&s) == GT_ERR_INVALID_ARG && s);
            CHECK(gt_ext_session_close(s, -1) == GT_EXT_CLOSE_ERROR);
            ext_drop(&s);
        } else {
            CHECK(gt_ext_session_open(&channel, GT_EXT_HOST, &c, &s) == GT_ERR_INVALID_ARG && !s &&
                  channel);
            ext_drop_channel(&channel);
        }
        CHECK(!close(f[1]));
        return;
    }
    ext_pair_t p;
    ext_pair(&p, true);
    ext_wait(&p, GT_EXT_SESSION_CONSENT);
    gt_ext_session_info_t i = ext_info(p.h);
    gt_ext_lease_t l = {0};
    CHECK(i.decision == GT_EXT_DECISION_UNKNOWN && i.consent_token[0] &&
          !gt_ext_session_lease(p.h, &l) &&
          gt_ext_session_access(p.h, &l, "ui.form") == GT_EXT_ACCESS_UNKNOWN);
    CHECK(!strcmp(i.peer.identity, "fixture-local-owner") &&
          !strcmp(i.peer_host_version, "0.1.0") && i.budgets.resource_bytes == 262144);
    CHECK(gt_ext_session_decide(p.h, i.consent_token, GT_EXT_DECISION_UNKNOWN) == GT_OK &&
          ext_info(p.h).phase == GT_EXT_SESSION_CONSENT);
    if (!strcmp(mode, "deny")) {
        CHECK(gt_ext_session_decide(p.h, i.consent_token, GT_EXT_DECISION_DENY) == GT_OK);
        CHECK(gt_ext_session_access(p.h, &l, "ui.form") == GT_EXT_ACCESS_DENIED);
        ext_wait(&p, GT_EXT_SESSION_ENDED);
        CHECK(ext_info(p.e).end == GT_EXT_END_DENIED);
    } else if (!strcmp(mode, "expired")) {
        p.clock += 60000;
        CHECK(gt_ext_session_decide(p.h, i.consent_token, GT_EXT_DECISION_ALLOW) == GT_ERR_IO &&
              !gt_ext_session_lease(p.h, &l) && ext_info(p.h).end == GT_EXT_END_EXPIRED &&
              ext_info(p.h).decision == GT_EXT_DECISION_UNKNOWN);
    } else {
        if (!strcmp(mode, "ticket")) {
            char wrong[33];
            memcpy(wrong, i.consent_token, 33);
            wrong[0] = wrong[0] == '0' ? '1' : '0';
            CHECK(gt_ext_session_decide(p.h, wrong, GT_EXT_DECISION_ALLOW) == GT_ERR_INVALID_ARG &&
                  ext_info(p.h).decision == GT_EXT_DECISION_UNKNOWN);
            CHECK(gt_ext_session_decide(p.e, i.consent_token, GT_EXT_DECISION_ALLOW) ==
                  GT_ERR_INVALID_ARG);
            ext_pair_t other;
            ext_pair(&other, true);
            ext_wait(&other, GT_EXT_SESSION_CONSENT);
            CHECK(gt_ext_session_decide(other.h, i.consent_token, GT_EXT_DECISION_ALLOW) ==
                  GT_ERR_INVALID_ARG);
            ext_drop(&other.h);
            ext_drop(&other.e);
        }
        l = ext_allow(&p);
        gt_ext_lease_t e;
        CHECK(gt_ext_session_lease(p.e, &e) && strcmp(e.token, l.token));
        CHECK(gt_ext_session_access(p.h, &l, "ui.form") == GT_EXT_ACCESS_ALLOWED &&
              gt_ext_session_access(p.h, &l, "ui.chat") == GT_EXT_ACCESS_UNSUPPORTED &&
              gt_ext_session_access(p.h, &e, "ui.form") == GT_EXT_ACCESS_DENIED);
        gt_ext_lease_t wrong = l;
        wrong.epoch++;
        CHECK(gt_ext_session_access(p.h, &wrong, "ui.form") == GT_EXT_ACCESS_DENIED);
        CHECK(gt_ext_session_decide(p.h, i.consent_token, GT_EXT_DECISION_ALLOW) ==
              GT_ERR_INVALID_ARG);
        if (!strcmp(mode, "stop")) {
            pthread_t t;
            CHECK(!pthread_create(&t, NULL, ext_stop_later, p.h));
            CHECK(gt_ext_session_step(p.h, 5000) == GT_EXT_SESSION_ENDED && !pthread_join(t, NULL));
            CHECK(!gt_ext_session_lease(p.h, &l) &&
                  gt_ext_session_access(p.h, &e, "ui.form") == GT_EXT_ACCESS_REVOKED);
        } else if (!strcmp(mode, "lost")) {
            ext_drop(&p.e);
            for (unsigned n = 0; n < 100; n++) {
                if (gt_ext_session_step(p.h, 0) == GT_EXT_SESSION_ENDED) {
                    break;
                }
            }
            CHECK(ext_info(p.h).end == GT_EXT_END_LOST);
            gt_ext_session_stop(p.h);
            CHECK(ext_info(p.h).end == GT_EXT_END_LOST && ext_info(p.h).error == GT_ERR_IO);
        } else {
            CHECK(!strcmp(mode, "allow") || !strcmp(mode, "ticket") || !strcmp(mode, "revoke"));
            gt_ext_session_revoke(p.h);
            CHECK(gt_ext_session_access(p.h, &l, "ui.form") == GT_EXT_ACCESS_REVOKED &&
                  !gt_ext_session_lease(p.h, &l));
            ext_wait(&p, GT_EXT_SESSION_ENDED);
            CHECK(ext_info(p.e).end == GT_EXT_END_PEER);
        }
    }
    ext_drop(&p.h);
    if (p.e) {
        ext_drop(&p.e);
    }
}

static void ext_executor(void) {
    CHECK(!isatty(0) && !isatty(1));
    gt_ext_channel_t *c = NULL;
    CHECK(gt_ext_channel_open_fd(0, 1, &c) == GT_OK);
    gt_ext_session_config_t config = ext_config();
    gt_ext_session_t *s = ext_adopt(&c, GT_EXT_EXECUTOR, &config);
    uint64_t end = ext_now() + 15000;
    bool active = false;
    while (ext_info(s).phase < GT_EXT_SESSION_ENDED) {
        gt_ext_session_step(s, 20);
        if (ext_info(s).phase == GT_EXT_SESSION_ACTIVE) {
            active = true;
        }
        CHECK(ext_now() < end);
    }
    gt_ext_session_info_t i = ext_info(s);
    CHECK((active && i.end == GT_EXT_END_PEER) || (!active && i.end == GT_EXT_END_DENIED));
    ext_drop(&s);
}
#ifdef GT_HAVE_LIBSSH2
extern uint16_t gt_ext_test_port;
extern unsigned gt_ext_test_dials;

static void ext_network(int argc, char **argv) {
    unsigned count = (unsigned)atoi(argv[2]);
    CHECK(count >= 2 && count <= 3 && argc == (int)(6 + count));
    gt_ext_test_port = (uint16_t)atoi(argv[6]);
    uint8_t prefix[4];
    CHECK(read(0, prefix, 4) == 4);
    size_t size = ((size_t)prefix[0] << 24) | ((size_t)prefix[1] << 16) | ((size_t)prefix[2] << 8) |
                  prefix[3];
    CHECK(size > 0 && size < 8192);
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
    gt_ext_session_config_t config = ext_config();
    config.peer.kind = GT_EXT_PEER_SSH;
    strcpy(config.peer.identity, "fixture-verified-ssh-route");
    gt_ext_session_t *s = NULL;
    CHECK(gt_ext_session_open(&c, GT_EXT_HOST, &config, &s) == GT_ERR_INVALID_ARG && c && !s);
    uint64_t end = ext_now() + 15000;
    while (gt_ext_channel_step(c, 5) != GT_EXT_READY) {
        gt_ext_channel_info_t i;
        CHECK(gt_ext_channel_info(c, &i) && i.phase == GT_EXT_CONNECTING && ext_now() < end);
    }
    s = ext_adopt(&c, GT_EXT_HOST, &config);
    bool decided = false, active = false;
    gt_ext_lease_t lease = {0};
    while (ext_info(s).phase < GT_EXT_SESSION_ENDED) {
        gt_ext_session_step(s, 5);
        gt_ext_session_info_t i = ext_info(s);
        CHECK(ext_now() < end);
        if (i.phase == GT_EXT_SESSION_CONSENT) {
            CHECK(!decided && i.decision == GT_EXT_DECISION_UNKNOWN &&
                  i.peer.kind == GT_EXT_PEER_SSH &&
                  !strcmp(i.peer.identity, "fixture-verified-ssh-route"));
            CHECK(gt_ext_session_decide(s,
                                        i.consent_token,
                                        !strcmp(argv[1], "ssh-deny")
                                            ? GT_EXT_DECISION_DENY
                                            : GT_EXT_DECISION_ALLOW) == GT_OK);
            decided = true;
        }
        if (i.phase == GT_EXT_SESSION_ACTIVE) {
            CHECK(gt_ext_session_lease(s, &lease) &&
                  gt_ext_session_access(s, &lease, "ui.form") == GT_EXT_ACCESS_ALLOWED);
            active = true;
            gt_ext_session_revoke(s);
        }
    }
    CHECK(decided && active == (!strcmp(argv[1], "ssh-allow")) && gt_ext_test_dials == 1);
    CHECK(ext_info(s).end == (active ? GT_EXT_END_REVOKED : GT_EXT_END_DENIED) &&
          !gt_ext_session_lease(s, &lease));
    ext_drop(&s);
    printf("PASS ext_session_%s_%u\n", argv[1], count);
}
#endif
int main(int argc, char **argv) {
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "--executor")) {
        ext_executor();
        return 0;
    }
#ifdef GT_HAVE_LIBSSH2
    if (argc > 2) {
        ext_network(argc, argv);
        return 0;
    }
#endif
    size_t before = ext_fds();
    ext_native(argv[1]);
    CHECK(ext_fds() == before);
    printf("PASS ext_session_%s\n", argv[1]);
    return 0;
}
