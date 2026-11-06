#ifndef GHOSTERM_ANDROID_PNG_H
#define GHOSTERM_ANDROID_PNG_H
#include <stdbool.h>
#include <jni.h>
#include <stdint.h>
/* Once per process, before creating any VT owner. Uses Android's PNG decoder;
 * no bundled codec, application callback or UI-thread dependency. */
bool gat_android_png_init(JNIEnv *env);
/* Native owner helpers; returned bitmap is an immutable global JNI reference. */
JNIEnv *gat_android_env_acquire(bool *attached);
void gat_android_env_release(bool attached);
jobject gat_android_bitmap_copy(
    JNIEnv *env, uint32_t width, uint32_t height, const uint8_t *pixels, int format);
#endif
