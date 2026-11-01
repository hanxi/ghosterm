#include "android_png.h"
#include <android/bitmap.h>
#include <ghostty/vt.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define PNG_BYTES (8u*1024u*1024u)
#define PIXEL_BYTES (8u*1024u*1024u)
#define DIMENSION_LIMIT 4096u

typedef struct {
    JavaVM *vm;
    jclass factory, options, bitmap, config;
    jobject rgba;
    jmethodID options_new, decode, recycle, create;
    jfieldID scaled, premultiplied, preferred;
} png_jni;
static png_jni codec;
static atomic_bool ready;
static pthread_mutex_t initialization=PTHREAD_MUTEX_INITIALIZER;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static bool decode_png(void *unused,const GhosttyAllocator *allocator,const uint8_t *data,size_t length,GhosttySysImage *out) {
    (void)unused;
    static const uint8_t magic[]={137,80,78,71,13,10,26,10};
    if(!atomic_load(&ready) || !data || !out || length<33 || length>PNG_BYTES ||
       memcmp(data,magic,8) || be32(data+8)!=13 || memcmp(data+12,"IHDR",4))return false;
    uint32_t width=be32(data+16),height=be32(data+20);
    if(!width || !height || width>DIMENSION_LIMIT || height>DIMENSION_LIMIT || (uint64_t)width*height>PIXEL_BYTES/4)return false;
    const size_t size=(size_t)width*height*4;
    uint8_t *pixels=ghostty_alloc(allocator,size);
    if(!pixels)return false;
    JNIEnv *env=NULL;bool attached=false,locked=false,success=false;
    jobject bitmap=NULL,options=NULL;jbyteArray bytes=NULL;void *source=NULL;
    jint status=(*codec.vm)->GetEnv(codec.vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*codec.vm)->AttachCurrentThread(codec.vm,&env,NULL)!=JNI_OK)goto done;
        attached=true;
    } else if(status!=JNI_OK)goto done;
    bytes=(*env)->NewByteArray(env,(jsize)length);
    if(!bytes)goto done;
    (*env)->SetByteArrayRegion(env,bytes,0,(jsize)length,(const jbyte *)data);
    if((*env)->ExceptionCheck(env))goto done;
    options=(*env)->NewObject(env,codec.options,codec.options_new);
    if(!options)goto done;
    (*env)->SetBooleanField(env,options,codec.scaled,JNI_FALSE);
    (*env)->SetBooleanField(env,options,codec.premultiplied,JNI_FALSE);
    (*env)->SetObjectField(env,options,codec.preferred,codec.rgba);
    if((*env)->ExceptionCheck(env))goto done;
    bitmap=(*env)->CallStaticObjectMethod(env,codec.factory,codec.decode,bytes,0,(jint)length,options);
    if((*env)->ExceptionCheck(env) || !bitmap)goto done;
    AndroidBitmapInfo info;
    if(AndroidBitmap_getInfo(env,bitmap,&info)!=ANDROID_BITMAP_RESULT_SUCCESS ||
       info.width!=width || info.height!=height || info.format!=ANDROID_BITMAP_FORMAT_RGBA_8888 || info.stride<width*4)goto done;
    if(AndroidBitmap_lockPixels(env,bitmap,&source)!=ANDROID_BITMAP_RESULT_SUCCESS)goto done;
    locked=true;if(!source)goto done;
    for(uint32_t y=0;y<height;y++)memcpy(pixels+(size_t)y*width*4,(const uint8_t *)source+(size_t)y*info.stride,(size_t)width*4);
    out->width=width;out->height=height;out->data=pixels;out->data_len=size;success=true;
done:
    if(env) {
        /* Decode failures are protocol errors, never a pending Java exception
         * left on the native IO owner. No exception content or image is logged. */
        if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
        if(locked)AndroidBitmap_unlockPixels(env,bitmap);
        if(bitmap) { (*env)->CallVoidMethod(env,bitmap,codec.recycle);(*env)->DeleteLocalRef(env,bitmap); }
        if(options)(*env)->DeleteLocalRef(env,options);
        if(bytes)(*env)->DeleteLocalRef(env,bytes);
        if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    }
    if(attached)(*codec.vm)->DetachCurrentThread(codec.vm);
    if(!success)ghostty_free(allocator,pixels,size);
    return success;
}

