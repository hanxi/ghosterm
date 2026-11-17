#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "release.h"
#include "catalog.h"
#include "stdio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#define STORE_ITEMS 256u
#define TREE_ITEMS 4096u
#define TREE_BYTES UINT64_C(67174464)
#define INTENT_BYTES 104u
static const unsigned char intent_magic[8] = {'G', 'T', 'D', 'E', 'L', '0', '1', 0};

typedef struct {
    int root, lock;
    const char *path;
    struct stat identity, locked;
} store_t;

typedef struct {
    char *path;
    struct stat identity;
    char hash[65];
} item_t;

typedef struct {
    store_t store;
    int directory;
    const char *version;
    char quarantine[80], intent[88], location[80];
    struct stat identity, intent_identity;
    unsigned char intent_bytes[INTENT_BYTES];
    bool pending, in_use, removed, quarantined;
    unsigned references, count;
    uint64_t bytes, path_bytes;
    item_t *items;
    char digest[65];
} review_t;

static bool same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool stable(const struct stat *a, const struct stat *b) {
    bool common = same(a, b) && a->st_mode == b->st_mode && a->st_uid == b->st_uid &&
                  a->st_gid == b->st_gid && a->st_nlink == b->st_nlink && a->st_size == b->st_size;
#ifdef __APPLE__
    return common && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return common && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static bool named(int parent, const char *name, const struct stat *s) {
    struct stat now;
    return !fstatat(parent, name, &now, AT_SYMLINK_NOFOLLOW) && same(&now, s);
}

static bool private_file(const struct stat *s, mode_t mode) {
    return S_ISREG(s->st_mode) && s->st_uid == geteuid() && s->st_nlink == 1 &&
           (s->st_mode & 07777) == mode;
}

static bool current(const store_t *s) {
    struct stat now;
    return !lstat(s->path, &now) && same(&now, &s->identity) && S_ISDIR(now.st_mode) &&
           now.st_uid == geteuid() && (now.st_mode & 07777) == 0700 &&
           named(s->root, ".store.lock", &s->locked);
}

static int store_open(store_t *s, const char *path, bool writing) {
    memset(s, 0, sizeof(*s));
    s->root = s->lock = -1;
    s->path = path;
    if (!gterm_catalog_path(path) || strlen(path) > 3500) {
        return 2;
    }
    s->root = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (s->root < 0 || fstat(s->root, &s->identity) || s->identity.st_uid != geteuid() ||
        (s->identity.st_mode & 07777) != 0700) {
        return 5;
    }
    s->lock = openat(s->root, ".store.lock", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (s->lock < 0 || fstat(s->lock, &s->locked) || !private_file(&s->locked, 0600) ||
        !current(s)) {
        return 5;
    }
    struct flock lock = {.l_type = writing ? F_WRLCK : F_RDLCK, .l_whence = SEEK_SET};
    if (fcntl(s->lock, F_SETLK, &lock)) {
        return errno == EACCES || errno == EAGAIN ? 11 : 5;
    }
    return current(s) ? 0 : 9;
}

static bool store_close(store_t *s) {
    bool ok = true;
    if (s->lock >= 0 && close(s->lock)) {
        ok = false;
    }
    if (s->root >= 0 && close(s->root)) {
        ok = false;
    }
    s->root = s->lock = -1;
    return ok;
}

static int directory_lock(int fd, bool exclusive) {
    if (!flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        return 0;
    }
    return errno == EWOULDBLOCK || errno == EAGAIN ? 11 : errno == ENOTSUP ? 4 : 5;
}

int gterm_release_pending(int root, const char *version) {
    char name[88];
    struct stat s;
    snprintf(name, sizeof(name), ".remove-%s.intent", version);
    if (!fstatat(root, name, &s, AT_SYMLINK_NOFOLLOW)) {
        return 11;
    }
    if (errno != ENOENT) {
        return 5;
    }
    snprintf(name, sizeof(name), ".remove-%s", version);
    if (!fstatat(root, name, &s, AT_SYMLINK_NOFOLLOW)) {
        return 11;
    }
    return errno == ENOENT ? 0 : 5;
}

/* A reference is an open directory description, not a PID or a public name.
 * The short store lock closes the pin/retirement race; the directory lock lives
 * until all corresponding local owners finish, without blocking new installs. */
int gterm_release_pin_profile(int argc, char **argv, int *reference) {
    *reference = -1;
    const char *root = NULL;
    for (int i = 0; i < argc && strcmp(argv[i], "--");) {
        int n = gterm_stdio_option_arity(argv[i]);
        if (n < 0 || n >= argc - i) {
            return 2;
        }
        if (!strcmp(argv[i], "--release-evidence")) {
            if (root) {
                return 2;
            }
            root = argv[i + 1];
        }
        i += n + 1;
    }
    if (!root) {
        return 0;
    }
    if (!gterm_release_supported()) {
        return 4;
    }
    if (!gterm_catalog_path(root)) {
        return 2;
    }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat identity, now;
    if (fd < 0) {
        return 5;
    }
    int result = 5;
    store_t store = {.root = -1, .lock = -1};
    char *parent = NULL;
    if (fstat(fd, &identity) || identity.st_uid != geteuid() || (identity.st_mode & 0022)) {
        goto done;
    }
    const char *leaf = strrchr(root, '/') + 1;
    if (gterm_catalog_digest(leaf)) {
        parent = strdup(root);
        if (!parent) {
            goto done;
        }
        parent[leaf - root - 1] = 0;
        size_t n = strlen(parent);
        char *lock_path = malloc(n + 13);
        if (!lock_path) {
            goto done;
        }
        snprintf(lock_path, n + 13, "%s/.store.lock", parent);
        bool managed = !lstat(lock_path, &now);
        int saved = errno;
        free(lock_path);
        if (!managed && saved != ENOENT) {
            goto done;
        }
        if (managed) {
            result = store_open(&store, *parent ? parent : "/", false);
            if (result) {
                goto done;
            }
            result = gterm_release_pending(store.root, leaf);
            if (result || !named(store.root, leaf, &identity)) {
                if (!result) {
                    result = 9;
                }
                goto done;
            }
        }
    }
    result = directory_lock(fd, false);
    if (!result && (lstat(root, &now) || !same(&now, &identity))) {
        result = 9;
    }
    if (!result && store.root >= 0) {
        result = gterm_release_pending(store.root, leaf);
    }
    if (!result) {
        *reference = fd;
        fd = -1;
    }
done:
    if (fd >= 0 && close(fd)) {
        result = 5;
    }
    if (!store_close(&store)) {
        result = 5;
    }
    if (result && *reference >= 0) {
        close(*reference);
        *reference = -1;
    }
    free(parent);
    return result;
}

static int hash_file(int parent,
                     const char *name,
                     const struct stat *expected,
                     char hash[65],
                     volatile sig_atomic_t *cancelled) {
    if (expected->st_size < 0 || (uint64_t)expected->st_size > 16777216) {
        return 2;
    }
    int fd = openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat before, after;
    int result = 5;
    unsigned char *bytes = NULL;
    if (fd < 0) {
        return 5;
    }
    if (fstat(fd, &before) || !stable(&before, expected) || !S_ISREG(before.st_mode) ||
        before.st_nlink != 1) {
        goto done;
    }
    size_t size = (size_t)before.st_size, at = 0;
    bytes = malloc(size + 1);
    if (!bytes) {
        goto done;
    }
    while (at <= size && !*cancelled) {
        ssize_t n = read(fd, bytes + at, size + 1 - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            goto done;
        }
        if (!n) {
            break;
        }
        at += (size_t)n;
        if (at > size) {
            result = 9;
            goto done;
        }
    }
    if (*cancelled) {
        result = 130;
        goto done;
    }
    if (at != size || fstat(fd, &after) || !stable(&before, &after) ||
        !named(parent, name, &before)) {
        result = 9;
        goto done;
    }
    result = gterm_release_hash(bytes, size, hash) ? 0 : 5;
done:
    free(bytes);
    if (close(fd)) {
        result = 5;
    }
    return result;
}

static bool segment(const char *name) {
    size_t n = strlen(name);
    if (!n || !gterm_catalog_utf8((const unsigned char *)name, n)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)name[i] < 32 || name[i] == 127 || name[i] == '\\' || name[i] == ':') {
            return false;
        }
    }
    return true;
}

