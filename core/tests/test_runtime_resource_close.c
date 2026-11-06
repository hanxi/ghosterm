/* A real consumed close reports EIO for one Source, while another still waits
 * for its peer. IO must remain visible and retry must not close that fd again. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <unistd.h>
static int resource_close_fault_fd = -1;
static unsigned resource_close_fault_calls;

static int resource_close_fault(int fd) {
    int rc = close(fd);
    if (fd == resource_close_fault_fd) {
        resource_close_fault_fd = -1;
        resource_close_fault_calls++;
        errno = EIO;
        return -1;
    }
    return rc;
}

#define close resource_close_fault
#include "../src/extension/source.c"
#undef close
#include "../src/extension/source-api.c"
#define source_terminal runtime_owner_source_terminal
#include "../src/extension/runtime-resource.c"
#undef source_terminal
#define RUNTIME_RESOURCE_MAIN runtime_close_fixture_main
#include "test_runtime_resources.c"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    res_root = argv[1];
    size_t before = ext_fds();
    pub_source_fixture_t f = {.root = -1};
    pub_pair(&f.pair, "roundtrip");
    pub_file(&f, 262144, "file.bin");
    gt_runtime_resources_t *owner =
        resource_owner(f.pair.rpc.pair.h, f.pair.h.s, &f.pair.rpc.h, f.root);
    (void)publish(owner);
    (void)publish(owner);
    gt_ext_source_t *a = owner->sources[0].source, *b = owner->sources[1].source;
    pub_ready(b);
    CHECK(gt_ext_source_send(b) == GT_EXT_SOURCE_OK);
    for (unsigned i = 0; i < 40; i++) {
        rpc_pump(&f.pair.rpc);
        gt_ext_source_step(b);
    }
    CHECK(pub_info(b).has_transfer && !pub_info(b).confirmed && pub_info(b).accepted > 0 &&
          pub_info(b).accepted < 262144 && res_usage(&f.pair.e).reserved_bytes == 262144);
    resource_close_fault_fd = a->inner->file;
    CHECK(resource_close_fault_fd >= 0);
    CHECK(gt__runtime_resources_destroy(&owner) == GT_RUNTIME_RESOURCE_IO && owner &&
          resource_close_fault_calls == 1);
    gt_ext_session_stop(f.pair.rpc.pair.h);
    gt_ext_session_stop(f.pair.rpc.pair.e);
    gt_ext_session_step(f.pair.rpc.pair.h, 0);
    gt_ext_session_step(f.pair.rpc.pair.e, 0);
    CHECK(gt__runtime_resources_destroy(&owner) == GT_RUNTIME_RESOURCE_OK && !owner &&
          resource_close_fault_calls == 1);
    pub_fixture_drop(&f);
    /* Storage's already-missing release consumes its ref. A boundary peer removes
  * the real item first; runtime must also forget the stale native pin rather
  * than retaining a zeroed snapshot forever. */
    f = (pub_source_fixture_t){.root = -1};
    pub_pair(&f.pair, "roundtrip");
    pub_file(&f, 3, "file.bin");
    gt_runtime_resources_t *sender =
                               resource_owner(f.pair.rpc.pair.h, f.pair.h.s, &f.pair.rpc.h, f.root),
                           *receiver =
                               resource_owner(f.pair.rpc.pair.e, f.pair.e.s, &f.pair.rpc.e, -1);
    uint64_t id = publish(sender), ref = resource_complete(&f, sender, receiver, id, 3);
    gt_ext_resource_snapshot_t removed = receiver->pins[0].snapshot;
    CHECK(gt_ext_resource_store_release(f.pair.e.s, &f.pair.rpc.e, &removed) ==
              GT_EXT_RESOURCE_OK &&
          !removed.handle);
    CHECK(gt__runtime_resources_release(receiver, ref) == GT_RUNTIME_RESOURCE_OK &&
          !receiver->pins[0].id);
    CHECK(gt__runtime_resources_destroy(&sender) == GT_RUNTIME_RESOURCE_OK &&
          gt__runtime_resources_destroy(&receiver) == GT_RUNTIME_RESOURCE_OK);
    pub_fixture_drop(&f);
    CHECK(ext_fds() == before);
    puts("PASS runtime_resource_close_error_before_wait_and_missing_release");
    return 0;
}
