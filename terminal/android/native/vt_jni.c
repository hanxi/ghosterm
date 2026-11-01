#include <jni.h>
#include <limits.h>
#include <stdlib.h>
#include <ghostty/vt.h>
#include "android_png.h"

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

/* Bounded component probe for the real VT parser/current-frame/clock ABI.
 * Each result is changed, next-delay (-1 idle), ARGB pixel, generation low bits.
 * No raw terminal output enters the UI or a transport event queue. */
JNIEXPORT jintArray JNICALL
Java_com_hanxi_ghosterm_terminal_TerminalEngine_probeKittyFrames(JNIEnv *env,jobject self,jbyteArray input,jint image_id,jlongArray ticks) {
    (void)self;
    if(!input || !ticks || image_id<=0) { fail(env,"Kitty probe arguments required");return NULL; }
    jsize length=(*env)->GetArrayLength(env,input),count=(*env)->GetArrayLength(env,ticks);
    if(length>1024*1024 || count<1 || count>64) { fail(env,"Kitty probe limits exceeded");return NULL; }
    jlong times[64];jint values[256];
    (*env)->GetLongArrayRegion(env,ticks,0,count,times);
    if((*env)->ExceptionCheck(env))return NULL;
    for(jsize i=0;i<count;i++)if(times[i]<0 || (i && times[i]<times[i-1])) { fail(env,"Kitty probe clock must be monotonic");return NULL; }
    GhosttyTerminal terminal=NULL;
    if(ghostty_terminal_new(NULL,&terminal,80,24)!=GHOSTTY_SUCCESS) { fail(env,"Kitty VT allocation failed");return NULL; }
    bool ok=false;size_t limit=1024*1024;
    if(ghostty_terminal_set(terminal,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)!=GHOSTTY_SUCCESS)goto done;
    jbyte *bytes=(*env)->GetByteArrayElements(env,input,NULL);
    if(!bytes)goto done;
    ghostty_terminal_vt_write(terminal,(const uint8_t *)bytes,(size_t)length);
    (*env)->ReleaseByteArrayElements(env,input,bytes,JNI_ABORT);
    for(jsize i=0;i<count;i++) {
        bool changed=false;uint64_t delay=UINT64_MAX;
        if(ghostty_kitty_graphics_animation_tick(terminal,(uint64_t)times[i],&changed,&delay)!=GHOSTTY_SUCCESS)goto done;
        GhosttyKittyGraphics graphics=NULL;
        if(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)!=GHOSTTY_SUCCESS)goto done;
        GhosttyKittyGraphicsImage image=ghostty_kitty_graphics_image(graphics,(uint32_t)image_id);
        if(!image)goto done;
        GhosttyKittyImageFormat format;const uint8_t *pixels=NULL;size_t size=0;uint64_t generation=0;
        if(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_FORMAT,&format)!=GHOSTTY_SUCCESS ||
           ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels)!=GHOSTTY_SUCCESS ||
           ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&size)!=GHOSTTY_SUCCESS ||
           ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_GENERATION,&generation)!=GHOSTTY_SUCCESS ||
           format!=GHOSTTY_KITTY_IMAGE_FORMAT_RGBA || !pixels || size<4)goto done;
        values[i*4]=changed?1:0;values[i*4+1]=delay==UINT64_MAX?-1:delay>INT_MAX?INT_MAX:(jint)delay;
        values[i*4+2]=(jint)(((uint32_t)pixels[3]<<24)|((uint32_t)pixels[0]<<16)|((uint32_t)pixels[1]<<8)|pixels[2]);
        values[i*4+3]=(jint)(uint32_t)generation;
    }
    ok=true;
done:
    ghostty_terminal_free(terminal);
    if(!ok) { if(!(*env)->ExceptionCheck(env))fail(env,"Kitty frame/clock probe failed");return NULL; }
    jintArray result=(*env)->NewIntArray(env,count*4);
    if(result)(*env)->SetIntArrayRegion(env,result,0,count*4,values);
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved) {
    (void)reserved;JNIEnv *env=NULL;
    if((*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK || !gat_android_png_init(env))return JNI_ERR;
    return JNI_VERSION_1_6;
}
