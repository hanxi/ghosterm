#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Only the explicit test build redirects catalog-store calls here. Core and
 * the normal CLI never consult these environment variables. Failed close
 * consumes the actual descriptor; callers must not retry it. */
int gterm_test_fsync(int fd) {
    const char *mode = getenv("GTERM_TEST_CATALOG_IO");
    struct stat s;
    if (mode && !fstat(fd, &s) &&
        ((!strcmp(mode, "file-sync") && S_ISREG(s.st_mode)) ||
         (!strcmp(mode, "directory-sync") && S_ISDIR(s.st_mode)))) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

int gterm_test_close(int fd) {
    const char *mode = getenv("GTERM_TEST_CATALOG_IO");
    struct stat s;
    bool fail = mode && !strcmp(mode, "directory-close") && !fstat(fd, &s) && S_ISDIR(s.st_mode);
    int status = close(fd);
    if (!status && fail) {
        errno = EIO;
        return -1;
    }
    return status;
}
