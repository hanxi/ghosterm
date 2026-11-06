#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/memory.h"
#include "extension/source.h"
#include "base/sha256.h"
#include "base/secure-random.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct gt_bridge_source {
    gt_allocator_t allocator;
    gt_bridge_session_t *session;
    gt_bridge_lease_t lease;
    gt_bridge_source_info_t info;
    uint64_t limit;
    size_t count;
    int directory[GT_BRIDGE_SOURCE_DEPTH], file;
    char names[GT_BRIDGE_SOURCE_DEPTH][256];
    struct stat roots[GT_BRIDGE_SOURCE_DEPTH], version;
    gt_sha256_t hash;
    uint8_t buffer[GT_BRIDGE_SOURCE_QUANTUM];
    size_t used, at;
    bool hash_eof, send_eof, close_requested;
};

static void *source_allocate(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}

static void source_release(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)n;
    free(p);
}

static bool source_terminal(const gt_bridge_source_t *s) {
    return s->info.phase >= GT_BRIDGE_SOURCE_SUCCEEDED;
}

static gt_bridge_source_status_t source_authority(gt_bridge_session_t *session,
                                                  const gt_bridge_lease_t *lease) {
    const char *caps[] = {"file.readScoped", "resource.receive"};
    for (size_t i = 0; i < 2; i++) {
        if (!gt__bridge_session_has_capability(session, caps[i])) {
            return GT_BRIDGE_SOURCE_UNSUPPORTED;
        }
        gt_bridge_access_t a = gt__bridge_session_access(session, lease, caps[i]);
        if (a != GT_BRIDGE_ACCESS_ALLOWED) {
            return a == GT_BRIDGE_ACCESS_UNKNOWN       ? GT_BRIDGE_SOURCE_WAIT
                   : a == GT_BRIDGE_ACCESS_UNSUPPORTED ? GT_BRIDGE_SOURCE_UNSUPPORTED
                   : a == GT_BRIDGE_ACCESS_REVOKED     ? GT_BRIDGE_SOURCE_CLOSED
                                                       : GT_BRIDGE_SOURCE_DENIED;
        }
    }
    return GT_BRIDGE_SOURCE_OK;
}

static gt_bridge_source_status_t source_access(const gt_bridge_source_t *s) {
    return source_authority(s->session, &s->lease);
}

static bool source_utf8(const char *p, size_t n) {
    size_t at = 0;
    while (at < n) {
        uint32_t cp, minimum;
        unsigned tails;
        uint8_t b = (uint8_t)p[at++];
        if (b < 0x80) {
            cp = b;
            minimum = 0;
            tails = 0;
        } else if (b >= 0xc2 && b <= 0xdf) {
            cp = b & 31;
            minimum = 0x80;
            tails = 1;
        } else if (b >= 0xe0 && b <= 0xef) {
            cp = b & 15;
            minimum = 0x800;
            tails = 2;
        } else if (b >= 0xf0 && b <= 0xf4) {
            cp = b & 7;
            minimum = 0x10000;
            tails = 3;
        } else {
            return false;
        }
        if (tails > n - at) {
            return false;
        }
        while (tails--) {
            b = (uint8_t)p[at++];
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 ||
            (cp >= 127 && cp <= 159)) {
            return false;
        }
    }
    return true;
}

static bool source_mime(const char *p) {
    size_t n = strnlen(p, 129), slash = 0;
    if (!n || n > 128) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '/') {
            if (!i || i + 1 == n || ++slash > 1) {
                return false;
            }
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("!#$&^_.+-", c))) {
            return false;
        }
    }
    return slash == 1;
}

static gt_bridge_source_status_t source_path(gt_bridge_source_t *s, const char *p) {
    size_t n = strnlen(p, GT_BRIDGE_SOURCE_PATH + 1);
    if (!n || n > GT_BRIDGE_SOURCE_PATH || p[0] == '/' || p[n - 1] == '/') {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    if (!source_utf8(p, n)) {
        return GT_BRIDGE_SOURCE_UNSUPPORTED;
    }
    size_t at = 0;
    while (at < n) {
        while (at < n && p[at] == '/') {
            at++;
        }
        size_t start = at;
        while (at < n && p[at] != '/') {
            at++;
        }
        size_t size = at - start;
        if (size == 1 && p[start] == '.') {
            continue;
        }
        if (size == 2 && p[start] == '.' && p[start + 1] == '.') {
            return GT_BRIDGE_SOURCE_DENIED;
        }
        if (!size) {
            continue;
        }
        if (size > 255 || s->count == GT_BRIDGE_SOURCE_DEPTH) {
            return GT_BRIDGE_SOURCE_LIMIT;
        }
        memcpy(s->names[s->count], p + start, size);
        s->names[s->count][size] = 0;
        s->count++;
    }
    return s->count ? GT_BRIDGE_SOURCE_OK : GT_BRIDGE_SOURCE_INVALID;
}

static bool source_inode(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT);
}

