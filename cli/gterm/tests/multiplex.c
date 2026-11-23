#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "echo-outcome.h"
#include "ghosterm.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static gt_ext_mux_t *mux;
static gt_ext_session_t *sessions[8];
static gt_ext_lease_t leases[8];
static uint64_t deadline;

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void pump(void) {
    CHECK(now() < deadline);
    (void)gt_ext_mux_step(mux, 0);
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            gt_ext_session_step(sessions[i], 0);
        }
    }
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static void close_session(unsigned slot) {
    while (gt_ext_session_close(sessions[slot], 0) != GT_EXT_RELEASED) {
        pump();
    }
    CHECK(gt_ext_session_destroy(&sessions[slot]) == GT_OK && !sessions[slot]);
}

static void cleanup(void) {
    if (!mux) {
        return;
    }
    gt_ext_mux_stop(mux);
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            gt_ext_session_stop(sessions[i]);
            if (gt_ext_session_close(sessions[i], 0) == GT_EXT_RELEASED) {
                gt_ext_session_destroy(&sessions[i]);
            }
        }
    }
    for (unsigned i = 0; i < 5000; i++) {
        if (gt_ext_mux_close(mux, 0) == GT_EXT_RELEASED) {
            gt_ext_mux_destroy(&mux);
            return;
        }
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
}

static void rpc_wave(unsigned wave, bool skip_third) {
    uint64_t requests[8] = {0};
    bool done[8] = {0};
    unsigned completed = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (skip_third && i == 3) {
            done[i] = true;
            completed++;
            continue;
        }
        char input[96];
        int size =
            snprintf(input, sizeof(input), "{\"text\":\"scope-%u\",\"count\":%u}", i, 17 + i);
        CHECK(
            size > 0 &&
            gt_ext_session_request(
                sessions[i], &leases[i], "fixture.echo", input, (size_t)size, 5000, &requests[i]) ==
                GT_EXT_RPC_OK);
    }
    while (completed < 8) {
        pump();
        for (unsigned i = 0; i < 8; i++) {
            if (done[i]) {
                continue;
            }
            gt_ext_rpc_result_t result;
            CHECK(gt_ext_session_request_result(sessions[i], &leases[i], requests[i], &result) ==
                  GT_EXT_RPC_OK);
            if (!result.confirmed) {
                CHECK(result.state == GT_EXT_RPC_QUEUED || result.state == GT_EXT_RPC_PENDING);
                continue;
            }
            char expected[24];
            CHECK(snprintf(expected, sizeof(expected), "scope-%u", i) > 0);
            if (!echo_matches(&result, expected, 17 + i, i, wave)) {
                fprintf(stderr,
                        "slot %u wave %u state %u outcome %.*s\n",
                        i,
                        wave,
                        result.state,
                        (int)result.outcome_size,
                        result.outcome);
                CHECK(false);
            }
            CHECK(gt_ext_session_forget_request(sessions[i], &leases[i], requests[i]) ==
                  GT_EXT_RPC_OK);
            done[i] = true;
            completed++;
        }
    }
}

static gt_ext_rpc_result_t table_call(unsigned slot, const char *method, const char *input) {
    uint64_t handle;
    CHECK(gt_ext_session_request(
              sessions[slot], &leases[slot], method, input, strlen(input), 5000, &handle) ==
          GT_EXT_RPC_OK);
    gt_ext_rpc_result_t result;
    do {
        pump();
        CHECK(gt_ext_session_request_result(sessions[slot], &leases[slot], handle, &result) ==
              GT_EXT_RPC_OK);
    } while (!result.confirmed);
    CHECK(gt_ext_session_forget_request(sessions[slot], &leases[slot], handle) == GT_EXT_RPC_OK);
    return result;
}

