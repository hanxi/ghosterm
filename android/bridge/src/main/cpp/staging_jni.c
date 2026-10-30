#define _GNU_SOURCE
#include <jni.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

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
