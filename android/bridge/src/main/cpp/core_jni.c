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

JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_CoreLibrary_keyPassphraseRequirement(JNIEnv *env, jobject self, jbyteArray input) {
    (void)self;
    if (!input) { fail(env,"缺少密钥材料"); return -1; }
    jsize n=(*env)->GetArrayLength(env,input);
    if (n<=0 || n>1024*1024) { fail(env,"密钥大小无效"); return -1; }
    char *pem=calloc((size_t)n+1,1);
    if (!pem) { fail(env,"无法读取密钥材料"); return -1; }
    (*env)->GetByteArrayRegion(env,input,0,n,(jbyte *)pem);
    int result=-1;
    if (!(*env)->ExceptionCheck(env) && !memchr(pem,0,(size_t)n)) result=gt_ssh_key_needs_passphrase(pem);
    volatile unsigned char *erase=(volatile unsigned char *)pem;
    for (jsize i=0;i<n;i++) erase[i]=0;
    free(pem); return result;
}
