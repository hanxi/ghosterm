#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include <fcntl.h>
#include <inttypes.h>
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
        /* Give gterm's real owner a chance to stop/reap its Node/Exec child. */
        kill(child, SIGTERM);
        for (unsigned i = 0; i < 5000; i++) {
            pid_t waited = waitpid(child, NULL, WNOHANG);
            if (waited == child) {
                child = 0;
                return;
            }
            if (waited < 0) {
                return;
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
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
    bool preview =
        argc >= 7 && (!strcmp(argv[4], "preview") || !strcmp(argv[4], "preview-unicode"));
    bool preview_unicode = preview && !strcmp(argv[4], "preview-unicode");
    bool process_prefix = argc == 11 && !strcmp(argv[4], "process-prefix");
    bool process_config = (argc == 9 && !strcmp(argv[4], "process-config")) || process_prefix;
    CHECK(argc == 5 || (preview && argc == (preview_unicode ? 8 : 7)) || process_config);
    CHECK(!atexit(cleanup));
    const char *binary = argv[1], *directory = argv[2], *package_digest = argv[3], *mode = argv[4];
    bool table = !strcmp(mode, "table-describe");
    bool task = !strcmp(mode, "task-read");
    bool diff = !strcmp(mode, "diff-page");
    bool codex = !strcmp(mode, "codex-thread");
    bool claude = !strcmp(mode, "claude-status");
    bool views = !strcmp(mode, "views-beta");
    bool view = table || task || diff || views;
    const bool fixture_denied = getenv("GT_GTERM_FIXTURE_DENY") != NULL;
    const char *workspace = preview || process_config ? argv[5] : NULL;
    const char *store_directory = preview ? argv[6] : NULL;
    const char *preview_path = preview_unicode ? argv[7] : getenv("GT_GTERM_PREVIEW_MAIN");
    if (!preview_path) {
        preview_path = "site/index.md";
    }
    char client_digest[65];
    memset(client_digest, 'd', 64);
    client_digest[64] = 0;
    const char *fixture_client_digest = getenv("GT_GTERM_FIXTURE_CLIENT_DIGEST");
    if (fixture_client_digest) {
        CHECK(strlen(fixture_client_digest) == 64);
        memcpy(client_digest, fixture_client_digest, 64);
    }
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
        const char *attach = getenv("GT_GTERM_FIXTURE_ATTACH");
        if (attach) {
            const char *instance = getenv("GT_GTERM_FIXTURE_ATTACH_INSTANCE");
            CHECK(instance);
            if (preview) {
                if (preview_unicode) {
                    execl(binary,
                          binary,
                          "relay",
                          "--attach",
                          attach,
                          "--instance",
                          instance,
                          "--",
                          "preview",
                          preview_path,
                          (char *)NULL);
                } else {
                    execl(binary,
                          binary,
                          "relay",
                          "--attach",
                          attach,
                          "--instance",
                          instance,
                          "--",
                          "preview",
                          preview_path,
                          "--assets",
                          "site/theme\\,dark.css",
                          (char *)NULL);
                }
            } else if (claude || codex) {
                execl(binary,
                      binary,
                      "relay",
                      "--attach",
                      attach,
                      "--instance",
                      instance,
                      "--",
                      claude ? "ai claude" : "ai codex",
                      (char *)NULL);
            } else if (views) {
                execl(binary,
                      binary,
                      "relay",
                      "--attach",
                      attach,
                      "--instance",
                      instance,
                      "--",
                      "views demo",
                      (char *)NULL);
            } else if (diff) {
                execl(binary,
                      binary,
                      "relay",
                      "--attach",
                      attach,
                      "--instance",
                      instance,
                      "--",
                      "diff",
                      "--before",
                      "\"old\"",
                      "--after",
                      "\"new\"",
                      (char *)NULL);
            } else if (view) {
                execl(binary,
                      binary,
                      "relay",
                      "--attach",
                      attach,
                      "--instance",
                      instance,
                      "--",
                      table ? "table" : "task primes",
                      table ? "--count" : "--limit",
                      "17",
                      (char *)NULL);
            } else {
                execl(binary,
                      binary,
                      "relay",
                      "--attach",
                      attach,
                      "--instance",
                      instance,
                      "--",
                      getenv("GT_GTERM_FIXTURE_COMMAND") ? getenv("GT_GTERM_FIXTURE_COMMAND")
                                                         : "fixture",
                      "中文",
                      "",
                      "';throw Error('injected');//",
                      (char *)NULL);
            }
            _exit(127);
        }
        const char *external = getenv("GT_GTERM_FIXTURE_RUNTIME");
        if (external) {
            const char *args[72];
            size_t n = 0;
            args[n++] = binary;
            args[n++] = "relay";
            args[n++] = "--stdio";
            args[n++] = "--runtime";
            args[n++] = external;
            if (!getenv("GT_GTERM_FIXTURE_NO_APPROVAL")) {
                args[n++] = "--allow-process-execution";
            }
            if (!strcmp(external, "node")) {
                args[n++] = "--node-runtime";
                args[n++] = getenv("GT_GTERM_FIXTURE_NODE");
                args[n++] = getenv("GT_GTERM_FIXTURE_VERSION");
                args[n++] = getenv("GT_GTERM_FIXTURE_NODE_SHA");
                args[n++] = "--node-sdk";
                args[n++] = getenv("GT_GTERM_FIXTURE_SDK");
                args[n++] = getenv("GT_GTERM_FIXTURE_SDK_SHA");
            }
            if (getenv("GT_GTERM_FIXTURE_UNSUPPORTED")) {
                args[n++] = "--process-environment";
                args[n++] = "GT_GTERM_FIXTURE_UNSUPPORTED";
            }
            args[n++] = "--trust-development-digest";
            args[n++] = trusted;
            args[n++] = "--client-digest";
            args[n++] = client_digest;
            if (process_config) {
                args[n++] = "--root";
                args[n++] = "workspace";
                args[n++] = workspace;
                args[n++] = "--process-program";
                args[n++] = "cat";
                args[n++] = argv[6];
                args[n++] = argv[7];
                args[n++] = argv[8];
            }
            args[n++] = "--method";
            args[n++] = "fixture.echo";
            args[n++] = "--";
            args[n++] = directory;
            args[n++] = "fixture";
            args[n++] = "中文";
            args[n++] = "";
            args[n++] = "';throw Error('injected');//";
            args[n] = NULL;
            for (size_t i = 0; i < n; i++) {
                CHECK(args[i]);
            }
            execv(binary, (char *const *)args);
        } else if (process_prefix) {
            execl(binary,
                  binary,
                  "relay",
                  "--stdio",
                  "--trust-development-digest",
                  package_digest,
                  "--client-digest",
                  client_digest,
                  "--root",
                  "workspace",
                  workspace,
                  "--process-program",
                  "cat",
                  argv[6],
                  argv[7],
                  argv[8],
                  "--process-argument",
                  "cat",
                  argv[9],
                  "--process-argument",
                  "cat",
                  "--profile",
                  "--process-argument",
                  "cat",
                  argv[10],
                  "--process-environment",
                  "CLAUDE_CONFIG_DIR",
                  "--process-environment",
                  "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC",
                  "--process-environment",
                  "GT_GTERM_NODE_EXTRA",
                  "--method",
                  "fixture.echo",
                  "--",
                  directory,
                  "fixture",
                  "中文",
                  "",
                  "';throw Error('injected');//",
                  (char *)NULL);
        } else if (process_config) {
            execl(binary,
                  binary,
                  "relay",
                  "--stdio",
                  "--trust-development-digest",
                  package_digest,
                  "--client-digest",
                  client_digest,
                  "--root",
                  "workspace",
                  workspace,
                  "--process-program",
                  "cat",
                  argv[6],
                  argv[7],
                  argv[8],
                  "--method",
                  "fixture.echo",
                  "--",
                  directory,
                  "fixture",
                  "中文",
                  "",
                  "';throw Error('injected');//",
                  (char *)NULL);
        } else if (preview) {
            if (preview_unicode) {
                execl(binary,
                      binary,
                      "relay",
                      "--stdio",
                      "--trust-development-digest",
                      package_digest,
                      "--client-digest",
                      client_digest,
                      "--root",
                      "workspace",
                      workspace,
                      "--",
                      directory,
                      "preview",
                      preview_path,
                      (char *)NULL);
            } else {
                execl(binary,
                      binary,
                      "relay",
                      "--stdio",
                      "--trust-development-digest",
                      package_digest,
                      "--client-digest",
                      client_digest,
                      "--root",
                      "workspace",
                      workspace,
                      "--",
                      directory,
                      "preview",
                      preview_path,
                      "--assets",
                      "site/theme\\,dark.css",
                      (char *)NULL);
            }
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
    gt_ext_session_config_t c = {.required_count =
                                     preview ? 2 : ((process_config || claude || codex) ? 3 : 1),
                                 .available_count = 1,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 60000,
                                 .peer = {GT_EXT_PEER_LOCAL, "test-explicit-child"},
                                 .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(c.plugin_id, preview ? "org.ghosterm.preview" : "org.ghosterm.fixture");
    strcpy(c.plugin_version, "0.1.0");
    const char *fixture_id = getenv("GT_GTERM_FIXTURE_PLUGIN_ID");
    const char *fixture_version = getenv("GT_GTERM_FIXTURE_PLUGIN_VERSION");
    if (fixture_id) {
        CHECK(strlen(fixture_id) < sizeof(c.plugin_id));
        strcpy(c.plugin_id, fixture_id);
    }
    if (fixture_version) {
        CHECK(strlen(fixture_version) < sizeof(c.plugin_version));
        strcpy(c.plugin_version, fixture_version);
    }
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.host_version, "0.1.0");
    strcpy(c.client_digest, client_digest);
    strcpy(c.executor_digest, package_digest);
    if (process_config || claude || codex) {
        c.available_count = 3;
        strcpy(c.required[0], "session.rpc");
        strcpy(c.required[1], "process.spawn");
        strcpy(c.required[2], "process.pipes");
        strcpy(c.available[0], "session.rpc");
        strcpy(c.available[1], "process.spawn");
        strcpy(c.available[2], "process.pipes");
    } else if (preview) {
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
    gt_ext_rpc_method_t methods[4] = {{.method = "fixture.echo",
                                       .capability = "session.rpc",
                                       .receiver = GT_EXT_EXECUTOR,
                                       .input_shape = GT_EXT_JSON_OBJECT,
                                       .output_shape = GT_EXT_JSON_OBJECT}};
    size_t method_count = 1;
    if (view) {
        strcpy(methods[0].method,
               table  ? "view.table.describe"
               : task ? "view.task.describe"
               : diff ? "view.diff.describe"
                      : "beta.describe");
        methods[1] = methods[0];
        strcpy(methods[1].method,
               table  ? "view.table.page"
               : task ? "view.task.read"
               : diff ? "view.diff.page"
                      : "beta.page");
        method_count++;
    }
    if (claude || codex) {
        strcpy(methods[0].method, claude ? "claude.status" : "codex.thread.start");
    }
    bool view_reopen = false;
    if (views) {
        methods[2] = methods[0];
        strcpy(methods[2].method, "views.reopen");
        methods[3] = methods[0];
        strcpy(methods[3].method, "views.client.open");
        methods[3].receiver = GT_EXT_HOST;
        method_count += 2;
    }
    bool view_read = false;
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
        CHECK(gt_ext_session_open_resources(
                  &channel, GT_EXT_HOST, &c, NULL, 0, store, 1048576, &session) == GT_OK);
    } else {
        CHECK(gt_ext_session_open_rpc(&channel, GT_EXT_HOST, &c, methods, method_count, &session) ==
              GT_OK);
    }
    uint64_t deadline = now() + (process_prefix || !strcmp(mode, "active-hold") ? 30000 : 10000),
             consent_at = 0, handle = 0, process_ready_at = 0;
    bool confirmed = false, host_method_confirmed = false, decided = false, signalled = false,
         lost = false, held = false;
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
        CHECK(!fixture_denied || info.phase != GT_EXT_SESSION_ACTIVE);
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
                                                (fixture_denied || !strcmp(mode, "deny"))
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
                CHECK(gt_ext_resource_store_info(
                          store, &resource_lease, &snapshot, &resource_info) == GT_EXT_RESOURCE_OK);
                const char *main_name =
                    preview_unicode ? strrchr(preview_path, '/') + 1 : "site/index.md";
                const char *name =
                    !strcmp(resource_info.descriptor.display_name, main_name) ? main_name
                    : !preview_unicode &&
                            !strcmp(resource_info.descriptor.display_name, "site/theme,dark.css")
                        ? "site/theme,dark.css"
                        : NULL;
                CHECK(name);
                bool markdown = !strcmp(name, main_name);
                const char *mime = markdown ? "text/markdown" : "text/css";
                const char *expected_bytes =
                    markdown ? "# Preview runtime fixture\n" : "body { color: rgb(1, 2, 3); }\n";
                const char *expected_sha256 =
                    markdown ? "75a7f6f9435ed30b30df8d0e4b1f9c49b29442158dd8c815a540358daecff365"
                             : "39eac723d7bb8a05bea2aa57d7b528eb83ed8f8dfe18e4f9659997457961a497";
                size_t expected_size = strlen(expected_bytes), read_size = 0;
                char bytes[128] = {0};
                CHECK(!strcmp(resource_info.descriptor.mime, mime) &&
                      resource_info.descriptor.size_known &&
                      resource_info.descriptor.size == expected_size &&
                      !strcmp(resource_info.descriptor.sha256, expected_sha256) &&
                      gt_ext_resource_store_read(
                          store, &resource_lease, &snapshot, 0, bytes, sizeof(bytes), &read_size) ==
                          GT_EXT_RESOURCE_OK &&
                      read_size == expected_size && !memcmp(bytes, expected_bytes, expected_size));
                if (markdown) {
                    CHECK(!preview_main);
                    preview_main = true;
                } else {
                    CHECK(!preview_asset);
                    preview_asset = true;
                }
                CHECK(gt_ext_resource_store_release(store, &resource_lease, &snapshot) ==
                      GT_EXT_RESOURCE_OK);
                if (preview_main && (preview_unicode || preview_asset) && !preview_complete_at) {
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
            if (process_config) {
                if (!process_ready_at) {
                    process_ready_at = now() + 100;
                }
                if (now() < process_ready_at) {
                    idle();
                    continue;
                }
            }
            if (strcmp(mode, "arguments-invalid")) {
                CHECK(gt_ext_session_lease(session, &lease));
                const char *input =
                    (view || claude || codex) ? "{}" : "{\"text\":\"stdio\",\"count\":17}";
                CHECK(gt_ext_session_request(session,
                                             &lease,
                                             methods[0].method,
                                             input,
                                             strlen(input),
                                             process_prefix ? 25000 : 3000,
                                             &handle) == GT_EXT_RPC_OK &&
                      handle);
            }
        }
        if (info.phase == GT_EXT_SESSION_ACTIVE && !host_method_confirmed &&
            (!strcmp(mode, "host-method") || view_reopen)) {
            gt_ext_rpc_request_t request;
            gt_ext_rpc_status_t request_status =
                gt_ext_session_take_request(session, &lease, &request);
            CHECK(request_status == GT_EXT_RPC_WAIT || request_status == GT_EXT_RPC_OK);
            if (request_status == GT_EXT_RPC_OK) {
                CHECK(!strcmp(request.method, views ? "views.client.open" : "fixture.ask"));
                if (views) {
                    const char beta[] = "{\"viewId\":\"beta\"}";
                    CHECK(request.input_size == sizeof(beta) - 1 &&
                          !memcmp(request.input, beta, sizeof(beta) - 1));
                }
                char outcome[GT_EXT_RPC_BODY_MAX + 1];
                int size =
                    snprintf(outcome,
                             sizeof(outcome),
                             "{\"kind\":\"success\",\"value\":%.*s}",
                             views ? (int)strlen("{\"opened\":true}") : (int)request.input_size,
                             views ? "{\"opened\":true}" : request.input);
                CHECK(size > 0 && (size_t)size < sizeof(outcome));
                CHECK(gt_ext_session_respond(
                          session, &lease, request.ticket, outcome, (size_t)size) == GT_EXT_RPC_OK);
                host_method_confirmed = true;
            }
        }
        if (held && access(getenv("GT_GTERM_FIXTURE_RELEASE"), R_OK) == 0) {
            held = false;
            gt_ext_session_revoke(session);
        }
        if (handle && !confirmed) {
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(session, &lease, handle, &result) == GT_EXT_RPC_OK);
            if (!strcmp(mode, "pending-loss") && result.state == GT_EXT_RPC_PENDING && !lost &&
                (!getenv("GT_GTERM_FIXTURE_HANDLER") ||
                 access(getenv("GT_GTERM_FIXTURE_HANDLER"), R_OK) == 0)) {
                CHECK(!shutdown(fd[0], SHUT_RDWR));
                lost = true;
            }
            if (result.confirmed && strcmp(mode, "pending-loss")) {
                bool valid = strstr(result.outcome, "stdio") != NULL;
                char page_input[160];
                int page_size = 0;
                if (codex) {
                    valid = strstr(result.outcome, "\"threadID\":\"thread-fixture\"") != NULL;
                } else if (claude) {
                    valid = strstr(result.outcome, "\"phase\":\"new\"") &&
                            strstr(result.outcome, "\"reaped\":false") &&
                            strstr(result.outcome, "\"exitJSON\":\"null\"");
                } else if (views && !view_read) {
                    page_size = snprintf(
                        page_input,
                        sizeof(page_input),
                        "{\"viewId\":\"beta\",\"revision\":\"1\",\"offset\":4,\"limit\":2}");
                    valid = strstr(result.outcome, "\"viewId\":\"beta\"") &&
                            strstr(result.outcome, "\"revision\":\"1\"") &&
                            strstr(result.outcome, "Beta numbers") &&
                            strstr(result.outcome, "\"total\":5");
                } else if (view && !view_read) {
                    page_size = snprintf(page_input,
                                         sizeof(page_input),
                                         "{\"viewId\":\"%s\",\"revision\":\"%" PRIu64 "\"%s}",
                                         lease.instance,
                                         lease.epoch,
                                         table  ? ",\"offset\":16,\"limit\":16"
                                         : diff ? ",\"offset\":0,\"limit\":2"
                                                : "");
                    char view_key[64], revision_key[64];
                    snprintf(view_key, sizeof(view_key), "\"viewId\":\"%s\"", lease.instance);
                    snprintf(revision_key,
                             sizeof(revision_key),
                             "\"revision\":\"%" PRIu64 "\"",
                             lease.epoch);
                    valid =
                        strstr(result.outcome, view_key) && strstr(result.outcome, revision_key);
                    if (diff) {
                        valid = valid && strstr(result.outcome, "Text comparison") &&
                                strstr(result.outcome, "\"added\":1") &&
                                strstr(result.outcome, "\"removed\":1");
                    } else {
                        valid =
                            valid && (table ? strstr(result.outcome, "Number powers") &&
                                                  strstr(result.outcome, "\"total\":17") &&
                                                  strstr(result.outcome, "\"pageSize\":16")
                                            : strstr(result.outcome, "Count primes") &&
                                                  strstr(result.outcome, "\"cancellable\":true"));
                    }
                } else if (view_reopen) {
                    valid = host_method_confirmed && strstr(result.outcome, "\"opened\":true");
                } else if (views) {
                    valid = strstr(result.outcome, "\"viewId\":\"beta\"") &&
                            strstr(result.outcome, "\"offset\":4") &&
                            strstr(result.outcome, "\"cells\":[\"5\",\"50\"]");
                } else if (table) {
                    valid = strstr(result.outcome, "\"offset\":16") &&
                            strstr(result.outcome, "\"total\":17") &&
                            strstr(result.outcome, "\"cells\":[\"17\",\"289\",\"4913\"]");
                } else if (diff) {
                    valid = strstr(result.outcome, "\"total\":2") &&
                            strstr(result.outcome, "\"kind\":\"delete\"") &&
                            strstr(result.outcome, "\"text\":\"old\"") &&
                            strstr(result.outcome, "\"kind\":\"insert\"") &&
                            strstr(result.outcome, "\"text\":\"new\"");
                } else if (task) {
                    valid = strstr(result.outcome, "\"phase\":\"succeeded\"") &&
                            strstr(result.outcome, "7 primes up to 17");
                }
                if (result.state != GT_EXT_RPC_SUCCEEDED || !valid) {
                    fprintf(stderr,
                            "RPC state %d outcome %.*s\n",
                            (int)result.state,
                            (int)result.outcome_size,
                            result.outcome);
                    CHECK(false);
                }
                CHECK(gt_ext_session_forget_request(session, &lease, handle) == GT_EXT_RPC_OK);
                if (view && !view_read) {
                    CHECK(page_size > 0 && (size_t)page_size < sizeof(page_input));
                    CHECK(gt_ext_session_request(session,
                                                 &lease,
                                                 methods[1].method,
                                                 page_input,
                                                 (size_t)page_size,
                                                 3000,
                                                 &handle) == GT_EXT_RPC_OK &&
                          handle);
                    view_read = true;
                    continue;
                }
                if (views && !view_reopen) {
                    const char input[] = "{\"viewId\":\"beta\"}";
                    CHECK(gt_ext_session_request(session,
                                                 &lease,
                                                 methods[2].method,
                                                 input,
                                                 sizeof(input) - 1,
                                                 5000,
                                                 &handle) == GT_EXT_RPC_OK &&
                          handle);
                    view_reopen = true;
                    continue;
                }
                confirmed = true;
                if (!strcmp(mode, "active-cancel")) {
                    CHECK(!kill(child, SIGTERM));
                    signalled = true;
                } else if (!strcmp(mode, "lost")) {
                    CHECK(!shutdown(fd[0], SHUT_RDWR));
                    lost = true;
                } else if (!strcmp(mode, "active-hold")) {
                    const char *marker = getenv("GT_GTERM_FIXTURE_READY");
                    CHECK(marker && getenv("GT_GTERM_FIXTURE_RELEASE"));
                    int ready =
                        open(marker, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                    CHECK(ready >= 0 && write(ready, "active", 6) == 6 && !fsync(ready) &&
                          !close(ready));
                    held = true;
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
    int expected = !strcmp(mode, "runtime-unsupported")               ? 4
                   : !strcmp(mode, "runtime-failure")                 ? 5
                   : !strcmp(mode, "denied-launch")                   ? 6
                   : !strcmp(mode, "digest") || !strcmp(mode, "deny") ? 6
                   : !strcmp(mode, "arguments-invalid")               ? 5
                   : !strcmp(mode, "command")                         ? 2
                   : !strcmp(mode, "peer-digest") || !strcmp(mode, "lost") ||
                           !strcmp(mode, "pending-loss") || !strcmp(mode, "silent")
                       ? 7
                   : !strcmp(mode, "cancel") || !strcmp(mode, "active-cancel") ? 130
                   : !strcmp(mode, "dispose-error")                            ? 5
                                                                               : 0;
    if (getenv("GT_GTERM_FIXTURE_ATTACH")) {
        expected = !strcmp(mode, "cancel") || !strcmp(mode, "active-cancel") ? 130 : 0;
    }
    CHECK(WEXITSTATUS(status) == expected);
    if (!strcmp(mode, "runtime-failure") || !strcmp(mode, "runtime-unsupported")) {
        CHECK(decided && !confirmed);
    }
    if (!strcmp(mode, "digest") || !strcmp(mode, "command") || !strcmp(mode, "denied-launch")) {
        gt_ext_session_info_t info;
        CHECK(gt_ext_session_info(session, &info) && !info.received);
    }
    if (fixture_denied) {
        CHECK(decided && consent_at && !handle && !preview_main && !preview_asset);
    } else if (preview) {
        CHECK(preview_main && (preview_unicode || preview_asset) && decided && consent_at);
    }
    if (!fixture_denied && (view || claude || codex || !strcmp(mode, "roundtrip") ||
                            process_config || !strcmp(mode, "dispose-error") || pipes ||
                            !strcmp(mode, "active-cancel") || !strcmp(mode, "lost"))) {
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
