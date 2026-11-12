#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static pid_t child;

static void cleanup(void) {
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
}

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void idle(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
    bool preview = argc == 7 && !strcmp(argv[4], "preview");
    CHECK(argc == 5 || preview);
    CHECK(!atexit(cleanup));
    const char *binary = argv[1], *directory = argv[2], *package_digest = argv[3], *mode = argv[4];
    const char *workspace = preview ? argv[5] : NULL;
    const char *store_directory = preview ? argv[6] : NULL;
    char client_digest[65];
    memset(client_digest, 'd', 64);
    client_digest[64] = 0;
    bool pipes = !strcmp(mode, "pipes");
    int fd[2], up[2] = {-1, -1}, down[2] = {-1, -1};
    if (pipes) {
        CHECK(!pipe(up) && !pipe(down));
        fd[0] = down[0];
        fd[1] = up[1];
    } else {
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    }
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (pipes) {
            close(up[1]);
            close(down[0]);
            CHECK(dup2(up[0], 0) == 0 && dup2(down[1], 1) == 1);
            close(up[0]);
            close(down[1]);
        } else {
            close(fd[0]);
            CHECK(dup2(fd[1], 0) == 0 && dup2(fd[1], 1) == 1);
            close(fd[1]);
        }
        const char *trusted = !strcmp(mode, "digest") ? client_digest : package_digest;
        if (preview) {
            execl(binary, binary, "relay", "--stdio", "--trust-development-digest",
                  package_digest, "--client-digest", client_digest, "--root", "workspace",
                  workspace, "--", directory, "preview", "site/index.md", "--assets",
                  "site/theme\\,dark.css", (char *)NULL);
        } else if (!strcmp(mode, "arguments-codec")) {
            execl(binary,
                  binary,
                  "relay",
                  "--stdio",
                  "--trust-development-digest",
                  trusted,
                  "--client-digest",
                  client_digest,
                  "--method",
                  "fixture.echo",
                  "--",
                  directory,
                  "fixture",
                  "中文",
                  "",
                  "';throw Error('injected');//",
                  "--count=17",
                  "--wrap",
                  (char *)NULL);
        } else if (!strcmp(mode, "host-method")) {
            execl(binary,
                  binary,
                  "relay",
                  "--stdio",
                  "--trust-development-digest",
                  trusted,
                  "--client-digest",
                  client_digest,
                  "--method",
                  "fixture.echo",
                  "--host-method",
                  "fixture.ask",
                  "--",
                  directory,
                  "fixture",
                  "中文",
                  "",
                  "';throw Error('injected');//",
                  (char *)NULL);
        } else if (!strcmp(mode, "arguments-invalid")) {
            execl(binary,
                  binary,
                  "relay",
                  "--stdio",
                  "--trust-development-digest",
                  trusted,
                  "--client-digest",
                  client_digest,
                  "--method",
                  "fixture.echo",
                  "--",
                  directory,
                  "fixture",
                  "中文",
                  "",
                  "';throw Error('injected');//",
                  "--count=101",
                  (char *)NULL);
        } else {
            execl(binary,
                  binary,
                  "relay",
                  "--stdio",
                  "--trust-development-digest",
                  trusted,
                  "--client-digest",
                  client_digest,
                  "--method",
                  "fixture.echo",
                  "--",
                  directory,
                  !strcmp(mode, "command") ? "other" : "fixture",
                  "中文",
                  "",
                  "';throw Error('injected');//",
                  (char *)NULL);
        }
        _exit(127);
    }
    if (pipes) {
        close(up[0]);
        close(down[1]);
    } else {
        close(fd[1]);
    }
    gt_ext_channel_t *channel = NULL;
    CHECK(gt_ext_channel_open_fd(fd[0], pipes ? fd[1] : fd[0], &channel) == GT_OK);
    gt_ext_session_config_t c = {.required_count = preview ? 2 : 1,
                                 .available_count = 1,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 60000,
                                 .peer = {GT_EXT_PEER_LOCAL, "test-explicit-child"},
                                 .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(c.plugin_id, preview ? "org.ghosterm.preview" : "org.ghosterm.fixture");
    strcpy(c.plugin_version, "0.1.0");
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.host_version, "0.1.0");
    strcpy(c.client_digest, client_digest);
    strcpy(c.executor_digest, package_digest);
    if (preview) {
        c.available_count = 2;
        c.features = GT_EXT_FEATURE_RESOURCES | GT_EXT_FEATURE_REQUESTS;
        strcpy(c.required[0], "resource.receive");
        strcpy(c.required[1], "file.readScoped");
        strcpy(c.available[0], "resource.receive");
        strcpy(c.available[1], "file.readScoped");
    } else {
        strcpy(c.required[0], "session.rpc");
        strcpy(c.available[0], "session.rpc");
    }
    if (!strcmp(mode, "peer-digest")) {
        strcpy(c.client_digest, package_digest);
    }
    gt_ext_rpc_method_t methods[2] = {{.method = "fixture.echo",
                                       .capability = "session.rpc",
                                       .receiver = GT_EXT_EXECUTOR,
                                       .input_shape = GT_EXT_JSON_OBJECT,
                                       .output_shape = GT_EXT_JSON_OBJECT}};
    size_t method_count = 1;
    if (!strcmp(mode, "host-method")) {
        methods[1] = (gt_ext_rpc_method_t){.method = "fixture.ask",
                                           .capability = "session.rpc",
                                           .receiver = GT_EXT_HOST,
                                           .input_shape = GT_EXT_JSON_OBJECT,
                                           .output_shape = GT_EXT_JSON_OBJECT};
        method_count++;
    }
    gt_ext_session_t *session = NULL;
    gt_ext_resource_store_t *store = NULL;
    if (preview) {
        int store_fd = open(store_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(store_fd >= 0);
        gt_ext_resource_store_config_t resource_config = {store_fd, 1048576, 262144, 8};
        CHECK(gt_ext_resource_store_open(&resource_config, &store) == GT_EXT_RESOURCE_OK);
        CHECK(!close(store_fd));
        CHECK(gt_ext_session_open_resources(&channel, GT_EXT_HOST, &c, NULL, 0, store,
                                            1048576, &session) == GT_OK);
    } else {
        CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &c, methods, method_count, &session) ==
              GT_OK);
    }
    uint64_t deadline = now() + 10000, consent_at = 0, handle = 0;
    bool confirmed = false, host_method_confirmed = false, decided = false, signalled = false,
         lost = false;
    bool preview_main = false, preview_asset = false;
    uint64_t preview_complete_at = 0;
    gt_ext_lease_t lease;
    int status = 0;
    bool ended = false;
    while (now() < deadline) {
        if (strcmp(mode, "silent")) {
            gt_ext_session_step(session, 0);
        }
        gt_ext_session_info_t info;
        CHECK(gt_ext_session_info(session, &info));
        if (info.phase == GT_EXT_SESSION_CONSENT && !decided) {
            if (!consent_at) {
                consent_at = now();
            }
            /* Withhold approval for a measurable interval. No RPC/activation is admitted. */
            if (now() - consent_at >= 100) {
                if (!strcmp(mode, "cancel")) {
                    CHECK(!kill(child, SIGTERM));
                    signalled = true;
                    decided = true;
                } else {
                    CHECK(gt_ext_session_decide(session,
                                                info.consent_token,
                                                !strcmp(mode, "deny")
                                                    ? GT_EXT_DECISION_DENY
                                                    : GT_EXT_DECISION_ALLOW) == GT_OK);
                    decided = true;
                }
            }
        }
        if (info.phase == GT_EXT_SESSION_ACTIVE && preview) {
            gt_ext_lease_t resource_lease;
            CHECK(gt_ext_session_lease(session, &resource_lease));
            gt_ext_resource_snapshot_t snapshot;
            gt_ext_resource_status_t received =
                gt_ext_session_take_resource(session, &resource_lease, &snapshot);
            if (received == GT_EXT_RESOURCE_OK) {
                gt_ext_resource_info_t resource_info;
                CHECK(gt_ext_resource_store_info(store, &resource_lease, &snapshot,
                                                 &resource_info) == GT_EXT_RESOURCE_OK);
                const char *name = !strcmp(resource_info.descriptor.display_name, "site/index.md")
                                       ? "site/index.md"
                                   : !strcmp(resource_info.descriptor.display_name,
                                             "site/theme,dark.css")
                                       ? "site/theme,dark.css"
                                       : NULL;
                CHECK(name);
                const char *mime = !strcmp(name, "site/index.md") ? "text/markdown" : "text/css";
                const char *expected_bytes = !strcmp(name, "site/index.md")
                                                 ? "# Preview runtime fixture\n"
                                                 : "body { color: rgb(1, 2, 3); }\n";
                const char *expected_sha256 = !strcmp(name, "site/index.md")
                                                  ? "75a7f6f9435ed30b30df8d0e4b1f9c49b29442158dd8c815a540358daecff365"
                                                  : "39eac723d7bb8a05bea2aa57d7b528eb83ed8f8dfe18e4f9659997457961a497";
                size_t expected_size = strlen(expected_bytes), read_size = 0;
                char bytes[128] = {0};
                CHECK(!strcmp(resource_info.descriptor.mime, mime) &&
                      resource_info.descriptor.size_known &&
                      resource_info.descriptor.size == expected_size &&
                      !strcmp(resource_info.descriptor.sha256, expected_sha256) &&
                      gt_ext_resource_store_read(store, &resource_lease, &snapshot, 0, bytes,
                                                 sizeof(bytes), &read_size) == GT_EXT_RESOURCE_OK &&
                      read_size == expected_size && !memcmp(bytes, expected_bytes, expected_size));
                if (!strcmp(name, "site/index.md")) {
                    CHECK(!preview_main);
                    preview_main = true;
                } else {
                    CHECK(!preview_asset);
                    preview_asset = true;
                }
                CHECK(gt_ext_resource_store_release(store, &resource_lease, &snapshot) ==
                      GT_EXT_RESOURCE_OK);
                if (preview_main && preview_asset && !preview_complete_at) {
                    preview_complete_at = now();
                }
            } else {
                CHECK(received == GT_EXT_RESOURCE_WAIT);
            }
            if (preview_complete_at && now() - preview_complete_at >= 250) {
                gt_ext_session_revoke(session);
            }
        } else if (info.phase == GT_EXT_SESSION_ACTIVE && !handle) {
            CHECK(strcmp(mode, "deny") && strcmp(mode, "cancel"));
            if (strcmp(mode, "arguments-invalid")) {
                CHECK(gt_ext_session_lease(session, &lease));
                const char *input = "{\"text\":\"stdio\",\"count\":17}";
                CHECK(gt_ext_session_request(
                          session, &lease, "fixture.echo", input, strlen(input), 3000, &handle) ==
                          GT_EXT_RPC_OK &&
                      handle);
            }
        }
        if (info.phase == GT_EXT_SESSION_ACTIVE && !host_method_confirmed &&
            !strcmp(mode, "host-method")) {
            gt_ext_rpc_request_t request;
            gt_ext_rpc_status_t request_status =
                gt_ext_session_take_request(session, &lease, &request);
            CHECK(request_status == GT_EXT_RPC_WAIT || request_status == GT_EXT_RPC_OK);
            if (request_status == GT_EXT_RPC_OK) {
                CHECK(!strcmp(request.method, "fixture.ask"));
                char outcome[GT_EXT_RPC_BODY_MAX + 1];
                int size = snprintf(outcome,
                                    sizeof(outcome),
                                    "{\"kind\":\"success\",\"value\":%.*s}",
                                    (int)request.input_size,
                                    request.input);
                CHECK(size > 0 && (size_t)size < sizeof(outcome));
                CHECK(gt_ext_session_respond(
                          session, &lease, request.ticket, outcome, (size_t)size) == GT_EXT_RPC_OK);
                host_method_confirmed = true;
            }
        }
        if (handle && !confirmed) {
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(session, &lease, handle, &result) == GT_EXT_RPC_OK);
            if (!strcmp(mode, "pending-loss") && result.state == GT_EXT_RPC_PENDING && !lost) {
                CHECK(!shutdown(fd[0], SHUT_RDWR));
                lost = true;
            }
            if (result.confirmed && strcmp(mode, "pending-loss")) {
                if (result.state != GT_EXT_RPC_SUCCEEDED || !strstr(result.outcome, "stdio")) {
                    fprintf(stderr,
                            "RPC state %d outcome %.*s\n",
                            (int)result.state,
                            (int)result.outcome_size,
                            result.outcome);
                    CHECK(false);
                }
                confirmed = true;
                CHECK(gt_ext_session_forget_request(session, &lease, handle) == GT_EXT_RPC_OK);
                if (!strcmp(mode, "active-cancel")) {
                    CHECK(!kill(child, SIGTERM));
                    signalled = true;
                } else if (!strcmp(mode, "lost")) {
                    CHECK(!shutdown(fd[0], SHUT_RDWR));
                    lost = true;
                } else {
                    if (strcmp(mode, "host-method") || host_method_confirmed) {
                        gt_ext_session_revoke(session);
                    }
                }
            }
        }
        pid_t waited = waitpid(child, &status, WNOHANG);
        CHECK(waited >= 0);
        if (waited == child) {
            ended = true;
            child = 0;
            break;
        }
        idle();
    }
    CHECK(ended);
    if (!WIFEXITED(status)) {
        fprintf(stderr, "ABNORMAL child status %d\n", status);
        return 2;
    }
    int expected = !strcmp(mode, "digest") || !strcmp(mode, "deny") ? 6
                   : !strcmp(mode, "arguments-invalid")             ? 5
                   : !strcmp(mode, "command")                       ? 2
                   : !strcmp(mode, "peer-digest") || !strcmp(mode, "lost") ||
                           !strcmp(mode, "pending-loss") || !strcmp(mode, "silent")
                       ? 7
                   : !strcmp(mode, "cancel") || !strcmp(mode, "active-cancel") ? 130
                   : !strcmp(mode, "dispose-error")                            ? 5
                                                                               : 0;
    CHECK(WEXITSTATUS(status) == expected);
    if (!strcmp(mode, "digest") || !strcmp(mode, "command")) {
        gt_ext_session_info_t info;
        CHECK(gt_ext_session_info(session, &info) && !info.received);
    }
    if (preview) {
        CHECK(preview_main && preview_asset && decided && consent_at);
    }
    if (!strcmp(mode, "roundtrip") || !strcmp(mode, "dispose-error") || pipes ||
        !strcmp(mode, "active-cancel") || !strcmp(mode, "lost")) {
        CHECK(confirmed && decided && consent_at);
    }
    if (!strcmp(mode, "host-method")) {
        CHECK(confirmed && host_method_confirmed && decided && consent_at);
    }
    if (!strcmp(mode, "cancel")) {
        CHECK(signalled && !handle);
    }
    if (!strcmp(mode, "active-cancel")) {
        CHECK(signalled && confirmed);
    }
    if (!strcmp(mode, "lost") || !strcmp(mode, "pending-loss")) {
        CHECK(lost);
    }
    if (!strcmp(mode, "pending-loss")) {
        gt_ext_session_step(session, 0);
        gt_ext_rpc_result_t result;
        CHECK(gt_ext_session_request_result(session, &lease, handle, &result) == GT_EXT_RPC_OK &&
              result.state == GT_EXT_RPC_UNKNOWN && !result.confirmed);
    }
    if (!strcmp(mode, "silent")) {
        CHECK(!decided && !handle);
    }
    if (!strcmp(mode, "arguments-invalid")) {
        CHECK(decided && !handle);
    }
    for (unsigned i = 0; i < 100; i++) {
        if (gt_ext_session_close(session, 1) == GT_EXT_RELEASED) {
            break;
        }
    }
    CHECK(gt_ext_session_destroy(&session) == GT_OK);
    if (store) {
        CHECK(gt_ext_resource_store_destroy(&store) == GT_EXT_RESOURCE_OK && !store);
        CHECK(!rmdir(store_directory));
    }
    printf("PASS stdio %s\n", mode);
    return 0;
}
