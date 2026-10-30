#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ghosterm.h"

static void fail(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env, type, message);
}
JNIEXPORT jobjectArray JNICALL
Java_com_hanxi_ghosterm_bridge_KnownHostsBridge_removeBytes(JNIEnv *env, jobject self,
    jbyteArray path, jbyteArray expected, jint index) {
    (void)self;
    if (!path || !expected || index < 0) { fail(env, "主机验证记录参数无效"); return NULL; }
    jsize path_size = (*env)->GetArrayLength(env, path), size = (*env)->GetArrayLength(env, expected);
    if (path_size < 1 || path_size > 65536 || size > 1024 * 1024) { fail(env, "主机验证记录过大"); return NULL; }
    char *file = malloc((size_t)path_size + 1), *bytes = malloc((size_t)size + 1);
    if (!file || !bytes) { free(file); free(bytes); fail(env, "内存不足"); return NULL; }
    (*env)->GetByteArrayRegion(env, path, 0, path_size, (jbyte *)file);
    (*env)->GetByteArrayRegion(env, expected, 0, size, (jbyte *)bytes);
    if ((*env)->ExceptionCheck(env)) { free(file); free(bytes); return NULL; }
    file[path_size] = bytes[size] = 0;
    if (memchr(file, 0, (size_t)path_size)) { free(file); free(bytes); fail(env, "路径包含 NUL"); return NULL; }
    gt_known_hosts_update_result_t update;
    gt_status_t status = gt_known_hosts_remove(file, bytes, (size_t)size, (size_t)index, &update);
    free(file); free(bytes);
    char code[16]; snprintf(code, sizeof code, "%d", status);
    const char *fields[] = {code, update.committed ? "1" : "0", update.conflict ? "1" : "0",
        status == GT_OK ? "" : gt_last_error()};
    jclass type = (*env)->FindClass(env, "[B");
    jobjectArray result = type ? (*env)->NewObjectArray(env, 4, type, NULL) : NULL;
    for (int i = 0; result && i < 4 && !(*env)->ExceptionCheck(env); i++) {
        jsize length = (jsize)strlen(fields[i]);
        jbyteArray field = (*env)->NewByteArray(env, length);
        if (!field) break;
        (*env)->SetByteArrayRegion(env, field, 0, length, (const jbyte *)fields[i]);
        (*env)->SetObjectArrayElement(env, result, i, field);
        (*env)->DeleteLocalRef(env, field);
    }
    return result;
}
JNIEXPORT jobjectArray JNICALL
Java_com_hanxi_ghosterm_bridge_KnownHostsBridge_recordBytes(JNIEnv *env, jobject self, jbyteArray input) {
    (void)self;
    if (!input) { fail(env, "主机验证记录缺失"); return NULL; }
    jsize size = (*env)->GetArrayLength(env, input);
    if (size > 1024 * 1024) { fail(env, "主机验证记录过大"); return NULL; }
    char *text = malloc((size_t)size + 1);
    if (!text) { fail(env, "内存不足"); return NULL; }
    (*env)->GetByteArrayRegion(env, input, 0, size, (jbyte *)text);
    if ((*env)->ExceptionCheck(env)) { free(text); return NULL; }
    text[size] = 0;
    gt_known_hosts_t *snapshot = gt_known_hosts_parse(text, (size_t)size);
    free(text);
    if (!snapshot) { fail(env, "无法读取主机验证记录：包含 NUL 或超过记录限额"); return NULL; }
    size_t count = gt_known_hosts_count(snapshot);
    jclass type = (*env)->FindClass(env, "[B");
    jobjectArray result = type ? (*env)->NewObjectArray(env, (jsize)(count * 9), type, NULL) : NULL;
    for (size_t i = 0; result && i < count && !(*env)->ExceptionCheck(env); i++) {
        gt_known_host_entry_t entry;
        if (gt_known_hosts_entry_at(snapshot, i, &entry) != GT_OK) { fail(env, "主机验证条目读取失败"); break; }
        char line[24], start[24], end[24];
        snprintf(line, sizeof line, "%zu", entry.source_line);
        snprintf(start, sizeof start, "%zu", entry.byte_start);
        snprintf(end, sizeof end, "%zu", entry.byte_end);
        const char *fields[] = {line, start, end, entry.marker, entry.hosts, entry.key_type,
            entry.key_base64, entry.recognized ? "1" : "0", entry.hashed ? "1" : "0"};
        for (int f = 0; f < 9 && !(*env)->ExceptionCheck(env); f++) {
            jsize length = (jsize)strlen(fields[f]);
            jbyteArray field = (*env)->NewByteArray(env, length);
            if (!field) break;
            (*env)->SetByteArrayRegion(env, field, 0, length, (const jbyte *)fields[f]);
            (*env)->SetObjectArrayElement(env, result, (jsize)(i * 9 + (size_t)f), field);
            (*env)->DeleteLocalRef(env, field);
        }
    }
    gt_known_hosts_free(snapshot);
    return result;
}