static int
walk(review_t *r, int fd, const char *prefix, unsigned depth, volatile sig_atomic_t *cancelled) {
    if (depth > 34) {
        return 2;
    }
    struct stat before, after;
    if (fstat(fd, &before)) {
        return 5;
    }
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (copy < 0) {
        return 5;
    }
    DIR *stream = fdopendir(copy);
    if (!stream) {
        close(copy);
        return 5;
    }
    int result = 0;
    while (!*cancelled) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno) {
                result = 5;
            }
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        if (!segment(name) || r->count == TREE_ITEMS) {
            result = 2;
            break;
        }
        size_t length = strlen(prefix) + strlen(name) + (*prefix ? 1 : 0);
        if (length > 4096 || r->path_bytes + length + 1 > 16777216) {
            result = 2;
            break;
        }
        item_t *item = &r->items[r->count++];
        item->path = malloc(length + 1);
        if (!item->path) {
            result = 5;
            break;
        }
        snprintf(item->path, length + 1, "%s%s%s", prefix, *prefix ? "/" : "", name);
        r->path_bytes += length + 1;
        struct stat *s = &item->identity;
        if (fstatat(fd, name, s, AT_SYMLINK_NOFOLLOW)) {
            result = 5;
            break;
        }
        mode_t mode = s->st_mode & 07777;
        if (s->st_uid != geteuid() || s->st_dev != r->identity.st_dev) {
            result = 6;
            break;
        }
        if (S_ISDIR(s->st_mode)) {
            if (mode != 0500 && !(r->pending && mode == 0700)) {
                result = 6;
                break;
            }
            int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat actual;
            if (child < 0) {
                result = 5;
                break;
            }
            result = fstat(child, &actual) || !stable(s, &actual)
                         ? 9
                         : walk(r, child, item->path, depth + 1, cancelled);
            if (close(child)) {
                result = 5;
            }
        } else if (S_ISREG(s->st_mode) && s->st_nlink == 1 && (mode == 0400 || mode == 0500)) {
            if (s->st_size < 0 || (uint64_t)s->st_size > TREE_BYTES - r->bytes) {
                result = 2;
                break;
            }
            r->bytes += (uint64_t)s->st_size;
            result = hash_file(fd, name, s, item->hash, cancelled);
        } else {
            result = 6;
        }
        if (result) {
            break;
        }
    }
    if (closedir(stream)) {
        result = 5;
    }
    if (!result && (fstat(fd, &after) || !stable(&before, &after))) {
        result = 9;
    }
    return *cancelled ? 130 : result;
}

