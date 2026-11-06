#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include "extension/resource.h"
#include "base/sha256.h"
#include "base/secure-random.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
/* Real filesystem; substitute only selected syscall outcomes to exercise
 * short IO / EINTR / errors without changing production's public surface. */
static const char *fault;
static unsigned fault_calls;

static ssize_t resource_test_write(int fd, const void *p, size_t n) {
    fault_calls++;
    if (fault && !strcmp(fault, "short-write") && n > 2) {
        n = 2;
    }
    if (fault && !strcmp(fault, "write-intr") && fault_calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (fault && !strcmp(fault, "write-error")) {
        errno = ENOSPC;
        return -1;
    }
    if (fault && !strcmp(fault, "write-zero")) {
        return 0;
    }
    return write(fd, p, n);
}

static ssize_t resource_test_pread(int fd, void *p, size_t n, off_t offset) {
    fault_calls++;
    if (fault && !strcmp(fault, "read-intr") && fault_calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (fault && !strcmp(fault, "read-error")) {
        errno = EIO;
        return -1;
    }
    if (fault && !strcmp(fault, "read-zero")) {
        return 0;
    }
    if (fault && !strcmp(fault, "short-read") && n > 2) {
        n = 2;
    }
    return pread(fd, p, n, offset);
}

static int resource_test_unlinkat(int fd, const char *name, int flags) {
    if (fault && !strcmp(fault, "unlink-error")) {
        errno = EACCES;
        return -1;
    }
    return unlinkat(fd, name, flags);
}

static gt_status_t resource_test_random(uint8_t *out, size_t n) {
    if (fault && !strcmp(fault, "entropy-error")) {
        memset(out, 0, n);
        return GT_ERR_UNSUPPORTED;
    }
    return gt__secure_random(out, n);
}

#define write resource_test_write
#define pread resource_test_pread
#define unlinkat resource_test_unlinkat
#define gt__secure_random resource_test_random
#include "../src/extension/resource.c"
#undef write
#undef pread
#undef unlinkat
#undef gt__secure_random

typedef struct {
    size_t live, calls, bytes;
    bool fail;
} resource_test_budget_t;

static void *resource_test_allocate(void *context, size_t n) {
    resource_test_budget_t *b = context;
    b->calls++;
    if (b->fail) {
        return NULL;
    }
    b->live++;
    b->bytes = n;
    return malloc(n);
}

static void resource_test_free(void *context, void *p, size_t n) {
    resource_test_budget_t *b = context;
    CHECK(b->live == 1 && b->bytes == n);
    for (size_t i = 0; i < n; i++) {
        CHECK(!((uint8_t *)p)[i]);
    }
    b->live--;
    free(p);
}

typedef struct {
    char path[4096];
    int directory;
    gt_bridge_resource_store_t *store;
    gt_bridge_lease_t lease;
    uint64_t owner;
    resource_test_budget_t budget;
} resource_test_fixture_t;

static const char abc_digest[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static const char empty_digest[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

static unsigned resource_test_fds(void) {
    unsigned n = 0;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) != -1) {
            n++;
        }
    }
    return n;
}

static size_t resource_test_entries(int fd) {
    DIR *d = fdopendir(openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    CHECK(d);
    size_t count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            count++;
        }
    }
    CHECK(!closedir(d));
    return count;
}

static gt_bridge_lease_t resource_test_lease(void) {
    gt_bridge_lease_t l = {0};
    memset(l.connection, 'a', 32);
    memset(l.instance, 'b', 32);
    memset(l.token, 'c', 32);
    l.epoch = 1;
    return l;
}

