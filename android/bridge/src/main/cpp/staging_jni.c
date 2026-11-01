#define _GNU_SOURCE
#include <jni.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

/* android.system.Os.fcntlInt is public only from API 30. Keep the API 29
 * adapter here with the other platform filesystem controls. Java owns and
 * pins the passed descriptor; no Core handles or file bytes cross this seam. */
static void descriptor_error(JNIEnv *env, const char *operation, int error) {
    jclass type=(*env)->FindClass(env,"android/system/ErrnoException");
    if(!type)return;
    jmethodID constructor=(*env)->GetMethodID(env,type,"<init>","(Ljava/lang/String;I)V");
    if(!constructor)return;
    jstring name=(*env)->NewStringUTF(env,operation);
    if(!name)return;
    jobject failure=(*env)->NewObject(env,type,constructor,name,(jint)error);
    if(failure)(*env)->Throw(env,(jthrowable)failure);
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_FileDescriptorFlags_getStatus(
    JNIEnv *env, jobject self, jint fd) {
    (void)self;
    int result=fcntl(fd,F_GETFL);
    if(result<0)descriptor_error(env,"fcntl(F_GETFL)",errno);
    return result;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_FileDescriptorFlags_setStatus(
    JNIEnv *env, jobject self, jint fd, jint flags) {
    (void)self;
    if(fcntl(fd,F_SETFL,flags)<0)descriptor_error(env,"fcntl(F_SETFL)",errno);
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_FileDescriptorFlags_getDescriptor(
    JNIEnv *env, jobject self, jint fd) {
    (void)self;
    int result=fcntl(fd,F_GETFD);
    if(result<0)descriptor_error(env,"fcntl(F_GETFD)",errno);
    return result;
}

/* Android-only filesystem adapter; no Core handles or data bytes cross JNI.
 * O_TMPFILE avoids a crash window with a named temporary file. O_EXCL forbids
 * later linking this inode into the user's filesystem. */
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_AnonymousStagingFile_open(
    JNIEnv *env, jobject self, jbyteArray path) {
    (void)self;
    if (!path) return -1;
    jsize n=(*env)->GetArrayLength(env,path);
    if(n<=0 || n>=65536) {
        (*env)->ThrowNew(env,(*env)->FindClass(env,"java/lang/IllegalArgumentException"),"无效的暂存路径");return -1;
    }
    char *value=calloc((size_t)n+1,1);
    if(!value) {
        (*env)->ThrowNew(env,(*env)->FindClass(env,"java/lang/OutOfMemoryError"),"无法分配暂存路径");return -1;
    }
    (*env)->GetByteArrayRegion(env,path,0,n,(jbyte *)value);
    if((*env)->ExceptionCheck(env)){free(value);return -1;}
    if(memchr(value,0,(size_t)n)) {
        free(value);(*env)->ThrowNew(env,(*env)->FindClass(env,"java/lang/IllegalArgumentException"),"暂存路径含 NUL");return -1;
    }
    int directory=open(value,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    free(value);
    int fd=-1,error=errno;
    if(directory>=0) {
        fd=openat(directory,".",O_TMPFILE|O_RDWR|O_EXCL|O_CLOEXEC,0600);
        error=errno;close(directory);
    }
    if(fd<0) {
        char message[160];
        snprintf(message,sizeof message,"无法创建匿名暂存文件（系统错误 %d）；设备可能不支持或空间不足",error);
        (*env)->ThrowNew(env,(*env)->FindClass(env,"java/io/IOException"),message);
    }
    return fd;
}
