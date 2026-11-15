/* Disposable, explicitly mapped business child. No shell, network or accounts. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const void *input, size_t size) {
    const char *bytes = input;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return 0;
        }
        bytes += n;
        size -= (size_t)n;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "quoted ' \" $() 中文") || !getenv("GT_TEST_VALUE") ||
        strcmp(getenv("GT_TEST_VALUE"), "selected") || getenv("HOME") || getenv("PATH") ||
        getenv("NODE_OPTIONS")) {
        return 80;
    }
    char proof[32] = {0};
    int fd = open("sentinel", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || read(fd, proof, sizeof(proof)) != 15 || strcmp(proof, "workspace-proof")) {
        return 81;
    }
    close(fd);
    fd = open("business.pid", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    int n = snprintf(proof, sizeof(proof), "%ld", (long)getpid());
    if (fd < 0 || n < 1 || !write_all(fd, proof, (size_t)n)) {
        return 82;
    }
    close(fd);
    if (!strcmp(argv[2], "stderr")) {
        if (!write_all(STDERR_FILENO, "stderr-proof", 12)) {
            return 83;
        }
        return 7;
    }
    if (!strcmp(argv[2], "wait")) {
        if (!write_all(STDOUT_FILENO, "ready", 5)) {
            return 84;
        }
        for (;;) {
            pause();
        }
    }
    if (!strcmp(argv[2], "delayed")) {
        struct timespec delay = {1, 200000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {
        }
    } else if (strcmp(argv[2], "roundtrip")) {
        return 85;
    }
    for (;;) {
        char bytes[1024];
        ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return 86;
        }
        if (!count) {
            return 0;
        }
        if (!write_all(STDOUT_FILENO, bytes, (size_t)count)) {
            return 87;
        }
    }
}