static bool source_version(const struct stat *a, const struct stat *b) {
    if (!source_inode(a, b) || a->st_size != b->st_size || a->st_nlink != b->st_nlink ||
        a->st_mode != b->st_mode) {
        return false;
    }
#if defined(__APPLE__)
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static gt_bridge_source_status_t source_check(gt_bridge_source_t *s) {
    struct stat st;
    if (fstat(s->file, &st)) {
        return GT_BRIDGE_SOURCE_IO;
    }
    if (!S_ISREG(st.st_mode) || !source_version(&st, &s->version)) {
        return GT_BRIDGE_SOURCE_CHANGED;
    }
    for (size_t i = 0; i < s->count; i++) {
        if (fstat(s->directory[i], &st)) {
            return GT_BRIDGE_SOURCE_IO;
        }
        if (!S_ISDIR(st.st_mode) || !st.st_nlink || !source_inode(&st, &s->roots[i])) {
            return GT_BRIDGE_SOURCE_CHANGED;
        }
        if (fstatat(s->directory[i], s->names[i], &st, AT_SYMLINK_NOFOLLOW)) {
            return errno == ENOENT || errno == ENOTDIR ? GT_BRIDGE_SOURCE_CHANGED
                                                       : GT_BRIDGE_SOURCE_IO;
        }
        if (i + 1 < s->count) {
            if (!S_ISDIR(st.st_mode) || !source_inode(&st, &s->roots[i + 1])) {
                return GT_BRIDGE_SOURCE_CHANGED;
            }
        } else if (!S_ISREG(st.st_mode) || !source_version(&st, &s->version)) {
            return GT_BRIDGE_SOURCE_CHANGED;
        }
    }
    return source_access(s);
}

static bool source_close_fds(gt_bridge_source_t *s) {
    bool failed = false;
    if (s->file >= 0) {
        int fd = s->file;
        s->file = -1;
        if (close(fd)) {
            failed = true;
        }
    }
    for (size_t i = 0; i < GT_BRIDGE_SOURCE_DEPTH; i++) {
        if (s->directory[i] >= 0) {
            int fd = s->directory[i];
            s->directory[i] = -1;
            if (close(fd)) {
                failed = true;
            }
        }
    }
    gt__secure_erase(s->buffer, sizeof(s->buffer));
    gt__secure_erase(&s->hash, sizeof(s->hash));
    s->used = s->at = 0;
    return !failed;
}

static void source_fail(gt_bridge_source_t *s, gt_bridge_source_status_t error) {
    gt__secure_erase(s->buffer, sizeof(s->buffer));
    gt__secure_erase(&s->hash, sizeof(s->hash));
    s->used = s->at = 0;
    s->info.error = error;
    if (s->info.transfer_handle) {
        (void)gt__bridge_session_cancel_resource(s->session, &s->lease, s->info.transfer_handle);
        s->info.phase = GT_BRIDGE_SOURCE_CANCELLING;
    } else {
        s->info.phase = error == GT_BRIDGE_SOURCE_CANCELLED || error == GT_BRIDGE_SOURCE_CLOSED
                            ? GT_BRIDGE_SOURCE_STOPPED
                            : GT_BRIDGE_SOURCE_FAILED;
    }
}

gt_bridge_source_status_t gt__bridge_source_open(const gt_bridge_source_config_t *config,
                                                 const gt_allocator_t *allocator,
                                                 gt_bridge_source_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!config || !out || !config->session || config->root_fd < 0 || !config->relative_path ||
        !config->mime || !config->byte_limit || config->byte_limit > GT_BRIDGE_RESOURCE_BYTES_MAX) {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    gt_bridge_source_status_t status = source_authority(config->session, &config->lease);
    if (status != GT_BRIDGE_SOURCE_OK) {
        return status;
    }
    if (!source_mime(config->mime)) {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    gt_allocator_t a =
        allocator ? *allocator : (gt_allocator_t){NULL, source_allocate, source_release};
    if (!a.allocate || !a.release) {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    gt_bridge_source_t *s = gt__plugin_memory_alloc(&a, sizeof(*s));
    if (!s) {
        return GT_BRIDGE_SOURCE_IO;
    }
    memset(s, 0, sizeof(*s));
    s->allocator = a;
    s->session = config->session;
    s->lease = config->lease;
    s->file = -1;
    for (size_t i = 0; i < GT_BRIDGE_SOURCE_DEPTH; i++) {
        s->directory[i] = -1;
    }
    status = source_path(s, config->relative_path);
    gt_bridge_session_info_t session;
    if (status == GT_BRIDGE_SOURCE_OK && !gt__bridge_session_info(s->session, &session)) {
        status = GT_BRIDGE_SOURCE_IO;
    }
    if (status == GT_BRIDGE_SOURCE_OK) {
        s->limit = config->byte_limit < session.budgets.resource_bytes
                       ? config->byte_limit
                       : session.budgets.resource_bytes;
        if (fstat(config->root_fd, &s->roots[0])) {
            status = GT_BRIDGE_SOURCE_IO;
        } else if (!S_ISDIR(s->roots[0].st_mode) || !s->roots[0].st_nlink) {
            status = GT_BRIDGE_SOURCE_UNSUPPORTED;
        }
    }
    if (status == GT_BRIDGE_SOURCE_OK) {
        status = source_access(s);
    }
    if (status == GT_BRIDGE_SOURCE_OK) {
        s->directory[0] = fcntl(config->root_fd, F_DUPFD_CLOEXEC, 3);
        if (s->directory[0] < 0) {
            status = GT_BRIDGE_SOURCE_IO;
        }
    }
    for (size_t i = 0; status == GT_BRIDGE_SOURCE_OK && i < s->count; i++) {
        status = source_access(s);
        if (status != GT_BRIDGE_SOURCE_OK) {
            break;
        }
        struct stat before, after;
        if (fstatat(s->directory[i], s->names[i], &before, AT_SYMLINK_NOFOLLOW)) {
            status = errno == ENOENT   ? GT_BRIDGE_SOURCE_NOT_FOUND
                     : errno == EACCES ? GT_BRIDGE_SOURCE_DENIED
                                       : GT_BRIDGE_SOURCE_IO;
            break;
        }
        bool final = i + 1 == s->count;
        if (final ? !S_ISREG(before.st_mode) : !S_ISDIR(before.st_mode)) {
            status = GT_BRIDGE_SOURCE_UNSUPPORTED;
            break;
        }
        status = source_access(s);
        if (status != GT_BRIDGE_SOURCE_OK) {
            break;
        }
        int fd = openat(s->directory[i],
                        s->names[i],
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));
        if (fd < 0) {
            status = errno == ELOOP || errno == ENOTDIR ? GT_BRIDGE_SOURCE_CHANGED
                     : errno == EACCES                  ? GT_BRIDGE_SOURCE_DENIED
                                                        : GT_BRIDGE_SOURCE_IO;
            break;
        }
        if (final) {
            s->file = fd;
        } else {
            s->directory[i + 1] = fd;
        }
        if (fstat(fd, &after)) {
            status = GT_BRIDGE_SOURCE_IO;
            break;
        }
        if (!source_inode(&before, &after) ||
            (final ? !S_ISREG(after.st_mode) : !S_ISDIR(after.st_mode))) {
            status = GT_BRIDGE_SOURCE_CHANGED;
            break;
        }
        if (final) {
            if (!source_version(&before, &after)) {
                status = GT_BRIDGE_SOURCE_CHANGED;
                break;
            }
            s->version = after;
            if (after.st_size < 0) {
                status = GT_BRIDGE_SOURCE_UNSUPPORTED;
            } else if ((uint64_t)after.st_size > s->limit) {
                status = GT_BRIDGE_SOURCE_LIMIT;
            }
        } else {
            s->roots[i + 1] = after;
        }
    }
    if (status == GT_BRIDGE_SOURCE_OK) {
        status = source_check(s);
    }
    if (status != GT_BRIDGE_SOURCE_OK) {
        if (!source_close_fds(s)) {
            status = GT_BRIDGE_SOURCE_IO;
        }
        gt__secure_erase(s, sizeof(*s));
        gt__plugin_memory_free(&a, s, sizeof(*s));
        return status;
    }
    strcpy(s->info.descriptor.mime, config->mime);
    strcpy(s->info.descriptor.display_name, s->names[s->count - 1]);
    s->info.descriptor.size_known = true;
    s->info.descriptor.size = (uint64_t)s->version.st_size;
    s->info.phase = GT_BRIDGE_SOURCE_HASHING;
    gt__sha256_init(&s->hash);
    *out = s;
    return GT_BRIDGE_SOURCE_OK;
}

static gt_bridge_source_status_t source_transfer_status(gt_bridge_resource_status_t status) {
    switch (status) {
        case GT_BRIDGE_RESOURCE_OK:
            return GT_BRIDGE_SOURCE_OK;
        case GT_BRIDGE_RESOURCE_WAIT:
            return GT_BRIDGE_SOURCE_WAIT;
        case GT_BRIDGE_RESOURCE_DENIED:
            return GT_BRIDGE_SOURCE_DENIED;
        case GT_BRIDGE_RESOURCE_UNSUPPORTED:
            return GT_BRIDGE_SOURCE_UNSUPPORTED;
        case GT_BRIDGE_RESOURCE_LIMIT:
            return GT_BRIDGE_SOURCE_LIMIT;
        case GT_BRIDGE_RESOURCE_CLOSED:
            return GT_BRIDGE_SOURCE_CLOSED;
        default:
            return GT_BRIDGE_SOURCE_IO;
    }
}

static void source_poll(gt_bridge_source_t *s) {
    if (!s->info.transfer_handle) {
        return;
    }
    gt_bridge_transfer_result_t r;
    if (!gt__bridge_session_resource_result(s->session, &s->lease, s->info.transfer_handle, &r)) {
        source_fail(s, GT_BRIDGE_SOURCE_IO);
        return;
    }
    s->info.transfer = r;
    if (r.confirmed) {
        if (r.state == GT_BRIDGE_TRANSFER_SUCCEEDED) {
            s->info.phase = GT_BRIDGE_SOURCE_SUCCEEDED;
            s->info.error = GT_BRIDGE_SOURCE_OK;
        } else if (r.state == GT_BRIDGE_TRANSFER_CANCELLED) {
            if (s->info.error == GT_BRIDGE_SOURCE_OK ||
                s->info.error == GT_BRIDGE_SOURCE_CANCELLED ||
                s->info.error == GT_BRIDGE_SOURCE_CLOSED) {
                s->info.phase = GT_BRIDGE_SOURCE_STOPPED;
                s->info.error = GT_BRIDGE_SOURCE_CANCELLED;
            } else {
                s->info.phase = GT_BRIDGE_SOURCE_FAILED;
            }
        } else if (r.state == GT_BRIDGE_TRANSFER_UNKNOWN) {
            s->info.phase = GT_BRIDGE_SOURCE_UNCERTAIN;
            s->info.error = GT_BRIDGE_SOURCE_UNKNOWN;
        } else {
            s->info.phase = GT_BRIDGE_SOURCE_FAILED;
            if (s->info.error == GT_BRIDGE_SOURCE_OK) {
                s->info.error = !strcmp(r.code, "BUSY")             ? GT_BRIDGE_SOURCE_BUSY
                                : !strcmp(r.code, "RESOURCE_LIMIT") ? GT_BRIDGE_SOURCE_LIMIT
                                : !strcmp(r.code, "DENIED")         ? GT_BRIDGE_SOURCE_DENIED
                                : !strcmp(r.code, "UNSUPPORTED")    ? GT_BRIDGE_SOURCE_UNSUPPORTED
                                : !strcmp(r.code, "INTEGRITY")      ? GT_BRIDGE_SOURCE_CHANGED
                                                                    : GT_BRIDGE_SOURCE_IO;
            }
        }
    } else if (r.state == GT_BRIDGE_TRANSFER_UNKNOWN) {
        s->info.phase = GT_BRIDGE_SOURCE_UNCERTAIN;
        s->info.error = GT_BRIDGE_SOURCE_UNKNOWN;
    } else if (r.state == GT_BRIDGE_TRANSFER_CANCELLED) {
        s->info.phase = s->info.error == GT_BRIDGE_SOURCE_OK ||
                                s->info.error == GT_BRIDGE_SOURCE_CANCELLED ||
                                s->info.error == GT_BRIDGE_SOURCE_CLOSED
                            ? GT_BRIDGE_SOURCE_STOPPED
                            : GT_BRIDGE_SOURCE_FAILED;
        if (s->info.error == GT_BRIDGE_SOURCE_OK) {
            s->info.error = GT_BRIDGE_SOURCE_CANCELLED;
        }
    }
}

gt_bridge_source_status_t gt__bridge_source_send(gt_bridge_source_t *s) {
    if (!s || s->info.closed) {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    if (s->info.phase != GT_BRIDGE_SOURCE_READY) {
        return GT_BRIDGE_SOURCE_WAIT;
    }
    gt_bridge_source_status_t status = source_access(s);
    if (status == GT_BRIDGE_SOURCE_OK) {
        status = source_check(s);
    }
    if (status != GT_BRIDGE_SOURCE_OK) {
        source_fail(s, status);
        return status;
    }
    status = source_transfer_status(gt__bridge_session_send_resource(
        s->session, &s->lease, &s->info.descriptor, &s->info.transfer_handle));
    if (status == GT_BRIDGE_SOURCE_OK) {
        s->info.phase = GT_BRIDGE_SOURCE_SENDING;
    } else if (status != GT_BRIDGE_SOURCE_WAIT) {
        source_fail(s, status);
    }
    return status;
}

static ssize_t source_read(gt_bridge_source_t *s, void *p, size_t n, uint64_t offset) {
    return pread(s->file, p, n, (off_t)offset);
}

gt_bridge_source_phase_t gt__bridge_source_step(gt_bridge_source_t *s) {
    if (!s) {
        return GT_BRIDGE_SOURCE_FAILED;
    }
    if (s->info.transfer_handle) {
        source_poll(s);
    }
    if (source_terminal(s) || s->info.closed) {
        return s->info.phase;
    }
    if (s->info.phase == GT_BRIDGE_SOURCE_CANCELLING || s->info.phase == GT_BRIDGE_SOURCE_RECEIPT) {
        return s->info.phase;
    }
    gt_bridge_source_status_t status = source_access(s);
    if (status != GT_BRIDGE_SOURCE_OK) {
        source_fail(s, status);
        return s->info.phase;
    }
    if ((status = source_check(s)) != GT_BRIDGE_SOURCE_OK) {
        source_fail(s, status);
        return s->info.phase;
    }
    if (s->info.phase == GT_BRIDGE_SOURCE_READY) {
        return s->info.phase;
    }
    if (s->info.phase == GT_BRIDGE_SOURCE_HASHING) {
        uint64_t offset = s->info.hashed;
        size_t n =
            offset == s->info.descriptor.size ? 1 : (size_t)(s->info.descriptor.size - offset);
        if (n > sizeof(s->buffer)) {
            n = sizeof(s->buffer);
        }
        ssize_t count = source_read(s, s->buffer, n, offset);
        int read_error = errno;
        if ((status = source_access(s)) != GT_BRIDGE_SOURCE_OK) {
            source_fail(s, status);
            return s->info.phase;
        }
        if (count < 0) {
            if (read_error != EINTR && read_error != EAGAIN) {
                source_fail(s, GT_BRIDGE_SOURCE_IO);
            }
            return s->info.phase;
        }
        if ((status = source_check(s)) != GT_BRIDGE_SOURCE_OK) {
            source_fail(s, status);
            return s->info.phase;
        }
        if (offset == s->info.descriptor.size) {
            if (count) {
                source_fail(s, GT_BRIDGE_SOURCE_CHANGED);
            } else if (!gt__sha256_final(&s->hash, s->info.descriptor.sha256)) {
                source_fail(s, GT_BRIDGE_SOURCE_IO);
            } else {
                s->hash_eof = true;
                s->info.phase = GT_BRIDGE_SOURCE_READY;
            }
        } else if (count <= 0 || (size_t)count > n) {
            source_fail(s, GT_BRIDGE_SOURCE_CHANGED);
        } else if (!gt__sha256_update(&s->hash, s->buffer, (size_t)count)) {
            source_fail(s, GT_BRIDGE_SOURCE_IO);
        } else {
            s->info.hashed += (uint64_t)count;
        }
        gt__secure_erase(s->buffer, sizeof(s->buffer));
        return s->info.phase;
    }
    if (s->at < s->used) {
        size_t accepted = 0;
        status = source_transfer_status(gt__bridge_session_write_resource(s->session,
                                                                          &s->lease,
                                                                          s->info.transfer_handle,
                                                                          s->buffer + s->at,
                                                                          s->used - s->at,
                                                                          &accepted));
        s->at += accepted;
        s->info.accepted += accepted;
        if (s->at == s->used) {
            gt__secure_erase(s->buffer, sizeof(s->buffer));
            s->at = s->used = 0;
        }
        if (status != GT_BRIDGE_SOURCE_OK && status != GT_BRIDGE_SOURCE_WAIT) {
            source_fail(s, status);
        }
        return s->info.phase;
    }
    uint64_t offset = s->info.read_bytes;
    size_t n = offset == s->info.descriptor.size ? 1 : (size_t)(s->info.descriptor.size - offset);
    if (n > sizeof(s->buffer)) {
        n = sizeof(s->buffer);
    }
    ssize_t count = source_read(s, s->buffer, n, offset);
    int read_error = errno;
    if ((status = source_access(s)) != GT_BRIDGE_SOURCE_OK) {
        source_fail(s, status);
        return s->info.phase;
    }
    if (count < 0) {
        if (read_error != EINTR && read_error != EAGAIN) {
            source_fail(s, GT_BRIDGE_SOURCE_IO);
        }
        return s->info.phase;
    }
    if ((status = source_check(s)) != GT_BRIDGE_SOURCE_OK) {
        source_fail(s, status);
        return s->info.phase;
    }
    if (offset == s->info.descriptor.size) {
        if (count) {
            source_fail(s, GT_BRIDGE_SOURCE_CHANGED);
            return s->info.phase;
        }
        s->send_eof = true;
        gt_bridge_resource_status_t finished = gt__bridge_session_finish_resource(
            s->session, &s->lease, s->info.transfer_handle, s->hash_eof && s->send_eof);
        status = finished == GT_BRIDGE_RESOURCE_INVALID ? GT_BRIDGE_SOURCE_CHANGED
                                                        : source_transfer_status(finished);
        if (status == GT_BRIDGE_SOURCE_OK) {
            s->info.phase = GT_BRIDGE_SOURCE_RECEIPT;
        } else {
            source_fail(s, status);
        }
    } else if (count <= 0 || (size_t)count > n) {
        source_fail(s, GT_BRIDGE_SOURCE_CHANGED);
    } else {
        s->used = (size_t)count;
        s->at = 0;
        s->info.read_bytes += (uint64_t)count;
    }
    return s->info.phase;
}

void gt__bridge_source_cancel(gt_bridge_source_t *s) {
    if (!s || source_terminal(s)) {
        return;
    }
    if (s->info.phase != GT_BRIDGE_SOURCE_CANCELLING) {
        source_fail(s, GT_BRIDGE_SOURCE_CANCELLED);
    }
    if (s->info.transfer_handle) {
        source_poll(s);
    }
}

bool gt__bridge_source_info(const gt_bridge_source_t *s, gt_bridge_source_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!s || !out) {
        return false;
    }
    *out = s->info;
    return true;
}

gt_bridge_source_status_t gt__bridge_source_close(gt_bridge_source_t *s) {
    if (!s) {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    if (!s->close_requested) {
        s->close_requested = true;
        gt__bridge_source_cancel(s);
    }
    if (!s->info.closed) {
        bool closed = source_close_fds(s);
        s->info.closed = true;
        if (!closed) {
            return GT_BRIDGE_SOURCE_IO;
        }
    }
    if (s->info.transfer_handle) {
        source_poll(s);
    }
    return source_terminal(s) ? GT_BRIDGE_SOURCE_OK : GT_BRIDGE_SOURCE_WAIT;
}

gt_bridge_source_status_t gt__bridge_source_destroy(gt_bridge_source_t *s) {
    if (!s) {
        return GT_BRIDGE_SOURCE_INVALID;
    }
    gt_bridge_source_status_t status = gt__bridge_source_close(s);
    if (status != GT_BRIDGE_SOURCE_OK) {
        return status;
    }
    if (s->info.transfer_handle) {
        status = source_transfer_status(
            gt__bridge_session_forget_resource(s->session, &s->lease, s->info.transfer_handle));
        if (status != GT_BRIDGE_SOURCE_OK) {
            return status;
        }
    }
    gt_allocator_t a = s->allocator;
    gt__secure_erase(s, sizeof(*s));
    gt__plugin_memory_free(&a, s, sizeof(*s));
    return GT_BRIDGE_SOURCE_OK;
}
