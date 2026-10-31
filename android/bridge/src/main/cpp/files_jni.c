#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include "ghosterm.h"
#include "ssh_session.h"

/* Dedicated SSH/SFTP owner. JNI only copies control/results; no Core
 * operation, blocking network call, join or destruction runs on a JNI caller. */
#define FILE_SLOTS 16
#define RESULT_LIMIT (4 * 1024 * 1024)
#define PREVIEW_LIMIT (256 * 1024)
typedef struct file_session {
    size_t refs;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    atomic_bool stop, mkdir_cancel;
    gaj_ssh *auth;
    int phase, error; /* 0 connecting, 1 ready, 2 failed, 3 stopped */
    uint64_t sequence, request;
    int kind, status;
    int source_fd, release_fd, owner_fence;
    uint64_t expected_size;
    uint32_t budget;
    bool complete, progress_enabled, progress_present;
    gt_sftp_progress_t progress;
    char *path, *destination;
    bool publish_directory;
    uint8_t *result;
    size_t length;
} file_session;
static struct { file_session *value; uint64_t generation; } registry[FILE_SLOTS];
static pthread_mutex_t registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static atomic_int owners;
static void fail(JNIEnv *env,const char *message) {
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if(type)(*env)->ThrowNew(env,type,message);
}
static void release(file_session *s) {
    pthread_mutex_lock(&registry_mutex);bool destroy=--s->refs==0;pthread_mutex_unlock(&registry_mutex);
    if(!destroy)return;
    gaj_ssh_free(s->auth);free(s->path);free(s->destination);free(s->result);
    if(s->source_fd>=0)close(s->source_fd);
    if(s->release_fd>=0)close(s->release_fd);
    int owner_fence=s->owner_fence;
    pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->mutex);free(s);
    /* Last reference: owner IO, authentication and pending descriptors are gone.
     * Unlike request/fd fences this also covers setup failure and idle close. */
    if(owner_fence>=0)close(owner_fence);
}
static file_session *borrow(JNIEnv *env,jlong id) {
    if(id<=0){fail(env,"文件连接已关闭");return NULL;}
    size_t slot=(uint64_t)id%FILE_SLOTS;uint64_t generation=(uint64_t)id/FILE_SLOTS;
    pthread_mutex_lock(&registry_mutex);
    file_session *s=registry[slot].generation==generation?registry[slot].value:NULL;
    if(s)s->refs++;
    pthread_mutex_unlock(&registry_mutex);
    if(!s)fail(env,"文件连接已关闭");return s;
}
static bool stopped(void *context){return atomic_load(&((file_session *)context)->stop);}
static bool mkdir_stopped(void *context){file_session *s=context;return stopped(s)||atomic_load(&s->mkdir_cancel);}
typedef struct { file_session *session; uint64_t request; } progress_context;
static void transfer_progress(const gt_sftp_progress_t *value, void *context) {
    progress_context *owner=context;file_session *s=owner->session;
    pthread_mutex_lock(&s->mutex);
    if(s->request==owner->request && !stopped(s) && !s->complete && s->progress_enabled) {
        s->progress=*value;s->progress_present=true;
    }
    pthread_mutex_unlock(&s->mutex);
}
static void u32(uint8_t **p,uint32_t value){for(int i=3;i>=0;i--)*(*p)++=(uint8_t)(value>>(i*8));}
static void u64(uint8_t **p,uint64_t value){for(int i=7;i>=0;i--)*(*p)++=(uint8_t)(value>>(i*8));}
static void string(uint8_t **p,const char *value){size_t n=strlen(value);u32(p,(uint32_t)n);memcpy(*p,value,n);*p+=n;}
static int directory(file_session *s,gt_sftp_t *files,const char *path,uint32_t budget,uint8_t **out,size_t *length) {
    char canonical[4096];gt_sftp_entry_t *entries=NULL;size_t count=0;
    int status=gt_sftp_browse_controlled(files,path,canonical,sizeof canonical,&entries,&count,budget,4096,stopped,s);
    if(status!=GT_OK)return status;
    size_t total=8+strlen(canonical);
    for(size_t i=0;i<count;i++) {
        size_t n=strlen(entries[i].full_path);
        if(n>RESULT_LIMIT || total>RESULT_LIMIT-24-n){gt_sftp_list_free(entries,count);return GT_ERR_IO;}
        total+=24+n;
    }
    uint8_t *bytes=malloc(total);if(!bytes){gt_sftp_list_free(entries,count);return GT_ERR_INTERNAL;}
    uint8_t *p=bytes;string(&p,canonical);u32(&p,(uint32_t)count);
    for(size_t i=0;i<count;i++) {
        const gt_sftp_stat_t *st=&entries[i].stat;
        string(&p,entries[i].full_path);
        u32(&p,(st->is_dir?1u:0u)|(st->is_symlink?2u:0u)|(st->is_regular?4u:0u)|(st->has_size?8u:0u));
        u64(&p,st->size);u64(&p,(uint64_t)st->mtime_unix);
    }
    gt_sftp_list_free(entries,count);*out=bytes;*length=total;return GT_OK;
}
static void *owner(void *context) {
    file_session *s=context;gt_ssh_t *ssh=gaj_ssh_connect(s->auth,&s->stop);gt_sftp_t *files=NULL;
    int status=GT_ERR_IO;int directory_fence=-1;
    if(!ssh || stopped(s))goto finished;
    gt_status_t opened=GT_OK;files=gaj_ssh_open_sftp(s->auth,ssh,&s->stop,&opened);status=opened;
    if(!files || stopped(s))goto finished;
    pthread_mutex_lock(&s->mutex);s->phase=1;pthread_mutex_unlock(&s->mutex);
    while(!stopped(s)) {
        pthread_mutex_lock(&s->mutex);
        while(!s->path && !stopped(s))pthread_cond_wait(&s->wake,&s->mutex);
        char *path=s->path;s->path=NULL;
        char *destination=s->destination;s->destination=NULL;bool publish_directory=s->publish_directory;
        uint64_t request=s->request;int kind=s->kind;uint32_t budget=s->budget;bool observe=s->progress_enabled;
        int source_fd=s->source_fd;s->source_fd=-1;uint64_t expected_size=s->expected_size;
        int release_fd=s->release_fd;s->release_fd=-1;
        pthread_mutex_unlock(&s->mutex);
        if(kind==6){directory_fence=release_fd;release_fd=-1;}
        if(stopped(s)){free(path);free(destination);if(source_fd>=0)close(source_fd);if(release_fd>=0)close(release_fd);break;}
        uint8_t *result=NULL;size_t length=0;progress_context observation={s,request};
        if(kind==1)status=directory(s,files,path,budget,&result,&length);
        else if(kind==3) {
            result=malloc(12);
            if(result) {
                gt_sftp_create_result_t outcome;
                if(observe) status=gt_sftp_create_observed(files,path,source_fd,expected_size,&outcome,budget,stopped,s,transfer_progress,&observation);
                else status=gt_sftp_create_controlled(files,path,source_fd,expected_size,&outcome,budget,stopped,s);
                uint8_t *p=result;u32(&p,(uint32_t)outcome.state);u64(&p,outcome.bytes_acked);length=12;
            } else status=GT_ERR_INTERNAL;
        } else if(kind==5) {
            result=malloc(8);
            if(result) {
                gt_sftp_publish_result_t outcome;
                status=gt_sftp_publish_controlled(files,path,destination,publish_directory,expected_size,&outcome,budget,stopped,s);
                uint8_t *p=result;u32(&p,(uint32_t)outcome.state);u32(&p,outcome.remote_status);length=8;
            } else status=GT_ERR_INTERNAL;
        } else if(kind==6) {
            result=malloc(8);
            if(result) {
                gt_sftp_mkdir_result_t outcome;
                status=gt_sftp_mkdir_controlled(files,path,destination,&outcome,budget,mkdir_stopped,s);
                uint8_t *p=result;u32(&p, (uint32_t)outcome.state);u32(&p, outcome.remote_status);length=8;
            } else status=GT_ERR_INTERNAL;
        } else if(kind==4) {
            uint64_t count=0;
            if(observe) status=gt_sftp_receive_observed(files,path,source_fd,expected_size,&count,budget,stopped,s,transfer_progress,&observation);
            else status=gt_sftp_receive_controlled(files,path,source_fd,expected_size,&count,budget,stopped,s);
            if(status==GT_OK) {
                result=malloc(8);
                if(result){uint8_t *p=result;u64(&p,count);length=8;}else status=GT_ERR_INTERNAL;
            }
        } else {
            result=malloc(PREVIEW_LIMIT);
            status=result?gt_sftp_preview_controlled(files,path,result,PREVIEW_LIMIT,&length,budget,stopped,s):GT_ERR_INTERNAL;
        }
        free(destination);
        free(path);if(source_fd>=0)close(source_fd);
        if(release_fd>=0)close(release_fd); /* EOF releases the caller's fd lease. */
        if(status!=GT_OK && kind!=3 && kind!=5 && kind!=6){free(result);result=NULL;length=0;}
        pthread_mutex_lock(&s->mutex);
        if(s->request==request && !stopped(s)) {
            s->result=result;s->length=length;s->status=status;s->complete=true;result=NULL;
        }
        pthread_mutex_unlock(&s->mutex);free(result);
        if(status!=GT_OK)break;
        if(directory_fence>=0){close(directory_fence);directory_fence=-1;}
    }
finished:
    /* A stop can win before the loop dequeues its pending descriptors. Close the
     * data duplicate BEFORE the completion pipe, even on this path. */
    pthread_mutex_lock(&s->mutex);
    if(s->source_fd>=0){close(s->source_fd);s->source_fd=-1;}
    if(s->release_fd>=0){
        if(s->kind==6){directory_fence=s->release_fd;}else close(s->release_fd);
        s->release_fd=-1;
    }
    pthread_mutex_unlock(&s->mutex);
    /* No terminal or transfer shares this socket. Abort before SFTP cleanup so
     * a peer withholding CHANNEL_CLOSE cannot keep this detached owner alive. */
    if(ssh)(void)gt_ssh_abort(ssh);
    gt_sftp_close(files);gt_ssh_close(ssh);
    pthread_mutex_lock(&s->mutex);
    s->phase=stopped(s)?3:2;s->error=stopped(s)?GT_ERR_CANCELLED:status;
    pthread_mutex_unlock(&s->mutex);
    atomic_fetch_sub(&owners,1);
    if(directory_fence>=0)close(directory_fence);
    /* The connection fence in release must follow every request-local fd. */
    release(s);
    return NULL;
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_create(JNIEnv *env,jobject self,
        jbyteArray host,jint port,jbyteArray user,jbyteArray known,jobjectArray addresses,jintArray scopes,jint timeout,jint completion_fd) {
    (void)self;gaj_ssh *auth=gaj_ssh_new(env,host,port,user,known,addresses,scopes,timeout);if(!auth)return 0;
    file_session *s=calloc(1,sizeof(*s));if(!s){gaj_ssh_free(auth);fail(env,"无法分配文件连接");return 0;}
    s->source_fd=-1;s->release_fd=-1;s->owner_fence=-1;s->auth=auth;atomic_init(&s->stop,false);atomic_init(&s->mkdir_cancel,false);
    if(pthread_mutex_init(&s->mutex,NULL)){gaj_ssh_free(auth);free(s);fail(env,"无法初始化文件锁");return 0;}
    if(pthread_cond_init(&s->wake,NULL)){pthread_mutex_destroy(&s->mutex);gaj_ssh_free(auth);free(s);fail(env,"无法初始化文件队列");return 0;}
    s->refs=1;s->owner_fence=fcntl(completion_fd,F_DUPFD_CLOEXEC,0);
    if(s->owner_fence<0){release(s);fail(env,"无法保留连接释放通知");return 0;}
    s->refs=2;
    pthread_mutex_lock(&registry_mutex);
    size_t slot=0;while(slot<FILE_SLOTS && registry[slot].value)slot++;
    if(slot==FILE_SLOTS || registry[slot].generation >= (uint64_t)INT64_MAX/FILE_SLOTS-1) {
        pthread_mutex_unlock(&registry_mutex);s->refs=1;release(s);fail(env,"文件连接数量已达上限");return 0;
    }
    uint64_t generation=++registry[slot].generation;registry[slot].value=s;
    jlong id=(jlong)(generation*FILE_SLOTS+slot);pthread_mutex_unlock(&registry_mutex);
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&owners,1);int rc=pthread_create(&thread,&attr,owner,s);pthread_attr_destroy(&attr);
    if(rc){atomic_fetch_sub(&owners,1);pthread_mutex_lock(&registry_mutex);registry[slot].value=NULL;pthread_mutex_unlock(&registry_mutex);release(s);release(s);fail(env,"无法启动文件线程");return 0;}
    return id;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_dispose(JNIEnv *env,jobject self,jlong id) {
    (void)env;(void)self;if(id<=0)return;
    size_t slot=(uint64_t)id%FILE_SLOTS;uint64_t generation=(uint64_t)id/FILE_SLOTS;
    pthread_mutex_lock(&registry_mutex);file_session *s=registry[slot].generation==generation?registry[slot].value:NULL;
    if(s){registry[slot].value=NULL;atomic_store(&s->stop,true);}
    pthread_mutex_unlock(&registry_mutex);
    if(s){pthread_mutex_lock(&s->mutex);pthread_cond_broadcast(&s->wake);pthread_mutex_unlock(&s->mutex);release(s);}
}
JNIEXPORT jintArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_state(JNIEnv *env,jobject self,jlong id) {
    (void)self;file_session *s=borrow(env,id);if(!s)return NULL;
    pthread_mutex_lock(&s->mutex);jint values[2]={s->phase,s->error};pthread_mutex_unlock(&s->mutex);
    jintArray out=(*env)->NewIntArray(env,2);if(out)(*env)->SetIntArrayRegion(env,out,0,2,values);release(s);return out;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_authentication(JNIEnv *env,jobject self,jlong id) {
    (void)self;file_session *s=borrow(env,id);if(!s)return NULL;
    jobjectArray out=gaj_ssh_info(env,s->auth);release(s);return out;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_respond(JNIEnv *env,jobject self,jlong id,jlong request,jint kind,jbyteArray first,jbyteArray second) {
    (void)self;file_session *s=borrow(env,id);if(!s)return;
    gaj_ssh_respond(env,s->auth,request,kind,first,second);release(s);
}
static jlong begin(JNIEnv *env,jlong id,jint kind,jbyteArray path,jint budget,int source_fd,uint64_t expected_size,int release_fd,jbyteArray destination,bool publish_directory,bool observe) {
    if(!path || (kind!=1 && kind!=2 && kind!=3 && kind!=4 && kind!=5 && kind!=6) || budget<=0 || budget>60000){fail(env,"文件请求无效");return 0;}
    jsize n=(*env)->GetArrayLength(env,path);if(n<=0 || n>=65536){fail(env,"文件路径长度无效");return 0;}
    char *value=calloc((size_t)n+1,1);if(!value){fail(env,"无法分配文件路径");return 0;}
    (*env)->GetByteArrayRegion(env,path,0,n,(jbyte *)value);
    if((*env)->ExceptionCheck(env) || memchr(value,0,(size_t)n)){free(value);if(!(*env)->ExceptionCheck(env))fail(env,"文件路径包含 NUL");return 0;}
    char *target=NULL;
    if(kind==5 || kind==6) {
        jsize count=destination?(*env)->GetArrayLength(env,destination):0;
        if(count<=0 || count>=65536){free(value);fail(env,"发布目标长度无效");return 0;}
        target=calloc((size_t)count+1,1);
        if(!target){free(value);fail(env,"无法分配发布目标");return 0;}
        (*env)->GetByteArrayRegion(env,destination,0,count,(jbyte *)target);
        if((*env)->ExceptionCheck(env) || memchr(target,0,(size_t)count)) {
            free(value);free(target);if(!(*env)->ExceptionCheck(env))fail(env,"发布目标包含 NUL");return 0;
        }
    }
    file_session *s=borrow(env,id);if(!s){free(value);free(target);return 0;}
    pthread_mutex_lock(&s->mutex);jlong request=0;
    if(s->phase==1 && !stopped(s) && !s->request && s->sequence<INT64_MAX) {
        s->request=++s->sequence;request=(jlong)s->request;s->kind=kind;s->budget=(uint32_t)budget;s->path=value;value=NULL;s->source_fd=source_fd;s->expected_size=expected_size;s->release_fd=release_fd;
        s->destination=target;target=NULL;s->publish_directory=publish_directory;
        s->progress_enabled=observe;s->progress_present=false;s->progress=(gt_sftp_progress_t){0};
        atomic_store(&s->mkdir_cancel,false);s->complete=false;s->status=GT_OK;pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->mutex);release(s);free(value);free(target);
    if(!request)fail(env,"文件连接未就绪或请求仍在执行");return request;
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_begin(JNIEnv *env,jobject self,jlong id,jint kind,jbyteArray path,jint budget) {
    (void)self;if(kind!=1 && kind!=2){fail(env,"文件请求无效");return 0;}
    return begin(env,id,kind,path,budget,-1,0,-1,NULL,false,false);
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_beginCreate(JNIEnv *env,jobject self,jlong id,jbyteArray path,jint source_fd,jlong expected_size,jint budget,jint release_fd,jboolean observe) {
    (void)self;if(source_fd<0 || expected_size<0){fail(env,"文件源描述符或尺寸无效");return 0;}
    int owned=fcntl(source_fd,F_DUPFD_CLOEXEC,0);
    if(owned<0){fail(env,"无法保留源文件描述符");return 0;}
    int fence=fcntl(release_fd,F_DUPFD_CLOEXEC,0);
    if(fence<0){close(owned);fail(env,"无法保留文件释放通知");return 0;}
    jlong request=begin(env,id,3,path,budget,owned,(uint64_t)expected_size,fence,NULL,false,observe);
    if(!request){close(owned);close(fence);}
    return request;
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_beginReceive(JNIEnv *env,jobject self,jlong id,jbyteArray path,jint destination_fd,jlong max_bytes,jint budget,jint release_fd,jboolean observe) {
    (void)self;if(destination_fd<0 || max_bytes<0){fail(env,"文件目标描述符或限额无效");return 0;}
    int owned=fcntl(destination_fd,F_DUPFD_CLOEXEC,0);
    if(owned<0){fail(env,"无法保留目标文件描述符");return 0;}
    int fence=fcntl(release_fd,F_DUPFD_CLOEXEC,0);
    if(fence<0){close(owned);fail(env,"无法保留文件释放通知");return 0;}
    jlong request=begin(env,id,4,path,budget,owned,(uint64_t)max_bytes,fence,NULL,false,observe);
    if(!request){close(owned);close(fence);}
    return request;
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_beginPublish(JNIEnv *env,jobject self,jlong id,jbyteArray source,jbyteArray destination,jboolean directory,jlong expected_size,jint budget) {
    (void)self;
    if(expected_size<0 || (directory && expected_size!=0)){fail(env,"发布源尺寸无效");return 0;}
    return begin(env,id,5,source,budget,-1,(uint64_t)expected_size,-1,destination,directory,false);
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_beginMkdir(JNIEnv *env,jobject self,jlong id,jbyteArray parent,jbyteArray name,jint budget,jint release_fd) {
    (void)self;
    int fence=fcntl(release_fd,F_DUPFD_CLOEXEC,0);
    if(fence<0){fail(env,"无法保留目录完成通知");return 0;}
    jlong request=begin(env,id,6,parent,budget,-1,0,fence,name,false,false);
    if(!request)close(fence);
    return request;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_cancelMkdir(JNIEnv *env,jobject self,jlong id,jlong request) {
    (void)env;(void)self;if(id<=0 || request<=0)return;
    size_t slot=(uint64_t)id%FILE_SLOTS;uint64_t generation=(uint64_t)id/FILE_SLOTS;
    pthread_mutex_lock(&registry_mutex);
    file_session *s = registry[slot].generation==generation ? registry[slot].value : NULL;
    if(s)s->refs++;
    pthread_mutex_unlock(&registry_mutex);
    if(!s)return;
    pthread_mutex_lock(&s->mutex);
    if(s->request==(uint64_t)request && s->kind==6 && !s->complete)atomic_store(&s->mkdir_cancel,true);
    pthread_mutex_unlock(&s->mutex);release(s);
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_progress(JNIEnv *env,jobject self,jlong id,jlong request) {
    (void)self;file_session *s=borrow(env,id);if(!s)return NULL;
    jlong values[5]={0,0,0,0,request};bool valid;
    pthread_mutex_lock(&s->mutex);
    valid=request>0 && s->request==(uint64_t)request && s->progress_enabled && (s->kind==3 || s->kind==4);
    if(valid && s->progress_present) {
        values[0]=(jlong)s->progress.kind;values[1]=(jlong)s->progress.bytes;
        values[2]=s->progress.total_known?1:0;values[3]=(jlong)s->progress.total_bytes;
    }
    pthread_mutex_unlock(&s->mutex);release(s);
    if(!valid){fail(env,"传输进度请求已失效或不属于当前操作");return NULL;}
    jlongArray out=(*env)->NewLongArray(env,5);
    if(out)(*env)->SetLongArrayRegion(env,out,0,5,values);
    return out;
}
JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_result(JNIEnv *env,jobject self,jlong id,jlong request) {
    (void)self;file_session *s=borrow(env,id);if(!s)return NULL;jbyteArray out=NULL;
    pthread_mutex_lock(&s->mutex);
    if(s->request!=(uint64_t)request || !request)fail(env,"文件请求已失效");
    else if(s->complete) {
        uint8_t header[8],*p=header;u32(&p,(uint32_t)s->status);u32(&p,(uint32_t)s->kind);
        out=(*env)->NewByteArray(env,(jsize)s->length+8);
        if(out){(*env)->SetByteArrayRegion(env,out,0,8,(jbyte *)header);if(s->length)(*env)->SetByteArrayRegion(env,out,8,(jsize)s->length,(jbyte *)s->result);}
    }
    pthread_mutex_unlock(&s->mutex);release(s);return out;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_finish(JNIEnv *env,jobject self,jlong id,jlong request) {
    (void)self;file_session *s=borrow(env,id);if(!s)return;
    pthread_mutex_lock(&s->mutex);
    if(s->request!=(uint64_t)request || !request || !s->complete)fail(env,"文件结果已失效或尚未完成");
    else {free(s->result);s->result=NULL;s->length=0;s->request=0;s->complete=false;}
    pthread_mutex_unlock(&s->mutex);release(s);
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_ownerCount(JNIEnv *env,jobject self){(void)env;(void)self;return atomic_load(&owners);}
