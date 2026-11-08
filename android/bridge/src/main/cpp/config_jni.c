#include <jni.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ghosterm.h"

/* No long-lived Core handles cross JNI. Every operation owns its parsed snapshot. */
static void fail(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type) {
        (*env)->ThrowNew(env, type, message);
    }
}

static char *utf8(JNIEnv *env, jbyteArray input) {
    if (!input) {
        fail(env, "配置参数缺失");
        return NULL;
    }
    jsize count = (*env)->GetArrayLength(env, input);
    char *value = malloc((size_t)count + 1);
    if (!value) {
        fail(env, "配置内存不足");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, input, 0, count, (jbyte *)value);
    if ((*env)->ExceptionCheck(env)) {
        free(value);
        return NULL;
    }
    value[count] = 0;
    if (memchr(value, 0, (size_t)count)) {
        free(value);
        fail(env, "SSH 配置不能包含 NUL 字符");
        return NULL;
    }
    return value;
}

static jbyteArray bytes(JNIEnv *env, const char *text) {
    size_t count = strlen(text);
    if (count > INT_MAX) {
        fail(env, "配置结果过大");
        return NULL;
    }
    jbyteArray result = (*env)->NewByteArray(env, (jsize)count);
    if (result) {
        (*env)->SetByteArrayRegion(env, result, 0, (jsize)count, (const jbyte *)text);
    }
    return result;
}

static gt_config_t *parse(JNIEnv *env, jbyteArray input) {
    char *text = utf8(env, input);
    if (!text) {
        return NULL;
    }
    gt_config_t *cfg = gt_config_parse(text, strlen(text));
    free(text);
    if (!cfg) {
        fail(env, "无法解析 SSH 配置，请检查 Include 文件是否可访问");
    }
    return cfg;
}

JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_identityBytes(
    JNIEnv *env, jobject self, jbyteArray input, jbyteArray name) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name);
    jobjectArray result = NULL;
    gt_config_host_info_t ownership = {0};
    gt_config_identity_info_t entry = {0};
    if (!alias) {
        goto done;
    }
    if (gt_config_inspect_host(cfg, alias, &ownership) != GT_OK) {
        fail(env, "找不到主机身份配置来源");
        goto done;
    }
    size_t count = 0;
    int rc;
    while ((rc = gt_config_identity_at(cfg, alias, count, &entry)) == GT_OK && count < 4096) {
        count++;
    }
    if (rc != GT_ERR_NOT_FOUND) {
        fail(env,
             rc == GT_OK ? "身份条目过多（最多 4096）"
                         : "无法确定身份来源，请检查 Match 或 IdentityFile 参数");
        goto done;
    }
    jclass type = (*env)->FindClass(env, "[B");
    if (!type) {
        goto done;
    }
    result = (*env)->NewObjectArray(env, (jsize)(count * 7), type, NULL);
    for (size_t i = 0; result && i < count && !(*env)->ExceptionCheck(env); i++) {
        if (gt_config_identity_at(cfg, alias, i, &entry) != GT_OK) {
            fail(env, "身份来源记录不完整");
            break;
        }
        char line[24];
        snprintf(line, sizeof line, "%zu", entry.source_line);
        const char *fields[] = {entry.value,
                                entry.source_path,
                                entry.source_header,
                                line,
                                entry.included ? "1" : "0",
                                entry.explicit_value ? "1" : "0",
                                entry.effective ? "1" : "0"};
        for (int f = 0; f < 7 && !(*env)->ExceptionCheck(env); f++) {
            jbyteArray field = bytes(env, fields[f]);
            if (field) {
                (*env)->SetObjectArrayElement(env, result, (jsize)(i * 7 + (size_t)f), field);
                (*env)->DeleteLocalRef(env, field);
            }
        }
    }