static int compare(const void *a, const void *b) {
    return strcmp(((const item_t *)a)->path, ((const item_t *)b)->path);
}

static item_t *find(review_t *r, const char *path) {
    for (unsigned i = 0; i < r->count; i++) {
        if (!strcmp(r->items[i].path, path)) {
            return &r->items[i];
        }
    }
    return NULL;
}

static void encode64(unsigned char *out, uint64_t n) {
    for (unsigned i = 0; i < 8; i++) {
        out[i] = (unsigned char)(n >> (56 - 8 * i));
    }
}

static uint64_t decode64(const unsigned char *p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; i++) {
        n = n << 8 | p[i];
    }
    return n;
}

static unsigned references(const gterm_catalog_t *c,
                           const char *store,
                           const char *version,
                           const struct stat *identity) {
    char root[3600];
    snprintf(root, sizeof(root), "%s/%s", store, version);
    size_t n = strlen(root);
    unsigned count = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        const gterm_catalog_entry_t *e = &c->entries[i];
        bool used = !strncmp(e->directory, root, n) && (!e->directory[n] || e->directory[n] == '/');
        for (uint32_t j = 0; j < e->profile_count && !used;) {
            int arity = gterm_stdio_option_arity(e->profile[j]);
            if (arity < 0 || (uint32_t)arity >= e->profile_count - j) {
                break;
            }
            if (!strcmp(e->profile[j], "--release-evidence")) {
                const char *path = e->profile[j + 1];
                struct stat s;
                used = !strcmp(path, root) || (identity && !lstat(path, &s) && same(identity, &s));
            }
            j += (uint32_t)arity + 1;
        }
        if (used) {
            count++;
        }
    }
    return count;
}

static bool append_hash(char *bytes, size_t *at, const void *data, size_t size) {
    char hash[65];
    if (!gterm_release_hash(data, size, hash)) {
        return false;
    }
    memcpy(bytes + *at, hash, 64);
    *at += 64;
    return true;
}

static bool hash_stat(char *bytes, size_t *at, const struct stat *s) {
    char text[512];
#ifdef __APPLE__
    intmax_t mt = s->st_mtimespec.tv_sec, mn = s->st_mtimespec.tv_nsec, ct = s->st_ctimespec.tv_sec,
             cn = s->st_ctimespec.tv_nsec;
#else
    intmax_t mt = s->st_mtim.tv_sec, mn = s->st_mtim.tv_nsec, ct = s->st_ctim.tv_sec,
             cn = s->st_ctim.tv_nsec;
#endif
    int n = snprintf(text,
                     sizeof(text),
                     "%ju/%ju/%ju/%ju/%ju/%ju/%jd/%jd/%jd/%jd/%jd",
                     (uintmax_t)s->st_dev,
                     (uintmax_t)s->st_ino,
                     (uintmax_t)s->st_mode,
                     (uintmax_t)s->st_uid,
                     (uintmax_t)s->st_gid,
                     (uintmax_t)s->st_nlink,
                     (intmax_t)s->st_size,
                     mt,
                     mn,
                     ct,
                     cn);
    return n > 0 && (size_t)n < sizeof(text) && append_hash(bytes, at, text, (size_t)n);
}

static int fingerprint(review_t *r, const gterm_catalog_t *c) {
    char *bytes = malloc((TREE_ITEMS * 3 + 20) * 64);
    if (!bytes) {
        return 5;
    }
    size_t at = 0;
    char facts[160];
    int n = snprintf(facts,
                     sizeof(facts),
                     "%" PRIu64 "/%u/%u/%u/%u/%u/%" PRIu64,
                     c->revision,
                     r->count,
                     r->references,
                     r->pending,
                     r->in_use,
                     r->removed,
                     r->bytes);
    bool ok =
        append_hash(bytes,
                    &at,
                    "ghosterm-cli-remove-review-v1",
                    sizeof("ghosterm-cli-remove-review-v1") - 1) &&
        append_hash(bytes, &at, r->store.path, strlen(r->store.path)) &&
        append_hash(bytes, &at, c->path, strlen(c->path)) &&
        append_hash(bytes, &at, r->version, 64) &&
        append_hash(bytes, &at, r->location, strlen(r->location)) &&
        append_hash(bytes, &at, facts, (size_t)n) && hash_stat(bytes, &at, &c->original) &&
        append_hash(bytes, &at, &r->store.identity.st_dev, sizeof(r->store.identity.st_dev)) &&
        append_hash(bytes, &at, &r->store.identity.st_ino, sizeof(r->store.identity.st_ino)) &&
        append_hash(bytes, &at, &c->root.st_dev, sizeof(c->root.st_dev)) &&
        append_hash(bytes, &at, &c->root.st_ino, sizeof(c->root.st_ino)) &&
        hash_stat(bytes, &at, &r->identity) && hash_stat(bytes, &at, &r->intent_identity) &&
        hash_stat(bytes, &at, &r->store.locked) &&
        append_hash(bytes, &at, r->intent_bytes, r->pending ? INTENT_BYTES : 0);
    for (unsigned i = 0; ok && i < r->count; i++) {
        item_t *item = &r->items[i];
        ok = append_hash(bytes, &at, item->path, strlen(item->path)) &&
             hash_stat(bytes, &at, &item->identity) &&
             append_hash(bytes, &at, item->hash, strlen(item->hash));
    }
    if (ok) {
        ok = gterm_release_hash(bytes, at, r->digest);
    }
    free(bytes);
    return ok ? 0 : 5;
}

