#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <sys/socket.h>
int jump_fixture_connect(int, const struct sockaddr *, socklen_t);
/* Substitute only native dialing. Even a reachable destination cannot be
 * dialed directly; all inner authentication must traverse direct-tcpip. */
#define connect jump_fixture_connect
#define gt__ssh_wire_wait jump_real_wire_wait
#include "../src/ssh.c"
#undef connect
#undef gt__ssh_wire_wait
#include "extension/backends/ssh_exec.h"
#include "extension/memory.h"
#include "extension/route.h"
#include <arpa/inet.h>
#include <dirent.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL jump: %s (%s)\n", #x, gt_last_error());                          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint16_t first_port;
static unsigned dials;
static bool fail_wait;
static unsigned wait_failures;

int gt__ssh_wire_wait(gt_ssh_t *ssh, short events, int ms, const int *wake, size_t count) {
    if (fail_wait && ms == 10 && !count) {
        wait_failures++;
        errno = EINTR;
        return -1;
    }
    return jump_real_wire_wait(ssh, events, ms, wake, count);
}

int jump_fixture_connect(int fd, const struct sockaddr *address, socklen_t size) {
    dials++;
    if (address->sa_family != AF_INET ||
        ntohs(((const struct sockaddr_in *)address)->sin_port) != first_port) {
        errno = EACCES;
        return -1;
    }
    return connect(fd, address, size);
}

static uint64_t milliseconds(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool clock_value(void *context, uint64_t *value) {
    (void)context;
    *value = milliseconds();
    return true;
}

static void pause_one(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static size_t fd_count(void) {
    DIR *dir = opendir("/dev/fd");
    CHECK(dir);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            count++;
        }
    }
    CHECK(!closedir(dir));
    return count;
}

static gt_bridge_stream_t *route_owner;
static gt_ssh_route_t *public_owner;

static void dispose(gt_ssh_t **session) {
    if (public_owner) {
        uint64_t start = milliseconds();
        bool released = false;
        while (!released) {
            CHECK(gt_ssh_route_dispose(&public_owner, &released) == GT_OK);
            CHECK(milliseconds() - start < 1000);
        }
        CHECK(!public_owner);
        CHECK(gt_ssh_route_dispose(&public_owner, &released) == GT_OK && released);
        *session = NULL;
        return;
    }
    if (route_owner) {
        uint64_t start = milliseconds();
        while (gt__bridge_stream_close(route_owner, 0) != GT_BRIDGE_RELEASED) {
            CHECK(milliseconds() - start < 1000);
        }
        gt_ssh_t *invalid = (gt_ssh_t *)1;
        CHECK(gt__bridge_route_borrow_session(route_owner, &invalid) == GT_ERR_INVALID_ARG &&
              !invalid);
        CHECK(gt__bridge_stream_destroy(route_owner) == GT_OK);
        route_owner = NULL;
        *session = NULL;
        return;
    }
    CHECK(gt_ssh_abort(*session) == GT_OK);
    uint64_t start = milliseconds();
    bool released = false;
    while (!released) {
        CHECK(gt__ssh_dispose_step(session, &released) == GT_OK);
        CHECK(milliseconds() - start < 1000);
    }
    CHECK(!*session);
}

