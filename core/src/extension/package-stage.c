#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/package-stage.h"
#include "extension/memory.h"
#include "base/secure-random.h"
#include "base/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *path;
    dev_t device;
    ino_t inode;
    bool directory;
} stage_entry_t;

struct gt_package_stage {
    gt_allocator_t allocator;
    gt_package_checked_t *checked;
    gt_package_snapshot_t *host;
    const atomic_bool *stop;
    int parent, root, file;
    char name[48], entry[4097];
    struct stat root_identity;
    gt_package_stage_status_t status;
    stage_entry_t entries[GT_PACKAGE_ENTRIES_MAX];
    size_t count, member, members, offset, sealing;
    bool executable, created, copying_done;
};

static const gt_allocator_t *stage_allocator(const gt_package_stage_t *s) {
    return s->allocator.allocate ? &s->allocator : NULL;
}

static bool stage_same(const struct stat *s, const stage_entry_t *e) {
    return s->st_dev == e->device && s->st_ino == e->inode &&
           (e->directory ? S_ISDIR(s->st_mode) : S_ISREG(s->st_mode));
}

static bool stage_root_bound(gt_package_stage_t *s) {
    struct stat st;
    return s->created && !fstatat(s->parent, s->name, &st, AT_SYMLINK_NOFOLLOW) &&
           S_ISDIR(st.st_mode) && st.st_dev == s->root_identity.st_dev &&
           st.st_ino == s->root_identity.st_ino;
}

static stage_entry_t *stage_find(gt_package_stage_t *s, const char *path) {
    for (size_t i = 0; i < s->count; i++) {
        if (!strcmp(s->entries[i].path, path)) {
            return &s->entries[i];
        }
    }
    return NULL;
}

/* Every component is matched to our recorded inode before it is used. */
static gt_package_stage_status_t
stage_parent(gt_package_stage_t *s, const char *path, bool create, int *out, char *leaf) {
    char text[4097], prefix[4097] = "";
    strcpy(text, path);
    int fd = fcntl(s->root, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) {
        return GT_STAGE_IO;
    }
    char *component = text;
    gt_package_stage_status_t result = GT_STAGE_OK;
    for (char *slash; (slash = strchr(component, '/')); component = slash + 1) {
        *slash = 0;
        if (prefix[0]) {
            strcat(prefix, "/");
        }
        strcat(prefix, component);
        stage_entry_t *e = stage_find(s, prefix);
        if (!e && create) {
            if (s->count == GT_PACKAGE_ENTRIES_MAX) {
                result = GT_STAGE_LIMIT;
                break;
            }
            size_t n = strlen(prefix) + 1;
            char *copy = gt__plugin_memory_alloc(stage_allocator(s), n);
            if (!copy) {
                result = GT_STAGE_NOMEM;
                break;
            }
            memcpy(copy, prefix, n);
            if (mkdirat(fd, component, 0700)) {
                result = errno == EEXIST ? GT_STAGE_CHANGED : GT_STAGE_IO;
                gt__plugin_memory_free(stage_allocator(s), copy, n);
                break;
            }
            struct stat st;
            if (fstatat(fd, component, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)) {
                /* In the trusted exclusive staging directory a successful mkdir
                 * is immediately observable. Do not remove a changed name. */
                gt__plugin_memory_free(stage_allocator(s), copy, n);
                result = GT_STAGE_CHANGED;
                break;
            }
            e = &s->entries[s->count++];
            *e = (stage_entry_t){copy, st.st_dev, st.st_ino, true};
        }
        struct stat st;
        int next = e && e->directory
                       ? openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                       : -1;
        if (next < 0 || fstat(next, &st) || !stage_same(&st, e)) {
            if (next >= 0) {
                (void)close(next);
            }
            result = GT_STAGE_CHANGED;
            break;
        }
        int closed = close(fd);
        fd = next;
        if (closed) {
            result = GT_STAGE_IO;
            break;
        }
    }
    if (result == GT_STAGE_OK) {
        strcpy(leaf, component);
        *out = fd;
    } else {
        (void)close(fd);
    }
    return result;
}