static void resource_test_open(
    resource_test_fixture_t *f, const char *root, uint64_t single, uint64_t total, size_t slots) {
    memset(f, 0, sizeof(*f));
    CHECK(snprintf(f->path, sizeof(f->path), "%s/plugin-resource-unit-XXXXXX", root) > 0 &&
          mkdtemp(f->path));
    f->directory = open(f->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(f->directory >= 0);
    gt_bridge_resource_config_t cfg = {f->directory, total, single, slots};
    gt_allocator_t a = {&f->budget, resource_test_allocate, resource_test_free};
    CHECK(gt__bridge_resource_open(&cfg, &a, &f->store) == GT_BRIDGE_RESOURCE_OK && f->store &&
          f->budget.live == 1);
    f->lease = resource_test_lease();
    CHECK(gt__bridge_resource_bind(f->store, &f->lease, total, &f->owner) ==
              GT_BRIDGE_RESOURCE_OK &&
          f->owner);
}

static void resource_test_close(resource_test_fixture_t *f) {
    CHECK(gt__bridge_resource_seal(f->store, f->owner, &f->lease) == GT_BRIDGE_RESOURCE_OK);
    CHECK(gt__bridge_resource_unbind(f->store, f->owner, &f->lease) == GT_BRIDGE_RESOURCE_OK);
    CHECK(gt__bridge_resource_destroy(f->store) == GT_BRIDGE_RESOURCE_OK && f->budget.live == 0);
    CHECK(!resource_test_entries(f->directory));
    CHECK(!close(f->directory) && !rmdir(f->path));
}

static gt_bridge_resource_descriptor_t
resource_test_descriptor(uint64_t generation, bool known, uint64_t size, const char *digest) {
    gt_bridge_resource_descriptor_t d = {0};
    memset(d.resource_id, 'd', 32);
    d.generation = generation;
    d.size_known = known;
    d.size = size;
    memcpy(d.sha256, digest, 65);
    strcpy(d.mime, "text/plain");
    strcpy(d.display_name, "../展示/abc.txt");
    return d;
}

static gt_bridge_resource_ref_t resource_test_begin(resource_test_fixture_t *f,
                                                    uint64_t generation,
                                                    bool known,
                                                    uint64_t size,
                                                    const char *digest) {
    gt_bridge_resource_descriptor_t d = resource_test_descriptor(generation, known, size, digest);
    gt_bridge_resource_ref_t ref;
    size_t entries = resource_test_entries(f->directory);
    CHECK(gt__bridge_resource_begin(f->store, f->owner, &f->lease, &d, &ref) ==
              GT_BRIDGE_RESOURCE_OK &&
          ref.handle && ref.token[0]);
    CHECK(resource_test_entries(f->directory) == entries);
    return ref;
}

static void resource_test_write_all(resource_test_fixture_t *f,
                                    const gt_bridge_resource_ref_t *ref,
                                    const char *text) {
    size_t at = 0, n = strlen(text);
    while (at < n) {
        size_t accepted = 0;
        gt_bridge_resource_status_t status = gt__bridge_resource_write(
            f->store, f->owner, &f->lease, ref, at, text + at, n - at, &accepted);
        CHECK(status == GT_BRIDGE_RESOURCE_OK || status == GT_BRIDGE_RESOURCE_WAIT);
        if (status == GT_BRIDGE_RESOURCE_WAIT) {
            CHECK(!accepted);
        } else {
            CHECK(accepted && accepted <= n - at);
        }
        at += accepted;
    }
}

static gt_bridge_resource_info_t resource_test_info(resource_test_fixture_t *f,
                                                    const gt_bridge_resource_ref_t *ref) {
    gt_bridge_resource_info_t i;
    CHECK(gt__bridge_resource_info(f->store, f->owner, &f->lease, ref, &i) ==
          GT_BRIDGE_RESOURCE_OK);
    return i;
}

static gt_bridge_resource_usage_t resource_test_usage(resource_test_fixture_t *f) {
    gt_bridge_resource_usage_t u;
    CHECK(gt__bridge_resource_usage(f->store, &u));
    return u;
}

static void resource_test_release(resource_test_fixture_t *f, const gt_bridge_resource_ref_t *ref) {
    CHECK(gt__bridge_resource_release(f->store, f->owner, &f->lease, ref) == GT_BRIDGE_RESOURCE_OK);
}

static void resource_test_publish(resource_test_fixture_t *f, const gt_bridge_resource_ref_t *ref) {
    resource_test_write_all(f, ref, "abc");
    CHECK(gt__bridge_resource_finish(f->store, f->owner, &f->lease, ref, true, 3, abc_digest) ==
          GT_BRIDGE_RESOURCE_OK);
}

static void resource_test_storage(const char *mode, const char *root) {
    resource_test_fixture_t f;
    resource_test_open(&f, root, 8, 16, !strcmp(mode, "quota") ? 2 : 8);
    if (!strcmp(mode, "roundtrip") || !strcmp(mode, "partial") || !strcmp(mode, "unknown") ||
        !strcmp(mode, "pins")) {
        bool unknown = !strcmp(mode, "unknown");
        gt_bridge_resource_ref_t ref =
            resource_test_begin(&f, 1, !unknown, unknown ? 0 : 3, abc_digest);
        CHECK(resource_test_info(&f, &ref).reserved == (unknown ? 8 : 3));
        char out[10] = {0};
        size_t n = 9;
        CHECK(gt__bridge_resource_read(f.store, f.owner, &f.lease, &ref, 0, out, sizeof(out), &n) ==
                  GT_BRIDGE_RESOURCE_CLOSED &&
              !n);
        CHECK(gt__bridge_resource_retain(f.store, f.owner, &f.lease, &ref) ==
              GT_BRIDGE_RESOURCE_CLOSED);
        CHECK(gt__bridge_resource_destroy(f.store) == GT_BRIDGE_RESOURCE_WAIT);
        if (!strcmp(mode, "partial")) {
            fault = "short-write";
            fault_calls = 0;
        }
        resource_test_publish(&f, &ref);
        fault = NULL;
        gt_bridge_resource_info_t info = resource_test_info(&f, &ref);
        CHECK(info.published && info.received == 3 && info.reserved == 3 &&
              info.descriptor.size_known && info.descriptor.size == 3 &&
              !strcmp(info.descriptor.display_name, "../展示/abc.txt"));
        CHECK(gt__bridge_resource_finish(f.store, f.owner, &f.lease, &ref, true, 3, abc_digest) ==
              GT_BRIDGE_RESOURCE_OK);
        CHECK(gt__bridge_resource_finish(f.store, f.owner, &f.lease, &ref, true, 2, abc_digest) ==
                  GT_BRIDGE_RESOURCE_PROTOCOL &&
              resource_test_info(&f, &ref).published);
        CHECK(gt__bridge_resource_read(f.store, f.owner, &f.lease, &ref, 1, out, sizeof(out), &n) ==
                  GT_BRIDGE_RESOURCE_OK &&
              n == 2 && !memcmp(out, "bc", 2));
        CHECK(gt__bridge_resource_read(f.store, f.owner, &f.lease, &ref, 3, out, sizeof(out), &n) ==
                  GT_BRIDGE_RESOURCE_OK &&
              !n);
        CHECK(gt__bridge_resource_read(
                  f.store, f.owner, &f.lease, &ref, UINT64_MAX, out, sizeof(out), &n) ==
                  GT_BRIDGE_RESOURCE_INVALID &&
              !n);
        resource_item_t *item = resource_item(f.store, f.owner, &ref);
        CHECK(item && item->writer == -1 &&
              (fcntl(item->reader, F_GETFL) & O_ACCMODE) == O_RDONLY &&
              (fcntl(item->reader, F_GETFD) & FD_CLOEXEC));
        struct stat st;
        CHECK(!fstat(item->reader, &st) && !st.st_nlink && (st.st_mode & 0777) == 0400);
        if (!strcmp(mode, "pins")) {
            CHECK(gt__bridge_resource_retain(f.store, f.owner, &f.lease, &ref) ==
                      GT_BRIDGE_RESOURCE_OK &&
                  resource_test_info(&f, &ref).references == 2);
            resource_test_release(&f, &ref);
            CHECK(resource_test_usage(&f).reserved_bytes == 3 &&
                  resource_test_info(&f, &ref).references == 1);
            CHECK(gt__bridge_resource_seal(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_OK);
            CHECK(gt__bridge_resource_unbind(f.store, f.owner, &f.lease) ==
                  GT_BRIDGE_RESOURCE_WAIT);
            CHECK(gt__bridge_resource_read(
                      f.store, f.owner, &f.lease, &ref, 0, out, sizeof(out), &n) ==
                      GT_BRIDGE_RESOURCE_OK &&
                  n == 3 && !memcmp(out, "abc", 3));
        }
        resource_test_release(&f, &ref);
        CHECK(!resource_test_usage(&f).reserved_bytes && !resource_test_usage(&f).resources);
        CHECK(gt__bridge_resource_info(f.store, f.owner, &f.lease, &ref, &info) ==
                  GT_BRIDGE_RESOURCE_NOT_FOUND &&
              !info.received);
    } else if (!strcmp(mode, "integrity")) {
        for (unsigned i = 0; i < 5; i++) {
            gt_bridge_resource_ref_t ref =
                resource_test_begin(&f, i + 1, true, 3, i == 3 ? empty_digest : abc_digest);
            resource_test_write_all(&f, &ref, i == 0 ? "abd" : "abc");
            CHECK(gt__bridge_resource_finish(f.store,
                                             f.owner,
                                             &f.lease,
                                             &ref,
                                             i != 1,
                                             i == 2 ? 2 : 3,
                                             i == 4 ? "invalid" : abc_digest) ==
                  GT_BRIDGE_RESOURCE_PROTOCOL);
            CHECK(!resource_test_usage(&f).resources && !resource_test_usage(&f).reserved_bytes);
            gt_bridge_resource_info_t info;
            CHECK(gt__bridge_resource_info(f.store, f.owner, &f.lease, &ref, &info) ==
                  GT_BRIDGE_RESOURCE_NOT_FOUND);
        }
        gt_bridge_resource_ref_t ref = resource_test_begin(&f, 6, true, 3, abc_digest);
        size_t accepted = 0;
        CHECK(gt__bridge_resource_write(f.store, f.owner, &f.lease, &ref, 0, "ab", 2, &accepted) ==
                  GT_BRIDGE_RESOURCE_OK &&
              accepted == 2);
        CHECK(gt__bridge_resource_finish(f.store, f.owner, &f.lease, &ref, true, 2, abc_digest) ==
                  GT_BRIDGE_RESOURCE_PROTOCOL &&
              !resource_test_usage(&f).resources);
        const char *ab = "fb8e20fc2e4c3f248c60c39bd652f3c1347298bb977b8b4d5903b85055620603";
        ref = resource_test_begin(&f, 7, true, 3, ab);
        resource_test_write_all(&f, &ref, "ab");
        CHECK(gt__bridge_resource_finish(f.store, f.owner, &f.lease, &ref, true, 2, ab) ==
                  GT_BRIDGE_RESOURCE_PROTOCOL &&
              !resource_test_usage(&f).resources);
    } else if (!strcmp(mode, "offset")) {
        for (unsigned i = 0; i < 3; i++) {
            gt_bridge_resource_ref_t ref = resource_test_begin(&f, i + 1, true, 3, abc_digest);
            size_t accepted = 9;
            CHECK(gt__bridge_resource_write(f.store,
                                            f.owner,
                                            &f.lease,
                                            &ref,
                                            i == 0 ? 1 : 0,
                                            i == 2 ? "abcd" : "abc",
                                            i == 2 ? 4 : 3,
                                            &accepted) == (i == 0   ? GT_BRIDGE_RESOURCE_PROTOCOL
                                                           : i == 2 ? GT_BRIDGE_RESOURCE_LIMIT
                                                                    : GT_BRIDGE_RESOURCE_OK));
            if (i == 1) {
                CHECK(gt__bridge_resource_write(
                          f.store, f.owner, &f.lease, &ref, 0, "abc", 3, &accepted) ==
                      GT_BRIDGE_RESOURCE_PROTOCOL);
            }
            CHECK(!accepted && !resource_test_usage(&f).resources);
        }
    } else if (!strcmp(mode, "budget") || !strcmp(mode, "quota")) {
        gt_bridge_resource_ref_t first = resource_test_begin(&f, 1, false, 0, abc_digest);
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(2, false, 0, abc_digest);
        d.resource_id[0] = 'e';
        gt_bridge_resource_ref_t second, third;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &second) ==
              GT_BRIDGE_RESOURCE_OK);
        d.generation = 3;
        d.resource_id[0] = 'f';
        d.size_known = true;
        d.size = !strcmp(mode, "quota") ? 0 : 1;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &third) ==
                  GT_BRIDGE_RESOURCE_WAIT &&
              !third.handle);
        CHECK(resource_test_usage(&f).reserved_bytes == 16 &&
              resource_test_usage(&f).resources == 2);
        resource_test_publish(&f, &first);
        CHECK(resource_test_usage(&f).reserved_bytes == 11);
        resource_test_release(&f, &second);
        CHECK(resource_test_usage(&f).reserved_bytes == 3);
        resource_test_release(&f, &first);
        d = resource_test_descriptor(3, true, 9, abc_digest);
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &third) ==
              GT_BRIDGE_RESOURCE_LIMIT);
        d = resource_test_descriptor(3, true, 0, empty_digest);
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &third) ==
              GT_BRIDGE_RESOURCE_OK);
        CHECK(
            gt__bridge_resource_finish(f.store, f.owner, &f.lease, &third, true, 0, empty_digest) ==
            GT_BRIDGE_RESOURCE_OK);
        resource_test_release(&f, &third);
        d.generation = 1;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &third) ==
              GT_BRIDGE_RESOURCE_PROTOCOL);
    } else if (!strcmp(mode, "scope")) {
        gt_bridge_resource_ref_t ref = resource_test_begin(&f, 1, true, 3, abc_digest);
        resource_test_publish(&f, &ref);
        for (unsigned i = 0; i < 4; i++) {
            gt_bridge_lease_t bad = f.lease;
            if (i == 0) {
                bad.epoch++;
            }
            if (i == 1) {
                bad.connection[0] = 'f';
            }
            if (i == 2) {
                bad.instance[0] = 'e';
            }
            if (i == 3) {
                bad.token[0] = 'd';
            }
            char out[3];
            size_t n = 99;
            gt_bridge_resource_info_t info;
            CHECK(gt__bridge_resource_read(f.store, f.owner, &bad, &ref, 0, out, 3, &n) ==
                      GT_BRIDGE_RESOURCE_DENIED &&
                  !n);
            CHECK(gt__bridge_resource_info(f.store, f.owner, &bad, &ref, &info) ==
                  GT_BRIDGE_RESOURCE_DENIED);
            CHECK(gt__bridge_resource_release(f.store, f.owner, &bad, &ref) ==
                  GT_BRIDGE_RESOURCE_DENIED);
            CHECK(gt__bridge_resource_seal(f.store, f.owner, &bad) == GT_BRIDGE_RESOURCE_DENIED);
        }
        gt_bridge_resource_ref_t forged = ref;
        forged.token[0] = forged.token[0] == 'a' ? 'b' : 'a';
        gt_bridge_resource_info_t info;
        CHECK(gt__bridge_resource_info(f.store, f.owner, &f.lease, &forged, &info) ==
              GT_BRIDGE_RESOURCE_NOT_FOUND);
        gt_bridge_lease_t other = f.lease;
        other.instance[0] = 'e';
        uint64_t owner = 0;
        CHECK(gt__bridge_resource_bind(f.store, &other, 4, &owner) == GT_BRIDGE_RESOURCE_OK);
        CHECK(gt__bridge_resource_info(f.store, owner, &other, &ref, &info) ==
              GT_BRIDGE_RESOURCE_NOT_FOUND);
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, false, 0, abc_digest);
        gt_bridge_resource_ref_t extra;
        CHECK(gt__bridge_resource_begin(f.store, owner, &other, &d, &extra) ==
              GT_BRIDGE_RESOURCE_LIMIT);
        d.size_known = true;
        d.size = 3;
        CHECK(gt__bridge_resource_begin(f.store, owner, &other, &d, &extra) ==
              GT_BRIDGE_RESOURCE_OK);
        d.resource_id[0] = 'e';
        d.generation = 2;
        d.size = 2;
        gt_bridge_resource_ref_t blocked;
        CHECK(gt__bridge_resource_begin(f.store, owner, &other, &d, &blocked) ==
              GT_BRIDGE_RESOURCE_WAIT);
        CHECK(gt__bridge_resource_seal(f.store, owner, &other) == GT_BRIDGE_RESOURCE_OK &&
              gt__bridge_resource_unbind(f.store, owner, &other) == GT_BRIDGE_RESOURCE_OK);
        resource_test_release(&f, &ref);
    } else if (!strcmp(mode, "seal")) {
        gt_bridge_resource_ref_t ref = resource_test_begin(&f, 1, true, 3, abc_digest);
        CHECK(gt__bridge_resource_unbind(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_WAIT);
        CHECK(gt__bridge_resource_seal(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_OK &&
              !resource_test_usage(&f).reserved_bytes && !resource_test_usage(&f).resources);
        size_t n = 3;
        CHECK(gt__bridge_resource_write(f.store, f.owner, &f.lease, &ref, 0, "abc", 3, &n) ==
                  GT_BRIDGE_RESOURCE_CLOSED &&
              !n);
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(2, true, 3, abc_digest);
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &ref) ==
                  GT_BRIDGE_RESOURCE_CLOSED &&
              !ref.handle);
    } else {
        CHECK(false);
    }
    resource_test_close(&f);
}