static int read_intent(review_t *r) {
    r->pending = false;
    struct stat named_stat;
    if (fstatat(r->store.root, r->intent, &named_stat, AT_SYMLINK_NOFOLLOW)) {
        return errno == ENOENT ? 0 : 5;
    }
    r->pending = true;
    int fd = openat(r->store.root, r->intent, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return 5;
    }
    struct stat before, after;
    unsigned char raw[INTENT_BYTES + 1];
    int result = 5;
    if (fstat(fd, &before) || !private_file(&before, 0400) || before.st_size != INTENT_BYTES ||
        !same(&before, &named_stat)) {
        goto done;
    }
    size_t at = 0;
    while (at < sizeof(raw)) {
        ssize_t n = read(fd, raw + at, sizeof(raw) - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            goto done;
        }
        if (!n) {
            break;
        }
        at += (size_t)n;
    }
    if (at != INTENT_BYTES || fstat(fd, &after) || !stable(&before, &after) ||
        !named(r->store.root, r->intent, &before)) {
        result = 9;
        goto done;
    }
    if (memcmp(raw, intent_magic, 8) || memcmp(raw + 8, r->version, 64) ||
        decode64(raw + 72) != (uint64_t)r->store.identity.st_dev ||
        decode64(raw + 80) != (uint64_t)r->store.identity.st_ino) {
        result = 6;
        goto done;
    }
    memcpy(r->intent_bytes, raw, INTENT_BYTES);
    r->intent_identity = before;
    r->pending = true;
    result = 0;
done:
    if (close(fd)) {
        result = 5;
    }
    return result;
}

