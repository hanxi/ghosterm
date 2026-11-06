/* Explicit missing-capability behavior independent of the host libssh2 build. */
#include "ghosterm.h"
#include "internal.h"
#include <stdio.h>
#undef GT_HAVE_LIBSSH2
#include "../src/sftp.c"

int main(void) {
    gt_sftp_remove_result_t out = {GT_SFTP_REMOVE_REMOVED, 99};
    int rc = gt_sftp_remove_controlled(NULL, "/", "owned", false, 0, 0, &out, 100, NULL, NULL);
    int bad = rc != GT_ERR_UNSUPPORTED || out.state != GT_SFTP_REMOVE_NOT_ATTEMPTED ||
              out.remote_status != 0;
    if (bad) {
        puts("FAIL: remove unsupported capability");
    }
    return bad;
}