static void resource_test_errors(const char *mode, const char *root) {
    resource_test_fixture_t f;
    resource_test_open(&f, root, 8, 16, 8);
    if (!strcmp(mode, "malformed")) {
        for (unsigned i = 0; i < 9; i++) {
            gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, true, 3, abc_digest);
            gt_bridge_resource_ref_t ref;
            if (i == 0) {
                d.resource_id[0] = '/';
            }
            if (i == 1) {
                d.generation = 0;
            }
            if (i == 2) {
                d.sha256[0] = 'G';
            }
            if (i == 3) {
                strcpy(d.mime, "Text/plain");
            }
            if (i == 4) {
                strcpy(d.display_name, "bad\nname");
            }
            if (i == 5) {
                strcpy(d.display_name, "\xc0\xaf");
            }
            if (i == 6) {
                strcpy(d.display_name, "\xed\xa0\x80");
            }
            if (i == 7) {
                d.size_known = false;
                d.size = 1;
            }
            if (i == 8) {
                memset(d.display_name, 'a', 257);
                d.display_name[257] = 0;
            }
            CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &ref) ==
                      GT_BRIDGE_RESOURCE_INVALID &&
                  !ref.handle && !resource_test_usage(&f).resources);
        }
        gt_bridge_resource_ref_t ref = resource_test_begin(&f, 1, true, 3, abc_digest), extra;
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(2, true, 3, abc_digest);
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &extra) ==
              GT_BRIDGE_RESOURCE_PROTOCOL);
        resource_test_release(&f, &ref);
        d.generation = 1;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &extra) ==
              GT_BRIDGE_RESOURCE_PROTOCOL);
    } else if (!strcmp(mode, "filesystem")) {
        CHECK((fcntl(f.store->config.directory_fd, F_GETFD) & FD_CLOEXEC));
        CHECK(!fchmod(f.directory, 0755));
        gt_bridge_resource_store_t *s = (void *)1;
        gt_bridge_resource_config_t cfg = {f.directory, 16, 8, 8};
        CHECK(gt__bridge_resource_open(&cfg, NULL, &s) == GT_BRIDGE_RESOURCE_DENIED && !s);
        CHECK(!fchmod(f.directory, 0700));
        int fd = openat(f.directory, "unrelated", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        CHECK(fd >= 0 && write(fd, "keep", 4) == 4 && !close(fd));
        gt_bridge_resource_ref_t ref = resource_test_begin(&f, 1, true, 3, abc_digest);
        resource_test_publish(&f, &ref);
        resource_test_release(&f, &ref);
        CHECK(resource_test_entries(f.directory) == 1);
        CHECK(!unlinkat(f.directory, "unrelated", 0));
        int borrowed = dup(f.directory);
        CHECK(borrowed >= 0);
        cfg.directory_fd = borrowed;
        CHECK(gt__bridge_resource_open(&cfg, NULL, &s) == GT_BRIDGE_RESOURCE_OK &&
              !close(borrowed));
        CHECK(gt__bridge_resource_destroy(s) == GT_BRIDGE_RESOURCE_OK);
    } else if (!strcmp(mode, "allocation")) {
        gt_bridge_resource_store_t *s = (void *)1;
        gt_bridge_resource_config_t cfg = {f.directory, 16, 8, 8};
        resource_test_budget_t b = {0, 0, 0, true};
        gt_allocator_t a = {&b, resource_test_allocate, resource_test_free};
        CHECK(gt__bridge_resource_open(&cfg, &a, &s) == GT_BRIDGE_RESOURCE_IO && !s && !b.live &&
              b.calls == 1);
        fault = "entropy-error";
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, true, 3, abc_digest);
        gt_bridge_resource_ref_t ref;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &ref) ==
                  GT_BRIDGE_RESOURCE_UNSUPPORTED &&
              !ref.handle && !resource_test_usage(&f).resources);
        fault = NULL;
        size_t before = f.budget.calls;
        ref = resource_test_begin(&f, 1, true, 3, abc_digest);
        resource_test_publish(&f, &ref);
        CHECK(f.budget.calls == before);
        resource_test_release(&f, &ref);
    } else if (!strcmp(mode, "io-write")) {
        const char *faults[] = {"write-error", "write-zero", "write-intr"};
        for (size_t i = 0; i < 3; i++) {
            gt_bridge_resource_ref_t ref = resource_test_begin(&f, i + 1, true, 3, abc_digest);
            fault = faults[i];
            fault_calls = 0;
            size_t n = 9;
            gt_bridge_resource_status_t status =
                gt__bridge_resource_write(f.store, f.owner, &f.lease, &ref, 0, "abc", 3, &n);
            CHECK(!n && status == (i == 2 ? GT_BRIDGE_RESOURCE_WAIT : GT_BRIDGE_RESOURCE_IO));
            fault = NULL;
            if (i == 2) {
                CHECK(!resource_test_info(&f, &ref).received);
                resource_test_publish(&f, &ref);
                resource_test_release(&f, &ref);
            } else {
                CHECK(!resource_test_usage(&f).resources);
            }
        }
    } else if (!strcmp(mode, "io-read")) {
        gt_bridge_resource_ref_t ref = resource_test_begin(&f, 1, true, 3, abc_digest);
        resource_test_publish(&f, &ref);
        const char *faults[] = {"read-error", "read-zero", "read-intr", "short-read"};
        char data[4];
        for (size_t i = 0; i < 4; i++) {
            fault = faults[i];
            fault_calls = 0;
            size_t n = 9;
            gt_bridge_resource_status_t status =
                gt__bridge_resource_read(f.store, f.owner, &f.lease, &ref, 0, data, 3, &n);
            CHECK(status == (i < 2    ? GT_BRIDGE_RESOURCE_IO
                             : i == 2 ? GT_BRIDGE_RESOURCE_WAIT
                                      : GT_BRIDGE_RESOURCE_OK) &&
                  n == (i == 3 ? 2 : 0));
            CHECK(resource_test_info(&f, &ref).published);
            fault = NULL;
        }
        resource_test_release(&f, &ref);
    } else if (!strcmp(mode, "io-cleanup") || !strcmp(mode, "cleanup-race")) {
        fault = "unlink-error";
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, true, 3, abc_digest);
        gt_bridge_resource_ref_t ref;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &ref) ==
                  GT_BRIDGE_RESOURCE_IO &&
              !ref.handle);
        CHECK(resource_test_usage(&f).reserved_bytes == 3 &&
              resource_test_usage(&f).resources == 1 && resource_test_entries(f.directory) == 1);
        CHECK(gt__bridge_resource_seal(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_IO &&
              gt__bridge_resource_unbind(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_WAIT);
        fault = NULL;
        if (!strcmp(mode, "cleanup-race")) {
            resource_item_t *item = NULL;
            for (size_t i = 0; i < f.store->config.slots; i++) {
                if (f.store->items[i].state == RESOURCE_CLEANING) {
                    item = &f.store->items[i];
                }
            }
            CHECK(item);
            CHECK(!unlinkat(f.directory, item->name, 0));
            int fd = openat(f.directory, "unrelated", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
            CHECK(fd >= 0 && write(fd, "keep", 4) == 4 && !close(fd));
            CHECK(!symlinkat("unrelated", f.directory, item->name));
            CHECK(gt__bridge_resource_seal(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_IO &&
                  resource_test_usage(&f).resources == 1 &&
                  resource_test_usage(&f).reserved_bytes == 3);
            struct stat st;
            CHECK(!fstatat(f.directory, item->name, &st, AT_SYMLINK_NOFOLLOW) &&
                  S_ISLNK(st.st_mode));
            char data[4];
            fd = openat(f.directory, "unrelated", O_RDONLY | O_CLOEXEC);
            CHECK(fd >= 0 && read(fd, data, 4) == 4 && !memcmp(data, "keep", 4) && !close(fd));
            CHECK(!unlinkat(f.directory, item->name, 0) && !unlinkat(f.directory, "unrelated", 0));
        }
        CHECK(gt__bridge_resource_seal(f.store, f.owner, &f.lease) == GT_BRIDGE_RESOURCE_OK &&
              !resource_test_usage(&f).resources && !resource_test_entries(f.directory));
    } else if (!strcmp(mode, "exhaustion")) {
        uint64_t saved = f.store->next_item;
        f.store->next_item = UINT64_MAX;
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, true, 3, abc_digest);
        gt_bridge_resource_ref_t ref;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &ref) ==
                  GT_BRIDGE_RESOURCE_UNSUPPORTED &&
              !ref.handle);
        f.store->next_item = saved;
        ref = resource_test_begin(&f, UINT64_MAX, true, 3, abc_digest);
        resource_test_publish(&f, &ref);
        resource_item_t *item = resource_item(f.store, f.owner, &ref);
        item->references = UINT32_MAX;
        CHECK(gt__bridge_resource_retain(f.store, f.owner, &f.lease, &ref) ==
              GT_BRIDGE_RESOURCE_LIMIT);
        item->references = 1;
        resource_test_release(&f, &ref);
        d.generation = UINT64_MAX;
        CHECK(gt__bridge_resource_begin(f.store, f.owner, &f.lease, &d, &ref) ==
              GT_BRIDGE_RESOURCE_PROTOCOL);
        gt_bridge_lease_t other = f.lease;
        other.instance[0] = 'e';
        f.store->next_owner = UINT64_MAX;
        uint64_t owner = 1;
        CHECK(gt__bridge_resource_bind(f.store, &other, 16, &owner) ==
                  GT_BRIDGE_RESOURCE_UNSUPPORTED &&
              !owner);
    } else {
        CHECK(false);
    }
    resource_test_close(&f);
}