static int
observe(review_t *r, const gterm_catalog_t *c, bool removing, volatile sig_atomic_t *cancelled) {
    snprintf(r->quarantine, sizeof(r->quarantine), ".remove-%s", r->version);
    snprintf(r->intent, sizeof(r->intent), ".remove-%s.intent", r->version);
    int result = read_intent(r);
    if (result) {
        return result;
    }
    struct stat original, quarantine;
    bool has_original = !fstatat(r->store.root, r->version, &original, AT_SYMLINK_NOFOLLOW);
    if (!has_original && errno != ENOENT) {
        return 5;
    }
    bool has_quarantine = !fstatat(r->store.root, r->quarantine, &quarantine, AT_SYMLINK_NOFOLLOW);
    if (!has_quarantine && errno != ENOENT) {
        return 5;
    }
    if (has_original && has_quarantine) {
        return 9;
    }
    if (has_quarantine && !r->pending) {
        return 6;
    }
    if (!has_original && !has_quarantine) {
        if (!r->pending) {
            return 10;
        }
        r->removed = true;
        r->identity.st_dev = (dev_t)decode64(r->intent_bytes + 88);
        r->identity.st_ino = (ino_t)decode64(r->intent_bytes + 96);
        strcpy(r->location, "absent");
    } else {
        r->quarantined = has_quarantine;
        strcpy(r->location, has_quarantine ? r->quarantine : r->version);
        r->identity = has_quarantine ? quarantine : original;
        mode_t mode = r->identity.st_mode & 07777;
        if (!S_ISDIR(r->identity.st_mode) || r->identity.st_uid != geteuid() ||
            r->identity.st_dev != r->store.identity.st_dev ||
            (mode != 0500 && !(r->pending && mode == 0700))) {
            return 6;
        }
        if (r->pending && (decode64(r->intent_bytes + 88) != (uint64_t)r->identity.st_dev ||
                           decode64(r->intent_bytes + 96) != (uint64_t)r->identity.st_ino)) {
            return 9;
        }
        r->directory =
            openat(r->store.root, r->location, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat actual;
        if (r->directory < 0 || fstat(r->directory, &actual) || !stable(&actual, &r->identity)) {
            return 9;
        }
        result = directory_lock(r->directory, true);
        if (result == 11 && !removing) {
            r->in_use = true;
            result = directory_lock(r->directory, false);
        }
        if (result) {
            return result;
        }
        r->items = calloc(TREE_ITEMS, sizeof(*r->items));
        if (!r->items) {
            return 5;
        }
        result = walk(r, r->directory, "", 0, cancelled);
        if (result) {
            return result;
        }
        qsort(r->items, r->count, sizeof(*r->items), compare);
        if (!r->pending) {
            item_t *descriptor = find(r, "release.json"), *signature = find(r, "release.sig"),
                   *payload = find(r, "payload");
            unsigned top = 0;
            for (unsigned i = 0; i < r->count; i++) {
                top += !strchr(r->items[i].path, '/');
            }
            if (top != 3 || !descriptor || !signature || !payload ||
                !S_ISDIR(payload->identity.st_mode) || !S_ISREG(descriptor->identity.st_mode) ||
                descriptor->identity.st_size > 65536 || strcmp(descriptor->hash, r->version) ||
                !S_ISREG(signature->identity.st_mode) || signature->identity.st_size != 64 ||
                !find(r, "payload/plugin.json") || !find(r, "payload/integrity.json") ||
                !S_ISREG(find(r, "payload/plugin.json")->identity.st_mode) ||
                !S_ISREG(find(r, "payload/integrity.json")->identity.st_mode)) {
                return 6;
            }
        }
        if (!named(r->store.root, r->location, &r->identity)) {
            return 9;
        }
    }
    r->references = references(c, r->store.path, r->version, &r->identity);
    if (!current(&r->store)) {
        return 9;
    }
    return fingerprint(r, c);
}

static bool review_close(review_t *r) {
    bool ok = true;
    for (unsigned i = 0; i < r->count; i++) {
        free(r->items[i].path);
    }
    free(r->items);
    r->items = NULL;
    r->count = 0;
    if (r->directory >= 0 && close(r->directory)) {
        ok = false;
    }
    r->directory = -1;
    if (!store_close(&r->store)) {
        ok = false;
    }
    return ok;
}

static void print_review(const review_t *r, const gterm_catalog_t *c) {
    printf("{\"result\":\"reviewed\",\"revision\":\"%" PRIu64 "\",\"version\":", c->revision);
    gterm_json_string(r->version);
    fputs(",\"reviewDigest\":", stdout);
    gterm_json_string(r->digest);
    printf(",\"pendingRemoval\":%s,\"quarantined\":%s,\"versionAbsent\":%s,\"inUse\":%s,"
           "\"catalogReferences\":%u,\"removable\":%s,\"fileBytes\":%" PRIu64 ",\"entries\":[",
           r->pending ? "true" : "false",
           r->quarantined ? "true" : "false",
           r->removed ? "true" : "false",
           r->in_use ? "true" : "false",
           r->references,
           !r->in_use && !r->references ? "true" : "false",
           r->bytes);
    for (unsigned i = 0; i < r->count; i++) {
        const item_t *item = &r->items[i];
        if (i) {
            putchar(',');
        }
        fputs("{\"path\":", stdout);
        gterm_json_string(item->path);
        printf(",\"kind\":\"%s\",\"bytes\":%ju,\"sha256\":",
               S_ISDIR(item->identity.st_mode) ? "directory" : "file",
               S_ISDIR(item->identity.st_mode) ? UINTMAX_C(0) : (uintmax_t)item->identity.st_size);
        if (item->hash[0]) {
            gterm_json_string(item->hash);
        } else {
            fputs("null", stdout);
        }
        putchar('}');
    }
    puts("],\"authorizationGranted\":false,\"publisherAuthenticated\":false,"
         "\"otherCatalogsChecked\":false}");
}

static int create_intent(review_t *r) {
    memcpy(r->intent_bytes, intent_magic, 8);
    memcpy(r->intent_bytes + 8, r->version, 64);
    encode64(r->intent_bytes + 72, (uint64_t)r->store.identity.st_dev);
    encode64(r->intent_bytes + 80, (uint64_t)r->store.identity.st_ino);
    encode64(r->intent_bytes + 88, (uint64_t)r->identity.st_dev);
    encode64(r->intent_bytes + 96, (uint64_t)r->identity.st_ino);
    mode_t old = umask(0077);
    int fd = openat(
        r->store.root, r->intent, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int saved = errno;
    umask(old);
    errno = saved;
    if (fd < 0) {
        return errno == EEXIST ? 9 : 5;
    }
    /* Even an incomplete write remains visible and blocks new pins. Never
     * silently discard an intent whose durability or identity is uncertain. */
    r->pending = true;
    int result = 5;
    size_t at = 0;
    while (at < INTENT_BYTES) {
        ssize_t n = write(fd, r->intent_bytes + at, INTENT_BYTES - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto done;
        }
        at += (size_t)n;
    }
    if (fchmod(fd, 0400) || fsync(fd) || fstat(fd, &r->intent_identity) ||
        !private_file(&r->intent_identity, 0400) ||
        !named(r->store.root, r->intent, &r->intent_identity)) {
        goto done;
    }
    result = 0;
done:
    if (close(fd)) {
        result = 5;
    }
    if (fsync(r->store.root)) {
        result = 5;
    }
    return result;
}

static int quarantine(review_t *r) {
    if (!current(&r->store) || !named(r->store.root, r->version, &r->identity)) {
        return 9;
    }
    int result;
#ifdef __APPLE__
    result = renameatx_np(r->store.root, r->version, r->store.root, r->quarantine, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    result =
        (int)syscall(SYS_renameat2, r->store.root, r->version, r->store.root, r->quarantine, 1);
#else
    errno = ENOTSUP;
    result = -1;
#endif
    if (result) {
        return errno == EEXIST ? 9 : errno == ENOTSUP || errno == ENOSYS ? 4 : 5;
    }
    r->quarantined = true;
    strcpy(r->location, r->quarantine);
    if (!named(r->store.root, r->location, &r->identity)) {
        return 9;
    }
    return fsync(r->store.root) ? 5 : 0;
}

/* Open each ancestor without following links, checking the exact directory
 * identity captured by review. Never chmod a regular or hard-linked file. */
static int parent_fd(review_t *r, const char *path, char **storage, const char **leaf) {
    *storage = strdup(path);
    if (!*storage) {
        return -1;
    }
    int fd = openat(r->directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char *at = *storage;
    for (char *slash = strchr(at, '/'); slash; slash = strchr(at, '/')) {
        *slash = 0;
        item_t *expected = find(r, *storage);
        struct stat actual;
        int next = openat(fd, at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        bool ok = expected && next >= 0 && !fstat(next, &actual) &&
                  same(&actual, &expected->identity) && actual.st_uid == geteuid() &&
                  S_ISDIR(actual.st_mode);
        if (close(fd)) {
            ok = false;
        }
        fd = next;
        if (!ok) {
            if (fd >= 0) {
                close(fd);
            }
            return -1;
        }
        *slash = '/';
        at = slash + 1;
    }
    *leaf = at;
    return fd;
}

/* Explicit owned-fixture build only. No environment-controlled pause exists
 * in the user-installed binary. A real SIGKILL exercises durable continuation. */
static int removal_checkpoint(volatile sig_atomic_t *cancelled) {
#ifdef GTERM_STORE_FAULTS
    const char *path = getenv("GTERM_TEST_STORE_PAUSE");
    if (path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            return 5;
        }
        bool ok = write(fd, "paused", 6) == 6 && !fsync(fd);
        if (close(fd)) {
            ok = false;
        }
        if (!ok) {
            return 5;
        }
        struct timespec tick = {0, 10000000};
        for (unsigned i = 0; i < 1000 && !*cancelled; i++) {
            nanosleep(&tick, NULL);
        }
        return *cancelled ? 130 : 5;
    }
#else
    (void)cancelled;
#endif
    return 0;
}

static int remove_tree(review_t *r, volatile sig_atomic_t *cancelled) {
    for (unsigned n = r->count; n > 0; n--) {
        if (*cancelled) {
            return 130;
        }
        if (!current(&r->store) || !named(r->store.root, r->quarantine, &r->identity)) {
            return 9;
        }
        item_t *item = &r->items[n - 1];
        char *storage = NULL;
        const char *leaf = NULL;
        int parent = parent_fd(r, item->path, &storage, &leaf);
        struct stat actual;
        int result = 9;
        if (parent < 0) {
            goto item_done;
        }
        if (fstatat(parent, leaf, &actual, AT_SYMLINK_NOFOLLOW) ||
            !same(&actual, &item->identity) || actual.st_uid != geteuid()) {
            goto item_done;
        }
        if (S_ISREG(item->identity.st_mode)) {
            if (!stable(&actual, &item->identity) || actual.st_nlink != 1) {
                goto item_done;
            }
            char hash[65];
            result = hash_file(parent, leaf, &actual, hash, cancelled);
            if (result) {
                goto item_done;
            }
            if (strcmp(hash, item->hash)) {
                result = 9;
                goto item_done;
            }
        } else if (!S_ISDIR(actual.st_mode) ||
                   ((actual.st_mode & 07777) != 0500 && (actual.st_mode & 07777) != 0700)) {
            result = 9;
            goto item_done;
        }
        if (*cancelled) {
            result = 130;
            goto item_done;
        }
        result = 5;
        if (!named(r->store.root, r->quarantine, &r->identity)) {
            result = 9;
            goto item_done;
        }
        if (fchmod(parent, 0700) || !named(parent, leaf, &actual) ||
            unlinkat(parent, leaf, S_ISDIR(actual.st_mode) ? AT_REMOVEDIR : 0) || fsync(parent)) {
            goto item_done;
        }
        result = n == r->count ? removal_checkpoint(cancelled) : 0;
    item_done:
        free(storage);
        if (parent >= 0 && close(parent)) {
            result = 5;
        }
        if (result) {
            return result;
        }
    }
    if (*cancelled) {
        return 130;
    }
    if (!current(&r->store) || !named(r->store.root, r->quarantine, &r->identity)) {
        return 9;
    }
    if (unlinkat(r->store.root, r->quarantine, AT_REMOVEDIR)) {
        return 5;
    }
    r->removed = true;
    return fsync(r->store.root) ? 5 : 0;
}

static int remove_version(review_t *r, volatile sig_atomic_t *cancelled) {
    if (r->references) {
        return 8;
    }
    if (r->in_use) {
        return 11;
    }
    if (*cancelled) {
        return 130;
    }
    if (!current(&r->store)) {
        return 9;
    }
    if (!r->removed && !named(r->store.root, r->location, &r->identity)) {
        return 9;
    }
    int result = 0;
    if (!r->pending) {
        result = create_intent(r);
        if (result) {
            return result;
        }
    }
    if (*cancelled) {
        return 130;
    }
    if (!r->removed && !r->quarantined) {
        result = quarantine(r);
        if (result) {
            return result;
        }
    }
    if (!r->removed) {
        result = remove_tree(r, cancelled);
        if (result) {
            return result;
        }
    }
    if (*cancelled) {
        return 130;
    }
    /* A fresh bounded read binds the final unlink to the intent actually
     * observed. It is not enough for an arbitrary file to have this name. */
    unsigned char expected[INTENT_BYTES];
    memcpy(expected, r->intent_bytes, INTENT_BYTES);
    struct stat identity = r->intent_identity;
    result = read_intent(r);
    if (result || !r->pending || !same(&identity, &r->intent_identity) ||
        memcmp(expected, r->intent_bytes, INTENT_BYTES)) {
        return result ? result : 9;
    }
    if (!current(&r->store) || !named(r->store.root, r->intent, &identity)) {
        return 9;
    }
    if (unlinkat(r->store.root, r->intent, 0)) {
        return 5;
    }
    r->pending = false;
    return fsync(r->store.root) ? 5 : 0;
}

typedef struct {
    char *name;
    struct stat identity;
    char version[65];
    int use;
    unsigned references;
} listed_t;

static int listed_compare(const void *a, const void *b) {
    return strcmp(((const listed_t *)a)->name, ((const listed_t *)b)->name);
}

static bool release_fds(review_t *r) {
    bool ok = true;
    if (r->directory >= 0 && close(r->directory)) {
        ok = false;
    }
    r->directory = -1;
    if (!store_close(&r->store)) {
        ok = false;
    }
    return ok;
}

static int list_versions(review_t *r, gterm_catalog_t *c, volatile sig_atomic_t *cancelled) {
    listed_t *items = calloc(STORE_ITEMS, sizeof(*items));
    if (!items) {
        return 5;
    }
    unsigned count = 0;
    int result = 0;
    int fd = openat(r->store.root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *stream = fd < 0 ? NULL : fdopendir(fd);
    if (!stream) {
        if (fd >= 0) {
            close(fd);
        }
        free(items);
        return 5;
    }
    while (!*cancelled) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno) {
                result = 5;
            }
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".store.lock")) {
            continue;
        }
        if (count == STORE_ITEMS) {
            result = 2;
            break;
        }
        listed_t *item = &items[count++];
        item->name = strdup(name);
        item->use = -1;
        if (!item->name || fstatat(r->store.root, name, &item->identity, AT_SYMLINK_NOFOLLOW)) {
            result = 5;
            break;
        }
        if (gterm_catalog_digest(name)) {
            memcpy(item->version, name, 65);
        } else if (!strncmp(name, ".remove-", 8) &&
                   (strlen(name) == 72 || (strlen(name) == 79 && !strcmp(name + 72, ".intent")))) {
            memcpy(item->version, name + 8, 64);
            item->version[64] = 0;
            if (!gterm_catalog_digest(item->version)) {
                item->version[0] = 0;
            }
        }
        if (item->version[0]) {
            item->references = references(c, r->store.path, item->version, &item->identity);
        }
        if (item->version[0] && S_ISDIR(item->identity.st_mode) &&
            item->identity.st_uid == geteuid()) {
            int probe =
                openat(r->store.root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat actual;
            if (probe >= 0 && !fstat(probe, &actual) && same(&actual, &item->identity)) {
                int status = directory_lock(probe, true);
                item->use = status == 11 ? 1 : status == 0 ? 0 : -1;
            }
            if (probe >= 0 && close(probe)) {
                result = 5;
                break;
            }
        }
    }
    if (closedir(stream)) {
        result = 5;
    }
    if (*cancelled) {
        result = 130;
    }
    if (!result && !current(&r->store)) {
        result = 9;
    }
    bool released = release_fds(r);
    if (!gterm_catalog_release(c)) {
        released = false;
    }
    if (!released) {
        result = 5;
    }
    if (!result) {
        qsort(items, count, sizeof(*items), listed_compare);
        printf("{\"result\":\"versions\",\"revision\":\"%" PRIu64 "\",\"entries\":[", c->revision);
        for (unsigned i = 0; i < count; i++) {
            listed_t *item = &items[i];
            if (i) {
                putchar(',');
            }
            if (gterm_catalog_utf8((unsigned char *)item->name, strlen(item->name))) {
                fputs("{\"name\":", stdout);
                gterm_json_string(item->name);
            } else {
                fputs("{\"nameHex\":\"", stdout);
                for (const unsigned char *p = (unsigned char *)item->name; *p; p++) {
                    printf("%02x", *p);
                }
                putchar('"');
            }
            fputs(",\"version\":", stdout);
            if (item->version[0]) {
                gterm_json_string(item->version);
            } else {
                fputs("null", stdout);
            }
            printf(",\"kind\":\"%s\",\"inUse\":%s,\"catalogReferences\":%u}",
                   S_ISDIR(item->identity.st_mode)   ? "directory"
                   : S_ISREG(item->identity.st_mode) ? "file"
                                                     : "other",
                   item->use < 0 ? "null"
                   : item->use   ? "true"
                                 : "false",
                   item->references);
        }
        puts("],\"authorizationGranted\":false,\"publisherAuthenticated\":false,"
             "\"otherCatalogsChecked\":false}");
    }
    for (unsigned i = 0; i < count; i++) {
        free(items[i].name);
    }
    free(items);
    return result;
}

static int report(int status, const char *code, const review_t *r) {
    const char *kind = status == 0     ? "removed"
                       : status == 2   ? "invalid"
                       : status == 4   ? "unsupported"
                       : status == 6   ? "denied"
                       : status == 8   ? "conflict"
                       : status == 9   ? "stale"
                       : status == 10  ? "not-found"
                       : status == 11  ? "busy"
                       : status == 130 ? "cancelled"
                                       : "io";
    fputs("{\"result\":", stdout);
    gterm_json_string(kind);
    fputs(",\"code\":", stdout);
    gterm_json_string(code);
    printf(",\"quarantined\":%s,\"removed\":%s,\"intentRetained\":%s,\"durabilityConfirmed\":%s,"
           "\"authorizationGranted\":false,\"publisherAuthenticated\":false}\n",
           r->quarantined ? "true" : "false",
           r->removed ? "true" : "false",
           r->pending ? "true" : "false",
           !status ? "true" : "false");
    return status;
}

int gterm_release_manage(const char *operation,
                         int argc,
                         char **argv,
                         volatile sig_atomic_t *cancelled) {
    const char *path = NULL, *store = NULL, *version = NULL, *revision = NULL, *digest = NULL;
    bool listing = !strcmp(operation, "versions"),
         removing = !strcmp(operation, "remove-installed");
    for (int i = 0; i < argc; i += 2) {
        if (i + 1 == argc) {
            return gterm_catalog_report(2, "VERSION_OPTIONS", NULL);
        }
        const char **field = !strcmp(argv[i], "--catalog")           ? &path
                             : !strcmp(argv[i], "--store")           ? &store
                             : !strcmp(argv[i], "--version")         ? &version
                             : !strcmp(argv[i], "--expect-revision") ? &revision
                             : !strcmp(argv[i], "--review-digest")   ? &digest
                                                                     : NULL;
        if (!field || *field) {
            return gterm_catalog_report(2, "VERSION_OPTIONS", NULL);
        }
        *field = argv[i + 1];
    }
    if (!path) {
        path = getenv("GTERM_CATALOG");
    }
    uint64_t expected = 0;
    if (!gterm_catalog_path(path) || !gterm_catalog_path(store) || strlen(store) > 3500 ||
        (listing ? (version || revision || digest) : !gterm_catalog_digest(version)) ||
        (removing ? (!gterm_catalog_revision(revision, &expected) || !gterm_catalog_digest(digest))
                  : (revision || digest))) {
        return gterm_catalog_report(2, "VERSION_OPTIONS", NULL);
    }
    if (!gterm_release_supported()) {
        return gterm_catalog_report(4, "RELEASE_VERIFY_UNAVAILABLE", NULL);
    }
    review_t r = {.directory = -1, .version = version, .store = {.root = -1, .lock = -1}};
    gterm_catalog_t c;
    int result = gterm_catalog_open(&c, path, removing);
    const char *code = "CATALOG_OPEN";
    if (result) {
        goto done;
    }
    if (removing && c.revision != expected) {
        result = 9;
        code = "CATALOG_REVISION";
        goto done;
    }
    result = store_open(&r.store, store, removing);
    code = "VERSION_STORE";
    if (result) {
        goto done;
    }
    if (listing) {
        result = list_versions(&r, &c, cancelled);
        code = "VERSION_LIST";
        goto done;
    }
    result = observe(&r, &c, removing, cancelled);
    code = "VERSION_REVIEW";
    if (result) {
        goto done;
    }
    if (!removing) {
        bool released = release_fds(&r);
        if (!gterm_catalog_release(&c)) {
            released = false;
        }
        if (!released) {
            result = 5;
            code = "VERSION_CLOSE";
            goto done;
        }
        print_review(&r, &c);
        goto done;
    }
    if (strcmp(digest, r.digest)) {
        result = 9;
        code = "VERSION_CONFIRMATION";
        goto done;
    }
    result = remove_version(&r, cancelled);
    code = "VERSION_REMOVE";
done:;
    bool closed = review_close(&r);
    if (!gterm_catalog_close(&c)) {
        closed = false;
    }
    if (!closed) {
        result = 5;
        code = "VERSION_CLOSE";
    }
    return result || removing ? report(result, code, &r) : 0;
}
