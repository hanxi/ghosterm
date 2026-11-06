/* Isolated C11 stream endpoint, NOT a Tailcat/WireGuard implementation.
 * A Go reference adapter supplies the network pipe. Exact bounded candidate
 * JSON fixtures exercise both directions; no production JSON parser or ABI.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    LIMIT = 65536
};

static void check(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL C stream: %s\n", message);
        exit(1);
    }
}

static int read_all(int fd, void *buffer, size_t size) {
    unsigned char *p = buffer;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return 0;
        }
        p += (size_t)n;
        size -= (size_t)n;
    }
    return 1;
}

static void write_all(int fd, const void *buffer, size_t size) {
    const unsigned char *p = buffer;
    /* Deliberate short writes: neither endpoint may rely on packet boundaries. */
    while (size) {
        size_t part = size < 17 ? size : 17;
        ssize_t n = write(fd, p, part);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        check(n > 0, "write");
        p += (size_t)n;
        size -= (size_t)n;
    }
}

static size_t fixture(const char *dir, const char *name, unsigned char *bytes) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    check(n > 0 && (size_t)n < sizeof(path), "fixture path");
    FILE *file = fopen(path, "rb");
    check(file != NULL, "fixture open");
    size_t size = fread(bytes, 1, LIMIT + 1, file);
    check(!ferror(file) && size > 0 && size <= LIMIT, "fixture budget");
    check(fclose(file) == 0, "fixture close");
    return size;
}

static void send_fixture(int fd, const char *dir, const char *name) {
    unsigned char bytes[LIMIT + 1];
    size_t size = fixture(dir, name, bytes);
    uint32_t header = htonl((uint32_t)size);
    write_all(fd, &header, sizeof(header));
    write_all(fd, bytes, size);
}

static void expect_fixture(int fd, const char *dir, const char *name) {
    unsigned char want[LIMIT + 1], got[LIMIT];
    size_t size = fixture(dir, name, want);
    uint32_t header;
    check(read_all(fd, &header, sizeof(header)), "frame header EOF");
    uint32_t length = ntohl(header);
    check(length > 0 && length <= LIMIT, "frame budget");
    check(read_all(fd, got, length), "truncated frame");
    check(length == size && memcmp(want, got, size) == 0, "frame identity/content/order");
}

static void name_for(char *out, size_t size, const char *kind, unsigned round) {
    int n = snprintf(out, size, "%s-%u", kind, round);
    check(n > 0 && (size_t)n < size, "fixture name");
}

static int connect_tcp(const char *text) {
    char *end = NULL;
    long port = strtol(text, &end, 10);
    check(end && *end == '\0' && port > 0 && port <= 65535, "port");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "client socket");
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    check(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1, "loopback address");
    check(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0, "client connect");
    return fd;
}

static int executor(const char *path, const char *dir, const char *receipt) {
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    check(listener >= 0, "executor socket");
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    check(strlen(path) < sizeof(address.sun_path), "unix socket path");
    memcpy(address.sun_path, path, strlen(path) + 1);
    check(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0, "executor bind");
    check(listen(listener, 1) == 0, "executor listen");
    puts("READY C executor");
    fflush(stdout);
    int fd = accept(listener, NULL, NULL);
    check(fd >= 0, "executor accept");
    check(close(listener) == 0, "listener close");
    char name[80];
    for (unsigned round = 0; round < 4; ++round) {
        name_for(name, sizeof(name), "request", round);
        expect_fixture(fd, dir, name);
        name_for(name, sizeof(name), "prompt", round);
        send_fixture(fd, dir, name);
        name_for(name, sizeof(name), "approval", round);
        expect_fixture(fd, dir, name);
        name_for(name, sizeof(name), "response", round);
        send_fixture(fd, dir, name);
    }
    expect_fixture(fd, dir, "exit");
    FILE *file = fopen(receipt, "wb");
    check(file != NULL, "receipt open");
    check(fputs("received-exit-without-response\n", file) >= 0 && fclose(file) == 0,
          "receipt write");
    check(close(fd) == 0 && unlink(path) == 0, "executor close");
    return 23; /* Received request, deliberately no result: outcome is unknown. */
}

static int client(const char *port, const char *dir) {
    int fd = connect_tcp(port);
    unsigned round = 0;
    char line[80], name[80];
    puts("READY C client");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(line, "round\n") == 0) {
            check(round < 4, "round count");
            name_for(name, sizeof(name), "request", round);
            send_fixture(fd, dir, name);
            name_for(name, sizeof(name), "prompt", round);
            expect_fixture(fd, dir, name);
            name_for(name, sizeof(name), "approval", round);
            send_fixture(fd, dir, name);
            name_for(name, sizeof(name), "response", round);
            expect_fixture(fd, dir, name);
            printf("PASS C round %u\n", round++);
            fflush(stdout);
        } else if (strcmp(line, "exit\n") == 0) {
            check(round == 4, "exit after rounds");
            send_fixture(fd, dir, "exit");
            unsigned char byte;
            ssize_t n;
            do {
                n = read(fd, &byte, 1);
            } while (n < 0 && errno == EINTR);
            check(n == 0, "exit must close without response");
            check(close(fd) == 0, "client close");
            fd = -1;
            puts("PASS C outcome unknown; route closed");
            fflush(stdout);
        } else if (strcmp(line, "late\n") == 0) {
            check(fd == -1, "late request before close");
            puts("PASS C late request rejected before write");
            fflush(stdout);
        } else {
            check(0, "unknown client command");
        }
    }
    if (fd >= 0) {
        check(close(fd) == 0, "final close");
    }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 5 && strcmp(argv[1], "executor") == 0) {
        return executor(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && strcmp(argv[1], "client") == 0) {
        return client(argv[2], argv[3]);
    }
    fprintf(stderr, "usage: stream executor socket fixtures receipt | client port fixtures\n");
    return 2;
}