static gt_package_stage_status_t stage_begin_file(gt_package_stage_t *s, const char *path) {
    if (s->count == GT_PACKAGE_ENTRIES_MAX) {
        return GT_STAGE_LIMIT;
    }
    size_t n = strlen(path) + 1;
    char *copy = gt__plugin_memory_alloc(stage_allocator(s), n);
    if (!copy) {
        return GT_STAGE_NOMEM;
    }
    memcpy(copy, path, n);
    char leaf[4097];
    int parent = -1;
    gt_package_stage_status_t result = stage_parent(s, path, true, &parent, leaf);
    if (result == GT_STAGE_OK && s->count == GT_PACKAGE_ENTRIES_MAX) {
        result = GT_STAGE_LIMIT;
    }
    if (result == GT_STAGE_OK) {
        s->file = openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (s->file < 0) {
            result = errno == EEXIST ? GT_STAGE_CHANGED : GT_STAGE_IO;
        } else {
            struct stat st;
            if (fstat(s->file, &st) || !S_ISREG(st.st_mode)) {
                result = GT_STAGE_IO;
            } else {
                s->entries[s->count++] = (stage_entry_t){copy, st.st_dev, st.st_ino, false};
                copy = NULL;
            }
        }
    }
    if (parent >= 0 && close(parent) && result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    if (copy) {
        gt__plugin_memory_free(stage_allocator(s), copy, n);
    }
    return result;
}

static gt_package_stage_status_t stage_create(gt_package_stage_t *s) {
    uint8_t random[16];
    gt_status_t entropy = gt__secure_random(random, sizeof(random));
    if (entropy != GT_OK) {
        return entropy == GT_ERR_UNSUPPORTED ? GT_STAGE_UNSUPPORTED : GT_STAGE_IO;
    }
    strcpy(s->name, "runtime-");
    for (size_t i = 0; i < sizeof(random); i++) {
        s->name[8 + i * 2] = "0123456789abcdef"[random[i] >> 4];
        s->name[9 + i * 2] = "0123456789abcdef"[random[i] & 15];
    }
    s->name[40] = 0;
    gt__secure_erase(random, sizeof(random));
    if (mkdirat(s->parent, s->name, 0700)) {
        return errno == EEXIST ? GT_STAGE_CHANGED : GT_STAGE_IO;
    }
    s->created = true;
    if (fstatat(s->parent, s->name, &s->root_identity, AT_SYMLINK_NOFOLLOW) ||
        !S_ISDIR(s->root_identity.st_mode)) {
        return GT_STAGE_CHANGED;
    }
    s->root = openat(s->parent, s->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (s->root < 0 || fstat(s->root, &st)) {
        return GT_STAGE_IO;
    }
    return st.st_dev == s->root_identity.st_dev && st.st_ino == s->root_identity.st_ino
               ? GT_STAGE_OK
               : GT_STAGE_CHANGED;
}

gt_package_stage_status_t gt__package_stage_open(gt_package_checked_t *checked,
                                                 int parent,
                                                 uint64_t limit,
                                                 const atomic_bool *stop,
                                                 const gt_allocator_t *allocator,
                                                 gt_package_stage_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out || !checked || parent < 0 || !limit || limit > GT_PACKAGE_TOTAL_BYTES_MAX ||
        (allocator && (!allocator->allocate || !allocator->release))) {
        return GT_STAGE_INVALID;
    }
    struct stat st;
    if (fstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0077)) {
        return GT_STAGE_INVALID;
    }
    gt_package_check_info_t info;
    size_t count;
    uint64_t bytes;
    if (!gt__package_checked_info(checked, &info) ||
        !gt__package_checked_stats(checked, &count, &bytes)) {
        return GT_STAGE_INVALID;
    }
    if (!info.executor || (strcmp(info.runtime, "node") && strcmp(info.runtime, "exec"))) {
        return GT_STAGE_UNSUPPORTED;
    }
    if (bytes > limit) {
        return GT_STAGE_LIMIT;
    }
    if (stop && atomic_load_explicit(stop, memory_order_relaxed)) {
        return GT_STAGE_CANCELLED;
    }
    gt_allocator_t a = allocator ? *allocator : (gt_allocator_t){0};
    gt_package_stage_t *s = gt__plugin_memory_alloc(allocator, sizeof(*s));
    if (!s) {
        return GT_STAGE_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->allocator = a;
    s->parent = s->root = s->file = -1;
    s->stop = stop;
    s->members = count;
    s->status = GT_STAGE_WAIT;
    s->executable = !strcmp(info.runtime, "exec");
    strcpy(s->entry, info.entry);
    s->parent = fcntl(parent, F_DUPFD_CLOEXEC, 3);
    if (s->parent < 0 || !gt__package_checked_retain(checked, &s->checked)) {
        if (s->parent >= 0) {
            (void)close(s->parent);
        }
        gt__plugin_memory_free(allocator, s, sizeof(*s));
        return GT_STAGE_IO;
    }
    *out = s;
    return GT_STAGE_OK;
}

bool gt__package_stage_catalog_digest(const gt_package_snapshot_t *snapshot, char digest[65]) {
    if (digest) {
        digest[0] = 0;
    }
    size_t count;
    uint64_t total;
    if (!digest || !gt__package_snapshot_count(snapshot, &count, &total) || !count ||
        count > GT_PACKAGE_MEMBERS_MAX) {
        return false;
    }
    gt_package_member_t members[GT_PACKAGE_MEMBERS_MAX];
    for (size_t i = 0; i < count; i++) {
        if (!gt__package_snapshot_member(snapshot, i, &members[i])) {
            return false;
        }
    }
    /* Bounded insertion sort; capture enumeration is not canonical. */
    for (size_t i = 1; i < count; i++) {
        gt_package_member_t value = members[i];
        size_t j = i;
        while (j && strcmp(members[j - 1].path, value.path) > 0) {
            members[j] = members[j - 1];
            j--;
        }
        members[j] = value;
    }
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(members[i].path);
        uint8_t header[12];
        for (size_t j = 0; j < 4; j++) {
            header[j] = (uint8_t)(n >> ((3 - j) * 8));
        }
        for (size_t j = 0; j < 8; j++) {
            header[4 + j] = (uint8_t)(members[i].bytes >> ((7 - j) * 8));
        }
        if (!gt__sha256_update(&hash, header, 12) ||
            !gt__sha256_update(&hash, members[i].path, n) ||
            !gt__sha256_update(&hash, members[i].sha256, 64)) {
            return false;
        }
    }
    return gt__sha256_final(&hash, digest);
}