static void resource_test_shared(const char *root) {
    resource_test_fixture_t f;
    resource_test_open(&f, root, 8, 16, 8);
    gt_bridge_lease_t leases[8];
    uint64_t owners[8];
    leases[0] = f.lease;
    owners[0] = f.owner;
    for (size_t i = 1; i < 8; i++) {
        leases[i] = f.lease;
        leases[i].instance[0] = (char)('0' + i);
        CHECK(gt__bridge_resource_bind(f.store, &leases[i], 16, &owners[i]) ==
              GT_BRIDGE_RESOURCE_OK);
    }
    gt_bridge_lease_t extra = f.lease;
    extra.instance[0] = 'f';
    uint64_t owner = 1;
    CHECK(gt__bridge_resource_bind(f.store, &extra, 16, &owner) == GT_BRIDGE_RESOURCE_WAIT &&
          !owner);
    CHECK(gt__bridge_resource_bind(f.store, &f.lease, 16, &owner) == GT_BRIDGE_RESOURCE_DENIED &&
          !owner);
    extra.connection[0] = 'f';
    CHECK(gt__bridge_resource_bind(f.store, &extra, 16, &owner) == GT_BRIDGE_RESOURCE_DENIED);
    extra = f.lease;
    extra.epoch++;
    CHECK(gt__bridge_resource_bind(f.store, &extra, 16, &owner) == GT_BRIDGE_RESOURCE_DENIED);
    gt_bridge_resource_ref_t refs[3];
    gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, false, 0, abc_digest);
    for (size_t i = 0; i < 2; i++) {
        CHECK(gt__bridge_resource_begin(f.store, owners[i], &leases[i], &d, &refs[i]) ==
              GT_BRIDGE_RESOURCE_OK);
    }
    d.size_known = true;
    d.size = 1;
    CHECK(gt__bridge_resource_begin(f.store, owners[2], &leases[2], &d, &refs[2]) ==
              GT_BRIDGE_RESOURCE_WAIT &&
          !refs[2].handle);
    CHECK(resource_test_usage(&f).resources == 2 && resource_test_usage(&f).scopes == 8 &&
          resource_test_usage(&f).reserved_bytes == 16);
    resource_test_publish(&f, &refs[0]);
    CHECK(gt__bridge_resource_begin(f.store, owners[2], &leases[2], &d, &refs[2]) ==
          GT_BRIDGE_RESOURCE_OK);
    CHECK(gt__bridge_resource_seal(f.store, owners[1], &leases[1]) == GT_BRIDGE_RESOURCE_OK);
    CHECK(resource_test_usage(&f).reserved_bytes == 4 &&
          resource_test_info(&f, &refs[0]).published);
    resource_test_release(&f, &refs[0]);
    for (size_t i = 1; i < 8; i++) {
        CHECK(gt__bridge_resource_seal(f.store, owners[i], &leases[i]) == GT_BRIDGE_RESOURCE_OK &&
              gt__bridge_resource_unbind(f.store, owners[i], &leases[i]) == GT_BRIDGE_RESOURCE_OK);
    }
    resource_test_close(&f);
}

