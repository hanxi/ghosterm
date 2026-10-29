#include <jni.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ghosterm.h"

/* No long-lived Core handles cross JNI. Every operation owns its parsed snapshot. */
static void fail(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env, type, message);
}

static char *utf8(JNIEnv *env, jbyteArray input) {
    if (!input) { fail(env, "配置参数缺失"); return NULL; }
    jsize count = (*env)->GetArrayLength(env, input);
    char *value = malloc((size_t)count + 1);
    if (!value) { fail(env, "配置内存不足"); return NULL; }
    (*env)->GetByteArrayRegion(env, input, 0, count, (jbyte *)value);
    if ((*env)->ExceptionCheck(env)) { free(value); return NULL; }
    value[count] = 0;
    if (memchr(value, 0, (size_t)count)) {
        free(value); fail(env, "SSH 配置不能包含 NUL 字符"); return NULL;
    }
    return value;
}

static jbyteArray bytes(JNIEnv *env, const char *text) {
    size_t count = strlen(text);
    if (count > INT_MAX) { fail(env, "配置结果过大"); return NULL; }
    jbyteArray result = (*env)->NewByteArray(env, (jsize)count);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)count, (const jbyte *)text);
    return result;
}

static gt_config_t *parse(JNIEnv *env, jbyteArray input) {
    char *text = utf8(env, input);
    if (!text) return NULL;
    gt_config_t *cfg = gt_config_parse(text, strlen(text));
    free(text);
    if (!cfg) fail(env, "无法解析 SSH 配置，请检查 Include 文件是否可访问");
    return cfg;
}

JNIEXPORT jobjectArray JNICALL
Java_com_hanxi_ghosterm_bridge_ConfigBridge_hostsBytes(JNIEnv *env, jobject self, jbyteArray input) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) return NULL;
    size_t count = gt_config_host_count(cfg);
    if (count > 4096) { gt_config_free(cfg); fail(env, "主机条目过多（最多 4096）"); return NULL; }
    jclass byte_array = (*env)->FindClass(env, "[B");
    enum { HOST_WIDTH = 31 };
    jobjectArray result = byte_array ? (*env)->NewObjectArray(env, (jsize)(count * HOST_WIDTH), byte_array, NULL) : NULL;
    gt_resolve_opts_t opts = {.default_user = "", .identity_policy = GT_IDENTITY_POLICY_NONE};
    for (size_t i = 0; result && i < count && !(*env)->ExceptionCheck(env); ++i) {
        const char *alias = gt_config_host_pattern(cfg, i);
        gt_resolved_t *host = gt_config_resolve(cfg, alias, &opts);
        if (!host) { fail(env, "主机配置求值失败"); break; }
        gt_config_host_info_t info = {0};
        /* Negated patterns are not connectable; the repository excludes them. */
        if (alias[0] != '!' && gt_config_inspect_host(cfg, alias, &info) != GT_OK) {
            gt_resolved_free(host); fail(env, "无法确定主机配置来源"); break;
        }
        char port[16];
        snprintf(port, sizeof port, "%d", host->port);
        char numbers[6][24];
        snprintf(numbers[0], sizeof numbers[0], "%zu", info.source_line);
        snprintf(numbers[1], sizeof numbers[1], "%zu", info.pattern_count);
        snprintf(numbers[2], sizeof numbers[2], "%zu", info.matching_blocks);
        const char *fields[HOST_WIDTH] = {alias, host->hostname, host->user, port,
            info.header, info.source_path, numbers[0], numbers[1], numbers[2], info.included ? "1" : "0"};
        const gt_config_field_info_t *provenance[] = {&info.hostname, &info.user, &info.port};
        for (int p = 0; p < 3; p++) {
            const gt_config_field_info_t *field = provenance[p];
            snprintf(numbers[p + 3], sizeof numbers[p + 3], "%zu", field->source_line);
            const char *values[] = {field->explicit_value ? "1" : "0", field->explicit_value,
                field->source_path, field->source_header, numbers[p + 3],
                field->source_included ? "1" : "0", field->effective_is_explicit ? "1" : "0"};
            for (int v = 0; v < 7; v++) fields[10 + p * 7 + v] = values[v];
        }
        for (int f = 0; f < HOST_WIDTH && !(*env)->ExceptionCheck(env); ++f) {
            jbyteArray field = bytes(env, fields[f] ? fields[f] : "");
            if (field) {
                (*env)->SetObjectArrayElement(env, result, (jsize)(i * HOST_WIDTH + (size_t)f), field);
                (*env)->DeleteLocalRef(env, field);
            }
        }
        gt_resolved_free(host);
    }
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_hanxi_ghosterm_bridge_ConfigBridge_editBytes(JNIEnv *env, jobject self, jbyteArray input,
        jbyteArray name, jobjectArray keys, jobjectArray values, jboolean create) {
    (void)self;
    if (!keys || !values) { fail(env, "编辑参数缺失"); return NULL; }
    jsize count = (*env)->GetArrayLength(env, keys);
    if (count > 3 || count != (*env)->GetArrayLength(env, values)) { fail(env, "编辑字段不匹配"); return NULL; }
    gt_config_t *cfg = parse(env, input);
    if (!cfg) return NULL;
    char *alias = utf8(env, name);
    gt_config_edit_t edits[3] = {0};
    char *key_storage[3] = {0}, *value_storage[3] = {0};
    const char *tokens[3][2] = {{0}};
    jbyteArray result = NULL;
    if (!alias) goto done;
    if (create) {
        for (size_t i = 0; i < gt_config_host_count(cfg); ++i) {
            if (strcasecmp(alias, gt_config_host_pattern(cfg, i)) == 0) {
                fail(env, "该主机别名已存在"); goto done;
            }
        }
    }
    for (jsize i = 0; i < count; ++i) {
        jbyteArray key = (*env)->GetObjectArrayElement(env, keys, i);
        jbyteArray value = (*env)->GetObjectArrayElement(env, values, i);
        bool has_value = value != NULL;
        key_storage[i] = utf8(env, key);
        /* A null array element is an explicit request to remove this directive. */
        if (has_value) value_storage[i] = utf8(env, value);
        (*env)->DeleteLocalRef(env, key); (*env)->DeleteLocalRef(env, value);
        if (!key_storage[i] || (has_value && !value_storage[i])) goto done;
        if (strcmp(key_storage[i], "HostName") && strcmp(key_storage[i], "User") && strcmp(key_storage[i], "Port")) {
            fail(env, "不支持的编辑字段"); goto done;
        }
        if (value_storage[i] && strpbrk(value_storage[i], "\r\n")) { fail(env, "编辑值不能包含换行"); goto done; }
        tokens[i][0] = value_storage[i];
        edits[i].keyword = key_storage[i]; edits[i].values = has_value ? tokens[i] : NULL;
    }
    if (gt_config_edit(cfg, alias, edits, (size_t)count, create) != GT_OK) {
        fail(env, "无法编辑该主机；请在 SSH 配置页检查原始块"); goto done;
    }
    char *serialized = gt_config_to_string(cfg);
    if (!serialized) { fail(env, "配置序列化失败"); goto done; }
    result = bytes(env, serialized);
    free(serialized);
done:
    for (int i = 0; i < 3; ++i) { free(key_storage[i]); free(value_storage[i]); }
    free(alias); gt_config_free(cfg);
    return result;
}
