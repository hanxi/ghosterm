#include "ssh_session.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct gaj_ssh {
    pthread_mutex_t mutex;
    char *host, *user, *known_hosts;
    uint16_t port;
    gt_ssh_auth_options_t options;
    gt_ssh_auth_info_t info;
    uint64_t request;
    bool waiting;
    int response;
    char *first, *second;
};
static void fail(JNIEnv *env, const char *message) {
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env,type,message);
}
static void erase(char *value) {
    if (!value) return;
    size_t n=strlen(value); volatile unsigned char *p=(volatile unsigned char *)value;
    while (n--) *p++=0;
    free(value);
}
static char *copy(JNIEnv *env, jbyteArray input, size_t limit, bool empty) {
    if (!input) { fail(env,"SSH 参数缺失"); return NULL; }
    jsize n=(*env)->GetArrayLength(env,input);
    if ((!empty && n==0) || (size_t)n>limit) { fail(env,"SSH 参数长度无效"); return NULL; }
    char *out=calloc((size_t)n+1,1);
    if (!out) { fail(env,"SSH 内存不足"); return NULL; }
    (*env)->GetByteArrayRegion(env,input,0,n,(jbyte *)out);
    if ((*env)->ExceptionCheck(env) || memchr(out,0,(size_t)n)) {
        /* Use the supplied length: a rejected embedded NUL must not hide a secret tail. */
        volatile unsigned char *p=(volatile unsigned char *)out;
        for (jsize i=0;i<n;i++) p[i]=0;
        free(out); if (!(*env)->ExceptionCheck(env)) fail(env,"SSH 参数包含 NUL"); return NULL;
    }
    return out;
}
gaj_ssh *gaj_ssh_new(JNIEnv *env, jbyteArray host, jint port, jbyteArray user,
        jbyteArray known_hosts, jbyteArray address, jint scope, jint timeout) {
    if (port<1 || port>65535 || timeout<1 || timeout>60000 || scope<0 || !address) {
        fail(env,"SSH 地址或超时无效"); return NULL;
    }
    jsize n=(*env)->GetArrayLength(env,address);
    if (n!=4 && n!=16) { fail(env,"SSH 二进制地址无效"); return NULL; }
    gaj_ssh *s=calloc(1,sizeof(*s));
    if (!s) { fail(env,"SSH 内存不足"); return NULL; }
    if (pthread_mutex_init(&s->mutex,NULL)) { free(s); fail(env,"SSH 锁初始化失败"); return NULL; }
    s->host=copy(env,host,4096,false); if (!s->host) goto invalid;
    s->user=copy(env,user,4096,false); if (!s->user) goto invalid;
    s->known_hosts=copy(env,known_hosts,4096,false); if (!s->known_hosts) goto invalid;
    if (s->known_hosts[0]!='/') { fail(env,"主机信任路径必须为绝对路径"); goto invalid; }
    s->port=(uint16_t)port; s->options.family=n==4?4:6; s->options.scope=(uint32_t)scope;
    s->options.timeout_ms=(uint32_t)timeout; s->options.defer_credentials=true;
    (*env)->GetByteArrayRegion(env,address,0,n,(jbyte *)s->options.address);
    if ((*env)->ExceptionCheck(env)) goto invalid;
    return s;
invalid:
    gaj_ssh_free(s); return NULL;
}
void gaj_ssh_free(gaj_ssh *s) {
    if (!s) return;
    erase(s->first); erase(s->second); free(s->host); free(s->user); free(s->known_hosts);
    pthread_mutex_destroy(&s->mutex); free(s);
}
/* Only the IO owner calls publish. No UI callback can call Core or mutate info. */
static void publish(gaj_ssh *s, const gt_ssh_auth_info_t *info) {
    pthread_mutex_lock(&s->mutex);
    bool prompt=info->phase==GT_SSH_AUTH_PHASE_HOSTKEY || info->phase==GT_SSH_AUTH_PHASE_CREDENTIALS;
    if (prompt && !s->waiting && !s->response) { s->request++; s->waiting=true; }
    if (!prompt) s->waiting=false;
    s->info=*info;
    pthread_mutex_unlock(&s->mutex);
}
gt_ssh_t *gaj_ssh_connect(gaj_ssh *s, atomic_bool *stop) {
    gt_ssh_opts_t opts={.host=s->host,.port=s->port,.username=s->user,.known_hosts_path=s->known_hosts};
    gt_ssh_t *ssh=NULL, *result=NULL;
    gt_ssh_authenticator_t *auth=NULL;
    gt_ssh_auth_info_t info={0};
    if (atomic_load(stop)) goto cancelled;
    ssh=gt_ssh_new(&opts);
    gt_status_t status=ssh?gt_ssh_authenticator_open(&ssh,&s->options,&auth):GT_ERR_INTERNAL;
    if (status!=GT_OK) { info.phase=GT_SSH_AUTH_PHASE_FAILED; info.error=status; publish(s,&info); goto done; }
    while (!atomic_load(stop)) {
        (void)gt_ssh_authenticator_step(auth);
        if (!gt_ssh_authenticator_info(auth,&info)) { info.phase=GT_SSH_AUTH_PHASE_FAILED; info.error=GT_ERR_INTERNAL; }
        publish(s,&info);
        if (info.phase==GT_SSH_AUTH_PHASE_READY) {
            if (!gt_ssh_authenticator_take(auth,&result)) { info.phase=GT_SSH_AUTH_PHASE_FAILED; info.error=GT_ERR_INTERNAL; }
            else info.phase=GT_SSH_AUTH_PHASE_TAKEN;
            publish(s,&info); goto done;
        }
        if (info.phase==GT_SSH_AUTH_PHASE_FAILED || info.phase==GT_SSH_AUTH_PHASE_CANCELLED) goto done;
        pthread_mutex_lock(&s->mutex);
        int response=s->response; char *first=s->first, *second=s->second;
        s->response=0; s->first=NULL; s->second=NULL;
        pthread_mutex_unlock(&s->mutex);
        if (response) {
            status=GT_ERR_INVALID_ARG;
            if (atomic_load(stop)) { erase(first); erase(second); goto cancelled; }
            if (response==1 || response==2) status=gt_ssh_authenticator_decide(auth,response==1?GT_HOSTKEY_ACCEPT:GT_HOSTKEY_REJECT);
            else {
                gt_ssh_t *material=gt_ssh_new(&opts);
                if (material) {
                    status=response==3?gt_ssh_auth_password(material,first):gt_ssh_auth_private_key(material,first,second);
                    if (status==GT_OK) status=gt_ssh_authenticator_supply(auth,&material);
                    gt_ssh_close(material);
                } else status=GT_ERR_INTERNAL;
            }
            erase(first); erase(second);
            if (status!=GT_OK) {
                /* Preserve HOST_UNKNOWN for trust-save IO; do not request credentials. */
                (void)gt_ssh_authenticator_info(auth,&info);
                info.phase=GT_SSH_AUTH_PHASE_FAILED; info.error=status; publish(s,&info); goto done;
            }
        }
        struct timespec pause={.tv_nsec=10*1000*1000}; nanosleep(&pause,NULL);
    }
cancelled:
    info.phase=GT_SSH_AUTH_PHASE_CANCELLED; info.error=GT_ERR_CANCELLED; publish(s,&info);
done:
    gt_ssh_authenticator_close(auth); gt_ssh_close(ssh);
    /* Pending credentials may race timeout/cancel; erase them on the owner now. */
    pthread_mutex_lock(&s->mutex);
    erase(s->first); erase(s->second); s->first=NULL; s->second=NULL; s->response=0; s->waiting=false;
    pthread_mutex_unlock(&s->mutex);
    return result;
}
void gaj_ssh_respond(JNIEnv *env, gaj_ssh *s, jlong request, jint kind, jbyteArray first, jbyteArray second) {
    if (!s || request<=0 || kind<1 || kind>4) { fail(env,"SSH 请求无效"); return; }
    char *a=NULL,*b=NULL;
    if (kind>=3) { a=copy(env,first,1024*1024,kind==3); if (!a) return; }
    if (kind==4) { b=copy(env,second,65536,true); if (!b) { erase(a); return; } }
    pthread_mutex_lock(&s->mutex);
    bool phase=(kind<=2 && s->info.phase==GT_SSH_AUTH_PHASE_HOSTKEY && s->info.host==GT_SSH_AUTH_HOST_UNKNOWN) ||
        (kind>=3 && s->info.phase==GT_SSH_AUTH_PHASE_CREDENTIALS && (s->info.auth_methods & (kind==3?GT_SSH_AUTH_PASSWORD:GT_SSH_AUTH_PUBLICKEY)));
    bool valid=s->waiting && !s->response && (uint64_t)request==s->request && phase;
    if (valid) { s->first=a; s->second=b; s->response=kind; s->waiting=false; a=NULL; b=NULL; }
    pthread_mutex_unlock(&s->mutex);
    erase(a); erase(b);
    if (!valid) fail(env,"SSH 请求已失效或认证方式不支持");
}
jobjectArray gaj_ssh_info(JNIEnv *env, gaj_ssh *s) {
    if (!s) { fail(env,"此会话不是 SSH 连接"); return NULL; }
    pthread_mutex_lock(&s->mutex);
    gt_ssh_auth_info_t info=s->info; uint64_t request=s->waiting?s->request:0;
    pthread_mutex_unlock(&s->mutex);
    char number[6][32];
    snprintf(number[0],sizeof number[0],"%llu",(unsigned long long)request);
    snprintf(number[1],sizeof number[1],"%d",info.phase);
    snprintf(number[2],sizeof number[2],"%d",info.error);
    snprintf(number[3],sizeof number[3],"%d",info.host);
    snprintf(number[4],sizeof number[4],"%d",info.deadline);
    snprintf(number[5],sizeof number[5],"%d",info.auth_methods);
    const char *fields[]={number[0],number[1],number[2],number[3],number[4],number[5],info.key_type,info.fingerprint};
    jclass type=(*env)->FindClass(env,"[B"); if (!type) return NULL;
    jobjectArray out=(*env)->NewObjectArray(env,8,type,NULL);
    for (int i=0;out && i<8 && !(*env)->ExceptionCheck(env);i++) {
        size_t n=strlen(fields[i]); jbyteArray value=(*env)->NewByteArray(env,(jsize)n); if (!value) break;
        (*env)->SetByteArrayRegion(env,value,0,(jsize)n,(const jbyte *)fields[i]);
        (*env)->SetObjectArrayElement(env,out,i,value); (*env)->DeleteLocalRef(env,value);
    }
    return out;
}