static gt_ssh_t *
authenticate(int count, char **ports, const char *user, const char *known, const char *pem) {
    gt_ssh_t *session = NULL;
    gt_bridge_stream_t *wire = NULL;
    if (getenv("GT_TEST_PUBLIC_ROUTE_OWNER")) {
        gt_ssh_t *owned[8] = {0};
        gt_ssh_route_hop_t configs[8] = {0};
        for (int hop = 0; hop < count; hop++) {
            char name[64];
            snprintf(name, sizeof name, "route-hop-%d.invalid", hop);
            gt_ssh_opts_t options = {.host = name,
                                     .port = (uint16_t)atoi(ports[hop]),
                                     .username = user,
                                     .known_hosts_path = known};
            owned[hop] = gt_ssh_new(&options);
            CHECK(owned[hop]);
            configs[hop].authentication = (gt_ssh_auth_options_t){
                .family = hop ? 0 : 4, .address = {127, 0, 0, 1}, .defer_credentials = true};
            configs[hop].via_host = hop ? "127.0.0.1" : NULL;
        }
        CHECK(gt_ssh_route_open(owned, configs, (uint32_t)count, 60001, &public_owner) ==
                  GT_ERR_INVALID_ARG &&
              !public_owner);
        for (int hop = 0; hop < count; hop++) {
            CHECK(owned[hop]);
        }
        CHECK(gt_ssh_route_open(owned, configs, (uint32_t)count, 0, &public_owner) == GT_OK);
        for (int hop = 0; hop < count; hop++) {
            CHECK(!owned[hop]);
        }
        CHECK(gt_ssh_route_borrow(public_owner, &session) == GT_ERR_INVALID_ARG && !session);
        uint64_t end = milliseconds() + 6000;
        unsigned supplied = 0;
        while (gt_ssh_route_step(public_owner) != GT_SSH_ROUTE_READY) {
            gt_ssh_route_info_t info;
            CHECK(gt_ssh_route_info(public_owner, &info));
            CHECK(info.phase < GT_SSH_ROUTE_FAILED && info.count == (unsigned)count &&
                  info.hop < (unsigned)count && milliseconds() < end);
            if (info.phase == GT_SSH_ROUTE_AUTHENTICATING &&
                info.authentication.phase == GT_SSH_AUTH_PHASE_CREDENTIALS) {
                CHECK(info.authentication.host == GT_SSH_AUTH_HOST_KNOWN &&
                      info.authentication.auth_methods && info.authentication.key_type[0] &&
                      info.authentication.fingerprint[0]);
                CHECK(!(supplied & (1u << info.hop)));
                supplied |= 1u << info.hop;
                char name[64];
                snprintf(name, sizeof name, "route-hop-%u.invalid", info.hop);
                gt_ssh_opts_t options = {.host = name,
                                         .port = (uint16_t)atoi(ports[info.hop]),
                                         .username = user,
                                         .known_hosts_path = known};
                gt_ssh_t *material = gt_ssh_new(&options);
                CHECK(material);
                CHECK(gt_ssh_auth_private_key(material, pem, NULL) == GT_OK);
                CHECK(gt_ssh_route_supply(public_owner, &material) == GT_OK && !material);
            }
            pause_one();
        }
        CHECK(supplied == (1u << count) - 1);
        CHECK(gt_ssh_route_borrow(public_owner, &session) == GT_OK && session);
        gt_ssh_t *second = (gt_ssh_t *)1;
        CHECK(gt_ssh_route_borrow(public_owner, &second) == GT_ERR_INVALID_ARG && !second);
        CHECK(dials == 1 && gt__ssh_wire_depth(session) == (unsigned)count);
        return session;
    }
    if (getenv("GT_TEST_ROUTE_OWNER")) {
        gt_ssh_t *owned[8] = {0};
        gt_bridge_route_hop_t configs[8] = {0};
        for (int hop = 0; hop < count; hop++) {
            char name[64];
            snprintf(name, sizeof name, "route-hop-%d.invalid", hop);
            gt_ssh_opts_t options = {.host = name,
                                     .port = (uint16_t)atoi(ports[hop]),
                                     .username = user,
                                     .known_hosts_path = known};
            owned[hop] = gt_ssh_new(&options);
            CHECK(owned[hop]);
            CHECK(gt_ssh_auth_private_key(owned[hop], pem, NULL) == GT_OK);
            configs[hop].connector = (gt_ssh_connector_config_t){.family = hop ? 0 : 4,
                                                                 .address = {127, 0, 0, 1},
                                                                 .timeout_ms = 5000,
                                                                 .clock = clock_value};
            configs[hop].via_host = hop ? "127.0.0.1" : NULL;
        }
        CHECK(gt__bridge_route_open_session(
                  owned, configs, (size_t)count, NULL, NULL, &route_owner) == GT_OK);
        for (int hop = 0; hop < count; hop++) {
            CHECK(!owned[hop]);
        }
        CHECK(gt__bridge_route_borrow_session(route_owner, &session) == GT_ERR_INVALID_ARG &&
              !session);
        uint64_t end = milliseconds() + 6000;
        while (gt__bridge_route_step(route_owner, 0) != GT_BRIDGE_ROUTE_READY) {
            gt_bridge_route_info_t info;
            CHECK(gt__bridge_route_info(route_owner, &info));
            CHECK(info.phase < GT_BRIDGE_ROUTE_FAILED && milliseconds() < end);
            pause_one();
        }
        CHECK(gt__bridge_route_borrow_session(route_owner, &session) == GT_OK && session);
        gt_ssh_t *second = (gt_ssh_t *)1;
        CHECK(gt__bridge_route_borrow_session(route_owner, &second) == GT_ERR_INVALID_ARG &&
              !second);
        uint8_t byte = 0;
        CHECK(gt__bridge_stream_read(route_owner, &byte, 1, 0).error == GT_ERR_UNSUPPORTED);
        CHECK(gt__bridge_stream_write(route_owner, &byte, 1, 0).error == GT_ERR_UNSUPPORTED);
        CHECK(dials == 1 && gt__ssh_wire_depth(session) == (unsigned)count);
        return session;
    }
    for (int hop = 0; hop < count; hop++) {
        char name[64];
        snprintf(name, sizeof name, "route-hop-%d.invalid", hop);
        gt_ssh_opts_t options = {.host = name,
                                 .port = (uint16_t)atoi(ports[hop]),
                                 .username = user,
                                 .known_hosts_path = known};
        gt_ssh_t *material = gt_ssh_new(&options);
        CHECK(material);
        CHECK(gt_ssh_auth_private_key(material, pem, NULL) == GT_OK);
        gt_ssh_connector_config_t config = {.family = hop ? 0 : 4,
                                            .address = {127, 0, 0, 1},
                                            .timeout_ms = 5000,
                                            .clock = clock_value};
        gt_ssh_connector_t *auth = NULL;
        CHECK((hop ? gt__ssh_connector_open_stream(&material, &config, &wire, NULL, &auth)
                   : gt__ssh_connector_open(&material, &config, NULL, &auth)) == GT_OK);
        CHECK(!material && !wire);
        uint64_t end = milliseconds() + 6000;
        for (;;) {
            gt_ssh_connector_phase_t phase = gt__ssh_connector_step(auth);
            if (phase == GT_SSH_CONNECT_READY) {
                break;
            }
            CHECK(phase < GT_SSH_CONNECT_TAKEN && phase != GT_SSH_CONNECT_HOSTKEY &&
                  milliseconds() < end);
            pause_one();
        }
        CHECK(gt__ssh_connector_take(auth, &session));
        bool released = false;
        CHECK(gt__ssh_connector_dispose(&auth, &released) == GT_OK && released && !auth);
        if (hop + 1 < count) {
            CHECK(gt__bridge_ssh_forward_open(
                      &session, "127.0.0.1", (uint16_t)atoi(ports[hop + 1]), NULL, NULL, &wire) ==
                  GT_OK);
            CHECK(!session);
            while (gt__bridge_ssh_step(wire, 0) != GT_BRIDGE_SSH_READY) {
                gt_bridge_ssh_info_t info;
                CHECK(gt__bridge_ssh_info(wire, &info));
                CHECK(info.phase < GT_BRIDGE_SSH_FAILED && milliseconds() < end);
                pause_one();
            }
        }
    }
    CHECK(dials == 1 && gt__ssh_wire_depth(session) == (unsigned)count);
    return session;
}

