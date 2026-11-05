/* Native transport-only bindings. Registry IDs never expose Core pointers.
 * One worker owns each channel; stop pins the owner across concurrent close. */
#include "ghosterm.h"
#include <jni.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint64_t id;gt_ext_channel_t *channel;gt_ext_session_t *session;gt_ext_resource_store_t *store;pthread_t owner;unsigned pins;bool retiring;} extension_slot_t;
static pthread_mutex_t extension_lock=PTHREAD_MUTEX_INITIALIZER;
static extension_slot_t extension_slots[GT_EXT_CHANNEL_LIMIT];
static uint64_t extension_next=1;
static void extension_erase(void *p,size_t n){volatile unsigned char *v=p;while(n--)*v++=0;}
static void extension_error(JNIEnv *e,const char *text){jclass t=(*e)->FindClass(e,"java/lang/IllegalStateException");if(t)(*e)->ThrowNew(e,t,text);}
static extension_slot_t *extension_reserve(JNIEnv *e){
  pthread_mutex_lock(&extension_lock);extension_slot_t *s=NULL;if(extension_next<=INT64_MAX)for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++)if(!extension_slots[i].id){s=&extension_slots[i];s->id=extension_next++;s->owner=pthread_self();break;}pthread_mutex_unlock(&extension_lock);if(!s)extension_error(e,"插件通道容量已占用，待旧 owner 释放");return s;
}
static void extension_abandon(extension_slot_t *s){pthread_mutex_lock(&extension_lock);memset(s,0,sizeof(*s));pthread_mutex_unlock(&extension_lock);}
static extension_slot_t *extension_pin(JNIEnv *e,jlong id,bool owner,bool quiet){
  pthread_mutex_lock(&extension_lock);extension_slot_t *s=NULL;for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++)if(id>0 && extension_slots[i].id==(uint64_t)id && extension_slots[i].channel && !extension_slots[i].retiring){s=&extension_slots[i];break;}
  if(s && owner && !pthread_equal(s->owner,pthread_self())){pthread_mutex_unlock(&extension_lock);if(!quiet)extension_error(e,"插件通道必须由创建它的 IO worker 调用");return NULL;}if(s)s->pins++;pthread_mutex_unlock(&extension_lock);if(!s && !quiet)extension_error(e,"插件通道句柄已失效");return s;
}
static void extension_unpin(extension_slot_t *s){pthread_mutex_lock(&extension_lock);s->pins--;pthread_mutex_unlock(&extension_lock);}
static char *extension_text(JNIEnv *e,jbyteArray bytes,size_t limit){
  if(!bytes){extension_error(e,"插件参数缺失");return NULL;}jsize n=(*e)->GetArrayLength(e,bytes);if((size_t)n>limit){extension_error(e,"插件参数超限");return NULL;}char *p=malloc((size_t)n+1);if(!p){extension_error(e,"插件参数内存不足");return NULL;}(*e)->GetByteArrayRegion(e,bytes,0,n,(jbyte *)p);if((*e)->ExceptionCheck(e) || memchr(p,0,(size_t)n)){extension_erase(p,(size_t)n+1);free(p);if(!(*e)->ExceptionCheck(e))extension_error(e,"插件参数含 NUL");return NULL;}p[n]=0;return p;
}
static void extension_secret_free(char *p){if(p){size_t n=strlen(p);volatile unsigned char *v=(volatile unsigned char *)p;while(n--)*v++=0;free(p);}}
static char *extension_item(JNIEnv *e,jobjectArray values,jsize at,size_t limit){jbyteArray bytes=(*e)->GetObjectArrayElement(e,values,at);if((*e)->ExceptionCheck(e))return NULL;char *p=extension_text(e,bytes,limit);if(bytes)(*e)->DeleteLocalRef(e,bytes);return p;}
static jlongArray extension_created(JNIEnv *e,jlongArray result,extension_slot_t *s,gt_status_t status,gt_ext_channel_t *channel){
  jlong values[2]={(jlong)status,0};if(status==GT_OK){pthread_mutex_lock(&extension_lock);s->channel=channel;values[1]=(jlong)s->id;pthread_mutex_unlock(&extension_lock);}else extension_abandon(s);(*e)->SetLongArrayRegion(e,result,0,2,values);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_openFd(JNIEnv *e,jobject self,jint input,jint output){
  (void)self;jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;extension_slot_t *s=extension_reserve(e);if(!s)return NULL;gt_ext_channel_t *channel=NULL;gt_status_t status=gt_ext_channel_open_fd(input,output,&channel);return extension_created(e,result,s,status,channel);
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_openRoute(JNIEnv *e,jobject self,jobjectArray hosts,jintArray ports,jobjectArray users,jobjectArray known,jobjectArray via,jbyteArray address,jint family,jint scope,jintArray timeouts,jbyteArray command,jint setup,jint admission,jint stderr_limit){
  (void)self;if(!hosts || !ports || !users || !known || !via || !address || !timeouts){extension_error(e,"插件路线参数缺失");return NULL;}jsize count=(*e)->GetArrayLength(e,hosts);
  if(count<1 || count>(jsize)GT_EXT_MAX_HOPS || (*e)->GetArrayLength(e,ports)!=count || (*e)->GetArrayLength(e,users)!=count || (*e)->GetArrayLength(e,known)!=count || (*e)->GetArrayLength(e,via)!=count || (*e)->GetArrayLength(e,timeouts)!=count || (*e)->GetArrayLength(e,address)!=16 || (family!=4 && family!=6) || setup<=0 || admission<=0 || stderr_limit<=0){extension_error(e,"插件路线参数无效");return NULL;}
  jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;extension_slot_t *s=extension_reserve(e);if(!s)return NULL;
  jint port[GT_EXT_MAX_HOPS],timeout[GT_EXT_MAX_HOPS];uint8_t resolved[16];(*e)->GetIntArrayRegion(e,ports,0,count,port);(*e)->GetIntArrayRegion(e,timeouts,0,count,timeout);(*e)->GetByteArrayRegion(e,address,0,16,(jbyte *)resolved);
  gt_ssh_t *materials[GT_EXT_MAX_HOPS]={0};gt_ext_ssh_hop_t hops[GT_EXT_MAX_HOPS]={0};char *names[GT_EXT_MAX_HOPS]={0},*logins[GT_EXT_MAX_HOPS]={0},*paths[GT_EXT_MAX_HOPS]={0},*destinations[GT_EXT_MAX_HOPS]={0};char *bootstrap=NULL;gt_status_t status=GT_ERR_INVALID_ARG;gt_ext_channel_t *channel=NULL;
  if((*e)->ExceptionCheck(e))goto done;bootstrap=extension_text(e,command,1024);if(!bootstrap)goto done;
  for(jsize i=0;i<count;i++){
    names[i]=extension_item(e,hosts,i,255);if(!names[i])goto done;logins[i]=extension_item(e,users,i,255);if(!logins[i])goto done;paths[i]=extension_item(e,known,i,4096);if(!paths[i])goto done;destinations[i]=extension_item(e,via,i,255);if(!destinations[i] || port[i]<1 || port[i]>65535 || timeout[i]<1 || timeout[i]>60000)goto done;
    gt_ssh_opts_t opts={.host=names[i],.port=(uint16_t)port[i],.username=logins[i],.known_hosts_path=paths[i][0]?paths[i]:NULL};materials[i]=gt_ssh_new(&opts);if(!materials[i]){status=GT_ERR_IO;goto done;}
    hops[i].authentication=(gt_ssh_auth_options_t){.family=i?0:(uint8_t)family,.scope=i?0:(uint32_t)scope,.timeout_ms=(uint32_t)timeout[i],.defer_credentials=true};if(!i)memcpy(hops[i].authentication.address,resolved,16);hops[i].via_host=i?destinations[i]:NULL;
    if(!i && destinations[i][0])goto done;
  }
  {gt_ext_ssh_options_t opts={(uint32_t)setup,(uint32_t)admission,(uint32_t)stderr_limit};status=gt_ext_channel_open_ssh(materials,hops,(size_t)count,bootstrap,&opts,&channel);}
done:
  for(jsize i=0;i<count;i++){gt_ssh_close(materials[i]);free(names[i]);free(logins[i]);free(paths[i]);free(destinations[i]);}free(bootstrap);if((*e)->ExceptionCheck(e)){extension_abandon(s);return NULL;}return extension_created(e,result,s,status,channel);
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_step(JNIEnv *e,jobject self,jlong id,jint ms){(void)self;extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return GT_EXT_FAILED;if(ms<0){extension_unpin(s);extension_error(e,"插件等待预算无效");return GT_EXT_FAILED;}gt_ext_phase_t phase=gt_ext_channel_step(s->channel,ms);extension_unpin(s);return (jint)phase;}
static jbyteArray extension_bytes(JNIEnv *e,const void *p,size_t n){jbyteArray out=(*e)->NewByteArray(e,(jsize)n);if(out && n)(*e)->SetByteArrayRegion(e,out,0,(jsize)n,p);return out;}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_info(JNIEnv *e,jobject self,jlong id){
  (void)self;extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return NULL;gt_ext_channel_info_t i;bool ok=gt_ext_channel_info(s->channel,&i);extension_unpin(s);if(!ok || i.hops>GT_EXT_MAX_HOPS || i.diagnostic_size>GT_EXT_DIAGNOSTIC_MAX){extension_error(e,"插件通道状态无效");return NULL;}
  jlong values[18+GT_EXT_MAX_HOPS*6]={(jlong)i.phase,(jlong)i.stage,(jlong)i.failure_stage,(jlong)i.error,(jlong)i.hop,(jlong)i.hops,i.setup_expired,(jlong)i.challenge,(jlong)i.accepted,(jlong)i.sent,(jlong)i.received,(jlong)i.stderr_bytes,(jlong)i.buffered,i.remote_eof,i.exit_status_known,i.exit_status,i.diagnostic_truncated,i.cleanup_error};
  for(size_t h=0;h<i.hops;h++){gt_ssh_auth_info_t *a=&i.authentication[h];jlong *v=values+18+h*6;v[0]=a->phase;v[1]=a->error;v[2]=a->host;v[3]=a->deadline;v[4]=a->auth_methods;v[5]=i.decisions[h];}
  jclass object=(*e)->FindClass(e,"java/lang/Object");if(!object)return NULL;jclass bytes=(*e)->FindClass(e,"[B");if(!bytes)return NULL;jobjectArray result=(*e)->NewObjectArray(e,3,object,NULL);if(!result)return NULL;jlongArray numbers=(*e)->NewLongArray(e,(jsize)(18+i.hops*6));if(!numbers)return NULL;jobjectArray texts=(*e)->NewObjectArray(e,(jsize)(i.hops*2),bytes,NULL);if(!texts)return NULL;(*e)->SetLongArrayRegion(e,numbers,0,(jsize)(18+i.hops*6),values);
  for(size_t h=0;h<i.hops;h++){jbyteArray key=extension_bytes(e,i.authentication[h].key_type,strlen(i.authentication[h].key_type));if(!key)return NULL;jbyteArray fingerprint=extension_bytes(e,i.authentication[h].fingerprint,strlen(i.authentication[h].fingerprint));if(!fingerprint)return NULL;(*e)->SetObjectArrayElement(e,texts,(jsize)(h*2),key);(*e)->SetObjectArrayElement(e,texts,(jsize)(h*2+1),fingerprint);(*e)->DeleteLocalRef(e,key);(*e)->DeleteLocalRef(e,fingerprint);}
  jbyteArray diagnostic=extension_bytes(e,i.diagnostic,i.diagnostic_size);if(!diagnostic)return NULL;(*e)->SetObjectArrayElement(e,result,0,numbers);(*e)->SetObjectArrayElement(e,result,1,texts);(*e)->SetObjectArrayElement(e,result,2,diagnostic);return result;
}
JNIEXPORT jintArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_write(JNIEnv *e,jobject self,jlong id,jbyteArray bytes,jint offset,jint count){
  (void)self;if(!bytes || offset<0 || count<1 || offset>(*e)->GetArrayLength(e,bytes)-count){extension_error(e,"插件写入范围无效");return NULL;}if(count>(jint)GT_EXT_IO_MAX)count=GT_EXT_IO_MAX;jintArray result=(*e)->NewIntArray(e,3);if(!result)return NULL;extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return NULL;uint8_t copy[GT_EXT_IO_MAX];(*e)->GetByteArrayRegion(e,bytes,offset,count,(jbyte *)copy);if((*e)->ExceptionCheck(e)){extension_unpin(s);return NULL;}gt_ext_io_t io=gt_ext_channel_write(s->channel,copy,(size_t)count);memset(copy,0,(size_t)count);extension_unpin(s);jint values[3]={io.state,(jint)io.count,io.error};(*e)->SetIntArrayRegion(e,result,0,3,values);return result;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_read(JNIEnv *e,jobject self,jlong id,jint count,jint ms){
  (void)self;if(count<1 || count>(jint)GT_EXT_IO_MAX || ms<0){extension_error(e,"插件读取参数无效");return NULL;}jclass object=(*e)->FindClass(e,"java/lang/Object");if(!object)return NULL;jobjectArray result=(*e)->NewObjectArray(e,2,object,NULL);if(!result)return NULL;jintArray numbers=(*e)->NewIntArray(e,3);if(!numbers)return NULL;jbyteArray bytes=(*e)->NewByteArray(e,count);if(!bytes)return NULL;
  extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return NULL;uint8_t copy[GT_EXT_IO_MAX];gt_ext_io_t io=gt_ext_channel_read(s->channel,copy,(size_t)count,ms);jint values[3]={io.state,(jint)io.count,io.error};(*e)->SetIntArrayRegion(e,numbers,0,3,values);if(io.count)(*e)->SetByteArrayRegion(e,bytes,0,(jsize)io.count,(jbyte *)copy);memset(copy,0,sizeof(copy));(*e)->SetObjectArrayElement(e,result,0,numbers);(*e)->SetObjectArrayElement(e,result,1,bytes);if((*e)->ExceptionCheck(e))gt_ext_channel_stop(s->channel);extension_unpin(s);return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_decide(JNIEnv *e,jobject self,jlong id,jint hop,jlong challenge,jint decision){(void)self;extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return GT_ERR_INVALID_ARG;gt_status_t status=gt_ext_channel_decide(s->channel,(uint32_t)hop,(uint64_t)challenge,(gt_hostkey_decision_t)decision);extension_unpin(s);return status;}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_supply(JNIEnv *e,jobject self,jlong id,jint hop,jlong challenge,jbyteArray host,jint port,jbyteArray user,jbyteArray known,jint kind,jbyteArray first,jbyteArray second){
  (void)self;extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return GT_ERR_INVALID_ARG;char *h=NULL,*u=NULL,*k=NULL,*one=NULL,*two=NULL;gt_status_t status=GT_ERR_INVALID_ARG;gt_ssh_t *material=NULL;
  h=extension_text(e,host,255);if(!h)goto done;u=extension_text(e,user,255);if(!u)goto done;k=extension_text(e,known,4096);if(!k)goto done;one=extension_text(e,first,1048576);if(!one)goto done;two=extension_text(e,second,16384);if(!two)goto done;
  if(h && u && k && one && two && port>0 && port<=65535 && (kind==1 || kind==2)){gt_ssh_opts_t opts={.host=h,.port=(uint16_t)port,.username=u,.known_hosts_path=k[0]?k:NULL};material=gt_ssh_new(&opts);if(!material)status=GT_ERR_IO;else{int auth=kind==1?gt_ssh_auth_password(material,one):gt_ssh_auth_private_key(material,one,two[0]?two:NULL);status=auth?(gt_status_t)auth:gt_ext_channel_supply(s->channel,(uint32_t)hop,(uint64_t)challenge,&material);}}
done:
  gt_ssh_close(material);extension_secret_free(one);extension_secret_free(two);free(h);free(u);free(k);extension_unpin(s);return status;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_stop(JNIEnv *e,jobject self,jlong id){(void)self;extension_slot_t *s=extension_pin(e,id,false,true);if(s){gt_ext_channel_stop(s->channel);extension_unpin(s);}}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_close(JNIEnv *e,jobject self,jlong id,jint ms){
  (void)self;extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return GT_EXT_CLOSE_ERROR;if(ms<0){extension_unpin(s);extension_error(e,"插件关闭预算无效");return GT_EXT_CLOSE_ERROR;}gt_ext_close_t state=gt_ext_channel_close(s->channel,ms);if(state!=GT_EXT_RELEASED){extension_unpin(s);return state;}
  pthread_mutex_lock(&extension_lock);if(s->pins!=1){s->pins--;pthread_mutex_unlock(&extension_lock);return GT_EXT_DRAINING;}s->retiring=true;gt_ext_channel_t *channel=s->channel;pthread_mutex_unlock(&extension_lock);gt_status_t status=gt_ext_channel_destroy(&channel);
  pthread_mutex_lock(&extension_lock);if(status==GT_OK)memset(s,0,sizeof(*s));else{s->retiring=false;s->pins--;}pthread_mutex_unlock(&extension_lock);return status==GT_OK?GT_EXT_RELEASED:GT_EXT_CLOSE_ERROR;
}
/* A Session replaces its channel in the same charged registry slot. Adoption
 * retires the old generation only after successful Core transfer. A concurrent
 * stop pin returns a visible retryable error before changing ownership. */
typedef struct {uint64_t id;gt_ext_resource_store_t *store;pthread_t owner;} resource_store_slot_t;
static resource_store_slot_t resource_store_slots[GT_EXT_CHANNEL_LIMIT];
/* Store has no cross-thread operation. The mutex protects ID lookup; same-owner
 * calls cannot race destruction, and Core counts associated Sessions/pins. */
static resource_store_slot_t *resource_store_owner(JNIEnv *e,jlong id){
 pthread_mutex_lock(&extension_lock);resource_store_slot_t *s=NULL;
 for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++)if(id>0 && resource_store_slots[i].id==(uint64_t)id && resource_store_slots[i].store){s=&resource_store_slots[i];break;}
 bool valid=s && pthread_equal(s->owner,pthread_self());pthread_mutex_unlock(&extension_lock);
 if(!valid){extension_error(e,"资源 Store 已释放或不在创建 worker");return NULL;}return s;
}
static jlongArray session_open_common(JNIEnv *e,jlong id,jint role,jobjectArray texts,jobjectArray required,jobjectArray available,jintArray numbers,jobjectArray names,jobjectArray caps,jintArray policies,bool requests,jlong store_id,jlong scope){
 if(!texts || !required || !available || !numbers || (*e)->GetArrayLength(e,texts)!=7 || (*e)->GetArrayLength(e,numbers)!=7 || (*e)->GetArrayLength(e,required)>16 || (*e)->GetArrayLength(e,available)>16){extension_error(e,"插件会话参数无效");return NULL;}
 jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;gt_ext_session_config_t c={0};jint v[7];(*e)->GetIntArrayRegion(e,numbers,0,7,v);if((*e)->ExceptionCheck(e))return NULL;
 char *fields[7]={c.plugin_id,c.plugin_version,c.client_digest,c.executor_digest,c.host_version,c.sdk_version,c.peer.identity};size_t capacities[7]={129,65,65,65,65,65,128};
 for(jsize i=0;i<7;i++){char *p=extension_item(e,texts,i,capacities[i]-1);if(!p)return NULL;memcpy(fields[i],p,strlen(p)+1);free(p);}
 c.required_count=(size_t)(*e)->GetArrayLength(e,required);c.available_count=(size_t)(*e)->GetArrayLength(e,available);
 for(size_t i=0;i<c.required_count;i++){char *p=extension_item(e,required,(jsize)i,128);if(!p)return NULL;memcpy(c.required[i],p,strlen(p)+1);free(p);}
 for(size_t i=0;i<c.available_count;i++){char *p=extension_item(e,available,(jsize)i,128);if(!p)return NULL;memcpy(c.available[i],p,strlen(p)+1);free(p);}
 c.peer.kind=(gt_ext_peer_kind_t)v[0];c.budgets=(gt_ext_budgets_t){(uint32_t)v[1],(uint32_t)v[2],(uint32_t)v[3]};c.handshake_ms=(uint32_t)v[4];c.consent_ms=(uint32_t)v[5];c.features=(uint32_t)v[6];
 gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX]={0};jsize method_count=0;
 if(requests){
  if(!names || !caps || !policies || (method_count=(*e)->GetArrayLength(e,names))>(jsize)GT_EXT_RPC_METHODS_MAX || (*e)->GetArrayLength(e,caps)!=method_count || (*e)->GetArrayLength(e,policies)!=method_count*3){extension_error(e,"请求方法表无效");return NULL;}
  jint flags[GT_EXT_RPC_METHODS_MAX*3];(*e)->GetIntArrayRegion(e,policies,0,method_count*3,flags);if((*e)->ExceptionCheck(e))return NULL;
  for(jsize i=0;i<method_count;i++){char *name=extension_item(e,names,i,128);if(!name)return NULL;memcpy(methods[i].method,name,strlen(name)+1);free(name);char *cap=extension_item(e,caps,i,128);if(!cap)return NULL;memcpy(methods[i].capability,cap,strlen(cap)+1);free(cap);methods[i].receiver=(gt_ext_role_t)flags[i*3];methods[i].input_shape=(gt_ext_json_shape_t)flags[i*3+1];methods[i].output_shape=(gt_ext_json_shape_t)flags[i*3+2];}
 }
 resource_store_slot_t *store=store_id?resource_store_owner(e,store_id):NULL;if(store_id && !store)return NULL;
 extension_slot_t *s=extension_pin(e,id,true,false);if(!s)return NULL;pthread_mutex_lock(&extension_lock);
 bool exclusive=s->pins==1 && extension_next<=INT64_MAX;uint64_t successor=0;if(exclusive){successor=extension_next++;s->retiring=true;}pthread_mutex_unlock(&extension_lock);
 gt_status_t status=GT_ERR_IO;if(exclusive)status=store?gt_ext_session_open_resources(&s->channel,(gt_ext_role_t)role,&c,methods,(size_t)method_count,store->store,(uint64_t)scope,&s->session):requests?gt_ext_session_open_rpc(&s->channel,(gt_ext_role_t)role,&c,methods,(size_t)method_count,&s->session):gt_ext_session_open(&s->channel,(gt_ext_role_t)role,&c,&s->session);jlong values[2]={status,0};
 pthread_mutex_lock(&extension_lock);if(status==GT_OK){s->store=store?store->store:NULL;s->id=successor;values[1]=(jlong)s->id;}s->retiring=false;s->pins--;pthread_mutex_unlock(&extension_lock);
 (*e)->SetLongArrayRegion(e,result,0,2,values);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_openSession(JNIEnv *e,jobject self,jlong id,jint role,jobjectArray texts,jobjectArray required,jobjectArray available,jintArray numbers){(void)self;return session_open_common(e,id,role,texts,required,available,numbers,NULL,NULL,NULL,false,0,0);}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_openSessionRpc(JNIEnv *e,jobject self,jlong id,jint role,jobjectArray texts,jobjectArray required,jobjectArray available,jintArray numbers,jobjectArray names,jobjectArray caps,jintArray policies){(void)self;return session_open_common(e,id,role,texts,required,available,numbers,names,caps,policies,true,0,0);}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionChannel_openSessionResources(JNIEnv *e,jobject self,jlong id,jint role,jobjectArray texts,jobjectArray required,jobjectArray available,jintArray numbers,jobjectArray names,jobjectArray caps,jintArray policies,jlong store,jlong scope){(void)self;return session_open_common(e,id,role,texts,required,available,numbers,names,caps,policies,true,store,scope);}
static extension_slot_t *session_pin(JNIEnv *e,jlong id,bool owner,bool quiet){
 pthread_mutex_lock(&extension_lock);extension_slot_t *s=NULL;for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++)if(id>0 && extension_slots[i].id==(uint64_t)id && extension_slots[i].session && !extension_slots[i].retiring){s=&extension_slots[i];break;}
 if(s && owner && !pthread_equal(s->owner,pthread_self())){pthread_mutex_unlock(&extension_lock);if(!quiet)extension_error(e,"插件会话必须由创建它的 IO worker 调用");return NULL;}
 if(s)s->pins++;pthread_mutex_unlock(&extension_lock);if(!s && !quiet)extension_error(e,"插件会话句柄已失效");return s;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_step(JNIEnv *e,jobject self,jlong id,jint ms){
 (void)self;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return GT_EXT_SESSION_ENDED;if(ms<0){extension_unpin(s);extension_error(e,"会话等待预算无效");return GT_EXT_SESSION_ENDED;}gt_ext_session_phase_t phase=gt_ext_session_step(s->session,ms);extension_unpin(s);return (jint)phase;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_info(JNIEnv *e,jobject self,jlong id){
 (void)self;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;gt_ext_session_info_t i;bool ok=gt_ext_session_info(s->session,&i);extension_unpin(s);if(!ok){extension_error(e,"会话状态无效");return NULL;}
 jlong v[13]={i.phase,i.end,i.error,i.decision,(jlong)i.epoch,(jlong)i.consent_deadline,(jlong)i.received,(jlong)i.sent,i.budgets.fragment_bytes,i.budgets.message_bytes,i.budgets.resource_bytes,i.peer.kind,i.cleanup_error};
 const char *strings[5]={i.connection,i.instance,i.consent_token,i.peer_host_version,i.peer.identity};jclass object=(*e)->FindClass(e,"java/lang/Object");if(!object)return NULL;jclass bytes=(*e)->FindClass(e,"[B");if(!bytes)return NULL;
 jobjectArray out=(*e)->NewObjectArray(e,2,object,NULL);if(!out)return NULL;jlongArray values=(*e)->NewLongArray(e,13);if(!values)return NULL;jobjectArray texts=(*e)->NewObjectArray(e,5,bytes,NULL);if(!texts)return NULL;(*e)->SetLongArrayRegion(e,values,0,13,v);
 for(jsize n=0;n<5;n++){jbyteArray text=extension_bytes(e,strings[n],strlen(strings[n]));if(!text)return NULL;(*e)->SetObjectArrayElement(e,texts,n,text);(*e)->DeleteLocalRef(e,text);if((*e)->ExceptionCheck(e))return NULL;}
 (*e)->SetObjectArrayElement(e,out,0,values);(*e)->SetObjectArrayElement(e,out,1,texts);return out;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_decide(JNIEnv *e,jobject self,jlong id,jbyteArray ticket,jint decision){
 (void)self;char *text=extension_text(e,ticket,32);if(!text)return GT_ERR_INVALID_ARG;extension_slot_t *s=session_pin(e,id,true,false);gt_status_t status=s?gt_ext_session_decide(s->session,text,(gt_ext_decision_t)decision):GT_ERR_INVALID_ARG;if(s)extension_unpin(s);free(text);return status;
}
JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_lease(JNIEnv *e,jobject self,jlong id){
 (void)self;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;gt_ext_lease_t l;bool ok=gt_ext_session_lease(s->session,&l);extension_unpin(s);if(!ok)return NULL;
 uint8_t encoded[107];memcpy(encoded,l.connection,33);memcpy(encoded+33,l.instance,33);memcpy(encoded+66,l.token,33);for(unsigned n=0;n<8;n++)encoded[99+n]=(uint8_t)(l.epoch>>(56-n*8));jbyteArray out=extension_bytes(e,encoded,sizeof(encoded));extension_erase(encoded,sizeof(encoded));extension_erase(&l,sizeof(l));return out;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_access(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jbyteArray capability){
 (void)self;if(!lease || (*e)->GetArrayLength(e,lease)!=107){extension_error(e,"会话租约无效");return GT_EXT_ACCESS_DENIED;}char *cap=extension_text(e,capability,128);if(!cap)return GT_EXT_ACCESS_DENIED;
 uint8_t encoded[107];(*e)->GetByteArrayRegion(e,lease,0,107,(jbyte *)encoded);if((*e)->ExceptionCheck(e)){free(cap);return GT_EXT_ACCESS_DENIED;}gt_ext_lease_t l={0};memcpy(l.connection,encoded,33);memcpy(l.instance,encoded+33,33);memcpy(l.token,encoded+66,33);for(unsigned n=0;n<8;n++)l.epoch=(l.epoch<<8)|encoded[99+n];extension_erase(encoded,sizeof(encoded));
 extension_slot_t *s=session_pin(e,id,true,false);gt_ext_access_t status=s?gt_ext_session_access(s->session,&l,cap):GT_EXT_ACCESS_DENIED;if(s)extension_unpin(s);free(cap);extension_erase(&l,sizeof(l));return status;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_revoke(JNIEnv *e,jobject self,jlong id){(void)self;extension_slot_t *s=session_pin(e,id,true,false);if(s){gt_ext_session_revoke(s->session);extension_unpin(s);}}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_stop(JNIEnv *e,jobject self,jlong id){(void)self;extension_slot_t *s=session_pin(e,id,false,true);if(s){gt_ext_session_stop(s->session);extension_unpin(s);}}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_close(JNIEnv *e,jobject self,jlong id,jint ms){
 (void)self;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return GT_EXT_CLOSE_ERROR;if(ms<0){extension_unpin(s);extension_error(e,"会话关闭预算无效");return GT_EXT_CLOSE_ERROR;}gt_ext_close_t state=gt_ext_session_close(s->session,ms);if(state!=GT_EXT_RELEASED){extension_unpin(s);return state;}
 pthread_mutex_lock(&extension_lock);if(s->pins!=1){s->pins--;pthread_mutex_unlock(&extension_lock);return GT_EXT_DRAINING;}s->retiring=true;gt_ext_session_t *session=s->session;pthread_mutex_unlock(&extension_lock);gt_status_t status=gt_ext_session_destroy(&session);
 pthread_mutex_lock(&extension_lock);if(status==GT_OK)memset(s,0,sizeof(*s));else{s->retiring=false;s->pins--;}pthread_mutex_unlock(&extension_lock);return status==GT_OK?GT_EXT_RELEASED:GT_EXT_CLOSE_ERROR;
}
static bool session_rpc_lease(JNIEnv *e,jbyteArray bytes,gt_ext_lease_t *l){
 if(!bytes || (*e)->GetArrayLength(e,bytes)!=107){extension_error(e,"请求租约无效");return false;}uint8_t copy[107];(*e)->GetByteArrayRegion(e,bytes,0,107,(jbyte *)copy);if((*e)->ExceptionCheck(e))return false;
 memcpy(l->connection,copy,33);memcpy(l->instance,copy+33,33);memcpy(l->token,copy+66,33);l->epoch=0;for(unsigned n=0;n<8;n++)l->epoch=(l->epoch<<8)|copy[99+n];extension_erase(copy,sizeof(copy));return true;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_request(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jbyteArray method,jbyteArray input,jint timeout){
 (void)self;jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;char *name=extension_text(e,method,128);if(!name)return NULL;char *json=extension_text(e,input,GT_EXT_RPC_BODY_MAX);if(!json){free(name);return NULL;}
 extension_slot_t *s=session_pin(e,id,true,false);if(!s){free(name);free(json);return NULL;}uint64_t handle=0;gt_ext_rpc_status_t status=gt_ext_session_request(s->session,&l,name,json,(size_t)(*e)->GetArrayLength(e,input),(uint32_t)timeout,&handle);extension_unpin(s);free(name);free(json);extension_erase(&l,sizeof(l));jlong values[2]={status,(jlong)handle};(*e)->SetLongArrayRegion(e,result,0,2,values);return result;
}
/* All Java allocation and array attachment completes before take consumes the
 * native incoming ticket. Native JSON views never escape this JNI call. */
static jobjectArray session_rpc_arrays(JNIEnv *e,jsize count,jsize size,jlongArray *meta,jbyteArray *bytes){
 jclass object=(*e)->FindClass(e,"java/lang/Object");if(!object)return NULL;jobjectArray result=(*e)->NewObjectArray(e,2,object,NULL);if(!result)return NULL;*meta=(*e)->NewLongArray(e,count);if(!*meta)return NULL;*bytes=(*e)->NewByteArray(e,size);if(!*bytes)return NULL;(*e)->SetObjectArrayElement(e,result,0,*meta);(*e)->SetObjectArrayElement(e,result,1,*bytes);if((*e)->ExceptionCheck(e))return NULL;return result;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_takeRequest(JNIEnv *e,jobject self,jlong id,jbyteArray lease){
 (void)self;jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,3,33+129+GT_EXT_RPC_BODY_MAX+1,&meta,&bytes);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;gt_ext_rpc_request_t q;gt_ext_rpc_status_t status=gt_ext_session_take_request(s->session,&l,&q);
 jlong values[3]={status,(jlong)q.ticket,q.input_size};(*e)->SetLongArrayRegion(e,meta,0,3,values);(*e)->SetByteArrayRegion(e,bytes,0,33,(jbyte *)q.request_id);(*e)->SetByteArrayRegion(e,bytes,33,129,(jbyte *)q.method);if(q.input_size)(*e)->SetByteArrayRegion(e,bytes,162,(jsize)q.input_size,(jbyte *)q.input);
 if((*e)->ExceptionCheck(e))gt_ext_session_stop(s->session);extension_unpin(s);extension_erase(&l,sizeof(l));extension_erase(&q,sizeof(q));return result;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_requestResult(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong handle){
 (void)self;jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,7,33+GT_EXT_RPC_BODY_MAX+1,&meta,&bytes);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;gt_ext_rpc_result_t r;gt_ext_rpc_status_t status=gt_ext_session_request_result(s->session,&l,(uint64_t)handle,&r);
 jlong values[7]={status,(jlong)r.handle,r.state,r.cancel_requested,r.deadline_elapsed,r.confirmed,r.outcome_size};(*e)->SetLongArrayRegion(e,meta,0,7,values);(*e)->SetByteArrayRegion(e,bytes,0,33,(jbyte *)r.request_id);if(r.outcome_size)(*e)->SetByteArrayRegion(e,bytes,33,(jsize)r.outcome_size,(jbyte *)r.outcome);
 if((*e)->ExceptionCheck(e))gt_ext_session_stop(s->session);extension_unpin(s);extension_erase(&l,sizeof(l));extension_erase(&r,sizeof(r));return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_requestAction(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jint action,jlong number){
 (void)self;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return GT_EXT_RPC_INVALID;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return GT_EXT_RPC_INVALID;gt_ext_rpc_status_t status;
 switch(action){case 0:status=gt_ext_session_cancel_request(s->session,&l,(uint64_t)number);break;case 1:status=gt_ext_session_forget_request(s->session,&l,(uint64_t)number);break;case 2:status=gt_ext_session_check_request(s->session,&l,(uint64_t)number);break;default:status=GT_EXT_RPC_INVALID;break;}
 extension_unpin(s);extension_erase(&l,sizeof(l));return status;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_respond(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong ticket,jbyteArray outcome){
 (void)self;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return GT_EXT_RPC_INVALID;char *json=extension_text(e,outcome,GT_EXT_RPC_BODY_MAX);if(!json)return GT_EXT_RPC_INVALID;extension_slot_t *s=session_pin(e,id,true,false);if(!s){free(json);return GT_EXT_RPC_INVALID;}
 gt_ext_rpc_status_t status=gt_ext_session_respond(s->session,&l,(uint64_t)ticket,json,(size_t)(*e)->GetArrayLength(e,outcome));extension_unpin(s);free(json);extension_erase(&l,sizeof(l));return status;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_open(JNIEnv *e,jobject self,jint fd,jlong total,jlong resource,jint slots){
 (void)self;jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;pthread_mutex_lock(&extension_lock);resource_store_slot_t *s=NULL;
 if(extension_next<=INT64_MAX)for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++)if(!resource_store_slots[i].id){s=&resource_store_slots[i];s->id=extension_next++;s->owner=pthread_self();break;}pthread_mutex_unlock(&extension_lock);
 if(!s){extension_error(e,"资源 Store 容量已占用");return NULL;}gt_ext_resource_store_config_t c={fd,(uint64_t)total,(uint64_t)resource,(size_t)slots};gt_ext_resource_store_t *store=NULL;gt_ext_resource_status_t status=gt_ext_resource_store_open(&c,&store);pthread_mutex_lock(&extension_lock);s->store=store;pthread_mutex_unlock(&extension_lock);jlong values[2]={status,status==GT_EXT_RESOURCE_OK?(jlong)s->id:0};
 if(status!=GT_EXT_RESOURCE_OK){pthread_mutex_lock(&extension_lock);memset(s,0,sizeof(*s));pthread_mutex_unlock(&extension_lock);}(*e)->SetLongArrayRegion(e,result,0,2,values);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_usage(JNIEnv *e,jobject self,jlong id){
 (void)self;jlongArray result=(*e)->NewLongArray(e,4);if(!result)return NULL;resource_store_slot_t *s=resource_store_owner(e,id);if(!s)return NULL;gt_ext_resource_usage_t u;
 if(!gt_ext_resource_store_usage(s->store,&u)){extension_error(e,"资源计费状态无效");return NULL;}jlong v[4]={(jlong)u.reserved_bytes,(jlong)u.resources,(jlong)u.scopes,(jlong)u.sessions};(*e)->SetLongArrayRegion(e,result,0,4,v);return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_close(JNIEnv *e,jobject self,jlong id){
 (void)self;resource_store_slot_t *s=resource_store_owner(e,id);if(!s)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_store_t *owned=s->store;gt_ext_resource_status_t status=gt_ext_resource_store_destroy(&owned);
 if(!owned){pthread_mutex_lock(&extension_lock);memset(s,0,sizeof(*s));pthread_mutex_unlock(&extension_lock);}return status;
}
static bool resource_snapshot_value(JNIEnv *e,jlong owner,jlong handle,jbyteArray token,gt_ext_resource_snapshot_t *ref){
 memset(ref,0,sizeof(*ref));if(!token || (*e)->GetArrayLength(e,token)!=33){extension_error(e,"资源引用无效");return false;}ref->owner=(uint64_t)owner;ref->handle=(uint64_t)handle;(*e)->GetByteArrayRegion(e,token,0,33,(jbyte *)ref->token);return !(*e)->ExceptionCheck(e);
}
static void resource_descriptor_bytes(JNIEnv *e,jbyteArray bytes,jsize at,const gt_ext_resource_descriptor_t *d){
 (*e)->SetByteArrayRegion(e,bytes,at,33,(const jbyte *)d->resource_id);(*e)->SetByteArrayRegion(e,bytes,at+33,65,(const jbyte *)d->sha256);(*e)->SetByteArrayRegion(e,bytes,at+98,129,(const jbyte *)d->mime);(*e)->SetByteArrayRegion(e,bytes,at+227,1025,(const jbyte *)d->display_name);
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_info(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong owner,jlong handle,jbyteArray token){
 (void)self;jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,8,1252,&meta,&bytes);if(!result)return NULL;gt_ext_lease_t l;gt_ext_resource_snapshot_t ref;if(!session_rpc_lease(e,lease,&l) || !resource_snapshot_value(e,owner,handle,token,&ref))return NULL;resource_store_slot_t *s=resource_store_owner(e,id);if(!s)return NULL;
 gt_ext_resource_info_t i;gt_ext_resource_status_t status=gt_ext_resource_store_info(s->store,&l,&ref,&i);jlong v[8]={status,(jlong)i.received,(jlong)i.reserved,i.references,i.published,(jlong)i.descriptor.generation,(jlong)i.descriptor.size,i.descriptor.size_known};(*e)->SetLongArrayRegion(e,meta,0,8,v);resource_descriptor_bytes(e,bytes,0,&i.descriptor);return result;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_read(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong owner,jlong handle,jbyteArray token,jlong offset,jint capacity){
 (void)self;if(offset<0 || capacity<1 || capacity>(jint)GT_EXT_RESOURCE_READ_MAX){extension_error(e,"资源读取范围无效");return NULL;}jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,2,capacity,&meta,&bytes);if(!result)return NULL;gt_ext_lease_t l;gt_ext_resource_snapshot_t ref;if(!session_rpc_lease(e,lease,&l) || !resource_snapshot_value(e,owner,handle,token,&ref))return NULL;resource_store_slot_t *s=resource_store_owner(e,id);if(!s)return NULL;
 uint8_t data[GT_EXT_RESOURCE_READ_MAX];size_t n=0;gt_ext_resource_status_t status=gt_ext_resource_store_read(s->store,&l,&ref,(uint64_t)offset,data,(size_t)capacity,&n);jlong v[2]={status,(jlong)n};(*e)->SetLongArrayRegion(e,meta,0,2,v);if(n)(*e)->SetByteArrayRegion(e,bytes,0,(jsize)n,(const jbyte *)data);extension_erase(data,sizeof(data));return result;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_reference(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong owner,jlong handle,jbyteArray token,jboolean retain){
 (void)self;jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;gt_ext_lease_t l;gt_ext_resource_snapshot_t ref;if(!session_rpc_lease(e,lease,&l) || !resource_snapshot_value(e,owner,handle,token,&ref))return NULL;resource_store_slot_t *s=resource_store_owner(e,id);if(!s)return NULL;
 gt_ext_resource_status_t status=retain?gt_ext_resource_store_retain(s->store,&l,&ref):gt_ext_resource_store_release(s->store,&l,&ref);jlong v[2]={status,(jlong)ref.handle};(*e)->SetLongArrayRegion(e,result,0,2,v);
 /* Failed Java handoff must not leak a newly retained pin. Release only the
  * additional ref; the caller's original snapshot continues owning its pin. */
 if(retain && status==GT_EXT_RESOURCE_OK && (*e)->ExceptionCheck(e))(void)gt_ext_resource_store_release(s->store,&l,&ref);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_sendResource(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jobjectArray strings,jlong generation,jlong size,jboolean known){
 (void)self;if(!strings || (*e)->GetArrayLength(e,strings)!=4){extension_error(e,"资源描述无效");return NULL;}jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;gt_ext_resource_descriptor_t d={.generation=(uint64_t)generation,.size=(uint64_t)size,.size_known=known};char *values[4]={d.resource_id,d.sha256,d.mime,d.display_name};size_t limits[4]={32,64,128,1024};for(jsize i=0;i<4;i++){char *p=extension_item(e,strings,i,limits[i]);if(!p)return NULL;memcpy(values[i],p,strlen(p)+1);free(p);}
 extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;uint64_t handle=0;gt_ext_resource_status_t status=gt_ext_session_send_resource(s->session,&l,&d,&handle);extension_unpin(s);jlong v[2]={status,(jlong)handle};(*e)->SetLongArrayRegion(e,result,0,2,v);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_writeResource(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong handle,jbyteArray bytes,jint offset,jint count){
 (void)self;if(!bytes || offset<0 || count<1 || offset>(*e)->GetArrayLength(e,bytes)-count){extension_error(e,"资源写入范围无效");return NULL;}if(count>(jint)GT_EXT_RESOURCE_READ_MAX)count=GT_EXT_RESOURCE_READ_MAX;jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;uint8_t copy[GT_EXT_RESOURCE_READ_MAX];(*e)->GetByteArrayRegion(e,bytes,offset,count,(jbyte *)copy);if((*e)->ExceptionCheck(e))return NULL;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;
 size_t n=0;gt_ext_resource_status_t status=gt_ext_session_write_resource(s->session,&l,(uint64_t)handle,copy,(size_t)count,&n);extension_unpin(s);extension_erase(copy,sizeof(copy));jlong v[2]={status,(jlong)n};(*e)->SetLongArrayRegion(e,result,0,2,v);return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_resourceAction(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong handle,jint action,jboolean complete){
 (void)self;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return GT_EXT_RESOURCE_INVALID;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_status_t status;
 switch(action){case 0:status=gt_ext_session_finish_resource(s->session,&l,(uint64_t)handle,complete);break;case 1:status=gt_ext_session_cancel_resource(s->session,&l,(uint64_t)handle);break;case 2:status=gt_ext_session_forget_resource(s->session,&l,(uint64_t)handle);break;case 3:status=gt_ext_session_reject_resource(s->session,&l);break;default:status=GT_EXT_RESOURCE_INVALID;break;}extension_unpin(s);return status;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_resourceResult(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlong handle){
 (void)self;jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,7,66,&meta,&bytes);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;gt_ext_transfer_result_t r;gt_ext_resource_status_t status=gt_ext_session_resource_result(s->session,&l,(uint64_t)handle,&r);extension_unpin(s);
 jlong v[7]={status,(jlong)r.handle,(jlong)r.accepted,r.state,r.confirmed,r.cancel_requested,(jlong)r.generation};(*e)->SetLongArrayRegion(e,meta,0,7,v);(*e)->SetByteArrayRegion(e,bytes,0,33,(const jbyte *)r.resource_id);(*e)->SetByteArrayRegion(e,bytes,33,33,(const jbyte *)r.code);return result;
}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSession_takeResource(JNIEnv *e,jobject self,jlong id,jbyteArray lease){
 (void)self;jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,6,1285,&meta,&bytes);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;extension_slot_t *s=session_pin(e,id,true,false);if(!s)return NULL;gt_ext_resource_snapshot_t r;gt_ext_resource_status_t status=gt_ext_session_take_resource(s->session,&l,&r);
 jlong v[6]={status,(jlong)r.owner,(jlong)r.handle,(jlong)r.descriptor.generation,(jlong)r.descriptor.size,r.descriptor.size_known};(*e)->SetLongArrayRegion(e,meta,0,6,v);(*e)->SetByteArrayRegion(e,bytes,0,33,(const jbyte *)r.token);resource_descriptor_bytes(e,bytes,33,&r.descriptor);
 if((*e)->ExceptionCheck(e)){if(status==GT_EXT_RESOURCE_OK)(void)gt_ext_resource_store_release(s->store,&l,&r);gt_ext_session_stop(s->session);}extension_unpin(s);extension_erase(&r,sizeof(r));return result;
}
/* Kotlin can fail to allocate/decode its Snapshot after the native handoff.
 * Cleanup takes existing arrays and returns a primitive, allocating no packet. */
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionResourceStore_discard(JNIEnv *e,jobject self,jlong id,jbyteArray lease,jlongArray meta,jbyteArray bytes){
 (void)self;if(!meta || !bytes || (*e)->GetArrayLength(e,meta)!=6 || (*e)->GetArrayLength(e,bytes)!=1285)return GT_EXT_RESOURCE_INVALID;
 jlong v[3];(*e)->GetLongArrayRegion(e,meta,0,3,v);if((*e)->ExceptionCheck(e) || v[0]!=GT_EXT_RESOURCE_OK)return GT_EXT_RESOURCE_INVALID;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return GT_EXT_RESOURCE_INVALID;
 gt_ext_resource_snapshot_t ref={.owner=(uint64_t)v[1],.handle=(uint64_t)v[2]};(*e)->GetByteArrayRegion(e,bytes,0,33,(jbyte *)ref.token);if((*e)->ExceptionCheck(e))return GT_EXT_RESOURCE_INVALID;resource_store_slot_t *s=resource_store_owner(e,id);return s?gt_ext_resource_store_release(s->store,&l,&ref):GT_EXT_RESOURCE_INVALID;
}
typedef struct {uint64_t id;gt_ext_source_t *source;pthread_t owner;} extension_source_slot_t;
static extension_source_slot_t extension_sources[GT_EXT_CHANNEL_LIMIT*GT_EXT_SOURCE_LIMIT];
static extension_source_slot_t *extension_source_owner(JNIEnv *e,jlong id){
 pthread_mutex_lock(&extension_lock);extension_source_slot_t *s=NULL;
 for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT*GT_EXT_SOURCE_LIMIT;i++)if(id>0 && extension_sources[i].id==(uint64_t)id && extension_sources[i].source){s=&extension_sources[i];break;}
 bool valid=s && pthread_equal(s->owner,pthread_self());pthread_mutex_unlock(&extension_lock);
 if(!valid){extension_error(e,"源文件已释放或不在创建 worker");return NULL;}return s;
}
JNIEXPORT jlongArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSource_open(JNIEnv *e,jobject self,jlong session,jbyteArray lease,jint root,jbyteArray path,jbyteArray mime,jlong limit){
 (void)self;jlongArray result=(*e)->NewLongArray(e,2);if(!result)return NULL;gt_ext_lease_t l;if(!session_rpc_lease(e,lease,&l))return NULL;
 char *name=extension_text(e,path,4096);if(!name)return NULL;char *type=extension_text(e,mime,128);if(!type){free(name);return NULL;}
 extension_slot_t *parent=session_pin(e,session,true,false);if(!parent){free(name);free(type);return NULL;}
 pthread_mutex_lock(&extension_lock);extension_source_slot_t *slot=NULL;
 if(extension_next<=INT64_MAX)for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT*GT_EXT_SOURCE_LIMIT;i++)if(!extension_sources[i].id){slot=&extension_sources[i];slot->id=extension_next++;slot->owner=pthread_self();break;}pthread_mutex_unlock(&extension_lock);
 if(!slot){extension_unpin(parent);free(name);free(type);extension_error(e,"源文件 native owner 容量已占用");return NULL;}
 gt_ext_source_config_t c={root,name,type,(uint64_t)limit};gt_ext_source_t *source=NULL;gt_ext_source_status_t status=gt_ext_source_open(parent->session,&l,&c,&source);free(name);free(type);extension_unpin(parent);
 jlong values[2]={status,status==GT_EXT_SOURCE_OK?(jlong)slot->id:0};pthread_mutex_lock(&extension_lock);if(status==GT_EXT_SOURCE_OK)slot->source=source;else memset(slot,0,sizeof(*slot));pthread_mutex_unlock(&extension_lock);
 (*e)->SetLongArrayRegion(e,result,0,2,values);return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSource_step(JNIEnv *e,jobject self,jlong id){(void)self;extension_source_slot_t *s=extension_source_owner(e,id);return s?gt_ext_source_step(s->source):GT_EXT_SOURCE_FAILED;}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSource_send(JNIEnv *e,jobject self,jlong id){(void)self;extension_source_slot_t *s=extension_source_owner(e,id);return s?gt_ext_source_send(s->source):GT_EXT_SOURCE_INVALID;}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSource_cancel(JNIEnv *e,jobject self,jlong id){(void)self;extension_source_slot_t *s=extension_source_owner(e,id);if(s)gt_ext_source_cancel(s->source);}
JNIEXPORT jobjectArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSource_info(JNIEnv *e,jobject self,jlong id){
 (void)self;jlongArray meta;jbyteArray bytes;jobjectArray result=session_rpc_arrays(e,16,1318,&meta,&bytes);if(!result)return NULL;extension_source_slot_t *s=extension_source_owner(e,id);if(!s)return NULL;gt_ext_source_info_t i;
 if(!gt_ext_source_info(s->source,&i)){extension_error(e,"源文件状态无效");return NULL;}
 jlong v[16]={i.phase,i.error,i.cleanup_error,i.closed,i.has_transfer,(jlong)i.hashed,(jlong)i.read_bytes,(jlong)i.accepted,(jlong)i.transfer_accepted,i.transfer_state,i.confirmed,i.cancel_requested,(jlong)i.generation,(jlong)i.descriptor.generation,(jlong)i.descriptor.size,i.descriptor.size_known};
 (*e)->SetLongArrayRegion(e,meta,0,16,v);resource_descriptor_bytes(e,bytes,0,&i.descriptor);(*e)->SetByteArrayRegion(e,bytes,1252,33,(const jbyte *)i.resource_id);(*e)->SetByteArrayRegion(e,bytes,1285,33,(const jbyte *)i.code);return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeExtensionSource_close(JNIEnv *e,jobject self,jlong id){
 (void)self;extension_source_slot_t *s=extension_source_owner(e,id);if(!s)return GT_EXT_SOURCE_INVALID;
 gt_ext_source_status_t status=gt_ext_source_close(s->source);if(status!=GT_EXT_SOURCE_OK)return status;gt_ext_source_t *source=s->source;status=gt_ext_source_destroy(&source);
 if(status==GT_EXT_SOURCE_OK){pthread_mutex_lock(&extension_lock);memset(s,0,sizeof(*s));pthread_mutex_unlock(&extension_lock);}return status;
}
