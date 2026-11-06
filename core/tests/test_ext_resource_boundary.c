/* Real close first, then injected EIO. A consumed fd is never retried. All
 * facade assertions use public APIs; the only seam is the OS close boundary. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
static bool fail_close;
static int failed_fd = -1;
static unsigned failed_closes, duplicate_closes;

static int boundary_close(int fd) {
    if (fd == failed_fd) {
        duplicate_closes++;
    }
    int result = close(fd);
    if (fail_close && !result) {
        fail_close = false;
        failed_fd = fd;
        failed_closes++;
        errno = EIO;
        return -1;
    }
    return result;
}

#define close boundary_close
#define resource_scope boundary_store_scope
#include "../src/extension/resource.c"
#undef resource_scope
#undef close
#include "extension/transfer.h"
static unsigned send_calls;

static gt_bridge_resource_status_t boundary_send(gt_bridge_session_t *s,
                                                 const gt_bridge_lease_t *l,
                                                 const gt_bridge_resource_descriptor_t *d,
                                                 uint64_t *handle) {
    send_calls++;
    return gt__bridge_session_send_resource(s, l, d, handle);
}

#define gt__bridge_session_send_resource boundary_send
#include "../src/extension/resource-api.c"
#undef gt__bridge_session_send_resource
#define EXT_RESOURCE_MAIN resource_boundary_fixture_main
#include "test_ext_resource.c"
#undef EXT_RESOURCE_MAIN

int main(int argc, char **argv) {
    CHECK(argc == 3);
    res_root = argv[2];
    size_t before = ext_fds();
    if (!strcmp(argv[1], "metadata-boundary")) {
        res_pair_t f;
        res_open(&f, "");
        for (unsigned field = 0; field < 4; field++) {
            gt_ext_resource_descriptor_t d = res_descriptor(3, false);
            char *fields[] = {d.resource_id, d.sha256, d.mime, d.display_name};
            size_t sizes[] = {33, 65, 129, 1025};
            memset(fields[field], 'x', sizes[field]);
            uint64_t handle = 99;
            CHECK(gt_ext_session_send_resource(f.rpc.pair.h, &f.rpc.h, &d, &handle) ==
                      GT_EXT_RESOURCE_INVALID &&
                  !handle);
            CHECK(send_calls == 0);
        }
        res_drop(&f);
    } else if (!strcmp(argv[1], "retired-close")) {
        res_pair_t f;
        res_open(&f, "");
        gt_ext_resource_snapshot_t snap = res_send(&f, false, 3, false, "");
        rpc_drop(&f.rpc);
        fail_close = true;
        CHECK(gt_ext_resource_store_release(f.e.s, &f.rpc.e, &snap) == GT_EXT_RESOURCE_IO &&
              snap.handle && failed_closes == 1 && !duplicate_closes);
        CHECK(res_usage(&f.e).scopes == 1 && !res_usage(&f.e).resources);
        CHECK(gt_ext_resource_store_release(f.e.s, &f.rpc.e, &snap) == GT_EXT_RESOURCE_NOT_FOUND &&
              !snap.handle);
        res_store_drop(&f.h);
        res_store_drop(&f.e);
        CHECK(!duplicate_closes);
    } else if (!strcmp(argv[1], "directory-close")) {
        res_store_t store = res_store(1048576);
        fail_close = true;
        CHECK(gt_ext_resource_store_destroy(&store.s) == GT_EXT_RESOURCE_IO && !store.s &&
              failed_closes == 1 && !duplicate_closes);
        CHECK(gt_ext_resource_store_destroy(&store.s) == GT_EXT_RESOURCE_INVALID &&
              !duplicate_closes);
        CHECK(!rmdir(store.path));
    } else {
        CHECK(false);
    }
    CHECK(ext_fds() == before);
    printf("PASS ext_resource_boundary_%s\n", argv[1]);
    return 0;
}
