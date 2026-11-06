#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/package-snapshot.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d %s\n", mode, __LINE__, #x);                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    size_t calls, fail, live, bytes;
    bool wiped;
    int fd;
    const char *race;
    unsigned char *last_data;
} memory_t;

static void *allocate(void *ctx, size_t n) {
    memory_t *m = ctx;
    if (++m->calls == m->fail) {
        return NULL;
    }
    if (m->race && n == 12) {
        const char *race = m->race;
        m->race = NULL;
        if (!strcmp(race, "symlink")) {
            if (unlinkat(m->fd, "payload.bin", 0) || symlinkat("target", m->fd, "payload.bin")) {
                abort();
            }
        } else if (!strcmp(race, "fifo")) {
            if (unlinkat(m->fd, "payload.bin", 0) || mkfifoat(m->fd, "payload.bin", 0600)) {
                abort();
            }
        } else if (!strcmp(race, "replacement")) {
            if (renameat(m->fd, "target", m->fd, "payload.bin")) {
                abort();
            }
        }
    }
    void *p = malloc(n);
    if (p) {
        memset(p, 0xa5, n);
        m->live++;
        m->bytes += n;
        if (n == 3) {
            m->last_data = p;
        }
    }
    return p;
}

static void release(void *ctx, void *p, size_t n) {
    memory_t *m = ctx;
    unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i]) {
            m->wiped = false;
        }
    }
    m->live--;
    m->bytes -= n;
    free(p);
}

static int put(int fd, const char *name, const void *data, size_t n) {
    int f = openat(fd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (f < 0) {
        return -1;
    }
    const unsigned char *p = data;
    while (n) {
        ssize_t w = write(f, p, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            close(f);
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return close(f);
}

static int clear(int fd) {
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) {
        return -1;
    }
    DIR *d = fdopendir(copy);
    if (!d) {
        close(copy);
        return -1;
    }
    struct dirent *e;
    int status = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            status = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                status = -1;
                break;
            }
            int result = clear(child);
            close(child);
            if (result || unlinkat(fd, e->d_name, AT_REMOVEDIR)) {
                status = -1;
                break;
            }
        } else if (unlinkat(fd, e->d_name, 0)) {
            status = -1;
            break;
        }
    }
    if (closedir(d)) {
        status = -1;
    }
    return status;
}

static gt_package_status_t finish(gt_package_snapshot_t *s) {
    for (size_t i = 0; i < 50000; i++) {
        gt_package_status_t status = gt__package_capture_step(s);
        if (status != GT_PACKAGE_WAIT) {
            return status;
        }
    }
    return GT_PACKAGE_WAIT;
}