static bool cancelled(void *context) {
    (void)context;
    return true;
}

static bool cancel_on_third(void *context) {
    return ++*(unsigned *)context >= 3;
}

int main(int argc, char **argv) {
    CHECK(argc >= 7); /* mode, fixture file, known_hosts, user, at least two ports */
    int hops = argc - 5;
    CHECK(hops >= 2 && hops <= 8);
    char key[65536];
    size_t length = fread(key, 1, sizeof key - 1, stdin);
    CHECK(length > 0 && feof(stdin));
    key[length] = 0;
    first_port = (uint16_t)atoi(argv[5]);
    size_t before = fd_count();
    gt_ssh_t *session = authenticate(hops, argv + 5, argv[4], argv[3], key);
    memset(key, 0, sizeof key);
    uint64_t closing = 0;
    if (!strncmp(argv[1], "pty", 3)) {
        gt_shell_opts_t options = {.term = "xterm-256color", .cols = 80, .rows = 24};
        gt_status_t status;
        gt_transport_t *transport = gt_transport_open_ssh_shell_controlled(
            session,
            &options,
            5000,
            !strcmp(argv[1], "pty-cancel") ? cancelled : NULL,
            NULL,
            &status);
        if (!strcmp(argv[1], "pty-cancel")) {
            CHECK(!transport && status == GT_ERR_CANCELLED);
            goto cleanup;
        }
        CHECK(transport && status == GT_OK);
        CHECK(gt_transport_resize(transport, 83, 27, 0, 0) == GT_OK);
        if (!strcmp(argv[1], "pty-echo") || !strcmp(argv[1], "pty-idle")) {
            char command[4096];
            CHECK(!strchr(argv[2], '\''));
            CHECK(snprintf(command,
                           sizeof command,
                           "exec '%s.echo' %s\n",
                           argv[2],
                           !strcmp(argv[1], "pty-idle") ? "idle" : "echo") > 0);
            CHECK(gt_transport_write(transport, (const uint8_t *)command, strlen(command)) ==
                  GT_OK);
            char banner[8192] = {0};
            size_t used = 0;
            uint64_t end = milliseconds() + 5000;
            while (!strstr(banner, "JUMP_READY\n")) {
                int n = gt_transport_read(
                    transport, (uint8_t *)banner + used, sizeof banner - used - 1, 20);
                CHECK(n != GT_TRANSPORT_READ_ERROR && n != GT_TRANSPORT_READ_EOF &&
                      milliseconds() < end);
                if (n > 0) {
                    used += (size_t)n;
                }
                CHECK(used < sizeof banner - 1);
            }
            if (!strcmp(argv[1], "pty-idle")) {
                uint8_t byte;
                uint64_t start = milliseconds();
                CHECK(gt_transport_read(transport, &byte, 1, 0) == GT_TRANSPORT_READ_TIMEOUT &&
                      milliseconds() - start < 50);
                start = milliseconds();
                CHECK(gt_transport_read(transport, &byte, 1, 20) == GT_TRANSPORT_READ_TIMEOUT &&
                      milliseconds() - start < 200);
            } else {
                uint8_t data[262144];
                for (size_t i = 0; i < sizeof data; i++) {
                    data[i] = (uint8_t)(i * 17u + 3u);
                }
                CHECK(gt_transport_write(transport, data, sizeof data) == GT_OK);
                size_t received = 0;
                end = milliseconds() + 6000;
                for (;;) {
                    uint8_t bytes[8192];
                    int n = gt_transport_read(transport, bytes, sizeof bytes, 20);
                    if (n == GT_TRANSPORT_READ_EOF) {
                        break;
                    }
                    CHECK(n != GT_TRANSPORT_READ_ERROR && milliseconds() < end);
                    if (n > 0) {
                        CHECK(received + (size_t)n <= sizeof data);
                        CHECK(!memcmp(data + received, bytes, (size_t)n));
                        received += (size_t)n;
                    }
                }
                CHECK(received == sizeof data);
            }
        } else {
            const char *command = "stty -echo\nprintf 'JUMP_%s\\n' 'OK'\nstty size\nexit\n";
            CHECK(gt_transport_write(transport, (const uint8_t *)command, strlen(command)) ==
                  GT_OK);
            char output[16384] = {0};
            size_t used = 0;
            uint64_t end = milliseconds() + 6000;
            for (;;) {
                int n = gt_transport_read(
                    transport, (uint8_t *)output + used, sizeof output - used - 1, 20);
                if (n == GT_TRANSPORT_READ_EOF) {
                    break;
                }
                CHECK(n != GT_TRANSPORT_READ_ERROR && milliseconds() < end &&
                      used < sizeof output - 1);
                if (n > 0) {
                    used += (size_t)n;
                }
            }
            CHECK(strstr(output, "JUMP_OK") && strstr(output, "27 83"));
        }
        closing = milliseconds();
        gt_transport_close(transport);
    } else {
        gt_status_t status = GT_OK;
        if (!strcmp(argv[1], "wait-error") || !strcmp(argv[1], "read-wait-error")) {
            uint64_t start = milliseconds();
            fail_wait = !strcmp(argv[1], "wait-error");
            gt_sftp_t *sftp = gt_ssh_sftp_controlled(session, 5000, NULL, NULL, &status);
            if (fail_wait) {
                CHECK(!sftp && status == GT_ERR_IO && wait_failures &&
                      milliseconds() - start < 1000);
            } else {
                CHECK(sftp && status == GT_OK);
                fail_wait = true;
                uint8_t data[262144];
                size_t received = 123;
                int code = gt_sftp_preview_controlled(
                    sftp, argv[2], data, sizeof data, &received, 5000, NULL, NULL);
                CHECK(code == GT_ERR_IO && !received && wait_failures &&
                      milliseconds() - start < 1000);
                fail_wait = false;
                closing = milliseconds();
                gt_sftp_close(sftp);
            }
            fail_wait = false;
        } else if (!strcmp(argv[1], "open-cancel")) {
            unsigned calls = 0;
            uint64_t start = milliseconds();
            gt_sftp_t *sftp =
                gt_ssh_sftp_controlled(session, 5000, cancel_on_third, &calls, &status);
            CHECK(!sftp && status == GT_ERR_CANCELLED && calls >= 3 &&
                  milliseconds() - start < 1000);
        } else if (!strcmp(argv[1], "cancel")) {
            uint64_t start = milliseconds();
            gt_sftp_t *sftp = gt_ssh_sftp_controlled(session, 5000, cancelled, NULL, &status);
            CHECK(!sftp && status == GT_ERR_CANCELLED && milliseconds() - start < 100);
        } else {
            gt_sftp_t *sftp = gt_ssh_sftp_controlled(session, 5000, NULL, NULL, &status);
            CHECK(sftp && status == GT_OK);
            uint8_t data[262144];
            size_t received = 0;
            CHECK(gt_sftp_preview_controlled(
                      sftp, argv[2], data, sizeof data, &received, 5000, NULL, NULL) == GT_OK);
            CHECK(received == sizeof data);
            for (size_t i = 0; i < sizeof data; i++) {
                CHECK(data[i] == (uint8_t)(i * 17u + 3u));
            }
            closing = milliseconds();
            gt_sftp_close(sftp);
        }
    }
cleanup:
    dispose(&session);
    CHECK(!closing || milliseconds() - closing < 1500);
    CHECK(gt__plugin_memory_info().used == 0 && fd_count() == before);
    puts("SSH JUMP IO PASS");
    return 0;
}