static void project_page(unsigned slot, char input[160]) {
    gt_ext_rpc_result_t description = table_call(slot, "view.table.describe", "{}");
    CHECK(description.state == GT_EXT_RPC_SUCCEEDED);
    gt_bridge_wire_t *doc = NULL;
    CHECK(gt__bridge_wire_new(NULL, &doc) == GT_OK &&
          gt__bridge_json_parse(doc, description.outcome, description.outcome_size) == GT_OK);
    const uint16_t value = gt__bridge_wire_member(doc, 1, "value");
    const uint8_t *id, *revision;
    size_t id_size, revision_size;
    CHECK(
        echo_string(doc,
                    gt__bridge_wire_member(doc, value, "title"),
                    slot % 2 ? "Project tasks · sample-b" : "Project tasks · sample-a") &&
        echo_uint(doc, gt__bridge_wire_member(doc, value, "total"), 2) &&
        gt__bridge_wire_string(doc, gt__bridge_wire_member(doc, value, "viewId"), &id, &id_size) &&
        gt__bridge_wire_string(
            doc, gt__bridge_wire_member(doc, value, "revision"), &revision, &revision_size) &&
        id_size == 32 && revision_size <= 20);
    CHECK(snprintf(input,
                   160,
                   "{\"viewId\":\"%.*s\",\"revision\":\"%.*s\",\"offset\":0,\"limit\":2}",
                   (int)id_size,
                   id,
                   (int)revision_size,
                   revision) > 0);
    gt__bridge_wire_free(doc);
    const gt_ext_rpc_result_t page = table_call(slot, "view.table.page", input);
    CHECK(page.state == GT_EXT_RPC_SUCCEEDED && gt__bridge_wire_new(NULL, &doc) == GT_OK &&
          gt__bridge_json_parse(doc, page.outcome, page.outcome_size) == GT_OK);
    const uint16_t rows =
        gt__bridge_wire_member(doc, gt__bridge_wire_member(doc, 1, "value"), "rows");
    const char *const expected_ids[2][2] = {{"sample-a-review", "sample-a-publish"},
                                            {"sample-b-inventory", "sample-b-verify"}};
    CHECK(gt__bridge_wire_count(doc, rows) == 2);
    for (size_t i = 0; i < 2; ++i) {
        const uint16_t row = gt__bridge_wire_at(doc, rows, i);
        CHECK(echo_string(doc, gt__bridge_wire_member(doc, row, "id"), expected_ids[slot % 2][i]));
    }
    gt__bridge_wire_free(doc);
}