gt_package_stage_status_t gt__package_stage_open_host(gt_package_snapshot_t **snapshot,
                                                      const char *trusted,
                                                      int parent,
                                                      const atomic_bool *stop,
                                                      const gt_allocator_t *allocator,
                                                      gt_package_stage_t **out) {
    if (out) {
        *out = NULL;
    }
    struct stat st;
    char digest[65];
    size_t count;
    uint64_t bytes;
    if (!out || !snapshot || !*snapshot || !trusted || strnlen(trusted, 65) != 64 ||
        (allocator && (!allocator->allocate || !allocator->release)) || fstat(parent, &st) ||
        !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) ||
        !gt__package_snapshot_count(*snapshot, &count, &bytes)) {
        return GT_STAGE_INVALID;
    }
    if (!gt__package_stage_catalog_digest(*snapshot, digest) || strcmp(digest, trusted)) {
        return GT_STAGE_CHANGED;
    }
    if (stop && atomic_load_explicit(stop, memory_order_relaxed)) {
        return GT_STAGE_CANCELLED;
    }
    gt_package_stage_t *s = gt__plugin_memory_alloc(allocator, sizeof(*s));
    if (!s) {
        return GT_STAGE_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->allocator = allocator ? *allocator : (gt_allocator_t){0};
    s->parent = fcntl(parent, F_DUPFD_CLOEXEC, 3);
    s->root = s->file = -1;
    if (s->parent < 0) {
        gt__plugin_memory_free(allocator, s, sizeof(*s));
        return GT_STAGE_IO;
    }
    s->stop = stop;
    s->members = count;
    s->status = GT_STAGE_WAIT;
    s->host = *snapshot;
    *snapshot = NULL;
    *out = s;
    return GT_STAGE_OK;
}

