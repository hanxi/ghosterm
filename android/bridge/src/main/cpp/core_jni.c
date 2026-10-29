#include <jni.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "ghosterm.h"

static void fail(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env, type, message);
}

static jbyteArray bytes(JNIEnv *env, const char *value, size_t count) {
    if (count > INT_MAX) { fail(env, "Native result too large"); return NULL; }
    jbyteArray result = (*env)->NewByteArray(env, (jsize)count);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)count, (const jbyte *)value);
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_hanxi_ghosterm_bridge_CoreLibrary_versionBytes(JNIEnv *env, jobject self) {
    (void)self;
    const char *version = gt_version();
    return bytes(env, version, strlen(version));
}

/* UTF-8 bytes, not JNI modified UTF-8: preserve supplementary Unicode and CRLF. */
JNIEXPORT jbyteArray JNICALL
Java_com_hanxi_ghosterm_bridge_CoreLibrary_roundTripBytes(JNIEnv *env, jobject self, jbyteArray input) {
    (void)self;
    if (!input) { fail(env, "Configuration is required"); return NULL; }
    jsize count = (*env)->GetArrayLength(env, input);
    char *text = malloc((size_t)count + 1);
    if (!text) { fail(env, "Configuration allocation failed"); return NULL; }
    (*env)->GetByteArrayRegion(env, input, 0, count, (jbyte *)text);
    if ((*env)->ExceptionCheck(env)) { free(text); return NULL; }
    text[count] = '\0';
    if (memchr(text, '\0', (size_t)count)) {
        free(text); fail(env, "Configuration contains NUL"); return NULL;
    }
    gt_config_t *config = gt_config_parse(text, (size_t)count);
    free(text);
    if (!config) { fail(env, "Configuration parse failed"); return NULL; }
    char *serialized = gt_config_to_string(config);
    gt_config_free(config);
    if (!serialized) { fail(env, "Configuration serialization failed"); return NULL; }
    jbyteArray result = bytes(env, serialized, strlen(serialized));
    free(serialized);
    return result;
}
