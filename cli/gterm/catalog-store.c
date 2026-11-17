#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "catalog.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CATALOG_BYTES 1048576u
static const char data_name[] = "executor-catalog-v1", lock_name[] = "executor-catalog.lock";
static const unsigned char magic[8] = {'G', 'T', 'C', 'A', 'T', '0', '1', 0};

typedef struct {
    unsigned char *bytes;
    size_t size, at;
} buffer_t;

bool gterm_catalog_path(const char *s) {
    if (!s || s[0] != '/' || !s[1] || strlen(s) > 4096 || s[strlen(s) - 1] == '/' ||
        strchr(s, '\n') || strchr(s, '\r')) {
        return false;
    }
    for (const char *p = s + 1; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return true;
}

bool gterm_catalog_digest(const char *s) {
    if (!s || strlen(s) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static int descriptor(int fd) {
    if (fd < 0 || fd > 2) {
        return fd;
    }
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return copy;
}

/* The CLI is single-threaded here. Preserve the caller's umask for the later
 * Executor; creation never changes permissions of an existing file. */
static int private_open(int directory, const char *name, int flags) {
    mode_t previous = umask(0077);
    int fd = openat(directory, name, flags, 0600);
    int saved = errno;
    umask(previous);
    errno = saved;
    return descriptor(fd);
}

static bool same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool private_file(int fd, struct stat *s) {
    return !fstat(fd, s) && S_ISREG(s->st_mode) && s->st_uid == geteuid() &&
           (s->st_mode & 07777) == 0600 && s->st_nlink == 1;
}

static bool current(gterm_catalog_t *c) {
    struct stat s;
    return !lstat(c->path, &s) && same(&s, &c->root) && S_ISDIR(s.st_mode) &&
           s.st_uid == geteuid() && (s.st_mode & 07777) == 0700;
}

static bool original(gterm_catalog_t *c) {
    struct stat s;
    if (!fstatat(c->directory, data_name, &s, AT_SYMLINK_NOFOLLOW)) {
        return c->present && same(&s, &c->original);
    }
    return !c->present && errno == ENOENT;
}

static bool get32(buffer_t *b, uint32_t *value) {
    if (b->at > b->size || b->size - b->at < 4) {
        return false;
    }
    const unsigned char *p = b->bytes + b->at;
    b->at += 4;
    *value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return true;
}

static bool put32(buffer_t *b, uint32_t value) {
    if (b->at > b->size || b->size - b->at < 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        b->bytes[b->at++] = (unsigned char)(value >> (24 - i * 8));
    }
    return true;
}

bool gterm_catalog_utf8(const unsigned char *s, size_t size) {
    for (size_t i = 0; i < size;) {
        uint32_t c = s[i++], minimum;
        size_t count;
        if (c < 0x80) {
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            minimum = 0x80;
            c &= 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            minimum = 0x800;
            c &= 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            minimum = 0x10000;
            c &= 7;
        } else {
            return false;
        }
        if (count > size - i) {
            return false;
        }
        while (count--) {
            unsigned char part = s[i++];
            if ((part & 0xc0) != 0x80) {
                return false;
            }
            c = c << 6 | (part & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static bool get_string(buffer_t *b, char **out, size_t limit) {
    uint32_t n;
    if (!get32(b, &n) || n > limit || b->at > b->size || n > b->size - b->at ||
        memchr(b->bytes + b->at, 0, n) || !gterm_catalog_utf8(b->bytes + b->at, n)) {
        return false;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        return false;
    }
    memcpy(s, b->bytes + b->at, n);
    s[n] = 0;
    b->at += n;
    *out = s;
    return true;
}

static bool put_string(buffer_t *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!s || n > 65536 || !gterm_catalog_utf8((const unsigned char *)s, n) ||
        !put32(b, (uint32_t)n) || n > b->size - b->at) {
        return false;
    }
    memcpy(b->bytes + b->at, s, n);
    b->at += n;
    return true;
}

void gterm_catalog_entry_clear(gterm_catalog_entry_t *e) {
    free(e->id);
    free(e->version);
    free(e->digest);
    free(e->directory);
    free(e->host);
    for (uint32_t i = 0; i < e->profile_count; i++) {
        free(e->profile[i]);
    }
    free(e->profile);
    memset(e, 0, sizeof(*e));
}

bool gterm_catalog_release(gterm_catalog_t *c) {
    bool ok = true;
    if (c->lock >= 0 && close(c->lock)) {
        ok = false;
    }
    if (c->directory >= 0 && close(c->directory)) {
        ok = false;
    }
    c->lock = c->directory = -1;
    return ok;
}

bool gterm_catalog_close(gterm_catalog_t *c) {
    for (uint32_t i = 0; i < c->count; i++) {
        gterm_catalog_entry_clear(&c->entries[i]);
    }
    c->count = 0;
    return gterm_catalog_release(c);
}

static bool decode(gterm_catalog_t *c, buffer_t *b) {
    uint32_t hi, lo, count;
    if (b->size < 20 || memcmp(b->bytes, magic, 8)) {
        return false;
    }
    b->at = 8;
    if (!get32(b, &hi) || !get32(b, &lo) || !get32(b, &count) || count > GTERM_CATALOG_ENTRIES) {
        return false;
    }
    c->revision = (uint64_t)hi << 32 | lo;
    if (!c->revision) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        gterm_catalog_entry_t *e = &c->entries[c->count++];
        uint32_t args;
        if (!get_string(b, &e->id, 128) || !*e->id || !get_string(b, &e->version, 64) ||
            !*e->version || !get_string(b, &e->digest, 64) || !gterm_catalog_digest(e->digest) ||
            !get_string(b, &e->directory, 4096) || !gterm_catalog_path(e->directory) ||
            !get_string(b, &e->host, 65536) || !*e->host || !get32(b, &args) ||
            args > GTERM_CATALOG_ARGUMENTS) {
            return false;
        }
        e->profile = calloc(args ? args : 1, sizeof(char *));
        if (!e->profile) {
            return false;
        }
        size_t total = 0;
        for (uint32_t j = 0; j < args; j++) {
            if (!get_string(b, &e->profile[j], 4096)) {
                return false;
            }
            e->profile_count++;
            total += strlen(e->profile[j]) + 1;
            if (total > 32768) {
                return false;
            }
        }
        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(e->id, c->entries[j].id)) {
                return false;
            }
        }
    }
    return b->at == b->size;
}

int gterm_catalog_open(gterm_catalog_t *c, const char *path, bool writing) {
    memset(c, 0, sizeof(*c));
    c->directory = c->lock = -1;
    c->path = path;
    if (!gterm_catalog_path(path)) {
        return 2;
    }
    c->directory = descriptor(open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (c->directory < 0 || fstat(c->directory, &c->root) || c->root.st_uid != geteuid() ||
        (c->root.st_mode & 07777) != 0700 || !current(c)) {
        return 5;
    }
    c->lock = private_open(c->directory,
                           lock_name,
                           O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | (writing ? O_CREAT : 0));
    struct stat locked;
    if (c->lock < 0) {
        struct stat absent;
        return !writing && errno == ENOENT &&
                       fstatat(c->directory, data_name, &absent, AT_SYMLINK_NOFOLLOW) < 0 &&
                       errno == ENOENT
                   ? 0
                   : 5;
    }
    if (!private_file(c->lock, &locked)) {
        return 5;
    }
    struct flock lock = {.l_type = writing ? F_WRLCK : F_RDLCK, .l_whence = SEEK_SET};
    if (fcntl(c->lock, F_SETLK, &lock)) {
        return errno == EACCES || errno == EAGAIN ? 11 : 5;
    }
    struct stat name;
    if (fstatat(c->directory, lock_name, &name, AT_SYMLINK_NOFOLLOW) || !same(&name, &locked)) {
        return 5;
    }
    int fd =
        descriptor(openat(c->directory, data_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (fd < 0) {
        return errno == ENOENT ? 0 : 5;
    }
    bool ok = private_file(fd, &c->original) && c->original.st_size >= 20 &&
              c->original.st_size <= CATALOG_BYTES;
    c->present = true;
    unsigned char *bytes = ok ? malloc((size_t)c->original.st_size + 1) : NULL;
    if (!bytes) {
        ok = false;
    }
    size_t at = 0, size = ok ? (size_t)c->original.st_size : 0;
    while (ok && at <= size) {
        ssize_t n = read(fd, bytes + at, size + 1 - at);
        if (n > 0) {
            at += (size_t)n;
            if (at > size) {
                ok = false;
            }
        } else if (!n) {
            break;
        } else if (errno != EINTR) {
            ok = false;
        }
    }
    struct stat end;
    if (ok) {
        ok = at == size && !fstat(fd, &end) && same(&end, &c->original) &&
             end.st_size == c->original.st_size && original(c) && current(c);
    }
    if (close(fd)) {
        ok = false;
    }
    buffer_t b = {bytes, size, 0};
    if (ok) {
        ok = decode(c, &b);
    }
    free(bytes);
    return ok ? 0 : 5;
}

int gterm_catalog_save(gterm_catalog_t *c, uint64_t expected, volatile sig_atomic_t *cancelled) {
    if (c->revision != expected || expected == UINT64_MAX) {
        return 9;
    }
    unsigned char *bytes = malloc(CATALOG_BYTES);
    if (!bytes) {
        return 5;
    }
    memcpy(bytes, magic, 8);
    buffer_t b = {bytes, CATALOG_BYTES, 8};
    uint64_t next = expected + 1;
    bool ok = put32(&b, (uint32_t)(next >> 32)) && put32(&b, (uint32_t)next) && put32(&b, c->count);
    for (uint32_t i = 0; ok && i < c->count; i++) {
        gterm_catalog_entry_t *e = &c->entries[i];
        ok = put_string(&b, e->id) && put_string(&b, e->version) && put_string(&b, e->digest) &&
             put_string(&b, e->directory) && put_string(&b, e->host) && put32(&b, e->profile_count);
        for (uint32_t j = 0; ok && j < e->profile_count; j++) {
            ok = put_string(&b, e->profile[j]);
        }
    }
    if (!ok) {
        free(bytes);
        return 5;
    }
    char name[64] = {0};
    int fd = -1;
    for (unsigned i = 0; i < 64; i++) {
        snprintf(name, sizeof(name), ".executor-catalog-%ld-%u", (long)getpid(), i);
        fd = private_open(c->directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    struct stat identity;
    bool owned = fd >= 0 && private_file(fd, &identity);
    ok = owned;
    size_t at = 0;
    while (ok && at < b.at && !*cancelled) {
        ssize_t n = write(fd, bytes + at, b.at - at);
        if (n > 0) {
            at += (size_t)n;
        } else if (!n || errno != EINTR) {
            ok = false;
        }
    }
    free(bytes);
    ok = ok && !*cancelled && at == b.at && !fsync(fd);
    if (fd >= 0 && close(fd)) {
        ok = false;
    }
    struct stat named;
    bool same_temp = owned && !fstatat(c->directory, name, &named, AT_SYMLINK_NOFOLLOW) &&
                     same(&named, &identity);
    ok = ok && same_temp && current(c) && original(c) && !*cancelled;
    if (ok && !renameat(c->directory, name, c->directory, data_name)) {
        c->committed = true;
        c->revision = next;
        return fsync(c->directory) ? 5 : 0;
    }
    if (same_temp && unlinkat(c->directory, name, 0)) {
        return 5;
    }
    return *cancelled ? 130 : 5;
}
