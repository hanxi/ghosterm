/* Reuse real checked metadata + admitted native Session fixtures. */
#define PACKAGE_RUNTIME_MAIN prefix_fixture_runtime_main
#include "test_package_runtime.c"
#include "extension/process-service.h"

static gt_ext_process_t *prefix_child;

static void prefix_cleanup(void) {
    if (!prefix_child) {
        return;
    }
    (void)gt__ext_process_signal(prefix_child, SIGKILL);
    gt_ext_process_info_t info;
    for (unsigned i = 0; i < 1000; i++) {
        if (gt__ext_process_poll(prefix_child, &info) != GT_OK || info.exited) {
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    (void)gt__ext_process_destroy(&prefix_child);
}

int main(int argc, char **argv) {
    CHECK(argc == 3 && !atexit(prefix_cleanup));
    const char *mode = argv[1];
    size_t fds = ext_fds();
    load_fixture_t fixture;
    load_fixture(&fixture, "process", argv[2]);
    rpc_pair_t pair;
    load_pair(&pair, &fixture, "process");
    char host_value[64] = "host中文😀;$()";
    const char *prefix[17] = {"%s|%s|%s", host_value, ""};
    char long_value[4098];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = 0;
    const char *executable = !strcmp(mode, "count-boundary") ? "/usr/bin/true" : "/usr/bin/printf";
    char digest[65];
    CHECK(process_file_digest(executable, digest));
    gt_ext_process_program_mapping_t mapping = {
        "cat", executable, "prefix-fixture", digest, prefix, 3};
    if (!strcmp(mode, "count")) {
        mapping.prefix_argc = 17;
    } else if (!strcmp(mode, "pointer")) {
        mapping.prefix_argv = NULL;
    } else if (!strcmp(mode, "null-item")) {
        prefix[1] = NULL;
    } else if (!strcmp(mode, "argument-bytes")) {
        prefix[0] = long_value;
    } else if (!strcmp(mode, "total-bytes") || !strcmp(mode, "combined-bytes")) {
        long_value[!strcmp(mode, "total-bytes") ? 4096 : 4095] = 0;
        mapping.prefix_argc = 4;
        for (size_t i = 0; i < 4; i++) {
            prefix[i] = long_value;
        }
    } else if (!strcmp(mode, "combined-count") || !strcmp(mode, "count-boundary")) {
        mapping.prefix_argc = 16;
        for (size_t i = 0; i < 16; i++) {
            prefix[i] = "";
        }
    }
    bool invalid = !strcmp(mode, "count") || !strcmp(mode, "pointer") ||
                   !strcmp(mode, "null-item") || !strcmp(mode, "argument-bytes") ||
                   !strcmp(mode, "total-bytes");
    gt_plugin_memory_info_t memory = gt__plugin_memory_info();
    size_t reserved = !strcmp(mode, "memory") ? memory.limit - memory.used - 1024 : 0;
    void *reservation = reserved ? gt__plugin_memory_alloc(NULL, reserved) : NULL;
    CHECK(!reserved || reservation);
    gt_ext_process_service_t *service = NULL;
    gt_status_t status =
        gt__ext_process_service_open(pair.pair.e, &pair.e, fixture.package, &mapping, 1, &service);
    CHECK(status == (reserved ? GT_ERR_IO : invalid ? GT_ERR_INVALID_ARG : GT_OK));
    gt__plugin_memory_free(NULL, reservation, reserved);
    if (invalid || reserved) {
        CHECK(!service);
        CHECK(gt__plugin_memory_info().used == memory.used);
        goto done;
    }
    CHECK(service);
    CHECK(gt__plugin_memory_info().used > memory.used);
    /* Both pointed-to bytes and the pointer vector may be released/changed
     * after construction. The child must still receive the captured prefix. */
    strcpy(host_value, "caller-changed");
    prefix[0] = "changed-format";
    const char *arguments[49] = {"plugin-alias", "PLUGIN"};
    size_t count = 2;
    if (!strcmp(mode, "combined-count") || !strcmp(mode, "count-boundary")) {
        count = !strcmp(mode, "combined-count") ? 49 : 48;
        for (size_t i = 1; i < count; i++) {
            arguments[i] = "plugin";
        }
    } else if (!strcmp(mode, "combined-bytes")) {
        count = 1;
    }
    if (!strcmp(mode, "revoked")) {
        gt_ext_session_revoke(pair.pair.e);
    }
    status = gt__ext_process_service_spawn(
        service, "cat", arguments, count, fixture.fd, NULL, 0, &prefix_child);
    gt_status_t expected = !strcmp(mode, "revoked") ? GT_ERR_AUTH
                           : !strcmp(mode, "combined-count") || !strcmp(mode, "combined-bytes")
                               ? GT_ERR_INVALID_ARG
                               : GT_OK;
    CHECK(status == expected);
    if (expected != GT_OK) {
        CHECK(!prefix_child);
    } else {
        char output[256] = {0};
        size_t used = 0;
        bool eof = false;
        gt_ext_process_info_t info = {0};
        for (unsigned i = 0; i < 2000 && (!eof || !info.exited); i++) {
            gt_ext_process_io_t read =
                gt__ext_process_read_stdout(prefix_child, output + used, sizeof(output) - used - 1);
            if (read.state == GT_EXT_PROCESS_DATA) {
                used += read.count;
            } else {
                CHECK(read.state == GT_EXT_PROCESS_WAIT || read.state == GT_EXT_PROCESS_EOF);
                eof = read.state == GT_EXT_PROCESS_EOF;
            }
            CHECK(gt__ext_process_poll(prefix_child, &info) == GT_OK);
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        CHECK(eof && info.exited && info.exit_code == 0 && !info.term_signal);
        CHECK(!strcmp(output, !strcmp(mode, "count-boundary") ? "" : "host中文😀;$()||PLUGIN"));
        CHECK(gt__ext_process_destroy(&prefix_child) == GT_OK && !prefix_child);
    }
    CHECK(gt__ext_process_service_destroy(&service) == GT_OK && !service);
    CHECK(gt__plugin_memory_info().used == memory.used);
done:
    rpc_drop(&pair);
    load_clear(&fixture);
    CHECK(ext_fds() == fds);
    printf("PASS process-prefix %s\n", mode);
    return 0;
}