static void resource_test_large(const char *root) {
    resource_test_fixture_t f;
    resource_test_open(&f, root, 262144, 524288, 8);
    const char *digest = "31a1f9dea0169551092d05e8bf4a446228c8c3eb4c9b713c66adcb7fd53c89be";
    gt_bridge_resource_ref_t ref = resource_test_begin(&f, 1, true, 262144, digest);
    uint8_t bytes[8192];
    size_t calls = f.budget.calls;
    for (size_t at = 0; at < 262144;) {
        for (size_t i = 0; i < sizeof(bytes); i++) {
            bytes[i] = (uint8_t)((at + i) % 251);
        }
        size_t n = 0;
        CHECK(gt__bridge_resource_write(
                  f.store, f.owner, &f.lease, &ref, at, bytes, sizeof(bytes), &n) ==
                  GT_BRIDGE_RESOURCE_OK &&
              n > 0);
        at += n;
    }
    CHECK(gt__bridge_resource_finish(f.store, f.owner, &f.lease, &ref, true, 262144, digest) ==
          GT_BRIDGE_RESOURCE_OK);
    for (size_t at = 0; at < 262144;) {
        size_t n = 0;
        CHECK(gt__bridge_resource_read(
                  f.store, f.owner, &f.lease, &ref, at, bytes, sizeof(bytes), &n) ==
                  GT_BRIDGE_RESOURCE_OK &&
              n > 0);
        for (size_t i = 0; i < n; i++) {
            CHECK(bytes[i] == (uint8_t)((at + i) % 251));
        }
        at += n;
    }
    CHECK(f.budget.calls == calls && resource_test_info(&f, &ref).reserved == 262144);
    resource_test_release(&f, &ref);
    resource_test_close(&f);
}

