#include <jni.h>
#include <limits.h>
#include <stdlib.h>
#include <ghostty/vt.h>

static void fail(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env, type, message);
}

JNIEXPORT jbyteArray JNICALL
Java_com_hanxi_ghosterm_terminal_TerminalEngine_versionBytes(JNIEnv *env, jobject self) {
    (void)self;
    GhosttyString version;
    if (ghostty_build_info(GHOSTTY_BUILD_INFO_VERSION_STRING, &version) != GHOSTTY_SUCCESS || version.len > INT_MAX) {
        fail(env, "VT build information unavailable"); return NULL;
    }
    jbyteArray result = (*env)->NewByteArray(env, (jsize)version.len);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)version.len, (const jbyte *)version.ptr);
    return result;
}

JNIEXPORT jintArray JNICALL
Java_com_hanxi_ghosterm_terminal_TerminalEngine_probeCursor(JNIEnv *env, jobject self, jbyteArray input) {
    (void)self;
    if (!input) { fail(env, "VT probe input required"); return NULL; }
    GhosttyTerminal terminal = NULL;
    if (ghostty_terminal_new(NULL, &terminal, 80, 24) != GHOSTTY_SUCCESS) {
        fail(env, "VT allocation failed"); return NULL;
    }
    jsize count = (*env)->GetArrayLength(env, input);
    jbyte *data = (*env)->GetByteArrayElements(env, input, NULL);
    if (!data) { ghostty_terminal_free(terminal); return NULL; }
    ghostty_terminal_vt_write(terminal, (const uint8_t *)data, (size_t)count);
    (*env)->ReleaseByteArrayElements(env, input, data, JNI_ABORT);
    uint16_t x = 0, y = 0;
    GhosttyResult rx = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &x);
    GhosttyResult ry = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &y);
    ghostty_terminal_free(terminal);
    if (rx != GHOSTTY_SUCCESS || ry != GHOSTTY_SUCCESS) { fail(env, "VT cursor query failed"); return NULL; }
    jint values[] = {x, y};
    jintArray result = (*env)->NewIntArray(env, 2);
    if (result) (*env)->SetIntArrayRegion(env, result, 0, 2, values);
    return result;
}
