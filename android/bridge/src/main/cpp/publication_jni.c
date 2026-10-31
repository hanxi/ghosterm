#define _GNU_SOURCE
#include <jni.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

static void failure(JNIEnv *env, int error) {
    char message[96];
    snprintf(message,sizeof message,"本机文件发布失败（系统错误 %d）",error);
    (*env)->ThrowNew(env,(*env)->FindClass(env,"java/io/IOException"),message);
}
static char *text(JNIEnv *env, jbyteArray input) {
    if(!input){failure(env,EINVAL);return NULL;}
    jsize size=(*env)->GetArrayLength(env,input);
    if(size>=65536){failure(env,ENAMETOOLONG);return NULL;}
    char *value=calloc((size_t)size+1,1);
    if(!value){failure(env,ENOMEM);return NULL;}
    (*env)->GetByteArrayRegion(env,input,0,size,(jbyte *)value);
    if((*env)->ExceptionCheck(env)){free(value);return NULL;}
    if(memchr(value,0,(size_t)size)){free(value);failure(env,EINVAL);return NULL;}
    return value;
}
static int component(const char *name) {
    return *name && strcmp(name,".") && strcmp(name,"..") && !strchr(name,'/');
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_openDirectory(
    JNIEnv *env,jobject self,jbyteArray root,jbyteArray relative) {
    (void)self;
    char *base=text(env,root);if(!base)return -1;
    char *path=text(env,relative);if(!path){free(base);return -1;}
    int fd=-1,error=0;
    /* Validate all components BEFORE opening anything. Empty relative means root. */
    if(!*base || (*path && (path[0]=='/' || path[strlen(path)-1]=='/' || strstr(path,"//"))))error=EINVAL;
    for(char *part=path;!error && *part;) {
        char *slash=strchr(part,'/');if(slash)*slash=0;
        if(!component(part))error=EINVAL;
        if(slash){*slash='/';part=slash+1;}else break;
    }
    if(!error){fd=open(base,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)error=errno;}
    for(char *part=path;!error && *part;) {
        char *slash=strchr(part,'/');if(slash)*slash=0;
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(next<0)error=errno;
        close(fd);fd=next;
        if(slash)part=slash+1;else break;
    }
    free(path);free(base);
    if(error){if(fd>=0)close(fd);failure(env,error);return -1;}
    return fd;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_openCandidate(
    JNIEnv *env,jobject self,jint directory,jbyteArray name) {
    (void)self;
    char *value=text(env,name);if(!value)return -1;
    if(!component(value)){free(value);failure(env,EINVAL);return -1;}
    int fd=openat(directory,value,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    int error=errno;free(value);
    if(fd<0)failure(env,error);
    return fd;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_createDirectory(
    JNIEnv *env,jobject self,jint directory,jbyteArray name) {
    (void)self;
    char *value=text(env,name);if(!value)return EINVAL;
    int error=0;
    if(!component(value))error=EINVAL;
    else if(mkdirat(directory,value,0700)<0)error=errno;
    free(value);return error;
}
/* A named task candidate is kept outside browsable HOME. Check identity before
 * rename/cleanup; this is not protection against arbitrary same-UID writers
 * racing inside the application's private staging directory. */
static int identity(int directory,const char *name,int descriptor,struct stat *actual) {
    struct stat named;
    if(fstat(descriptor,actual)<0 || fstatat(directory,name,&named,AT_SYMLINK_NOFOLLOW)<0)return errno;
    if(!S_ISREG(actual->st_mode) || !S_ISREG(named.st_mode) || actual->st_nlink!=1 ||
        actual->st_dev!=named.st_dev || actual->st_ino!=named.st_ino)return EINVAL;
    return 0;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_publishCandidate(
    JNIEnv *env,jobject self,jint destination,jint staging,jint candidate,jbyteArray source,
    jbyteArray name,jlong size) {
    (void)self;
    char *from=text(env,source);if(!from)return EINVAL;
    char *value=text(env,name);if(!value){free(from);return EINVAL;}
    struct stat st;
    int error=0;
    if(!component(value) || !component(from) || size<0)error=EINVAL;
    else error=identity(staging,from,candidate,&st);
    if(!error && st.st_size!=size)error=EINVAL;
    if(!error) {
        int flags=fcntl(candidate,F_GETFL);
        if(flags<0)error=errno;
        else if((flags&O_ACCMODE)!=O_RDONLY)error=EINVAL;
    }
    /* No fallback to rename/link/copy. ENOSYS/EOPNOTSUPP/EXDEV remain errors. */
    if(!error && syscall(__NR_renameat2,staging,from,destination,value,RENAME_NOREPLACE)<0)error=errno;
    free(from);free(value);return error;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_removeCandidate(
    JNIEnv *env,jobject self,jint directory,jint candidate,jbyteArray name) {
    (void)self;
    char *value=text(env,name);if(!value)return EINVAL;
    struct stat st;
    int error=component(value)?identity(directory,value,candidate,&st):EINVAL;
    if(error==ENOENT)error=0;
    else if(!error && unlinkat(directory,value,0)<0)error=errno;
    free(value);return error;
}
