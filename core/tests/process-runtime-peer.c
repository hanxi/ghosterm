#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "base/sha256.h"
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL peer:%d %s\n", __LINE__, #x);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void exact(int fd, void *buffer, size_t n, int writing) {
    size_t at = 0;
    while (at < n) {
        ssize_t count = writing ? write(fd, (char *)buffer + at, n - at)
                                : read(fd, (char *)buffer + at, n - at);
        CHECK(count > 0);
        at += (size_t)count;
    }
}

static void call(uint64_t *sequence, const char *fields, char reply[8193]) {
    char text[4096];
    int n = snprintf(text,
                     sizeof(text),
                     "{\"v\":1,\"id\":\"%llu\",%s}",
                     (unsigned long long)++*sequence,
                     fields);
    CHECK(n > 0 && n < 4096);
    uint8_t header[4] = {0, 0, (uint8_t)((unsigned)n >> 8), (uint8_t)n};
    exact(1, header, 4, 1);
    exact(1, text, (size_t)n, 1);
    exact(0, header, 4, 0);
    size_t size = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) | ((size_t)header[2] << 8) |
                  header[3];
    CHECK(size > 0 && size <= 8192);
    exact(0, reply, size, 0);
    reply[size] = 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 4 && !strcmp(argv[1], "--ghosterm-startup"));
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 262144);
    char *text = malloc((size_t)st.st_size + 1);
    CHECK(text);
    exact(fd, text, (size_t)st.st_size, 0);
    text[st.st_size] = 0;
    CHECK(!close(fd));
    gt_sha256_t hash;
    char digest[65];
    gt__sha256_init(&hash);
    CHECK(gt__sha256_update(&hash, text, (size_t)st.st_size) && gt__sha256_final(&hash, digest) &&
          !strcmp(digest, argv[3]));
    CHECK(strstr(text, "\"instanceId\"") && strstr(text, "\"decodedArguments\""));
    free(text);
    const char *mode = getenv("GT_RUNTIME_MODE");
    if (mode && !strcmp(mode, "stderr-eof")) {
        CHECK(!close(STDERR_FILENO));
    }
    if (mode && !strcmp(mode, "stdout-eof")) {
        CHECK(!close(STDOUT_FILENO));
        alarm(7);
        for (;;) {
            pause();
        }
    }
    if (mode && !strcmp(mode, "leader-exit")) {
        const char *marker = getenv("GT_RUNTIME_DESCENDANT_FILE");
        CHECK(marker);
        int ready[2];
        CHECK(!pipe(ready));
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            CHECK(!close(ready[0]));
            signal(SIGTERM, SIG_IGN);
            alarm(7); /* Broken-controller fixtures cannot leave a live orphan. */
            CHECK(write(ready[1], "R", 1) == 1 && !close(ready[1]));
            for (;;) {
                pause();
            }
        }
        CHECK(!close(ready[1]));
        char byte;
        CHECK(read(ready[0], &byte, 1) == 1 && !close(ready[0]));
        int file = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        CHECK(file >= 0 && write(file, &child, sizeof(child)) == sizeof(child) && !close(file));
        return 0;
    }
    if (mode && !strcmp(mode, "stubborn")) {
        signal(SIGTERM, SIG_IGN);
    }
    uint64_t sequence = 0;
    char reply[8193];
    call(&sequence, "\"op\":\"activated\"", reply);
    CHECK(strstr(reply, "\"status\":\"ok\""));
    for (;;) {
        call(&sequence, "\"op\":\"take\",\"methods\":[\"fixture.echo\"]", reply);
        const char *ticket = strstr(reply, "\"ticket\":\"");
        if (ticket) {
            char fields[512];
            unsigned long long id = strtoull(ticket + 10, NULL, 10);
            CHECK(id && snprintf(fields,
                                 sizeof(fields),
                                 "\"op\":\"respond\",\"ref\":\"%llu\",\"outcome\":{\"kind\":"
                                 "\"success\",\"value\":{\"peer\":true}}",
                                 id) > 0);
            call(&sequence, fields, reply);
            CHECK(strstr(reply, "\"status\":\"ok\""));
        }
    }
}