static size_t fd_count(void) {
    size_t n = 0;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) >= 0) {
            n++;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "invalid";
    CHECK(argc == 3);
    char root[4096];
    CHECK(snprintf(root, sizeof(root), "%s/package-snapshot-XXXXXX", argv[2]) > 0);
    CHECK(mkdtemp(root));
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    size_t fds = fd_count();
    bool existing[1024];
    for (int i = 0; i < 1024; i++) {
        existing[i] = fcntl(i, F_GETFD) >= 0;
    }
    memory_t m = {.wiped = true, .fd = fd};
    gt_allocator_t a = {&m, allocate, release};
    gt_package_snapshot_t *s = NULL;
    gt_package_capture_config_t c = {fd,
                                     GT_PACKAGE_MEMBERS_MAX,
                                     GT_PACKAGE_ENTRIES_MAX,
                                     GT_PACKAGE_MEMBER_BYTES_MAX,
                                     GT_PACKAGE_TOTAL_BYTES_MAX};
    gt_package_status_t expected = GT_PACKAGE_OK;
    bool special = false;
    if (!strcmp(mode, "roundtrip") || !strcmp(mode, "frozen") ||
        !strcmp(mode, "directory-offset")) {
        CHECK(put(fd, "z.js", "hello", 5) == 0);
        CHECK(put(fd, "empty", NULL, 0) == 0);
        CHECK(mkdirat(fd, "client", 0700) == 0);
        int child = openat(fd, "client", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(child >= 0);
        CHECK(put(child, "index.js", "abc", 3) == 0);
        CHECK(close(child) == 0);
    } else if (!strcmp(mode, "member-limit")) {
        CHECK(put(fd, "a", "x", 1) == 0 && put(fd, "b", "y", 1) == 0);
        c.members = 1;
        expected = GT_PACKAGE_LIMIT;
    } else if (!strcmp(mode, "max-members")) {
        for (size_t i = 0; i < GT_PACKAGE_MEMBERS_MAX; i++) {
            char name[32];
            snprintf(name, sizeof(name), "member-%04zu", i);
            CHECK(put(fd, name, NULL, 0) == 0);
        }
    } else if (!strcmp(mode, "max-entries")) {
        for (size_t i = 0; i < GT_PACKAGE_ENTRIES_MAX; i++) {
            char name[32];
            snprintf(name, sizeof(name), "dir-%04zu", i);
            CHECK(mkdirat(fd, name, 0700) == 0);
        }
    } else if (!strcmp(mode, "max-bytes")) {
        for (size_t i = 0; i < 4; i++) {
            char name[8];
            snprintf(name, sizeof(name), "f%zu", i);
            int file = openat(fd, name, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            CHECK(file >= 0);
            CHECK(ftruncate(file, GT_PACKAGE_MEMBER_BYTES_MAX) == 0);
            CHECK(close(file) == 0);
        }
    } else if (!strcmp(mode, "entry-limit")) {
        CHECK(mkdirat(fd, "a", 0700) == 0 && mkdirat(fd, "b", 0700) == 0);
        c.entries = 1;
        expected = GT_PACKAGE_LIMIT;
    } else if (!strcmp(mode, "entry-exact")) {
        CHECK(mkdirat(fd, "a", 0700) == 0);
        c.entries = 1;
    } else if (!strcmp(mode, "byte-limit")) {
        CHECK(put(fd, "a", "abc", 3) == 0);
        c.member_bytes = 2;
        expected = GT_PACKAGE_LIMIT;
    } else if (!strcmp(mode, "total-limit")) {
        CHECK(put(fd, "a", "abc", 3) == 0 && put(fd, "b", "def", 3) == 0);
        c.total_bytes = 5;
        expected = GT_PACKAGE_LIMIT;
    } else if (!strcmp(mode, "physical-self-count")) {
        CHECK(put(fd, "integrity.json", "123", 3) == 0 && put(fd, "signature", "123", 3) == 0);
        c.total_bytes = 5;
        expected = GT_PACKAGE_LIMIT;
    } else if (!strcmp(mode, "symlink-file")) {
        CHECK(put(fd, "target", "abc", 3) == 0 && symlinkat("target", fd, "link") == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "symlink-dir")) {
        CHECK(mkdirat(fd, "target", 0700) == 0 && symlinkat("target", fd, "link") == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "fifo")) {
        CHECK(mkfifoat(fd, "fifo", 0600) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "hardlink")) {
        CHECK(put(fd, "original", "x", 1) == 0);
        CHECK(linkat(fd, "original", fd, "alias", 0) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "socket")) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(sock >= 0);
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        CHECK(strlen(root) + 6 < sizeof(address.sun_path));
        strcpy(address.sun_path, root);
        strcat(address.sun_path, "/sock");
        CHECK(bind(sock, (struct sockaddr *)&address, sizeof(address)) == 0);
        CHECK(close(sock) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "bad-backslash")) {
        CHECK(put(fd, "bad\\name", "x", 1) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "bad-colon")) {
        CHECK(put(fd, "bad:name", "x", 1) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "bad-trailing-dot")) {
        CHECK(put(fd, "bad.", "x", 1) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "bad-trailing-space")) {
        CHECK(put(fd, "bad ", "x", 1) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "bad-control")) {
        CHECK(put(fd, "bad\x01name", "x", 1) == 0);
        expected = GT_PACKAGE_INVALID;
    } else if (!strcmp(mode, "unicode")) {
        CHECK(put(fd, "\xe6\x96\x87\xe4\xbb\xb6.js", "abc", 3) == 0);
    } else if (!strcmp(mode, "deep") || !strcmp(mode, "path-limit")) {
        int current = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(current >= 0);
        size_t depth = !strcmp(mode, "deep") ? 100 : 6;
        char component[201];
        memset(component, 'a', 200);
        component[200] = 0;
        const char *name = depth == 100 ? "a" : component;
        for (size_t i = 0; i < depth; i++) {
            CHECK(mkdirat(current, name, 0700) == 0);
            int next = openat(current, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            CHECK(next >= 0);
            CHECK(close(current) == 0);
            current = next;
        }
        CHECK(put(current, "x", "x", 1) == 0);
        CHECK(close(current) == 0);
        if (depth == 6) {
            expected = GT_PACKAGE_INVALID;
        }
    } else if (!strcmp(mode, "race-symlink") || !strcmp(mode, "race-fifo") ||
               !strcmp(mode, "race-replacement")) {
        CHECK(put(fd, "payload.bin", "abc", 3) == 0 && put(fd, "target", "xyz", 3) == 0);
        m.race = mode + 5;
        expected = GT_PACKAGE_CHANGED;
    }
#ifdef GT_PACKAGE_BOUNDARY
    else if (boundary_case(mode, fd)) {
        CHECK(put(fd, "payload.bin", "abc", 3) == 0);
        expected = boundary_expected(mode);
    }
#endif
    else if (!strcmp(mode, "allocation")) {
        CHECK(put(fd, "payload.bin", "abc", 3) == 0);
        special = true;
        for (size_t fail = 1; fail <= 4; fail++) {
            m.calls = 0;
            m.fail = fail;
            gt_package_status_t status = gt__package_capture_open(&c, &a, &s);
            if (status == GT_PACKAGE_OK) {
                status = finish(s);
                CHECK(gt__package_snapshot_destroy(&s) == GT_PACKAGE_OK);
            }
            CHECK(status == GT_PACKAGE_NOMEM);
            CHECK(!s && !m.live && !m.bytes && m.wiped && fd_count() == fds);
        }
        m.fail = 0;
    } else if (!strcmp(mode, "close-live")) {
        CHECK(put(fd, "payload.bin", "abc", 3) == 0);
        special = true;
        CHECK(gt__package_capture_open(&c, &a, &s) == GT_PACKAGE_OK);
        CHECK(gt__package_capture_step(s) == GT_PACKAGE_WAIT);
        CHECK(gt__package_snapshot_destroy(&s) == GT_PACKAGE_OK);
        CHECK(!m.live && m.wiped && fd_count() == fds);
    } else if (!strcmp(mode, "invalid")) {
        special = true;
        gt_package_capture_config_t bad = c;
        bad.entries = GT_PACKAGE_ENTRIES_MAX + 1;
        CHECK(gt__package_capture_open(&bad, &a, &s) == GT_PACKAGE_INVALID && !s);
        bad = c;
        bad.member_bytes = GT_PACKAGE_MEMBER_BYTES_MAX + 1;
        CHECK(gt__package_capture_open(&bad, &a, &s) == GT_PACKAGE_INVALID && !s);
        bad = c;
        bad.total_bytes = GT_PACKAGE_TOTAL_BYTES_MAX + 1;
        CHECK(gt__package_capture_open(&bad, &a, &s) == GT_PACKAGE_INVALID && !s);
        bad = c;
        bad.members = GT_PACKAGE_MEMBERS_MAX + 1;
        CHECK(gt__package_capture_open(&bad, &a, &s) == GT_PACKAGE_INVALID && !s);
        bad = c;
        bad.directory_fd = -1;
        CHECK(gt__package_capture_open(&bad, &a, &s) == GT_PACKAGE_INVALID && !s);
        gt_allocator_t invalid = {0};
        CHECK(gt__package_capture_open(&c, &invalid, &s) == GT_PACKAGE_INVALID && !s);
        CHECK(gt__package_capture_open(&c, &a, NULL) == GT_PACKAGE_INVALID);
        CHECK(gt__package_capture_step(NULL) == GT_PACKAGE_INVALID);
        CHECK(gt__package_snapshot_destroy(&s) == GT_PACKAGE_INVALID);
    } else {
        CHECK(!strcmp(mode, "empty") || !strcmp(mode, "boundaries"));
    }
    if (!special) {
        DIR *borrowed = NULL;
        long position = 0;
        if (!strcmp(mode, "directory-offset")) {
            int copy = dup(fd);
            CHECK(copy >= 0);
            borrowed = fdopendir(copy);
            CHECK(borrowed && readdir(borrowed));
            position = telldir(borrowed);
        }
        CHECK(gt__package_capture_open(&c, &a, &s) == GT_PACKAGE_OK);
        size_t count = 88, read = 88, index = 88;
        uint64_t bytes = 88;
        gt_package_member_t info;
        memset(&info, 1, sizeof(info));
        CHECK(!gt__package_snapshot_count(s, &count, &bytes) && count == 0 && bytes == 0);
        CHECK(!gt__package_snapshot_member(s, 0, &info) && info.path == NULL);
        CHECK(gt__package_snapshot_find(s, "x", &index) == GT_PACKAGE_INVALID && index == 0);
        CHECK(gt__package_snapshot_read(s, 0, 0, NULL, 0, &read) == GT_PACKAGE_INVALID &&
              read == 0);
        for (size_t step = 0; step < 16; step++) {
            gt_package_status_t status = gt__package_capture_step(s);
            for (int i = 0; i < 1024; i++) {
                int flags = fcntl(i, F_GETFD);
                CHECK(existing[i] || flags < 0 || (flags & FD_CLOEXEC));
            }
            if (status != GT_PACKAGE_WAIT) {
                break;
            }
        }
        CHECK(finish(s) == expected);
        CHECK(gt__package_capture_step(s) == expected);
        if (!strncmp(mode, "race-", 5)) {
            CHECK(m.last_data);
            for (size_t i = 0; i < 3; i++) {
                CHECK(m.last_data[i] == 0xa5);
            }
        }
        if (expected == GT_PACKAGE_OK) {
            CHECK(gt__package_snapshot_count(s, &count, &bytes));
            const char *previous = "";
            for (size_t i = 0; i < count; i++) {
                CHECK(gt__package_snapshot_member(s, i, &info));
                CHECK(strcmp(previous, info.path) < 0);
                previous = info.path;
                CHECK(gt__package_snapshot_find(s, info.path, &index) == GT_PACKAGE_OK &&
                      index == i);
            }
            if (!strcmp(mode, "roundtrip") || !strcmp(mode, "frozen") ||
                !strcmp(mode, "directory-offset")) {
                CHECK(count == 3 && bytes == 8);
                CHECK(gt__package_snapshot_find(s, "client/index.js", &index) == GT_PACKAGE_OK);
                CHECK(gt__package_snapshot_member(s, index, &info) && info.bytes == 3 &&
                      !strcmp(info.sha256,
                              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
                if (!strcmp(mode, "frozen")) {
                    CHECK(clear(fd) == 0);
                    CHECK(put(fd, "replacement.js", "evil", 4) == 0);
                }
                char out[16];
                memset(out, 0, sizeof(out));
                CHECK(gt__package_snapshot_read(s, index, 0, out, sizeof(out), &read) ==
                          GT_PACKAGE_OK &&
                      read == 3 && !memcmp(out, "abc", 3));
                CHECK(gt__package_snapshot_read(s, index, 1, out, 1, &read) == GT_PACKAGE_OK &&
                      read == 1 && out[0] == 'b');
                CHECK(gt__package_snapshot_read(s, index, 3, out, 1, &read) == GT_PACKAGE_OK &&
                      read == 0);
                CHECK(gt__package_snapshot_read(s, index, 4, out, 1, &read) == GT_PACKAGE_INVALID &&
                      read == 0);
                CHECK(
                    gt__package_snapshot_read(s, index, 0, out, GT_PACKAGE_CHUNK_MAX + 1, &read) ==
                        GT_PACKAGE_INVALID &&
                    read == 0);
                CHECK(gt__package_snapshot_find(s, "empty", &index) == GT_PACKAGE_OK);
                CHECK(gt__package_snapshot_member(s, index, &info) &&
                      !strcmp(info.sha256,
                              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
                CHECK(gt__package_snapshot_read(s, index, 0, NULL, 0, &read) == GT_PACKAGE_OK &&
                      read == 0);
            } else if (!strcmp(mode, "max-members")) {
                CHECK(count == GT_PACKAGE_MEMBERS_MAX && !bytes);
            } else if (!strcmp(mode, "max-entries")) {
                CHECK(!count && !bytes);
            } else if (!strcmp(mode, "max-bytes")) {
                CHECK(count == 4 && bytes == GT_PACKAGE_TOTAL_BYTES_MAX);
                unsigned char chunk[GT_PACKAGE_CHUNK_MAX];
                for (size_t i = 0; i < count; i++) {
                    CHECK(gt__package_snapshot_member(s, i, &info) &&
                          !strcmp(
                              info.sha256,
                              "080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e"));
                    for (uint64_t at = 0; at < GT_PACKAGE_MEMBER_BYTES_MAX; at += sizeof(chunk)) {
                        memset(chunk, 0xff, sizeof(chunk));
                        CHECK(gt__package_snapshot_read(s, i, at, chunk, sizeof(chunk), &read) ==
                                  GT_PACKAGE_OK &&
                              read == sizeof(chunk));
                        for (size_t b = 0; b < read; b++) {
                            CHECK(chunk[b] == 0);
                        }
                    }
                }
            } else if (!strcmp(mode, "empty") || !strcmp(mode, "boundaries")) {
                CHECK(!count && !bytes);
            }
            CHECK(gt__package_snapshot_find(s, "missing", &index) == GT_PACKAGE_NOT_FOUND &&
                  index == 0);
            CHECK(gt__package_snapshot_find(s, "../escape", &index) == GT_PACKAGE_INVALID);
        } else {
            CHECK(!gt__package_snapshot_count(s, &count, &bytes) && !count && !bytes);
            CHECK(!gt__package_snapshot_member(s, 0, &info));
            CHECK(gt__package_snapshot_read(s, 0, 0, NULL, 0, &read) == GT_PACKAGE_INVALID &&
                  !read);
        }
        if (borrowed) {
            CHECK(telldir(borrowed) == position);
            CHECK(closedir(borrowed) == 0);
        }
        gt_package_status_t destroyed = GT_PACKAGE_OK;
#ifdef GT_PACKAGE_BOUNDARY
        if (!strcmp(mode, "destroy-error")) {
            boundary_destroy_error = true;
            destroyed = GT_PACKAGE_IO;
        }
#endif
        CHECK(gt__package_snapshot_destroy(&s) == destroyed && !s);
        CHECK(!m.live && !m.bytes && m.wiped);
        CHECK(fd_count() == fds);
        CHECK(fcntl(fd, F_GETFD) >= 0);
    }
    CHECK(clear(fd) == 0);
    CHECK(close(fd) == 0);
    CHECK(rmdir(root) == 0);
    printf("PACKAGE SNAPSHOT PASS %s\n", mode);
    return 0;
}