done:
    free(alias);
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_hostsBytes(
    JNIEnv *env, jobject self, jbyteArray input) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    size_t count = gt_config_host_count(cfg);
    if (count > 4096) {
        gt_config_free(cfg);
        fail(env, "主机条目过多（最多 4096）");
        return NULL;
    }
    jclass byte_array = (*env)->FindClass(env, "[B");

    enum {
        HOST_WIDTH = 31
    };

    jobjectArray result =
        byte_array ? (*env)->NewObjectArray(env, (jsize)(count * HOST_WIDTH), byte_array, NULL)
                   : NULL;
    gt_resolve_opts_t opts = {.default_user = "", .identity_policy = GT_IDENTITY_POLICY_NONE};
    for (size_t i = 0; result && i < count && !(*env)->ExceptionCheck(env); ++i) {
        const char *alias = gt_config_host_pattern(cfg, i);
        gt_resolved_t *host = gt_config_resolve(cfg, alias, &opts);
        if (!host) {
            fail(env, "主机配置求值失败");
            break;
        }
        gt_config_host_info_t info = {0};
        /* Negated patterns are not connectable; the repository excludes them. */
        if (alias[0] != '!' && gt_config_inspect_host(cfg, alias, &info) != GT_OK) {
            gt_resolved_free(host);
            fail(env, "无法确定主机配置来源");
            break;
        }
        char port[16];
        snprintf(port, sizeof port, "%d", host->port);
        char numbers[6][24];
        snprintf(numbers[0], sizeof numbers[0], "%zu", info.source_line);
        snprintf(numbers[1], sizeof numbers[1], "%zu", info.pattern_count);
        snprintf(numbers[2], sizeof numbers[2], "%zu", info.matching_blocks);
        const char *fields[HOST_WIDTH] = {alias,
                                          host->hostname,
                                          host->user,
                                          port,
                                          info.header,
                                          info.source_path,
                                          numbers[0],
                                          numbers[1],
                                          numbers[2],
                                          info.included ? "1" : "0"};
        const gt_config_field_info_t *provenance[] = {&info.hostname, &info.user, &info.port};
        for (int p = 0; p < 3; p++) {
            const gt_config_field_info_t *field = provenance[p];
            snprintf(numbers[p + 3], sizeof numbers[p + 3], "%zu", field->source_line);
            const char *values[] = {field->explicit_value ? "1" : "0",
                                    field->explicit_value,
                                    field->source_path,
                                    field->source_header,
                                    numbers[p + 3],
                                    field->source_included ? "1" : "0",
                                    field->effective_is_explicit ? "1" : "0"};
            for (int v = 0; v < 7; v++) {
                fields[10 + p * 7 + v] = values[v];
            }
        }
        for (int f = 0; f < HOST_WIDTH && !(*env)->ExceptionCheck(env); ++f) {
            jbyteArray field = bytes(env, fields[f] ? fields[f] : "");
            if (field) {
                (*env)->SetObjectArrayElement(
                    env, result, (jsize)(i * HOST_WIDTH + (size_t)f), field);
                (*env)->DeleteLocalRef(env, field);
            }
        }
        gt_resolved_free(host);
    }
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_hanxi_ghosterm_bridge_ConfigBridge_editBytes(JNIEnv *env,
                                                      jobject self,
                                                      jbyteArray input,
                                                      jbyteArray name,
                                                      jobjectArray keys,
                                                      jobjectArray values,
                                                      jboolean create) {
    (void)self;
    if (!keys || !values) {
        fail(env, "编辑参数缺失");
        return NULL;
    }
    jsize count = (*env)->GetArrayLength(env, keys);
    if (count > 3 || count != (*env)->GetArrayLength(env, values)) {
        fail(env, "编辑字段不匹配");
        return NULL;
    }
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name);
    gt_config_edit_t edits[3] = {0};
    char *key_storage[3] = {0}, *value_storage[3] = {0};
    const char *tokens[3][2] = {{0}};
    jbyteArray result = NULL;
    if (!alias) {
        goto done;
    }
    if (create) {
        for (size_t i = 0; i < gt_config_host_count(cfg); ++i) {
            if (strcasecmp(alias, gt_config_host_pattern(cfg, i)) == 0) {
                fail(env, "该主机别名已存在");
                goto done;
            }
        }
    }
    for (jsize i = 0; i < count; ++i) {
        jbyteArray key = (*env)->GetObjectArrayElement(env, keys, i);
        jbyteArray value = (*env)->GetObjectArrayElement(env, values, i);
        bool has_value = value != NULL;
        key_storage[i] = utf8(env, key);
        /* A null array element is an explicit request to remove this directive. */
        if (has_value) {
            value_storage[i] = utf8(env, value);
        }
        (*env)->DeleteLocalRef(env, key);
        (*env)->DeleteLocalRef(env, value);
        if (!key_storage[i] || (has_value && !value_storage[i])) {
            goto done;
        }
        if (strcmp(key_storage[i], "HostName") && strcmp(key_storage[i], "User") &&
            strcmp(key_storage[i], "Port")) {
            fail(env, "不支持的编辑字段");
            goto done;
        }
        if (value_storage[i] && strpbrk(value_storage[i], "\r\n")) {
            fail(env, "编辑值不能包含换行");
            goto done;
        }
        tokens[i][0] = value_storage[i];
        edits[i].keyword = key_storage[i];
        edits[i].values = has_value ? tokens[i] : NULL;
    }
    if (gt_config_edit(cfg, alias, edits, (size_t)count, create) != GT_OK) {
        fail(env, "无法编辑该主机；请在 SSH 配置页检查原始块");
        goto done;
    }
    char *serialized = gt_config_to_string(cfg);
    if (!serialized) {
        fail(env, "配置序列化失败");
        goto done;
    }
    result = bytes(env, serialized);
    free(serialized);