int main(int argc, char **argv) {
    CHECK(argc == 14);
    CHECK(!atexit(cleanup));
    deadline = now() + 30000;
    bool resources = !strcmp(argv[2], "resources"), denied = !strcmp(argv[2], "denied"),
         cancelled = !strcmp(argv[2], "cancel"), projects = !strcmp(argv[2], "table-projects");
    CHECK(resources || denied || cancelled || projects || !strcmp(argv[2], "roundtrip"));
    char identities[8][33], epochs[8][24], temporary[8192];
    const char *arguments[40] = {
        argv[1], "relay", "--multiplex", "--connection", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    size_t count = 5;
    for (unsigned i = 0; i < 8; i++) {
        CHECK(snprintf(identities[i], sizeof(identities[i]), "%032x", i + 1) == 32);
        CHECK(snprintf(epochs[i], sizeof(epochs[i]), "%u", i + 1) > 0);
        arguments[count++] = "--instance";
        arguments[count++] = identities[i];
        arguments[count++] = epochs[i];
        arguments[count++] = argv[6 + i];
    }
    CHECK(snprintf(temporary, sizeof(temporary), "TMPDIR=%s", argv[5]) > 0);
    const char *environment[] = {temporary};
    int cwd = open(argv[5], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cwd >= 0);
    gt_ext_process_channel_config_t launch = {.executable = argv[1],
                                              .arguments = arguments,
                                              .argument_count = count,
                                              .cwd_fd = cwd,
                                              .environment = environment,
                                              .environment_count = 1,
                                              .execution_authorized = true,
                                              .lifetime_ms = 40000,
                                              .stdout_limit = 16777216};
    gt_ext_channel_t *physical = NULL;
    CHECK(gt_ext_channel_open_process(&launch, &physical) == GT_OK && !close(cwd));
    gt_ext_mux_config_t config = {.frame_bytes = 4096, .receive_frames = 64, .instances = 8};
    strcpy(config.connection, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(gt_ext_mux_open(&physical, &config, &mux) == GT_OK && !physical);
    gt_ext_resource_store_t *store = NULL;
    char store_path[8192];
    int root = -1;
    if (resources) {
        CHECK(snprintf(store_path, sizeof(store_path), "%s/host-resources-XXXXXX", argv[5]) > 0 &&
              mkdtemp(store_path));
        root = open(store_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(root >= 0);
        gt_ext_resource_store_config_t c = {root, 16777216, 1048576, 64};
        CHECK(gt_ext_resource_store_open(&c, &store) == GT_EXT_RESOURCE_OK);
    }
    gt_ext_session_config_t c = {.required_count = resources ? 3 : 1,
                                 .available_count = resources ? 3 : 1,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 60000,
                                 .peer = {GT_EXT_PEER_LOCAL, "owned-gterm-multiplex"},
                                 .features = GT_EXT_FEATURE_REQUESTS |
                                             (resources ? GT_EXT_FEATURE_RESOURCES : 0)};
    strcpy(c.plugin_id, argv[3]);
    strcpy(c.plugin_version, "0.1.0");
    memset(c.client_digest, 'd', 64);
    strcpy(c.executor_digest, argv[4]);
    strcpy(c.host_version, "0.1.0");
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.required[0], "session.rpc");
    if (resources) {
        strcpy(c.required[1], "resource.receive");
        strcpy(c.required[2], "file.readScoped");
    }
    memcpy(c.available, c.required, sizeof(c.available));
    gt_ext_rpc_method_t methods[2] = {{.capability = "session.rpc",
                                       .receiver = GT_EXT_EXECUTOR,
                                       .input_shape = GT_EXT_JSON_OBJECT,
                                       .output_shape = GT_EXT_JSON_OBJECT}};
    methods[1] = methods[0];
    strcpy(methods[0].method,
           projects    ? "view.table.describe"
           : resources ? "preview.open"
                       : "fixture.echo");
    strcpy(methods[1].method, "view.table.page");
    for (unsigned i = 0; i < 8; i++) {
        gt_ext_channel_t *child = NULL;
        CHECK(gt_ext_mux_open_channel(mux, identities[i], i + 1, &child) == GT_OK);
        gt_status_t status =
            resources ? gt_ext_session_open_resources(
                            &child, GT_EXT_HOST, &c, methods, 1, store, 1048576, &sessions[i])
                      : gt_ext_session_open_rpc(
                            &child, GT_EXT_HOST, &c, methods, projects ? 2 : 1, &sessions[i]);
        CHECK(status == GT_OK && !child);
    }
    for (;;) {
        pump();
        unsigned ready = 0;
        for (unsigned i = 0; i < 8; i++) {
            gt_ext_session_info_t info;
            CHECK(gt_ext_session_info(sessions[i], &info));
            if (info.phase == GT_EXT_SESSION_CONSENT) {
                CHECK(info.decision == GT_EXT_DECISION_UNKNOWN);
                CHECK(gt_ext_session_decide(sessions[i],
                                            info.consent_token,
                                            denied && i == 3 ? GT_EXT_DECISION_DENY
                                                             : GT_EXT_DECISION_ALLOW) == GT_OK);
            }
            if (denied && i == 3) {
                ready += info.end == GT_EXT_END_DENIED;
            } else {
                if (info.end != GT_EXT_END_NONE) {
                    gt_ext_transport_process_info_t process = {0};
                    gt_ext_mux_process_info(mux, &process);
                    fprintf(
                        stderr,
                        "handshake slot %u end %u error %d child-exit=%d signal=%d stderr %.*s\n",
                        i,
                        info.end,
                        info.error,
                        process.exit_code,
                        process.term_signal,
                        (int)process.diagnostic_size,
                        process.diagnostic);
                    CHECK(false);
                }
                ready += info.phase == GT_EXT_SESSION_ACTIVE;
            }
        }
        if (ready == 8) {
            break;
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (!denied || i != 3) {
            CHECK(gt_ext_session_lease(sessions[i], &leases[i]));
        }
    }
    gt_ext_resource_snapshot_t snapshots[8] = {0};
    char table_page[8][160];
    if (projects) {
        for (unsigned i = 0; i < 8; ++i) {
            project_page(i, table_page[i]);
        }
    } else if (resources) {
        const char input[] = "{\"path\":\"preview.md\",\"assets\":\"\",\"maxBytes\":64}";
        uint64_t requests[8];
        for (unsigned i = 0; i < 8; i++) {
            CHECK(gt_ext_session_request(sessions[i],
                                         &leases[i],
                                         "preview.open",
                                         input,
                                         sizeof(input) - 1,
                                         10000,
                                         &requests[i]) == GT_EXT_RPC_OK);
        }
        unsigned received = 0;
        while (received < 8) {
            pump();
            for (unsigned i = 0; i < 8; i++) {
                if (snapshots[i].owner) {
                    continue;
                }
                gt_ext_resource_status_t status =
                    gt_ext_session_take_resource(sessions[i], &leases[i], &snapshots[i]);
                CHECK(status == GT_EXT_RESOURCE_OK || status == GT_EXT_RESOURCE_WAIT);
                received += status == GT_EXT_RESOURCE_OK;
            }
        }
        for (unsigned i = 0; i < 8; i++) {
            char bytes[64], expected[64];
            size_t size;
            int length = snprintf(expected, sizeof(expected), "# mux slot %u\n", i);
            CHECK(gt_ext_resource_store_read(
                      store, &leases[(i + 1) % 8], &snapshots[i], 0, bytes, sizeof(bytes), &size) ==
                      GT_EXT_RESOURCE_DENIED &&
                  !size);
            CHECK(gt_ext_resource_store_read(
                      store, &leases[i], &snapshots[i], 0, bytes, sizeof(bytes), &size) ==
                      GT_EXT_RESOURCE_OK &&
                  size == (size_t)length && !memcmp(bytes, expected, size));
        }
        for (unsigned i = 0; i < 8; i++) {
            gt_ext_rpc_result_t result;
            do {
                pump();
                CHECK(gt_ext_session_request_result(
                          sessions[i], &leases[i], requests[i], &result) == GT_EXT_RPC_OK);
            } while (!result.confirmed);
            CHECK(result.state == GT_EXT_RPC_SUCCEEDED);
            CHECK(gt_ext_session_forget_request(sessions[i], &leases[i], requests[i]) ==
                  GT_EXT_RPC_OK);
        }
    } else {
        rpc_wave(1, denied);
    }
    if (cancelled) {
        /* Native physical close delivers TERM to the owned gterm process;
         * its handler must stop/release all eight live Runtime owners. */
        gt_ext_mux_stop(mux);
        for (unsigned i = 0; i < 8; i++) {
            close_session(i);
        }
        while (gt_ext_mux_close(mux, 0) != GT_EXT_RELEASED) {
            CHECK(now() < deadline);
            struct timespec t = {0, 1000000};
            nanosleep(&t, NULL);
        }
        gt_ext_transport_process_info_t process;
        CHECK(gt_ext_mux_process_info(mux, &process) == GT_OK && process.child_exited &&
              process.reaped);
        if (process.exit_code != 130 || process.term_signal) {
            fprintf(stderr,
                    "cancel exit=%d signal=%d stderr %.*s\n",
                    process.exit_code,
                    process.term_signal,
                    (int)process.diagnostic_size,
                    process.diagnostic);
            CHECK(false);
        }
        CHECK(gt_ext_mux_destroy(&mux) == GT_OK && !mux);
        puts("PASS gterm multiplex cancel: eight active runtimes released, cancellation exit "
             "reaped");
        return 0;
    }
    if (!denied) {
        gt_ext_session_revoke(sessions[3]);
    }
    close_session(3);
    uint64_t settle = now() + 100;
    while (now() < settle) {
        pump();
    }
    gt_ext_mux_info_t info;
    CHECK(gt_ext_mux_info(mux, &info) && info.instances == 7 && !info.stopping);
    gt_ext_transport_process_info_t process;
    CHECK(gt_ext_mux_process_info(mux, &process) == GT_OK && !process.child_exited &&
          !process.reaped);
    for (unsigned i = 0; i < 8; i++) {
        if (i == 3) {
            continue;
        }
        gt_ext_session_info_t state;
        CHECK(gt_ext_session_info(sessions[i], &state) && state.phase == GT_EXT_SESSION_ACTIVE);
    }
    if (projects) {
        const gt_ext_rpc_result_t stale = table_call(4, "view.table.page", table_page[3]);
        CHECK(stale.state == GT_EXT_RPC_FAILED && strstr(stale.outcome, "INVALID_INPUT"));
        project_page(4, table_page[4]);
    } else if (!resources) {
        rpc_wave(2, true);
    }
    for (unsigned i = 0; i < 8; i++) {
        if (sessions[i]) {
            gt_ext_session_revoke(sessions[i]);
            close_session(i);
        }
    }
    while (true) {
        (void)gt_ext_mux_step(mux, 0);
        CHECK(gt_ext_mux_process_info(mux, &process) == GT_OK);
        if (process.stdout_eof) {
            break;
        }
        CHECK(now() < deadline);
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    /* process_info is a snapshot. After physical EOF, close drives the final
     * observation/reap even though the mux no longer accepts protocol IO. */
    while (gt_ext_mux_close(mux, 0) != GT_EXT_RELEASED) {
        CHECK(now() < deadline);
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    CHECK(gt_ext_mux_process_info(mux, &process) == GT_OK && process.child_exited &&
          process.reaped);
    if (process.exit_code != (denied ? 6 : 0) || process.term_signal) {
        fprintf(stderr,
                "gterm exit %d signal %d stderr %.*s\n",
                process.exit_code,
                process.term_signal,
                (int)process.diagnostic_size,
                process.diagnostic);
        CHECK(false);
    }
    CHECK(gt_ext_mux_destroy(&mux) == GT_OK && !mux);
    if (resources) {
        /* Detached, explicitly retained snapshots survive their Session owner. */
        for (unsigned i = 0; i < 8; i++) {
            CHECK(gt_ext_resource_store_release(store, &leases[i], &snapshots[i]) ==
                  GT_EXT_RESOURCE_OK);
        }
        CHECK(gt_ext_resource_store_destroy(&store) == GT_EXT_RESOURCE_OK && !close(root) &&
              !rmdir(store_path));
    }
    printf(
        "PASS gterm multiplex %s: one process, eight instances, isolated teardown, reaped exit\n",
        argv[2]);
    return 0;
}
