/* Real OS close first, then EIO. Public facade must retain the source/Session
 * pin and preserve primary error without retrying the consumed numeric fd. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
static bool pub_close_fail;
static int pub_failed_fd = -1;
static unsigned pub_close_failures, pub_close_repeats;

static int pub_close_boundary(int fd) {
    if (fd == pub_failed_fd) {
        pub_close_repeats++;
    }
    int r = close(fd);
    if (pub_close_fail && !r) {
        pub_close_fail = false;
        pub_failed_fd = fd;
        pub_close_failures++;
        errno = EIO;
        return -1;
    }
    return r;
}

#define close pub_close_boundary
#include "../src/extension/source.c"
#undef close
#define EXT_SOURCE_MAIN ext_source_boundary_fixture_main
#include "test_ext_source.c"
#undef EXT_SOURCE_MAIN

int main(int argc, char **argv) {
    CHECK(argc == 3);
    res_root = argv[2];
    size_t before = ext_fds();
    pub_source_fixture_t f = {.root = -1};
    pub_pair(&f.pair, "");
    pub_file(&f, 3, "file.bin");
    gt_ext_source_t *s = pub_open(&f, "file.bin", 262144);
    gt_ext_source_status_t original = GT_EXT_SOURCE_CANCELLED;
    if (!strcmp(argv[1], "failure-close")) {
        pub_ready(s);
        int fd = open(f.file, O_WRONLY | O_CLOEXEC);
        CHECK(fd >= 0 && pwrite(fd, "Z", 1, 0) == 1 && !close(fd));
        CHECK(gt_ext_source_send(s) == GT_EXT_SOURCE_CHANGED);
        original = GT_EXT_SOURCE_CHANGED;
    } else {
        CHECK(!strcmp(argv[1], "close-error") || !strcmp(argv[1], "destroy-error"));
    }
    pub_close_fail = true;
    gt_ext_source_status_t status =
        !strcmp(argv[1], "destroy-error") ? gt_ext_source_destroy(&s) : gt_ext_source_close(s);
    CHECK(status == GT_EXT_SOURCE_IO && s && pub_close_failures == 1 && !pub_close_repeats);
    gt_ext_source_info_t saved = pub_info(s);
    CHECK(saved.closed && saved.cleanup_error == GT_EXT_SOURCE_IO && saved.error == original);
    CHECK(gt_ext_session_close(f.pair.rpc.pair.h, 0) == GT_EXT_DRAINING &&
          gt_ext_session_destroy(&f.pair.rpc.pair.h) == GT_ERR_INVALID_ARG);
    pub_drop(&s);
    CHECK(!pub_close_repeats && saved.cleanup_error == GT_EXT_SOURCE_IO && saved.error == original);
    pub_fixture_drop(&f);
    CHECK(ext_fds() == before);
    printf("PASS ext_source_boundary_%s\n", argv[1]);
    return 0;
}