static void resource_test_hash(void) {
    gt_sha256_t h;
    char hex[65];
    gt__sha256_init(&h);
    CHECK(gt__sha256_update(&h, NULL, 0) && gt__sha256_final(&h, hex) &&
          !strcmp(hex, empty_digest));
    CHECK(!gt__sha256_update(&h, "a", 1) && !gt__sha256_final(&h, hex) && !hex[0]);
    gt__sha256_init(&h);
    CHECK(gt__sha256_update(&h, "a", 1) && gt__sha256_update(&h, "bc", 2) &&
          gt__sha256_final(&h, hex) && !strcmp(hex, abc_digest));
    gt__sha256_init(&h);
    char a[1000];
    memset(a, 'a', sizeof(a));
    for (size_t i = 0; i < 1000; i++) {
        CHECK(gt__sha256_update(&h, a, sizeof(a)));
    }
    CHECK(gt__sha256_final(&h, hex) &&
          !strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    gt__sha256_init(&h);
    h.bytes = UINT64_MAX / 8;
    CHECK(!gt__sha256_update(&h, "a", 1) && h.bytes == UINT64_MAX / 8);
}

static void resource_test_hash_stdin(void) {
    gt_sha256_t h;
    gt__sha256_init(&h);
    uint8_t data[997];
    size_t n;
    while ((n = fread(data, 1, sizeof(data), stdin))) {
        CHECK(gt__sha256_update(&h, data, n));
    }
    CHECK(!ferror(stdin));
    char hex[65];
    CHECK(gt__sha256_final(&h, hex));
    puts(hex);
}

static void resource_test_crash(const char *root) {
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/plugin-resource-crash-XXXXXX", root) > 0 &&
          mkdtemp(path));
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        gt_bridge_resource_config_t cfg = {fd, 16, 8, 8};
        gt_bridge_resource_store_t *s;
        CHECK(gt__bridge_resource_open(&cfg, NULL, &s) == GT_BRIDGE_RESOURCE_OK);
        gt_bridge_lease_t l = resource_test_lease();
        uint64_t owner;
        CHECK(gt__bridge_resource_bind(s, &l, 16, &owner) == GT_BRIDGE_RESOURCE_OK);
        gt_bridge_resource_descriptor_t d = resource_test_descriptor(1, true, 3, abc_digest);
        gt_bridge_resource_ref_t ref;
        CHECK(gt__bridge_resource_begin(s, owner, &l, &d, &ref) == GT_BRIDGE_RESOURCE_OK);
        size_t n;
        CHECK(gt__bridge_resource_write(s, owner, &l, &ref, 0, "ab", 2, &n) ==
                  GT_BRIDGE_RESOURCE_OK &&
              n == 2);
        _exit(23);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 23 &&
          !resource_test_entries(fd));
    CHECK(!close(fd) && !rmdir(path));
}