static gt_package_stage_status_t stage_seal(gt_package_stage_t *s) {
    if (s->sealing == s->count) {
        return fchmod(s->root, 0500) ? GT_STAGE_IO : GT_STAGE_OK;
    }
    stage_entry_t *e = &s->entries[s->sealing++];
    int parent = -1;
    char leaf[4097];
    gt_package_stage_status_t result = stage_parent(s, e->path, false, &parent, leaf);
    if (result != GT_STAGE_OK) {
        return result;
    }
    int fd = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !stage_same(&st, e)) {
        result = GT_STAGE_CHANGED;
    } else if (e->directory && fchmod(fd, 0500)) {
        result = GT_STAGE_IO;
    }
    if (fd >= 0 && close(fd) && result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    if (close(parent) && result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    return result == GT_STAGE_OK ? GT_STAGE_WAIT : result;
}

gt_package_stage_status_t gt__package_stage_step(gt_package_stage_t *s) {
    if (!s) {
        return GT_STAGE_INVALID;
    }
    if (s->status != GT_STAGE_WAIT && s->status != GT_STAGE_OK) {
        return s->status;
    }
    if (s->stop && atomic_load_explicit(s->stop, memory_order_relaxed)) {
        return s->status = GT_STAGE_CANCELLED;
    }
    if (s->status == GT_STAGE_OK) {
        return s->status;
    }
    if (!s->created) {
        gt_package_stage_status_t result = stage_create(s);
        if (result != GT_STAGE_OK) {
            return s->status = result;
        }
    }
    if (!stage_root_bound(s)) {
        return s->status = GT_STAGE_CHANGED;
    }
    if (s->copying_done) {
        return s->status = stage_seal(s);
    }
    gt_package_member_t member;
    const uint8_t *bytes;
    size_t size;
    bool source = s->checked ? gt__package_checked_at(s->checked, s->member, &member, &bytes, &size)
                             : (gt__package_snapshot_member(s->host, s->member, &member) &&
                                gt__package_snapshot_view(s->host, s->member, &bytes, &size));
    if (!source || !gt__package_path_valid(member.path, strlen(member.path))) {
        return s->status = GT_STAGE_INVALID;
    }
    if (s->file < 0) {
        gt_package_stage_status_t result = stage_begin_file(s, member.path);
        if (result != GT_STAGE_OK) {
            return s->status = result;
        }
    }
    struct stat st;
    if (fstat(s->file, &st) || !stage_same(&st, &s->entries[s->count - 1]) || st.st_size < 0 ||
        (uint64_t)st.st_size != s->offset) {
        return s->status = GT_STAGE_CHANGED;
    }
    size_t n = size - s->offset;
    if (n > GT_PACKAGE_CHUNK_MAX) {
        n = GT_PACKAGE_CHUNK_MAX;
    }
    if (n) {
        ssize_t written = write(s->file, bytes + s->offset, n);
        if (written < 0 && errno == EINTR) {
            return GT_STAGE_WAIT;
        }
        if (written <= 0) {
            return s->status = GT_STAGE_IO;
        }
        s->offset += (size_t)written;
    }
    if (s->offset == size) {
        mode_t mode = s->executable && !strcmp(member.path, s->entry) ? 0500 : 0400;
        int changed = fchmod(s->file, mode), closed = close(s->file);
        s->file = -1;
        if (changed || closed) {
            return s->status = GT_STAGE_IO;
        }
        s->offset = 0;
        s->member++;
        s->copying_done = s->member == s->members;
    }
    return GT_STAGE_WAIT;
}

gt_package_stage_status_t
gt__package_stage_view(gt_package_stage_t *s, int *fd, const char **name, const char **entry) {
    if (fd) {
        *fd = -1;
    }
    if (name) {
        *name = NULL;
    }
    if (entry) {
        *entry = NULL;
    }
    if (!s || !fd || !name || !entry) {
        return GT_STAGE_INVALID;
    }
    if (s->status != GT_STAGE_OK) {
        return s->status;
    }
    if (s->stop && atomic_load_explicit(s->stop, memory_order_relaxed)) {
        return s->status = GT_STAGE_CANCELLED;
    }
    if (!stage_root_bound(s)) {
        return s->status = GT_STAGE_CHANGED;
    }
    *fd = s->root;
    *name = s->name;
    *entry = s->entry;
    return GT_STAGE_OK;
}

gt_package_stage_status_t gt__package_stage_destroy(gt_package_stage_t **out) {
    if (!out || !*out) {
        return GT_STAGE_INVALID;
    }
    gt_package_stage_t *s = *out;
    *out = NULL;
    gt_package_stage_status_t result = GT_STAGE_OK;
    if (s->file >= 0 && close(s->file)) {
        result = GT_STAGE_IO;
    }
    if (s->root >= 0 && fchmod(s->root, 0700)) {
        result = GT_STAGE_IO;
    }
    /* Restore owned directory permissions in creation order, then remove only
     * recorded identities in reverse order. Unknown children keep rmdir red. */
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < s->count; i++) {
            stage_entry_t *e = &s->entries[pass ? s->count - i - 1 : i];
            if (!pass && !e->directory) {
                continue;
            }
            int parent = -1;
            char leaf[4097];
            gt_package_stage_status_t status = stage_parent(s, e->path, false, &parent, leaf);
            struct stat st;
            if (status == GT_STAGE_OK &&
                (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) || !stage_same(&st, e))) {
                status = GT_STAGE_CHANGED;
            }
            if (status == GT_STAGE_OK && !pass) {
                int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0 || fstat(fd, &st) || !stage_same(&st, e)) {
                    status = GT_STAGE_CHANGED;
                } else if (fchmod(fd, 0700)) {
                    status = GT_STAGE_IO;
                }
                if (fd >= 0 && close(fd) && status == GT_STAGE_OK) {
                    status = GT_STAGE_IO;
                }
            } else if (status == GT_STAGE_OK &&
                       unlinkat(parent, leaf, e->directory ? AT_REMOVEDIR : 0)) {
                status = GT_STAGE_IO;
            }
            if (parent >= 0 && close(parent) && status == GT_STAGE_OK) {
                status = GT_STAGE_IO;
            }
            if (status == GT_STAGE_CHANGED || result == GT_STAGE_OK) {
                result = status;
            }
        }
    }
    if (s->root >= 0 && close(s->root) && result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    if (s->created) {
        if (!stage_root_bound(s)) {
            result = GT_STAGE_CHANGED;
        } else if (unlinkat(s->parent, s->name, AT_REMOVEDIR) && result == GT_STAGE_OK) {
            result = GT_STAGE_IO;
        }
    }
    if (close(s->parent) && result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    if (s->checked && gt__package_checked_destroy(&s->checked) != GT_PACKAGE_OK &&
        result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    if (s->host && gt__package_snapshot_destroy(&s->host) != GT_PACKAGE_OK &&
        result == GT_STAGE_OK) {
        result = GT_STAGE_IO;
    }
    gt_allocator_t allocator = s->allocator;
    for (size_t i = 0; i < s->count; i++) {
        char *path = s->entries[i].path;
        gt__plugin_memory_free(allocator.allocate ? &allocator : NULL, path, strlen(path) + 1);
    }
    gt__plugin_memory_free(allocator.allocate ? &allocator : NULL, s, sizeof(*s));
    return result;
}
