#include "tmux_session.h"
#include "ssh_session.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#define TMUX_SLOTS 16
#define PACKET_LIMIT (2u*1024u*1024u)
typedef struct tmux_owner {
    size_t refs;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    atomic_bool stop;
    gaj_ssh *auth;
    int phase,error,fence,budget;
    char *socket_path,*argument;
    uint64_t sequence,request,snapshot;
    bool pending,active,complete;
    int kind,index;
    uint64_t expected_snapshot;
    uint8_t *packet;size_t length;
    uint8_t address[16],family;
    uint32_t scope;
    /* Core state and the copied target list are exclusive to owner. */
    gt_tmux_t *tmux;
    gt_tmux_session_t *sessions;
    size_t count;
    _Atomic(atomic_bool *) attach_stop;
    gt_ssh_t *attach_ssh;
    gt_shell_opts_t attach_size;
    gt_transport_t *attached;
    gt_status_t attach_status;
    char attach_error[256];
} tmux_owner;
struct gaj_tmux_target { uint64_t owner,snapshot;int index; };
static struct { tmux_owner *value;uint64_t generation; } registry[TMUX_SLOTS];
static pthread_mutex_t registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static atomic_int owners;
static atomic_llong create_requests;
static void fail(JNIEnv *env,const char *text) {
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if(type)(*env)->ThrowNew(env,type,text);
}
static void release(tmux_owner *s) {
    pthread_mutex_lock(&registry_mutex);bool destroy=--s->refs==0;pthread_mutex_unlock(&registry_mutex);
    if(!destroy)return;
    gaj_ssh_free(s->auth);free(s->socket_path);free(s->argument);free(s->packet);free(s->sessions);
    int fence=s->fence;pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->mutex);free(s);
    if(fence>=0)close(fence);
}
static tmux_owner *lookup(uint64_t id) {
    if(!id || id>INT64_MAX)return NULL;
    size_t slot=id%TMUX_SLOTS;uint64_t generation=id/TMUX_SLOTS;
    pthread_mutex_lock(&registry_mutex);
    tmux_owner *s=registry[slot].generation==generation?registry[slot].value:NULL;
    if(s)s->refs++;
    pthread_mutex_unlock(&registry_mutex);return s;
}
static tmux_owner *borrow(JNIEnv *env,jlong id) {
    tmux_owner *s=lookup((uint64_t)id);if(!s)fail(env,"后台会话连接已关闭");return s;
}
static bool cancelled(void *context) {
    tmux_owner *s=context;
    atomic_bool *attach_stop=atomic_load(&s->attach_stop);
    return atomic_load(&s->stop) || (attach_stop && atomic_load(attach_stop));
}
static char *copy(JNIEnv *env,jbyteArray bytes,size_t limit,bool empty) {
    if(!bytes){fail(env,"后台会话参数缺失");return NULL;}
    jsize n=(*env)->GetArrayLength(env,bytes);
    if((!empty && !n) || (size_t)n>limit){fail(env,"后台会话参数过长或为空");return NULL;}
    char *text=calloc((size_t)n+1,1);if(!text){fail(env,"后台会话内存不足");return NULL;}
    (*env)->GetByteArrayRegion(env,bytes,0,n,(jbyte *)text);
    if((*env)->ExceptionCheck(env) || memchr(text,0,(size_t)n)){
        free(text);if(!(*env)->ExceptionCheck(env))fail(env,"后台会话参数包含 NUL");return NULL;
    }
    return text;
}
typedef struct { uint8_t *bytes,*next;size_t capacity;bool ok; } writer;
static void u32(writer *w,uint32_t value) {
    if(!w->ok || (size_t)(w->next-w->bytes)>w->capacity-4){w->ok=false;return;}
    for(int i=3;i>=0;i--)*w->next++=(uint8_t)(value>>(8*i));
}
static void u64(writer *w,uint64_t value){u32(w,(uint32_t)(value>>32));u32(w,(uint32_t)value);}
static void string(writer *w,const char *text,size_t bound) {
    size_t n=strnlen(text,bound);if(n==bound){w->ok=false;return;}
    u32(w,(uint32_t)n);
    if(!w->ok || n>w->capacity-(size_t)(w->next-w->bytes)){w->ok=false;return;}
    memcpy(w->next,text,n);w->next+=n;
}
static uint8_t *packet(int kind,int status,int outcome,int server,uint64_t snapshot,
    gt_tmux_candidate_t *candidates,size_t count,gt_tmux_session_t *sessions,size_t *length) {
    size_t size=64+4096+count*(kind==1?(GT_TMUX_PATH_MAX+GT_TMUX_VERSION_MAX+12):
        (GT_TMUX_PATH_MAX+GT_TMUX_NAME_MAX+64));
    if(size>PACKET_LIMIT)return NULL;
    uint8_t *bytes=malloc(size);if(!bytes)return NULL;
    writer w={bytes,bytes,size,true};
    u32(&w,1);u32(&w,(uint32_t)kind);u32(&w,(uint32_t)status);u32(&w,(uint32_t)outcome);
    u32(&w,(uint32_t)server);u64(&w,snapshot);u32(&w,(uint32_t)count);
    string(&w,status==GT_OK?"":gt_last_error(),4096);
    for(size_t i=0;i<count;i++) {
        if(kind==1) {
            u32(&w,candidates[i].status);
            string(&w,candidates[i].path,GT_TMUX_PATH_MAX);
            string(&w,candidates[i].version,GT_TMUX_VERSION_MAX);
        } else {
            const gt_tmux_session_t *p=&sessions[i];
            u64(&w,p->server_pid);u64(&w,p->server_started);u64(&w,p->id);u64(&w,p->created);
            u32(&w,p->windows);u32(&w,p->attached);
            u32(&w,(uint32_t)p->destroy_unattached);u32(&w,(uint32_t)p->exit_unattached);
            string(&w,p->name,GT_TMUX_NAME_MAX);string(&w,p->socket_path,GT_TMUX_PATH_MAX);
        }
    }
    if(!w.ok){free(bytes);return NULL;}
    *length=(size_t)(w.next-w.bytes);return bytes;
}
static void invalidate(tmux_owner *s) { s->count=0;s->snapshot++; }
static void *owner(void *context) {
    tmux_owner *s=context;gt_ssh_t *ssh=gaj_ssh_connect(s->auth,&s->stop);
    gt_status_t status=GT_ERR_IO;
    if(!ssh || cancelled(s))goto finished;
    if(!gaj_ssh_connected_address(s->auth,s->address,&s->family,&s->scope)){status=GT_ERR_INTERNAL;goto finished;}
    pthread_mutex_lock(&s->mutex);s->phase=1;pthread_mutex_unlock(&s->mutex);
    const gt_tmux_opts_t options={.socket_path=s->socket_path,.timeout_ms=(uint32_t)s->budget,.cancelled=cancelled,.context=s};
    while(!atomic_load(&s->stop)) {
        pthread_mutex_lock(&s->mutex);
        while(!s->pending && !atomic_load(&s->stop))pthread_cond_wait(&s->wake,&s->mutex);
        if(atomic_load(&s->stop)){pthread_mutex_unlock(&s->mutex);break;}
        s->pending=false;s->active=true;
        int kind=s->kind,index=s->index;uint64_t expected=s->expected_snapshot;
        char *argument=s->argument;s->argument=NULL;
        pthread_mutex_unlock(&s->mutex);
        size_t count=0;gt_tmux_candidate_t *candidates=NULL;
        gt_tmux_result_t outcome=GT_TMUX_REJECTED;gt_tmux_server_t server=GT_TMUX_SERVER_ABSENT;
        status=GT_ERR_INVALID_ARG;
        const bool selected=expected==s->snapshot && index>=0 && (size_t)index<s->count;
        gt_tmux_session_t target;
        if(selected)target=s->sessions[index];
        /* Every refresh/change invalidates the old UI target even on failure. */
        invalidate(s);
        if(kind==1) {
            candidates=calloc(GT_TMUX_CANDIDATE_MAX,sizeof(*candidates));
            status=candidates?gt_tmux_discover(ssh,&options,candidates,GT_TMUX_CANDIDATE_MAX,&count):GT_ERR_INTERNAL;
        } else if(kind==2) {
            gt_tmux_close(s->tmux);s->tmux=NULL;
            gt_tmux_opts_t selected_options=options;selected_options.executable=argument;
            s->tmux=gt_tmux_open(ssh,&selected_options,&status);
        } else if(kind==3 && s->tmux) {
            status=gt_tmux_list(s->tmux,s->sessions,GT_TMUX_SESSION_MAX,&count,&server);
            s->count=status==GT_OK?count:0;
        } else if(kind>=4 && kind<=6 && s->tmux && (kind==4 || selected)) {
            status=gt_tmux_change(s->tmux,kind==4?GT_TMUX_CREATE:kind==5?GT_TMUX_RENAME:GT_TMUX_TERMINATE,
                kind==4?NULL:&target,argument,&outcome);
        } else if(kind==7 && s->tmux && selected) {
            s->attached=gt_tmux_attach(s->tmux,&target,s->attach_ssh,&s->attach_size,&status);
            snprintf(s->attach_error,sizeof(s->attach_error),"%s",s->attached?"":gt_last_error());
        }
        free(argument);
        if(status!=GT_OK)count=0;
        size_t length=0;uint8_t *bytes=NULL;
        if(kind!=7) {
            bytes=packet(kind,status,kind>=4&&kind<=6?(int)outcome:-1,(int)server,s->snapshot,candidates,count,s->sessions,&length);
            if(!bytes){status=GT_ERR_INTERNAL;invalidate(s);}
        }
        free(candidates);
        pthread_mutex_lock(&s->mutex);
        s->attach_status=status;s->attach_stop=NULL;s->active=false;s->complete=true;
        s->packet=bytes;s->length=length;
        pthread_cond_broadcast(&s->wake);pthread_mutex_unlock(&s->mutex);
    }
finished:
    if(ssh)(void)gt_ssh_abort(ssh);
    gt_tmux_close(s->tmux);s->tmux=NULL;gt_ssh_close(ssh);
    pthread_mutex_lock(&s->mutex);s->phase=atomic_load(&s->stop)?3:2;s->error=atomic_load(&s->stop)?GT_ERR_CANCELLED:status;
    pthread_cond_broadcast(&s->wake);pthread_mutex_unlock(&s->mutex);
    atomic_fetch_sub(&owners,1);release(s);return NULL;
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_create(JNIEnv *env,jobject self,
    jbyteArray host,jint port,jbyteArray user,jbyteArray known,jobjectArray addresses,jintArray scopes,
    jint timeout,jint fence,jint budget,jbyteArray socket) {
    (void)self;
    if(budget<1 || budget>60000){fail(env,"后台操作超时无效");return 0;}
    gaj_ssh *auth=gaj_ssh_new(env,host,port,user,known,addresses,scopes,timeout);if(!auth)return 0;
    char *path=copy(env,socket,GT_TMUX_PATH_MAX-1,true);
    if(!path){gaj_ssh_free(auth);return 0;}
    if(path[0] && path[0]!='/'){free(path);gaj_ssh_free(auth);fail(env,"tmux socket 路径必须为绝对路径");return 0;}
    if(!path[0]){free(path);path=NULL;}
    tmux_owner *s=calloc(1,sizeof(*s));if(!s){free(path);gaj_ssh_free(auth);fail(env,"后台连接内存不足");return 0;}
    s->auth=auth;s->socket_path=path;s->fence=-1;s->budget=budget;s->snapshot=1;atomic_init(&s->stop,false);atomic_init(&s->attach_stop,NULL);
    if(pthread_mutex_init(&s->mutex,NULL)){free(path);gaj_ssh_free(auth);free(s);fail(env,"后台连接锁失败");return 0;}
    if(pthread_cond_init(&s->wake,NULL)){pthread_mutex_destroy(&s->mutex);free(path);gaj_ssh_free(auth);free(s);fail(env,"后台连接队列失败");return 0;}
    s->refs=1;s->fence=fcntl(fence,F_DUPFD_CLOEXEC,0);
    s->sessions=calloc(GT_TMUX_SESSION_MAX,sizeof(*s->sessions));
    if(s->fence<0 || !s->sessions){release(s);fail(env,"无法保留后台连接资源");return 0;}
    pthread_mutex_lock(&registry_mutex);
    size_t slot=0;while(slot<TMUX_SLOTS && registry[slot].value)slot++;
    if(slot==TMUX_SLOTS || registry[slot].generation>=(uint64_t)INT64_MAX/TMUX_SLOTS-1){
        pthread_mutex_unlock(&registry_mutex);release(s);fail(env,"后台连接数量已达上限");return 0;
    }
    uint64_t generation=++registry[slot].generation;s->refs=2;registry[slot].value=s;
    pthread_mutex_unlock(&registry_mutex);
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&owners,1);int code=pthread_create(&thread,&attr,owner,s);pthread_attr_destroy(&attr);
    if(code){atomic_fetch_sub(&owners,1);pthread_mutex_lock(&registry_mutex);registry[slot].value=NULL;pthread_mutex_unlock(&registry_mutex);
        release(s);release(s);fail(env,"无法启动后台连接线程");return 0;}
    return (jlong)(generation*TMUX_SLOTS+slot);
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_dispose(JNIEnv *env,jobject self,jlong id) {
    (void)env;(void)self;if(id<=0)return;
    pthread_mutex_lock(&registry_mutex);size_t slot=(uint64_t)id%TMUX_SLOTS;
    tmux_owner *s=registry[slot].generation==(uint64_t)id/TMUX_SLOTS?registry[slot].value:NULL;
    if(s){registry[slot].value=NULL;atomic_store(&s->stop,true);}pthread_mutex_unlock(&registry_mutex);
    if(s){pthread_mutex_lock(&s->mutex);pthread_cond_broadcast(&s->wake);pthread_mutex_unlock(&s->mutex);release(s);}
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_authentication(JNIEnv *env,jobject self,jlong id) {
    (void)self;tmux_owner *s=borrow(env,id);if(!s)return NULL;
    jobjectArray value=gaj_ssh_info(env,s->auth);release(s);return value;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_respond(JNIEnv *env,jobject self,
    jlong id,jlong request,jint kind,jbyteArray first,jbyteArray second) {
    (void)self;tmux_owner *s=borrow(env,id);if(!s)return;
    gaj_ssh_respond(env,s->auth,request,kind,first,second);release(s);
}
JNIEXPORT jintArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_state(JNIEnv *env,jobject self,jlong id) {
    (void)self;tmux_owner *s=borrow(env,id);if(!s)return NULL;
    pthread_mutex_lock(&s->mutex);jint values[]={s->phase,s->error,s->active?1:0};pthread_mutex_unlock(&s->mutex);
    jintArray result=(*env)->NewIntArray(env,3);if(result)(*env)->SetIntArrayRegion(env,result,0,3,values);
    release(s);return result;
}
JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_address(JNIEnv *env,jobject self,jlong id) {
    (void)self;tmux_owner *s=borrow(env,id);if(!s)return NULL;
    uint8_t bytes[20];size_t length=0;
    pthread_mutex_lock(&s->mutex);
    if(s->phase==1 && (s->family==4 || s->family==6)) {
        for(int i=0;i<4;i++)bytes[i]=(uint8_t)(s->scope>>(24-i*8));
        length=s->family==4?8:20;memcpy(bytes+4,s->address,length-4);
    }
    pthread_mutex_unlock(&s->mutex);
    jbyteArray result=NULL;
    if(!length)fail(env,"后台管理的实际连接地址尚未确认");
    else {
        result=(*env)->NewByteArray(env,(jsize)length);
        if(result)(*env)->SetByteArrayRegion(env,result,0,(jsize)length,(const jbyte *)bytes);
    }
    release(s);return result;
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_begin(JNIEnv *env,jobject self,jlong id,jint kind,
    jlong snapshot,jint index,jbyteArray argument) {
    (void)self;
    if(kind<1 || kind>6 || snapshot<0 || index<-1 || index>=GT_TMUX_SESSION_MAX){fail(env,"后台请求无效");return 0;}
    char *text=copy(env,argument,kind==2?GT_TMUX_PATH_MAX-1:GT_TMUX_NAME_MAX-1,true);if(!text)return 0;
    if(kind==2 && text[0]!='/'){free(text);fail(env,"tmux 路径必须为绝对路径");return 0;}
    tmux_owner *s=borrow(env,id);if(!s){free(text);return 0;}
    pthread_mutex_lock(&s->mutex);
    if(s->phase!=1 || atomic_load(&s->stop) || s->pending || s->active || s->complete || s->sequence>=INT64_MAX-1){
        pthread_mutex_unlock(&s->mutex);free(text);release(s);fail(env,"后台连接未就绪或另一个操作未结束");return 0;
    }
    s->request=++s->sequence;s->kind=kind;s->expected_snapshot=(uint64_t)snapshot;s->index=index;s->argument=text;
    if(kind==4)atomic_fetch_add(&create_requests,1);
    s->pending=true;uint64_t request=s->request;pthread_cond_broadcast(&s->wake);pthread_mutex_unlock(&s->mutex);
    release(s);return (jlong)request;
}
JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_result(JNIEnv *env,jobject self,jlong id,jlong request) {
    (void)self;tmux_owner *s=borrow(env,id);if(!s)return NULL;jbyteArray result=NULL;
    pthread_mutex_lock(&s->mutex);
    if(s->request==(uint64_t)request && s->complete){
        if(!s->packet)fail(env,"后台结果无法编码");
        else{result=(*env)->NewByteArray(env,(jsize)s->length);if(result)(*env)->SetByteArrayRegion(env,result,0,(jsize)s->length,(const jbyte *)s->packet);}
    }
    pthread_mutex_unlock(&s->mutex);release(s);return result;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_finish(JNIEnv *env,jobject self,jlong id,jlong request) {
    (void)self;tmux_owner *s=borrow(env,id);if(!s)return;
    pthread_mutex_lock(&s->mutex);
    if(s->request!=(uint64_t)request || !s->complete || s->kind==7)fail(env,"后台结果已失效");
    else{free(s->packet);s->packet=NULL;s->length=0;s->complete=false;}
    pthread_mutex_unlock(&s->mutex);release(s);
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_ownerCount(JNIEnv *env,jobject self) {
    (void)env;(void)self;return atomic_load(&owners);
}
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeTmux_createRequestCount(JNIEnv *env,jobject self) {
    (void)env;(void)self;return atomic_load(&create_requests);
}
gaj_tmux_target *gaj_tmux_target_new(JNIEnv *env,jlong owner_id,jlong snapshot,jint index) {
    if(owner_id<=0 || snapshot<1 || index<0 || index>=GT_TMUX_SESSION_MAX){fail(env,"后台会话选择已失效");return NULL;}
    gaj_tmux_target *target=malloc(sizeof(*target));if(!target){fail(env,"无法保留后台会话选择");return NULL;}
    *target=(gaj_tmux_target){(uint64_t)owner_id,(uint64_t)snapshot,index};return target;
}
void gaj_tmux_target_free(gaj_tmux_target *target){free(target);}
gt_transport_t *gaj_tmux_attach(gaj_tmux_target *target,gt_ssh_t *ssh,const gt_shell_opts_t *size,
    atomic_bool *stop,gt_status_t *status,char *error,size_t capacity) {
    if(error && capacity)snprintf(error,capacity,"后台列表已过期或管理连接未就绪，请刷新后重新选择");
    *status=GT_ERR_INVALID_ARG;if(!target || !ssh || !size || !stop)return NULL;
    tmux_owner *s=lookup(target->owner);if(!s)return NULL;
    pthread_mutex_lock(&s->mutex);
    if(s->phase!=1 || atomic_load(&s->stop) || atomic_load(stop) || s->pending || s->active || s->complete || s->sequence>=INT64_MAX-1){
        pthread_mutex_unlock(&s->mutex);release(s);return NULL;
    }
    s->request=++s->sequence;s->kind=7;s->expected_snapshot=target->snapshot;s->index=target->index;
    s->attach_ssh=ssh;s->attach_stop=stop;s->attach_size=*size;s->attached=NULL;s->pending=true;
    snprintf(s->attach_error,sizeof(s->attach_error),"后台列表已过期，请刷新后重新选择");
    pthread_cond_broadcast(&s->wake);
    /* The SSH owner cannot free ssh until this acknowledgement or manager exit.
     * close only sets atomic flags; no UI thread waits here. */
    while(!s->complete && s->phase==1)pthread_cond_wait(&s->wake,&s->mutex);
    gt_transport_t *transport=s->attached;s->attached=NULL;
    *status=s->complete?s->attach_status:GT_ERR_CANCELLED;
    if(error && capacity)snprintf(error,capacity,"%s",s->complete?s->attach_error:"后台管理连接已结束");
    s->attach_ssh=NULL;s->attach_stop=NULL;s->complete=false;
    pthread_mutex_unlock(&s->mutex);release(s);return transport;
}