int main(int argc, char **argv) {
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "--hash")) {
        resource_test_hash_stdin();
        return 0;
    }
    if (!strcmp(argv[1], "hash")) {
        resource_test_hash();
        puts("PASS resource hash");
        return 0;
    }
    CHECK(argc == 3);
    unsigned fds = resource_test_fds();
    if (!strcmp(argv[1], "crash")) {
        resource_test_crash(argv[2]);
    } else if (!strcmp(argv[1], "shared")) {
        resource_test_shared(argv[2]);
    } else if (!strcmp(argv[1], "large")) {
        resource_test_large(argv[2]);
    } else if (!strcmp(argv[1], "malformed") || !strcmp(argv[1], "filesystem") ||
               !strcmp(argv[1], "allocation") || !strcmp(argv[1], "io-write") ||
               !strcmp(argv[1], "io-read") || !strcmp(argv[1], "io-cleanup") ||
               !strcmp(argv[1], "cleanup-race") || !strcmp(argv[1], "exhaustion")) {
        resource_test_errors(argv[1], argv[2]);
    } else {
        resource_test_storage(argv[1], argv[2]);
    }
    CHECK(fds == resource_test_fds());
    printf("PASS resource %s; exact allocator / fd restoration\n", argv[1]);
    return 0;
}