done:
    for (int i = 0; i < 3; ++i) {
        free(key_storage[i]);
        free(value_storage[i]);
    }
    free(alias);
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_editIdentityBytes(
    JNIEnv *env, jobject self, jbyteArray input, jbyteArray name, jobjectArray values) {
    (void)self;
    if (!values) {
        fail(env, "身份列表缺失");
        return NULL;
    }
    jsize count = (*env)->GetArrayLength(env, values);
    if (count > 256) {
        fail(env, "最多设置 256 条身份");
        return NULL;
    }
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name);
    char *quoted[257] = {0};
    jbyteArray result = NULL;
    if (!alias) {
        goto done;
    }
    for (jsize i = 0; i < count; i++) {
        jbyteArray item = (*env)->GetObjectArrayElement(env, values, i);
        char *value = utf8(env, item);
        (*env)->DeleteLocalRef(env, item);
        if (!value) {
            goto done;
        }
        size_t length = strlen(value);
        if (!length || length > 65536 || strpbrk(value, "\r\n")) {
            free(value);
            fail(env, "身份引用为空、过长或包含换行");
            goto done;
        }
        /* gt_config_edit takes rendered values, while identity_at returns decoded tokens. */
        quoted[i] = malloc(length * 2 + 3);
        if (!quoted[i]) {
            free(value);
            fail(env, "身份编辑内存不足");
            goto done;
        }
        size_t offset = 0;
        quoted[i][offset++] = '"';
        for (size_t n = 0; n < length; n++) {
            if (value[n] == '"' || value[n] == '\\') {
                quoted[i][offset++] = '\\';
            }
            quoted[i][offset++] = value[n];
        }
        quoted[i][offset++] = '"';
        quoted[i][offset] = 0;
        free(value);
    }
    gt_config_edit_t edit = {.keyword = "IdentityFile",
                             .values = count ? (const char *const *)quoted : NULL};
    if (gt_config_edit(cfg, alias, &edit, 1, false) != GT_OK) {
        fail(env, "无法修改身份，请检查配置归属");
        goto done;
    }
    char *serialized = gt_config_to_string(cfg);
    if (!serialized) {
        fail(env, "身份配置序列化失败");
        goto done;
    }
    result = bytes(env, serialized);
    free(serialized);
done:
    for (jsize i = 0; i < count; i++) {
        free(quoted[i]);
    }
    free(alias);
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_renameBytes(
    JNIEnv *env, jobject self, jbyteArray input, jbyteArray name, jbyteArray replacement) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name), *new_alias = utf8(env, replacement);
    jbyteArray result = NULL;
    if (!alias || !new_alias) {
        goto done;
    }
    if (gt_config_rename_host(cfg, alias, new_alias) != GT_OK) {
        fail(env, "无法重命名，请检查别名冲突、配置归属及 Include 文件");
        goto done;
    }
    char *serialized = gt_config_to_string(cfg);
    if (!serialized) {
        fail(env, "配置序列化失败");
        goto done;
    }
    result = bytes(env, serialized);
    free(serialized);
