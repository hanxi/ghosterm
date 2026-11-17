#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "release.h"
#include "catalog.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef GTERM_HAVE_RELEASE
#include <zlib.h>

typedef struct {
    char *path;
    uint32_t offset, compressed, count, crc;
    uint16_t flags, method;
    bool executable;
} member_t;

static uint16_t u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool span(size_t size, size_t at, size_t n) {
    return at <= size && n <= size - at;
}

static bool path_ok(const unsigned char *p, size_t n) {
    if (!n || n > 4096 || p[0] == '/' || p[n - 1] == '/' || !gterm_catalog_utf8(p, n)) {
        return false;
    }
    unsigned depth = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && p[end] != '/') {
            if (p[end] < 32 || p[end] == 127 || p[end] == '\\' || p[end] == ':') {
                return false;
            }
            end++;
        }
        size_t count = end - at;
        if (!count || (count == 1 && p[at] == '.') ||
            (count == 2 && p[at] == '.' && p[at + 1] == '.') || ++depth > 32) {
            return false;
        }
        at = end + 1;
    }
    return true;
}

static int ordered(const void *a, const void *b) {
    const member_t *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

static bool write_all(int fd, const void *data, size_t count, volatile sig_atomic_t *cancelled) {
    size_t at = 0;
    while (at < count && !*cancelled) {
        ssize_t n = write(fd, (const unsigned char *)data + at, count - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        at += (size_t)n;
    }
    return at == count;
}

static bool file_write(int root,
                       const char *path,
                       const unsigned char *data,
                       size_t count,
                       mode_t mode,
                       volatile sig_atomic_t *cancelled) {
    int fd = openat(root, path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok = write_all(fd, data, count, cancelled) && !fchmod(fd, mode) && !fsync(fd);
    if (close(fd)) {
        ok = false;
    }
    return ok;
}

static bool extract_member(int root,
                           const member_t *m,
                           const unsigned char *zip,
                           volatile sig_atomic_t *cancelled,
                           unsigned *entries) {
    char *path = strdup(m->path);
    if (!path) {
        return false;
    }
    int directory = fcntl(root, F_DUPFD_CLOEXEC, 3), output = -1;
    bool ok = false;
    if (directory < 0) {
        goto done;
    }
    char *leaf = path;
    for (char *slash = strchr(leaf, '/'); slash; slash = strchr(leaf, '/')) {
        *slash = 0;
        if (!mkdirat(directory, leaf, 0700)) {
            if (++*entries > 4096) {
                goto done;
            }
        } else if (errno != EEXIST) {
            goto done;
        }
        int next = openat(directory, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            goto done;
        }
        struct stat s;
        bool own = !fstat(next, &s) && s.st_uid == geteuid() && (s.st_mode & 07777) == 0700;
        if (close(directory)) {
            own = false;
        }
        directory = next;
        if (!own) {
            goto done;
        }
        leaf = slash + 1;
    }
    output = openat(directory, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) {
        goto done;
    }
    const unsigned char *input = zip + m->offset;
    uLong crc = crc32(0, NULL, 0);
    uint64_t produced = 0;
    if (!m->method) {
        if (m->compressed != m->count || !write_all(output, input, m->count, cancelled)) {
            goto done;
        }
        crc = crc32(crc, input, m->count);
        produced = m->count;
    } else {
        z_stream stream = {0};
        if (inflateInit2(&stream, -15) != Z_OK) {
            goto done;
        }
        stream.next_in = (Bytef *)input;
        stream.avail_in = m->compressed;
        unsigned char buffer[16384];
        int result = Z_OK;
        while (result == Z_OK && !*cancelled) {
            stream.next_out = buffer;
            stream.avail_out = sizeof(buffer);
            uLong before = stream.total_in;
            result = inflate(&stream, Z_NO_FLUSH);
            size_t count = sizeof(buffer) - stream.avail_out;
            produced += count;
            if (produced > m->count || (result == Z_OK && !count && before == stream.total_in) ||
                !write_all(output, buffer, count, cancelled)) {
                result = Z_DATA_ERROR;
                break;
            }
            crc = crc32(crc, buffer, (uInt)count);
        }
        bool complete = result == Z_STREAM_END && stream.total_in == m->compressed;
        if (inflateEnd(&stream) != Z_OK || !complete) {
            goto done;
        }
    }
    ok = produced == m->count && crc == m->crc && !*cancelled &&
         !fchmod(output, m->executable ? 0500 : 0400) && !fsync(output);
done:
    if (output >= 0 && close(output)) {
        ok = false;
    }
    if (directory >= 0 && close(directory)) {
        ok = false;
    }
    free(path);
    return ok;
}

static int
unpack(int root, const unsigned char *zip, size_t size, volatile sig_atomic_t *cancelled) {
    if (size < 22) {
        return 2;
    }
    const unsigned char *end = zip + size - 22;
    uint32_t central = u32(end + 16), central_size = u32(end + 12);
    uint16_t count = u16(end + 10);
    if (u32(end) != 0x06054b50 || u16(end + 4) || u16(end + 6) || u16(end + 20) || !count ||
        count > 1024 || u16(end + 8) != count || !span(size - 22, central, central_size) ||
        (size_t)central + central_size != size - 22 || central_size < (uint32_t)count * 46 ||
        central_size > (uint32_t)count * (46 + 4096)) {
        return 2;
    }
    member_t *members = calloc(count, sizeof(*members));
    if (!members) {
        return 5;
    }
    size_t at = central;
    uint64_t total = 0;
    int result = 2;
    unsigned entries = count;
    for (uint16_t i = 0; i < count; i++) {
        if (!span(size - 22, at, 46) || u32(zip + at) != 0x02014b50) {
            goto done;
        }
        const unsigned char *p = zip + at;
        size_t n = u16(p + 28);
        uint32_t mode = u32(p + 38);
        member_t *m = &members[i];
        m->flags = u16(p + 8);
        m->method = u16(p + 10);
        m->crc = u32(p + 16);
        m->compressed = u32(p + 20);
        m->count = u32(p + 24);
        m->offset = u32(p + 42);
        m->executable = ((mode >> 16) & 0111) != 0;
        if (u16(p + 30) || u16(p + 32) || u16(p + 34) || !span(size - 22, at + 46, n) ||
            !path_ok(p + 46, n) || (m->flags & ~0x800) || (m->method != 0 && m->method != 8) ||
            (mode & 16) || (((mode >> 16) & S_IFMT) != 0 && ((mode >> 16) & S_IFMT) != S_IFREG) ||
            m->count > 16777216 || m->compressed > 75497472 || (total += m->count) > 67108864) {
            goto done;
        }
        if (!(m->flags & 0x800)) {
            for (size_t j = 0; j < n; j++) {
                if (p[46 + j] >= 128) {
                    goto done;
                }
            }
        }
        m->path = malloc(n + 1);
        if (!m->path) {
            result = 5;
            goto done;
        }
        memcpy(m->path, p + 46, n);
        m->path[n] = 0;
        at += 46 + n;
    }
    if (at != size - 22) {
        goto done;
    }
    qsort(members, count, sizeof(*members), ordered);
    at = 0;
    for (uint16_t i = 0; i < count; i++) {
        member_t *m = &members[i];
        size_t n = strlen(m->path);
        if (m->offset != at || !span(central, at, 30 + n) || u32(zip + at) != 0x04034b50 ||
            u16(zip + at + 6) != m->flags || u16(zip + at + 8) != m->method ||
            u32(zip + at + 14) != m->crc || u32(zip + at + 18) != m->compressed ||
            u32(zip + at + 22) != m->count || u16(zip + at + 26) != n || u16(zip + at + 28) ||
            memcmp(zip + at + 30, m->path, n)) {
            goto done;
        }
        m->offset = (uint32_t)(at + 30 + n);
        if (!span(central, m->offset, m->compressed)) {
            goto done;
        }
        at = (size_t)m->offset + m->compressed;
    }
    if (at != central) {
        goto done;
    }
    for (uint16_t i = 0; i < count; i++) {
        if (*cancelled) {
            result = 130;
            goto done;
        }
        if (!extract_member(root, &members[i], zip, cancelled, &entries)) {
            result = 5;
            goto done;
        }
    }
    result = 0;
done:
    for (uint16_t i = 0; i < count; i++) {
        free(members[i].path);
    }
    free(members);
    return result;
}

static bool same(int parent, const char *name, const struct stat *identity) {
    struct stat s;
    return !fstatat(parent, name, &s, AT_SYMLINK_NOFOLLOW) && s.st_dev == identity->st_dev &&
           s.st_ino == identity->st_ino;
}

/* Only descend opened, no-follow directories of the just-created transaction.
 * Never chmod regular files (which could have gained an external hard link). */
static bool tree(int fd, bool remove, unsigned depth) {
    if (depth > 34 || (remove && fchmod(fd, 0700))) {
        return false;
    }
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (copy < 0) {
        return false;
    }
    DIR *stream = fdopendir(copy);
    if (!stream) {
        close(copy);
        return false;
    }
    bool ok = true;
    unsigned count = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno) {
                ok = false;
            }
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        struct stat s;
        if (++count > 4096 || fstatat(fd, name, &s, AT_SYMLINK_NOFOLLOW)) {
            ok = false;
            break;
        }
        if (S_ISDIR(s.st_mode)) {
            int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat actual;
            bool nested = child >= 0 && !fstat(child, &actual) && actual.st_uid == geteuid() &&
                          actual.st_dev == s.st_dev && actual.st_ino == s.st_ino &&
                          tree(child, remove, depth + 1);
            if (child >= 0 && close(child)) {
                nested = false;
            }
            if (!nested || !same(fd, name, &s) || (remove && unlinkat(fd, name, AT_REMOVEDIR))) {
                ok = false;
                break;
            }
        } else if (remove) {
            if (!same(fd, name, &s) || unlinkat(fd, name, 0)) {
                ok = false;
                break;
            }
        } else if (!S_ISREG(s.st_mode) || s.st_nlink != 1 || s.st_uid != geteuid() ||
                   (s.st_mode & 0222)) {
            ok = false;
            break;
        }
    }
    if (closedir(stream)) {
        ok = false;
    }
    if (!remove && (fchmod(fd, 0500) || fsync(fd))) {
        ok = false;
    }
    return ok;
}

static int publish(int parent, const char *stage, const char *version) {
#ifdef __APPLE__
    return renameatx_np(parent, stage, parent, version, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, parent, stage, parent, version, 1);
#else
    (void)parent;
    (void)stage;
    (void)version;
    errno = ENOTSUP;
    return -1;
#endif
}

static int check_payload(int fd,
                         const char *host,
                         const gterm_release_t *proof,
                         volatile sig_atomic_t *cancelled) {
    gt_ext_package_t *package = NULL;
    gt_ext_package_config_t config = {fd, 1024, 4096, 16777216, 67108864};
    int result = 5;
    if (gt_ext_package_open(&config, &package) != GT_OK) {
        return result;
    }
    gt_ext_package_phase_t phase;
    do {
        if (*cancelled) {
            gt_ext_package_stop(package);
        }
        phase = gt_ext_package_step(package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    gt_ext_package_check_config_t check = {.host_json = host};
    gt_ext_package_metadata_t metadata;
    if (phase == GT_EXT_PACKAGE_CAPTURED &&
        gt_ext_package_check(package, &check) == GT_EXT_PACKAGE_CHECKED &&
        gt_ext_package_metadata(package, &metadata)) {
        result = metadata.executor && !strcmp(metadata.id, proof->id) &&
                         !strcmp(metadata.version, proof->version) &&
                         !strcmp(metadata.publisher, proof->publisher) &&
                         !strcmp(metadata.package_digest, proof->digest)
                     ? 0
                     : 6;
    } else {
        gt_ext_package_info_t info;
        if (gt_ext_package_info(package, &info)) {
            result = info.error == GT_EXT_PACKAGE_UNKNOWN       ? 3
                     : info.error == GT_EXT_PACKAGE_UNSUPPORTED ? 4
                     : info.error == GT_EXT_PACKAGE_INVALID     ? 2
                     : info.error == GT_EXT_PACKAGE_DENIED      ? 6
                                                                : 5;
        }
    }
    if (gt_ext_package_destroy(&package) != GT_OK) {
        result = 5;
    }
    return *cancelled ? 130 : result;
}
#endif

int gterm_release_install(const char *store,
                          const char *artifact,
                          const char *descriptor,
                          const char *signature,
                          const char *policy,
                          const char *host,
                          char **directory,
                          char policy_digest[65],
                          bool *installed,
                          volatile sig_atomic_t *cancelled) {
    *directory = NULL;
    *installed = false;
    policy_digest[0] = 0;
#ifndef GTERM_HAVE_RELEASE
    (void)store;
    (void)artifact;
    (void)descriptor;
    (void)signature;
    (void)policy;
    (void)host;
    (void)cancelled;
    return 4;
#else
    gterm_release_t proof;
    int result = gterm_release_authenticate(descriptor, signature, policy, NULL, &proof, cancelled);
    if (result) {
        return result;
    }
    mode_t original_umask = umask(0077);
    size_t zip_size = 0;
    unsigned char *zip = gterm_release_file(artifact, 75497472, &zip_size);
    char hash[65];
    int parent = -1, lock = -1, stage = -1, payload = -1;
    bool reserved = false, created = false;
    struct stat parent_identity = {0}, stage_identity = {0}, intent_identity = {0};
    char stage_name[48] = {0}, intent_name[48] = {0};
    result = 6;
    if (!zip || zip_size != proof.artifact_bytes || !gterm_release_hash(zip, zip_size, hash) ||
        strcmp(hash, proof.artifact_digest)) {
        goto done;
    }
    result = 2;
    if (!gterm_catalog_path(store) || strlen(store) > 3500) {
        goto done;
    }
    result = 5;
    mode_t previous = umask(0077);
    int made = mkdir(store, 0700), saved = errno;
    umask(previous);
    errno = saved;
    if (made && errno != EEXIST) {
        goto done;
    }
    parent = open(store, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0 || fstat(parent, &parent_identity) || parent_identity.st_uid != geteuid() ||
        (parent_identity.st_mode & 07777) != 0700) {
        goto done;
    }
    char *parent_path = strdup(store);
    if (!parent_path) {
        goto done;
    }
    char *slash = strrchr(parent_path, '/');
    *slash = 0;
    int ancestor =
        open(*parent_path ? parent_path : "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat rooted;
    bool rooted_ok = ancestor >= 0 && !fstatat(ancestor, slash + 1, &rooted, AT_SYMLINK_NOFOLLOW) &&
                     rooted.st_dev == parent_identity.st_dev &&
                     rooted.st_ino == parent_identity.st_ino && !fsync(ancestor);
    if (ancestor >= 0 && close(ancestor)) {
        rooted_ok = false;
    }
    free(parent_path);
    if (!rooted_ok) {
        goto done;
    }
    previous = umask(0077);
    lock =
        openat(parent, ".store.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    saved = errno;
    umask(previous);
    errno = saved;
    struct stat lock_identity;
    struct flock lease = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (lock < 0 || fstat(lock, &lock_identity) || !S_ISREG(lock_identity.st_mode) ||
        lock_identity.st_uid != geteuid() || lock_identity.st_nlink != 1 ||
        (lock_identity.st_mode & 07777) != 0600) {
        goto done;
    }
    if (fcntl(lock, F_SETLK, &lease)) {
        result = 11;
        goto done;
    }
    int copy = fcntl(parent, F_DUPFD_CLOEXEC, 3);
    DIR *stream = copy < 0 ? NULL : fdopendir(copy);
    if (!stream) {
        if (copy >= 0) {
            close(copy);
        }
        goto done;
    }
    unsigned count = 0;
    bool enumerated = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno) {
                enumerated = false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            strcmp(entry->d_name, ".store.lock")) {
            count++;
        }
        if (count >= 64) {
            enumerated = false;
            break;
        }
    }
    if (closedir(stream)) {
        enumerated = false;
    }
    if (!enumerated) {
        goto done;
    }
    unsigned char random[16];
    int entropy = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t got = entropy < 0 ? -1 : read(entropy, random, sizeof(random));
    bool random_ok = entropy >= 0 && got == (ssize_t)sizeof(random);
    if (entropy >= 0 && close(entropy)) {
        random_ok = false;
    }
    if (!random_ok) {
        goto done;
    }
    memcpy(stage_name, ".prepare-", 9);
    memcpy(intent_name, ".intent-", 8);
    for (size_t i = 0; i < 16; i++) {
        static const char digits[] = "0123456789abcdef";
        stage_name[9 + i * 2] = digits[random[i] >> 4];
        stage_name[10 + i * 2] = digits[random[i] & 15];
        intent_name[8 + i * 2] = digits[random[i] >> 4];
        intent_name[9 + i * 2] = digits[random[i] & 15];
    }
    if (!file_write(parent,
                    intent_name,
                    (const unsigned char *)stage_name,
                    strlen(stage_name),
                    0400,
                    cancelled) ||
        fstatat(parent, intent_name, &intent_identity, AT_SYMLINK_NOFOLLOW) || fsync(parent)) {
        goto done;
    }
    reserved = true;
    if (mkdirat(parent, stage_name, 0700)) {
        goto done;
    }
    created = true;
    stage = openat(parent, stage_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (stage < 0 || fstat(stage, &stage_identity)) {
        goto done;
    }
    if (fsync(parent) || mkdirat(stage, "payload", 0700)) {
        goto done;
    }
    payload = openat(stage, "payload", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (payload < 0) {
        goto done;
    }
    result = unpack(payload, zip, zip_size, cancelled);
    free(zip);
    zip = NULL;
    if (result) {
        goto done;
    }
    result = check_payload(payload, host, &proof, cancelled);
    if (result) {
        goto done;
    }
    result = 5;
    if (!file_write(
            stage, "release.json", proof.descriptor, proof.descriptor_size, 0400, cancelled) ||
        !file_write(stage, "release.sig", proof.signature, 64, 0400, cancelled) ||
        !tree(stage, false, 0)) {
        goto done;
    }
    /* The native process never exposes this path before publication. Recheck
     * inputs/parent and policy snapshot immediately before the atomic rename. */
    struct stat current;
    if (lstat(store, &current) || current.st_dev != parent_identity.st_dev ||
        current.st_ino != parent_identity.st_ino || current.st_uid != geteuid() ||
        (current.st_mode & 07777) != 0700 || !same(parent, stage_name, &stage_identity) ||
        !same(parent, ".store.lock", &lock_identity)) {
        result = 9;
        goto done;
    }
    size_t policy_size = 0;
    unsigned char *now = gterm_release_file(policy, 131072, &policy_size);
    bool fresh =
        now && gterm_release_hash(now, policy_size, hash) && !strcmp(hash, proof.policy_digest);
    free(now);
    if (!fresh) {
        result = 9;
        goto done;
    }
    if (*cancelled) {
        result = 130;
        goto done;
    }
    if (publish(parent, stage_name, proof.descriptor_digest)) {
        result = errno == EEXIST ? 8 : errno == ENOTSUP || errno == ENOSYS ? 4 : 5;
        goto done;
    }
    *installed = true;
    *directory = malloc(strlen(store) + strlen(proof.descriptor_digest) + 10);
    if (!*directory) {
        goto done;
    }
    sprintf(*directory, "%s/%s/payload", store, proof.descriptor_digest);
    memcpy(policy_digest, proof.policy_digest, 65);
    if (fsync(parent) || !same(parent, intent_name, &intent_identity) ||
        unlinkat(parent, intent_name, 0) || fsync(parent)) {
        goto done;
    }
    reserved = false;
    result = 0;
done:
    if (!*installed && created && same(parent, stage_name, &stage_identity)) {
        bool cleaned = tree(stage, true, 0) && same(parent, stage_name, &stage_identity) &&
                       !unlinkat(parent, stage_name, AT_REMOVEDIR);
        if (!cleaned) {
            result = 5;
        } else {
            created = false;
        }
    }
    if (!*installed && reserved && !created && same(parent, intent_name, &intent_identity)) {
        if (unlinkat(parent, intent_name, 0) || fsync(parent)) {
            result = 5;
        }
    }
    if (payload >= 0 && close(payload)) {
        result = 5;
    }
    if (stage >= 0 && close(stage)) {
        result = 5;
    }
    if (lock >= 0 && close(lock)) {
        result = 5;
    }
    if (parent >= 0 && close(parent)) {
        result = 5;
    }
    free(zip);
    gterm_release_clear(&proof);
    umask(original_umask);
    if (*cancelled && !*installed) {
        result = 130;
    }
    return result;
#endif
}
