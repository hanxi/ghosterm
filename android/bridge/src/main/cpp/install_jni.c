#define _GNU_SOURCE
#include <android/api-level.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

/* Android 10 seccomp forbids statx, even with a supporting kernel. On that
 * platform use the filesystem inode generation instead. Both are persistent
 * object metadata; absent support is explicit, never a fabricated identity. */
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_identityMetadata(
    JNIEnv *env, jobject self, jint descriptor) {
    (void)self;
    jlong result[4] = {0, 0, 0, 0};
    struct stat before;
    struct statx value;
    memset(&value, 0, sizeof(value));
    if (fstat(descriptor, &before) < 0) {
        result[0] = errno;
    } else if ((!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)) ||
               before.st_uid != geteuid() || (before.st_mode & 0077)) {
        result[0] = EINVAL;
    } else {
        int flags = fcntl(descriptor, F_GETFL);
        if (flags < 0) {
            result[0] = errno;
        } else if ((flags & O_ACCMODE) != O_RDONLY) {
            result[0] = EINVAL;
        } else if (android_get_device_api_level() < 30) {
            unsigned int generation = 0;
            if (ioctl(descriptor, FS_IOC_GETVERSION, &generation) < 0) {
                result[0] = errno;
            } else if (!generation) {
                result[0] = EOPNOTSUPP;
            } else {
                result[1] = 2; /* filesystem inode generation */
                result[2] = generation;
            }
        } else if (syscall(__NR_statx,
                           descriptor,
                           "",
                           AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                           STATX_BASIC_STATS | STATX_BTIME,
                           &value) < 0) {
            result[0] = errno;
        } else if ((value.stx_mask & (STATX_BTIME | STATX_INO | STATX_UID | STATX_TYPE)) !=
                   (STATX_BTIME | STATX_INO | STATX_UID | STATX_TYPE)) {
            result[0] = EOPNOTSUPP;
        } else if (value.stx_ino != before.st_ino || value.stx_uid != before.st_uid ||
                   value.stx_mode != before.st_mode ||
                   value.stx_dev_major != major(before.st_dev) ||
                   value.stx_dev_minor != minor(before.st_dev) ||
                   value.stx_btime.tv_nsec > 999999999) {
            result[0] = ESTALE;
        } else {
            result[1] = 1; /* statx birth time */
            result[2] = value.stx_btime.tv_sec;
            result[3] = value.stx_btime.tv_nsec;
        }
    }
    jlongArray array = (*env)->NewLongArray(env, 4);
    if (array) {
        (*env)->SetLongArrayRegion(env, array, 0, 4, result);
    }
    return array;
}

static int private_directory(int descriptor, struct stat *value) {
    if (fstat(descriptor, value) < 0) {
        return errno;
    }
    if (!S_ISDIR(value->st_mode) || value->st_uid != geteuid() || (value->st_mode & 0077)) {
        return EINVAL;
    }
    int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0) {
        return errno;
    }
    return (flags & O_ACCMODE) == O_RDONLY ? 0 : EINVAL;
}

JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_lockDirectory(
    JNIEnv *env, jobject self, jint descriptor) {
    (void)env;
    (void)self;
    struct stat value;
    int error = private_directory(descriptor, &value);
    if (!error && flock(descriptor, LOCK_EX | LOCK_NB) < 0) {
        error = errno;
    }
    return error;
}

JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_pinDirectory(
    JNIEnv *env, jobject self, jint descriptor) {
    (void)env;
    (void)self;
    struct stat value;
    int error = private_directory(descriptor, &value);
    if (!error && ((value.st_mode & 0222) || flock(descriptor, LOCK_SH | LOCK_NB) < 0)) {
        error = (value.st_mode & 0222) ? EINVAL : errno;
    }
    return error;
}

JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_publishDirectory(
    JNIEnv *env, jobject self, jint parent, jint descriptor, jbyteArray from, jbyteArray to) {
    (void)self;
    char source[46] = {0}, target[65] = {0};
    if (!from || !to || (*env)->GetArrayLength(env, from) != 45 ||
        (*env)->GetArrayLength(env, to) != 64) {
        return EINVAL;
    }
    (*env)->GetByteArrayRegion(env, from, 0, 45, (jbyte *)source);
    (*env)->GetByteArrayRegion(env, to, 0, 64, (jbyte *)target);
    if ((*env)->ExceptionCheck(env)) {
        return EINVAL;
    }
    if (memcmp(source, ".prepare-", 9)) {
        return EINVAL;
    }
    for (int i = 9; i < 45; i++) {
        if (!((source[i] >= '0' && source[i] <= '9') || (source[i] >= 'a' && source[i] <= 'f') ||
              source[i] == '-')) {
            return EINVAL;
        }
    }
    for (int i = 0; i < 64; i++) {
        if (!((target[i] >= '0' && target[i] <= '9') || (target[i] >= 'a' && target[i] <= 'f'))) {
            return EINVAL;
        }
    }
    struct stat root, selected, named;
    int error = private_directory(parent, &root);
    if (!error) {
        error = private_directory(descriptor, &selected);
    }
    if (!error && (selected.st_mode & 0222)) {
        error = EINVAL;
    }
    if (!error && fstatat(parent, source, &named, AT_SYMLINK_NOFOLLOW) < 0) {
        error = errno;
    }
    if (!error && (!S_ISDIR(named.st_mode) || selected.st_dev != named.st_dev ||
                   selected.st_ino != named.st_ino || selected.st_mode != named.st_mode ||
                   selected.st_uid != named.st_uid || selected.st_dev != root.st_dev)) {
        error = EINVAL;
    }
    /* Exactly one atomic syscall. Unsupported filesystems remain unsupported;
     * never fall back to an overwriting rename, copy, link or pre-delete. */
    if (!error && syscall(__NR_renameat2, parent, source, parent, target, RENAME_NOREPLACE) < 0) {
        error = errno;
    }
    return error;
}

JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_detachDirectory(
    JNIEnv *env, jobject self, jint parent, jint descriptor, jbyteArray from, jbyteArray to) {
    (void)self;
    char source[65] = {0}, target[47] = {0};
    int length = from ? (*env)->GetArrayLength(env, from) : 0;
    int preparing = length == 45;
    if (!from || !to || (!preparing && length != 64) || (*env)->GetArrayLength(env, to) != 46) {
        return EINVAL;
    }
    (*env)->GetByteArrayRegion(env, from, 0, length, (jbyte *)source);
    (*env)->GetByteArrayRegion(env, to, 0, 46, (jbyte *)target);
    if ((*env)->ExceptionCheck(env) || memcmp(target, ".removing-", 10)) {
        return EINVAL;
    }
    if (preparing && memcmp(source, ".prepare-", 9)) {
        return EINVAL;
    }
    for (int i = preparing ? 9 : 0; i < length; i++) {
        int separator = preparing && (i == 17 || i == 22 || i == 27 || i == 32);
        if (separator ? source[i] != '-'
                      : !((source[i] >= '0' && source[i] <= '9') ||
                          (source[i] >= 'a' && source[i] <= 'f'))) {
            return EINVAL;
        }
    }
    for (int i = 10; i < 46; i++) {
        int separator = i == 18 || i == 23 || i == 28 || i == 33;
        if (separator ? target[i] != '-'
                      : !((target[i] >= '0' && target[i] <= '9') ||
                          (target[i] >= 'a' && target[i] <= 'f'))) {
            return EINVAL;
        }
    }
    struct stat root, selected, named;
    int error = private_directory(parent, &root);
    if (!error) {
        error = private_directory(descriptor, &selected);
    }
    if (!error && !preparing && (selected.st_mode & 0222)) {
        error = EINVAL;
    }
    if (!error && fstatat(parent, source, &named, AT_SYMLINK_NOFOLLOW) < 0) {
        error = errno;
    }
    if (!error && (!S_ISDIR(named.st_mode) || selected.st_dev != named.st_dev ||
                   selected.st_ino != named.st_ino || selected.st_uid != named.st_uid ||
                   selected.st_mode != named.st_mode || selected.st_dev != root.st_dev)) {
        error = EINVAL;
    }
    if (!error && syscall(__NR_renameat2, parent, source, parent, target, RENAME_NOREPLACE) < 0) {
        error = errno;
    }
    return error;
}

/* A separate entry prevents unknown-tree cleanup from relaxing the ordinary
 * frozen-version/source-name contract. Kotlin owns the reviewed intent. */
JNIEXPORT jint JNICALL
Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_detachUnclassifiedDirectory(
    JNIEnv *env, jobject self, jint parent, jint descriptor, jbyteArray from, jbyteArray to) {
    (void)self;
    char source_name[256] = {0}, target_name[47] = {0};
    int length = from ? (*env)->GetArrayLength(env, from) : 0;
    if (!to || length < 1 || length > 255 || (*env)->GetArrayLength(env, to) != 46) {
        return EINVAL;
    }
    (*env)->GetByteArrayRegion(env, from, 0, length, (jbyte *)source_name);
    (*env)->GetByteArrayRegion(env, to, 0, 46, (jbyte *)target_name);
    if ((*env)->ExceptionCheck(env) || !strcmp(source_name, ".") || !strcmp(source_name, "..") ||
        !strcmp(source_name, "selection-v1.json") || !strcmp(source_name, "removal-v1.jsonl") ||
        !strncmp(source_name, ".install-", 9) || !strncmp(source_name, ".intent-", 8) ||
        !strncmp(source_name, ".selection-", 11) || !strncmp(source_name, ".removal-record-", 16) ||
        memcmp(target_name, ".removing-", 10)) {
        return EINVAL;
    }
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)source_name[i];
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') {
            return EINVAL;
        }
    }
    for (int i = 10; i < 46; i++) {
        int dash = i == 18 || i == 23 || i == 28 || i == 33;
        if (dash ? target_name[i] != '-'
                 : !((target_name[i] >= '0' && target_name[i] <= '9') ||
                     (target_name[i] >= 'a' && target_name[i] <= 'f'))) {
            return EINVAL;
        }
    }
    struct stat root_value, candidate, original;
    int error = private_directory(parent, &root_value);
    if (!error) {
        error = private_directory(descriptor, &candidate);
    }
    if (!error && fstatat(parent, source_name, &original, AT_SYMLINK_NOFOLLOW) < 0) {
        error = errno;
    }
    if (!error &&
        (!S_ISDIR(original.st_mode) || candidate.st_dev != original.st_dev ||
         candidate.st_ino != original.st_ino || candidate.st_uid != original.st_uid ||
         candidate.st_mode != original.st_mode || candidate.st_dev != root_value.st_dev)) {
        error = EINVAL;
    }
    if (!error &&
        syscall(__NR_renameat2, parent, source_name, parent, target_name, RENAME_NOREPLACE) < 0) {
        error = errno;
    }
    return error;
}