static jclass global_class(JNIEnv *env,const char *name) {
    jclass local=(*env)->FindClass(env,name);
    if(!local)return NULL;
    jclass global=(*env)->NewGlobalRef(env,local);
    (*env)->DeleteLocalRef(env,local);return global;
}
static void release_refs(JNIEnv *env,png_jni *value) {
    if(value->factory)(*env)->DeleteGlobalRef(env,value->factory);
    if(value->options)(*env)->DeleteGlobalRef(env,value->options);
    if(value->bitmap)(*env)->DeleteGlobalRef(env,value->bitmap);
    if(value->config)(*env)->DeleteGlobalRef(env,value->config);
    if(value->rgba)(*env)->DeleteGlobalRef(env,value->rgba);
}
bool gat_android_png_init(JNIEnv *env) {
    pthread_mutex_lock(&initialization);
    if(atomic_load(&ready)) { pthread_mutex_unlock(&initialization);return true; }
    png_jni value={0};bool success=false;
#define REQUIRED(field,call) do { value.field=(call);if(!value.field)goto done; } while(0)
    if((*env)->GetJavaVM(env,&value.vm)!=JNI_OK)goto done;
    REQUIRED(factory,global_class(env,"android/graphics/BitmapFactory"));
    REQUIRED(options,global_class(env,"android/graphics/BitmapFactory$Options"));
    REQUIRED(bitmap,global_class(env,"android/graphics/Bitmap"));
    REQUIRED(config,global_class(env,"android/graphics/Bitmap$Config"));
    REQUIRED(options_new,(*env)->GetMethodID(env,value.options,"<init>","()V"));
    REQUIRED(decode,(*env)->GetStaticMethodID(env,value.factory,"decodeByteArray","([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;"));
    REQUIRED(recycle,(*env)->GetMethodID(env,value.bitmap,"recycle","()V"));
    REQUIRED(create,(*env)->GetStaticMethodID(env,value.bitmap,"createBitmap","(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"));
    REQUIRED(scaled,(*env)->GetFieldID(env,value.options,"inScaled","Z"));
    REQUIRED(premultiplied,(*env)->GetFieldID(env,value.options,"inPremultiplied","Z"));
    REQUIRED(preferred,(*env)->GetFieldID(env,value.options,"inPreferredConfig","Landroid/graphics/Bitmap$Config;"));
    jfieldID config_field=(*env)->GetStaticFieldID(env,value.config,"ARGB_8888","Landroid/graphics/Bitmap$Config;");
    if(!config_field)goto done;
    jobject config=(*env)->GetStaticObjectField(env,value.config,config_field);
    if(!config)goto done;
    value.rgba=(*env)->NewGlobalRef(env,config);(*env)->DeleteLocalRef(env,config);
    if(!value.rgba)goto done;
    if(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void *)decode_png)!=GHOSTTY_SUCCESS)goto done;
    codec=value;atomic_store(&ready,true);success=true;
done:
    if(!success)release_refs(env,&value);
    pthread_mutex_unlock(&initialization);return success;
#undef REQUIRED
}

JNIEnv *gat_android_env_acquire(bool *attached) {
    *attached=false;if(!atomic_load(&ready))return NULL;
    JNIEnv *env=NULL;jint status=(*codec.vm)->GetEnv(codec.vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*codec.vm)->AttachCurrentThread(codec.vm,&env,NULL)!=JNI_OK)return NULL;
        *attached=true;
    } else if(status!=JNI_OK)return NULL;
    return env;
}
void gat_android_env_release(bool attached) { if(attached)(*codec.vm)->DetachCurrentThread(codec.vm); }

jobject gat_android_bitmap_copy(JNIEnv *env,uint32_t width,uint32_t height,const uint8_t *pixels,int format) {
    if(!env || !pixels || !width || !height || width>4096 || height>4096 || (format!=0 && format!=1))return NULL;
    jobject bitmap=(*env)->CallStaticObjectMethod(env,codec.bitmap,codec.create,(jint)width,(jint)height,codec.rgba);
    if((*env)->ExceptionCheck(env) || !bitmap) { if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);return NULL; }
    AndroidBitmapInfo info;void *target=NULL;jobject result=NULL;bool locked=false;
    if(AndroidBitmap_getInfo(env,bitmap,&info)!=ANDROID_BITMAP_RESULT_SUCCESS || info.format!=ANDROID_BITMAP_FORMAT_RGBA_8888 || info.stride<width*4)goto done;
    if(AndroidBitmap_lockPixels(env,bitmap,&target)!=ANDROID_BITMAP_RESULT_SUCCESS)goto done;
    locked=true;if(!target)goto done;
    const size_t channels=format==0?3:4;
    for(uint32_t y=0;y<height;y++) {
        uint8_t *row=(uint8_t *)target+(size_t)y*info.stride;
        const uint8_t *source=pixels+(size_t)y*width*channels;
        for(uint32_t x=0;x<width;x++) {
            unsigned alpha=channels==4?source[x*channels+3]:255;
            row[x*4]=(uint8_t)((source[x*channels]*alpha+127)/255);
            row[x*4+1]=(uint8_t)((source[x*channels+1]*alpha+127)/255);
            row[x*4+2]=(uint8_t)((source[x*channels+2]*alpha+127)/255);
            row[x*4+3]=(uint8_t)alpha;
        }
    }
    AndroidBitmap_unlockPixels(env,bitmap);locked=false;
    result=(*env)->NewGlobalRef(env,bitmap);
done:
    if(locked)AndroidBitmap_unlockPixels(env,bitmap);
    (*env)->DeleteLocalRef(env,bitmap);
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    return result;
}
