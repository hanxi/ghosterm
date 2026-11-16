#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

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