done:
    free(alias);
    free(new_alias);
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_connectionIssueBytes(
    JNIEnv *env, jobject self, jbyteArray input, jbyteArray name) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name);
    jobjectArray result = NULL;
    if (!alias) {
        goto done;
    }
    size_t count = 0;
    gt_config_connection_issue_t info = {0};
    int status;
    while ((status = gt_config_check_connection(cfg, alias, count, &info)) == GT_ERR_UNSUPPORTED &&
           count < 256) {
        count++;
    }
    if (status != GT_OK || count >= 256) {
        fail(env, "无法检查连接配置");
        goto done;
    }
    jclass type = (*env)->FindClass(env, "[B");
    if (!type) {
        goto done;
    }
    result = (*env)->NewObjectArray(env, (jsize)(count * 4), type, NULL);
    for (size_t i = 0; result && i < count && !(*env)->ExceptionCheck(env); i++) {
        if (gt_config_check_connection(cfg, alias, i, &info) != GT_ERR_UNSUPPORTED) {
            fail(env, "连接配置检查结果改变");
            break;
        }
        char line[24];
        snprintf(line, sizeof line, "%zu", info.source_line);
        const char *fields[] = {info.keyword, info.source_path, line, info.included ? "1" : "0"};
        for (size_t f = 0; f < 4; f++) {
            jbyteArray value = bytes(env, fields[f]);
            if (!value) {
                break;
            }
            (*env)->SetObjectArrayElement(env, result, (jsize)(i * 4 + f), value);
            (*env)->DeleteLocalRef(env, value);
        }
    }
done:
    free(alias);
    gt_config_free(cfg);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_removeHostBytes(
    JNIEnv *env, jobject self, jbyteArray input, jbyteArray name) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name);
    jbyteArray result = NULL;
    if (!alias) {
        goto done;
    }
    if (gt_config_remove_host(cfg, alias) != GT_OK) {
        fail(env, "无法删除：配置来自 Include、别名重复或块内含 Include；请在 SSH 配置页核对");
        goto done;
    }
    char *serialized = gt_config_to_string(cfg);
    if (!serialized) {
        fail(env, "配置序列化失败");
        goto done;
    }
    result = bytes(env, serialized);
    free(serialized);
done:
    free(alias);
    gt_config_free(cfg);
    return result;
}

/* Resolve every hop from one AST so Includes cannot change between hops. */
typedef struct {
    gt_resolved_t *hosts[8];
    char *aliases[8];
    const char *visiting[8];
    size_t count, depth;
} android_route;

static bool route_append(JNIEnv *env,
                         const gt_config_t *cfg,
                         android_route *r,
                         const char *alias,
                         const char *user,
                         int port,
                         bool expand) {
    if (!*alias || r->count + r->depth >= 8) {
        goto invalid;
    }
    for (size_t i = 0; i < r->depth; i++) {
        if (!strcasecmp(r->visiting[i], alias)) {
            goto invalid;
        }
    }
    r->visiting[r->depth++] = alias;
    gt_resolved_t *h = NULL;
    char jump[8193];
    int jump_length = gt_config_effective(cfg, alias, "proxyjump", NULL, jump, sizeof jump);
    if (jump_length < 0 || (size_t)jump_length >= sizeof jump) {
        goto failed;
    }
    for (size_t i = 0;; i++) {
        gt_config_connection_issue_t issue = {0};
        int status = gt_config_check_connection(cfg, alias, i, &issue);
        if (status == GT_OK) {
            break;
        }
        if (status != GT_ERR_UNSUPPORTED || !issue.keyword || strcmp(issue.keyword, "proxyjump")) {
            goto failed;
        }
    }
    gt_resolve_opts_t opts = {.default_user = "", .identity_policy = GT_IDENTITY_POLICY_NONE};
    h = gt_config_resolve(cfg, alias, &opts);
    if (!h) {
        goto failed;
    }
    if (user) {
        char *replacement = strdup(user);
        if (!replacement) {
            goto failed;
        }
        free(h->user);
        h->user = replacement;
    }
    if (port) {
        h->port = port;
    }
    if (h->port < 1 || h->port > 65535) {
        goto failed;
    }
    if (*jump && strcasecmp(jump, "none")) {
        char *next = jump;
        unsigned index = 0;
        while (next) {
            char *item = next, *comma = strchr(item, ',');
            next = comma ? comma + 1 : NULL;
            if (comma) {
                *comma = 0;
            }
            if (!*item || ++index >= 8) {
                goto failed;
            }
            for (const unsigned char *p = (unsigned char *)item; *p; p++) {
                if (*p <= 32 || *p == 127) {
                    goto failed;
                }
            }
            if (!strncmp(item, "ssh://", 6)) {
                item += 6;
            }
            if (strpbrk(item, "/%?#")) {
                goto failed;
            }
            char *hop_user = NULL, *at = strchr(item, '@');
            if (at) {
                *at = 0;
                hop_user = item;
                item = at + 1;
                if (!*hop_user || strchr(item, '@')) {
                    goto failed;
                }
            }
            char *hop_host = item, *port_text = NULL;
            if (*item == '[') {
                char *end = strchr(++hop_host, ']');
                if (!end) {
                    goto failed;
                }
                *end = 0;
                unsigned char address[16];
                if (inet_pton(AF_INET6, hop_host, address) != 1) {
                    goto failed;
                }
                if (end[1]) {
                    if (end[1] != ':') {
                        goto failed;
                    }
                    port_text = end + 2;
                }
            } else {
                char *colon = strchr(item, ':');
                if (colon) {
                    *colon = 0;
                    port_text = colon + 1;
                }
                if (strpbrk(hop_host, "[]")) {
                    goto failed;
                }
            }
            if (!*hop_host || strlen(hop_host) > 255 || *hop_host == '-' || strchr(hop_host, '*')) {
                goto failed;
            }
            int hop_port = 0;
            if (port_text) {
                if (!*port_text) {
                    goto failed;
                }
                for (char *p = port_text; *p; p++) {
                    if (*p < '0' || *p > '9' || hop_port > 6553) {
                        goto failed;
                    }
                    hop_port = hop_port * 10 + *p - '0';
                }
                if (hop_port < 1 || hop_port > 65535) {
                    goto failed;
                }
            }
            if (expand && !route_append(env, cfg, r, hop_host, hop_user, hop_port, index == 1)) {
                goto failed;
            }
        }
    }
    for (size_t i = 0; i < r->count; i++) {
        gt_resolved_t *prior = r->hosts[i];
        if (!strcasecmp(prior->hostname, h->hostname) && prior->port == h->port &&
            !strcmp(prior->user, h->user)) {
            goto failed;
        }
    }
    if (r->count >= 8) {
        goto failed;
    }
    char *saved = strdup(alias);
    if (!saved) {
        goto failed;
    }
    r->aliases[r->count] = saved;
    r->hosts[r->count++] = h;
    r->depth--;
    return true;
failed:
    gt_resolved_free(h);
    r->depth--;
invalid:
    if (!(*env)->ExceptionCheck(env)) {
        fail(env, "ProxyJump 路线无效或含不支持配置（含目标最多八跳）");
    }
    return false;
}

JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_ConfigBridge_preparedRouteBytes(
    JNIEnv *env, jobject self, jbyteArray input, jbyteArray name) {
    (void)self;
    gt_config_t *cfg = parse(env, input);
    if (!cfg) {
        return NULL;
    }
    char *alias = utf8(env, name);
    android_route route = {0};
    jobjectArray result = NULL;
    if (!alias || !route_append(env, cfg, &route, alias, NULL, 0, true)) {
        goto done;
    }
    size_t fields = 0;
    for (size_t i = 0; i < route.count; i++) {
        fields += 5;
        for (size_t j = 0; route.hosts[i]->identity_files && route.hosts[i]->identity_files[j];
             j++) {
            if (j >= 256) {
                fail(env, "身份文件条目过多");
                goto done;
            }
            fields++;
        }
    }
    jclass type = (*env)->FindClass(env, "[B");
    if (!type) {
        goto done;
    }
    result = (*env)->NewObjectArray(env, (jsize)fields, type, NULL);
    jsize out = 0;
    for (size_t i = 0; result && i < route.count && !(*env)->ExceptionCheck(env); i++) {
        gt_resolved_t *h = route.hosts[i];
        size_t count = 0;
        while (h->identity_files && h->identity_files[count]) {
            count++;
        }
        char port[24], identities[24];
        snprintf(port, sizeof port, "%d", h->port);
        snprintf(identities, sizeof identities, "%zu", count);
        const char *head[] = {route.aliases[i], h->hostname, h->user, port, identities};
        for (size_t j = 0; j < 5 + count && !(*env)->ExceptionCheck(env); j++) {
            jbyteArray value = bytes(env, j < 5 ? head[j] : h->identity_files[j - 5]);
            if (!value) {
                break;
            }
            (*env)->SetObjectArrayElement(env, result, out++, value);
            (*env)->DeleteLocalRef(env, value);
        }
    }
done:
    for (size_t i = 0; i < route.count; i++) {
        gt_resolved_free(route.hosts[i]);
        free(route.aliases[i]);
    }
    free(alias);
    gt_config_free(cfg);
    return result;
}
