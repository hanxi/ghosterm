/* Included only in the explicitly enabled, content-pinned engine build. */
#include "extension/session-owner.h"
#include "base/sha256.h"
#include "base/secure-random.h"
#include <quickjs.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
typedef struct {char name[256];char *source;size_t size;} runtime_module_t;
struct gt_bridge_runtime {
 gt_ext_session_t *session;gt_ext_lease_t lease;gt_ext_role_t role;
 runtime_module_t modules[64];size_t count;char entry[256];
 JSRuntime *engine;JSContext *context;JSValue module,space,pump,dispose,unhandled[64];size_t unhandled_count;
 atomic_bool stopped;uint64_t deadline,activation_deadline;uint32_t cpu_ms,jobs_per_step,activation_ms;
 gt_runtime_info_t info;bool started,rejected,interrupted;
};
static const char *runtime_status[]={"ok","wait","invalid","denied","unsupported","closed"};
static const char *runtime_states[]={"queued","pending","succeeded","failed","cancelled","unknown"};
static bool runtime_now(uint64_t *out){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static void runtime_fail(gt_bridge_runtime_t *r,gt_runtime_error_t error){if(r->info.phase!=GT_RUNTIME_FAILED && r->info.phase!=GT_RUNTIME_STOPPED){r->info.phase=error==GT_RUNTIME_CANCELLED?GT_RUNTIME_STOPPED:GT_RUNTIME_FAILED;r->info.error=error;}gt_ext_session_revoke(r->session);}
static int runtime_interrupt(JSContext *ctx,void *opaque){
 (void)ctx;gt_bridge_runtime_t *r=opaque;uint64_t now;
 if(atomic_load_explicit(&r->stopped,memory_order_relaxed)){r->interrupted=true;return 1;}
 if(!runtime_now(&now) || now>=r->deadline){r->interrupted=true;return 1;}return 0;
}
static void runtime_exception(gt_bridge_runtime_t *r){
 JSValue e=JS_GetException(r->context);JS_FreeValue(r->context,e);
 runtime_fail(r,atomic_load_explicit(&r->stopped,memory_order_relaxed)?GT_RUNTIME_CANCELLED:r->interrupted?GT_RUNTIME_CPU:GT_RUNTIME_SCRIPT);
}
static void runtime_rejection(JSContext *ctx,JSValueConst promise,JSValueConst reason,bool handled,void *opaque){
 (void)reason;gt_bridge_runtime_t *r=opaque;
 for(size_t i=0;i<r->unhandled_count;i++)if(JS_VALUE_GET_PTR(r->unhandled[i])==JS_VALUE_GET_PTR(promise)){
  if(handled){JS_FreeValue(ctx,r->unhandled[i]);r->unhandled[i]=r->unhandled[--r->unhandled_count];}return;
 }
 if(!handled){if(r->unhandled_count==64)r->rejected=true;else r->unhandled[r->unhandled_count++]=JS_DupValue(ctx,promise);}
}
static bool runtime_name(const char *name){
 if(!name || strnlen(name,256)>255)return false;const char *p=!strncmp(name,"plugin:/",8)?name+8:!strncmp(name,"sdk:/",5)?name+5:NULL;if(!p || !*p || *p=='/')return false;
 for(const char *at=p;*at;at++)if(!((*at>='a' && *at<='z') || (*at>='A' && *at<='Z') || (*at>='0' && *at<='9') || *at=='_' || *at=='-' || *at=='.' || *at=='/'))return false;
 for(const char *at=p;*at;){const char *end=strchr(at,'/');size_t n=end?(size_t)(end-at):strlen(at);if(!n || (n==1 && *at=='.') || (n==2 && !strncmp(at,"..",2)))return false;if(!end)break;at=end+1;if(!*at)return false;}
 return true;
}
static char *runtime_normalize(JSContext *ctx,const char *base,const char *name,void *opaque){
 (void)opaque;char path[256];
 if(!strcmp(name,"host:/native") && !strcmp(base,"host:/bootstrap.js"))return js_strdup(ctx,name);
 if(!strcmp(name,"@ghosterm/plugin-sdk"))return js_strdup(ctx,"sdk:/index.js");
 if(runtime_name(name))return js_strdup(ctx,name);
 if(name[0]!='.' || (strncmp(base,"plugin:/",8) && strncmp(base,"sdk:/",5)))goto denied;
 const char *slash=strrchr(base,'/');if(!slash || (size_t)(slash-base)+1+strlen(name)>=sizeof(path))goto denied;
 memcpy(path,base,(size_t)(slash-base)+1);strcpy(path+(slash-base)+1,name);
 {char result[256];size_t prefix=!strncmp(base,"plugin:/",8)?8:5,used=prefix;memcpy(result,base,prefix);const char *at=path+prefix;
 while(*at){const char *end=strchr(at,'/');size_t n=end?(size_t)(end-at):strlen(at);
  if(n==1 && *at=='.'){}else if(n==2 && !strncmp(at,"..",2)){if(used<=prefix)goto denied;used--;while(used>prefix && result[used-1]!='/')used--;}
  else {if(!n || used+n+1>=sizeof(result))goto denied;memcpy(result+used,at,n);used+=n;if(end)result[used++]='/';}
  if(!end)break;at=end+1;
 }result[used]=0;if(!runtime_name(result))goto denied;return js_strdup(ctx,result);}
denied:JS_ThrowReferenceError(ctx,"module unavailable");return NULL;
}
static JSValue runtime_decimal(JSContext *ctx,uint64_t n){char b[32];(void)snprintf(b,sizeof(b),"%" PRIu64,n);return JS_NewString(ctx,b);}
static bool runtime_handle(JSContext *ctx,JSValueConst v,uint64_t *out){
 if(!JS_IsString(v))return false;size_t n;const char *s=JS_ToCStringLen(ctx,&n,v);if(!s)return false;uint64_t value=0;bool ok=n && n<=20 && s[0]>='1' && s[0]<='9';
 for(size_t i=0;i<n && ok;i++){if(s[i]<'0' || s[i]>'9' || value>(UINT64_MAX-(unsigned)(s[i]-'0'))/10)ok=false;else value=value*10+(unsigned)(s[i]-'0');}
 JS_FreeCString(ctx,s);if(ok)*out=value;return ok;
}
static int runtime_set(JSContext *c,JSValue object,const char *key,JSValue value){return JS_SetPropertyStr(c,object,key,value);}
static JSValue runtime_native(JSContext *c,JSValueConst self,int argc,JSValueConst *argv,int op){
 (void)self;gt_bridge_runtime_t *r=JS_GetContextOpaque(c);if(!r || !r->session)return JS_ThrowInternalError(c,"runtime closed");
 if(op==7){gt_ext_session_revoke(r->session);return JS_UNDEFINED;}
 if(op==8){gt_ext_lease_t live;return JS_NewBool(c,!atomic_load_explicit(&r->stopped,memory_order_relaxed) && r->info.phase<GT_RUNTIME_FAILED && gt_ext_session_lease(r->session,&live) && live.epoch==r->lease.epoch && !memcmp(live.token,r->lease.token,33));}
 if((op==0 || op==4 || op==5 || op==6) && (atomic_load_explicit(&r->stopped,memory_order_relaxed) || r->info.phase>=GT_RUNTIME_FAILED))return op==4?JS_NULL:JS_ThrowInternalError(c,"runtime stopped");
 uint64_t h=0;gt_ext_rpc_status_t status;
 if(op==0){
  if(argc!=3 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]) || !JS_IsNumber(argv[2]))goto invalid;
  size_t namesize,n;const char *name=JS_ToCStringLen(c,&namesize,argv[0]),*json=JS_ToCStringLen(c,&n,argv[1]);double ms;
  if(!name || !json){JS_FreeCString(c,name);JS_FreeCString(c,json);return JS_EXCEPTION;}
  if(namesize>128 || memchr(name,0,namesize) || JS_ToFloat64(c,&ms,argv[2]) || !(ms>=1 && ms<=3600000) || ms!=(uint32_t)ms){JS_FreeCString(c,name);JS_FreeCString(c,json);goto invalid;}
  status=gt_ext_session_request(r->session,&r->lease,name,json,n,(uint32_t)ms,&h);JS_FreeCString(c,name);JS_FreeCString(c,json);JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out)){runtime_fail(r,GT_RUNTIME_MEMORY);return out;}
  if(runtime_set(c,out,"status",JS_NewString(c,runtime_status[status]))<0 || (h && runtime_set(c,out,"handle",runtime_decimal(c,h))<0)){JS_FreeValue(c,out);runtime_fail(r,GT_RUNTIME_MEMORY);return JS_EXCEPTION;}return out;
 }
 if(op==4){
  gt_ext_rpc_request_t q;status=gt_ext_session_take_request(r->session,&r->lease,&q);if(status==GT_EXT_RPC_WAIT || status==GT_EXT_RPC_CLOSED || status==GT_EXT_RPC_DENIED)return JS_NULL;
  if(status!=GT_EXT_RPC_OK)return JS_ThrowInternalError(c,"request unavailable");JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out)){runtime_fail(r,GT_RUNTIME_MEMORY);return out;}
  if(runtime_set(c,out,"ticket",runtime_decimal(c,q.ticket))<0 || runtime_set(c,out,"method",JS_NewString(c,q.method))<0 || runtime_set(c,out,"json",JS_NewStringLen(c,q.input,q.input_size))<0){JS_FreeValue(c,out);runtime_fail(r,GT_RUNTIME_MEMORY);return JS_EXCEPTION;}return out;
 }
 if(argc<1 || !runtime_handle(c,argv[0],&h))goto invalid;
 if(op==1){
  gt_ext_rpc_result_t v;status=gt_ext_session_request_result(r->session,&r->lease,h,&v);if(status!=GT_EXT_RPC_OK)return JS_ThrowInternalError(c,"result unavailable");JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out)){runtime_fail(r,GT_RUNTIME_MEMORY);return out;}
  if(runtime_set(c,out,"state",JS_NewString(c,runtime_states[v.state]))<0 || runtime_set(c,out,"confirmed",JS_NewBool(c,v.confirmed))<0 || runtime_set(c,out,"outcome",JS_NewStringLen(c,v.outcome,v.outcome_size))<0){JS_FreeValue(c,out);runtime_fail(r,GT_RUNTIME_MEMORY);return JS_EXCEPTION;}return out;
 }
 if(op==2)status=gt_ext_session_cancel_request(r->session,&r->lease,h);
 else if(op==3)status=gt_ext_session_forget_request(r->session,&r->lease,h);
 else if(op==5)status=gt_ext_session_check_request(r->session,&r->lease,h);
 else if(op==6){if(argc!=2 || !JS_IsString(argv[1]))goto invalid;size_t n;const char *json=JS_ToCStringLen(c,&n,argv[1]);if(!json)return JS_EXCEPTION;status=gt_ext_session_respond(r->session,&r->lease,h,json,n);JS_FreeCString(c,json);}
 else goto invalid;
 return JS_NewString(c,runtime_status[status]);
invalid:return JS_ThrowTypeError(c,"invalid native request");
}
static int runtime_bind(JSContext *c,JSModuleDef *module){
 const char *names[]={"request","result","cancel","forget","take","check","respond","revoke","alive"};
 for(int i=0;i<9;i++)if(JS_SetModuleExport(c,module,names[i],JS_NewCFunctionMagic(c,runtime_native,names[i],0,JS_CFUNC_generic_magic,i)))return -1;return 0;
}
static JSModuleDef *runtime_load(JSContext *ctx,const char *name,void *opaque){
 gt_bridge_runtime_t *r=opaque;
 if(!strcmp(name,"host:/native")){JSModuleDef *m=JS_NewCModule(ctx,name,runtime_bind);if(!m)return NULL;const char *names[]={"request","result","cancel","forget","take","check","respond","revoke","alive"};for(int i=0;i<9;i++)if(JS_AddModuleExport(ctx,m,names[i]))return NULL;return m;}
 for(size_t i=0;i<r->count;i++)if(!strcmp(name,r->modules[i].name)){JSValue v=JS_Eval(ctx,r->modules[i].source,r->modules[i].size,name,JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);if(JS_IsException(v))return NULL;JSModuleDef *m=JS_VALUE_GET_PTR(v);JS_FreeValue(ctx,v);return m;}
 JS_ThrowReferenceError(ctx,"module unavailable");return NULL;
}
static void runtime_free_engine(gt_bridge_runtime_t *r){
 if(r->engine)JS_SetHostPromiseRejectionTracker(r->engine,NULL,NULL);
 for(size_t i=0;i<r->unhandled_count;i++)JS_FreeValue(r->context,r->unhandled[i]);r->unhandled_count=0;
 if(r->context){JS_FreeValue(r->context,r->module);JS_FreeValue(r->context,r->space);JS_FreeValue(r->context,r->pump);JS_FreeValue(r->context,r->dispose);JS_FreeContext(r->context);r->context=NULL;}if(r->engine){JS_FreeRuntime(r->engine);r->engine=NULL;}
}
static void runtime_free_modules(gt_bridge_runtime_t *r){for(size_t i=0;i<r->count;i++)free(r->modules[i].source);}
gt_status_t gt__bridge_runtime_open(gt_ext_session_t **session,const gt_runtime_config_t *c,gt_bridge_runtime_t **out){
 if(out)*out=NULL;if(!out || !session || !*session || !c || !c->modules || !c->module_count || c->module_count>64 || !runtime_name(c->entry) || strncmp(c->entry,"plugin:/",8) || (c->role!=GT_EXT_HOST && c->role!=GT_EXT_EXECUTOR))return GT_ERR_INVALID_ARG;
 gt_ext_session_info_t info;if(!gt_ext_session_info(*session,&info) || info.phase!=GT_EXT_SESSION_ACTIVE || !(*session)->requests || (*session)->sources || !gt__bridge_session_runtime_admissible((*session)->inner))return GT_ERR_INVALID_ARG;
 gt_bridge_role_t actual_role;size_t method_count;gt__bridge_session_methods((*session)->inner,&method_count,&actual_role);if((unsigned)actual_role!=(unsigned)c->role)return GT_ERR_INVALID_ARG;
 size_t memory=c->memory_bytes?c->memory_bytes:16*1024*1024,stack=c->stack_bytes?c->stack_bytes:256*1024;uint32_t cpu=c->cpu_ms?c->cpu_ms:25,jobs=c->jobs_per_step?c->jobs_per_step:32;
 if(c->activation_ms>60000)return GT_ERR_INVALID_ARG;
 if(memory<1024*1024 || memory>128*1024*1024 || stack<64*1024 || stack>1024*1024 || cpu>1000 || jobs>128)return GT_ERR_INVALID_ARG;
 gt_bridge_runtime_t *r=calloc(1,sizeof(*r));if(!r)return GT_ERR_IO;atomic_init(&r->stopped,false);r->cpu_ms=cpu;r->jobs_per_step=jobs;r->activation_ms=c->activation_ms?c->activation_ms:5000;r->role=c->role;strcpy(r->entry,c->entry);r->module=r->space=r->pump=r->dispose=JS_UNDEFINED;
 gt_status_t status=GT_ERR_INVALID_ARG;bool entry=false;
 for(size_t i=0;i<c->module_count;i++){
  const gt_runtime_module_t *m=&c->modules[i];if(!runtime_name(m->name) || !m->source || !m->size || m->size>128*1024 || memchr(m->source,0,m->size) || r->info.module_bytes+m->size>1024*1024 || strnlen(m->sha256,65)!=64)goto fail;
  for(size_t j=0;j<i;j++)if(!strcmp(m->name,r->modules[j].name))goto fail;
  gt_sha256_t hash;char hex[65];gt__sha256_init(&hash);if(!gt__sha256_update(&hash,m->source,m->size) || !gt__sha256_final(&hash,hex) || strcmp(hex,m->sha256)){status=GT_ERR_PROTOCOL;goto fail;}
  runtime_module_t *copy=&r->modules[r->count++];strcpy(copy->name,m->name);copy->source=malloc(m->size+1);if(!copy->source){status=GT_ERR_IO;goto fail;}memcpy(copy->source,m->source,m->size);copy->source[m->size]=0;copy->size=m->size;r->info.module_bytes+=m->size;if(!strcmp(m->name,c->entry))entry=true;
 }
 if(!entry)goto fail;r->engine=JS_NewRuntime();if(!r->engine){status=GT_ERR_IO;goto fail;}JS_SetMemoryLimit(r->engine,memory);JS_SetCanBlock(r->engine,false);JS_SetMaxStackSize(r->engine,stack);r->context=JS_NewContext(r->engine);if(!r->context){status=GT_ERR_IO;goto fail;}
 JS_SetContextOpaque(r->context,r);JS_SetModuleLoaderFunc(r->engine,runtime_normalize,runtime_load,r);JS_SetInterruptHandler(r->engine,runtime_interrupt,r);JS_SetHostPromiseRejectionTracker(r->engine,runtime_rejection,r);
 if(!gt_ext_session_lease(*session,&r->lease))goto fail;r->session=*session;*session=NULL;*out=r;return GT_OK;
fail:runtime_free_engine(r);runtime_free_modules(r);gt__secure_erase(r,sizeof(*r));free(r);return status;
}
static bool runtime_start(gt_bridge_runtime_t *r){
 char script[4096];int n=snprintf(script,sizeof(script),
 "import {SessionChannel} from 'sdk:/index.js';import * as port from 'host:/native';import {activate} from '%s';"
 "const session=new SessionChannel(port);let state=0,disposable;const cancellation=Object.freeze({get cancelled(){return !port.alive()}});"
 "const context=Object.freeze({session,side:'%s',instanceId:'%s',epoch:'%" PRIu64 "',cancellation});"
 "export function pump(){session.step();return state}export function dispose(){session.dispose();try{disposable?.dispose()}catch{}}"
 "Promise.resolve().then(()=>activate(context)).then(value=>{if(!value || typeof value.dispose!=='function')throw Error('entry');disposable=value;state=1},()=>{state=-1}).catch(()=>{state=-1});",
 r->entry,r->role==GT_EXT_HOST?"client":"executor",r->lease.instance,r->lease.epoch);
 if(n<0 || (size_t)n>=sizeof(script)){runtime_fail(r,GT_RUNTIME_PROTOCOL);return false;}
 JSValue compiled=JS_Eval(r->context,script,(size_t)n,"host:/bootstrap.js",JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);if(JS_IsException(compiled)){runtime_exception(r);return false;}
 JSModuleDef *m=JS_VALUE_GET_PTR(compiled);r->module=JS_EvalFunction(r->context,compiled);if(JS_IsException(r->module)){r->module=JS_UNDEFINED;runtime_exception(r);return false;}
 r->space=JS_GetModuleNamespace(r->context,m);if(JS_IsException(r->space)){r->space=JS_UNDEFINED;runtime_exception(r);return false;}JS_PromiseMarkAsHandled(r->context,r->module);return true;
}
static bool runtime_jobs(gt_bridge_runtime_t *r,uint32_t *used){
 while(JS_IsJobPending(r->engine)){
  if(*used>=r->jobs_per_step){runtime_fail(r,GT_RUNTIME_JOBS);return false;}
  JSContext *ctx=NULL;if(JS_ExecutePendingJob(r->engine,&ctx)<0){runtime_exception(r);return false;}(*used)++;r->info.jobs++;if(r->info.phase>=GT_RUNTIME_FAILED)return false;
 }return true;
}
gt_runtime_phase_t gt__bridge_runtime_step(gt_bridge_runtime_t *r){
 if(!r)return GT_RUNTIME_CLOSED;if(r->info.phase>=GT_RUNTIME_FAILED)return r->info.phase;
 if(atomic_load_explicit(&r->stopped,memory_order_relaxed)){runtime_fail(r,GT_RUNTIME_CANCELLED);return r->info.phase;}
 uint64_t now;if(!runtime_now(&now)){runtime_fail(r,GT_RUNTIME_IO);return r->info.phase;}r->deadline=now+r->cpu_ms;r->interrupted=false;r->info.steps++;
 gt_ext_session_phase_t native=gt_ext_session_step(r->session,0);gt_ext_session_info_t info;if(gt_ext_session_info(r->session,&info)){r->info.session_end=info.end;r->info.session_error=info.error;}
 if(native!=GT_EXT_SESSION_ACTIVE){runtime_fail(r,GT_RUNTIME_PROTOCOL);return r->info.phase;}
 if(!r->started){r->started=true;r->activation_deadline=now+r->activation_ms;r->info.phase=GT_RUNTIME_STARTING;if(!runtime_start(r))return r->info.phase;}
 uint32_t used=0;if(!runtime_jobs(r,&used))return r->info.phase;
 JSPromiseStateEnum module=JS_PromiseState(r->context,r->module);
 if(module==JS_PROMISE_REJECTED){runtime_fail(r,atomic_load_explicit(&r->stopped,memory_order_relaxed)?GT_RUNTIME_CANCELLED:r->interrupted?GT_RUNTIME_CPU:GT_RUNTIME_SCRIPT);return r->info.phase;}
 if(module!=JS_PROMISE_PENDING){
  if(JS_IsUndefined(r->pump)){r->pump=JS_GetPropertyStr(r->context,r->space,"pump");r->dispose=JS_GetPropertyStr(r->context,r->space,"dispose");if(JS_IsException(r->pump) || JS_IsException(r->dispose)){runtime_exception(r);return r->info.phase;}}
  JSValue value=JS_Call(r->context,r->pump,JS_UNDEFINED,0,NULL);if(JS_IsException(value)){runtime_exception(r);return r->info.phase;}int32_t state;
  if(JS_ToInt32(r->context,&state,value)){JS_FreeValue(r->context,value);runtime_exception(r);return r->info.phase;}JS_FreeValue(r->context,value);
  if(state<0){runtime_fail(r,GT_RUNTIME_SCRIPT);return r->info.phase;}if(state>0)r->info.phase=GT_RUNTIME_ACTIVE;
  if(!runtime_jobs(r,&used))return r->info.phase;
 }
 if(r->rejected || r->unhandled_count)runtime_fail(r,GT_RUNTIME_SCRIPT);
 else if(r->info.phase==GT_RUNTIME_STARTING && (!runtime_now(&now) || now>=r->activation_deadline))runtime_fail(r,GT_RUNTIME_EXPIRED);
 return r->info.phase;
}
bool gt__bridge_runtime_info(const gt_bridge_runtime_t *r,gt_runtime_info_t *out){if(out)memset(out,0,sizeof(*out));if(!r || !out)return false;*out=r->info;out->engine_released=r->engine==NULL;return true;}
void gt__bridge_runtime_stop(gt_bridge_runtime_t *r){if(r)atomic_store_explicit(&r->stopped,true,memory_order_relaxed);}
gt_ext_close_t gt__bridge_runtime_close(gt_bridge_runtime_t *r){
 if(!r)return GT_EXT_CLOSE_ERROR;if(r->info.phase==GT_RUNTIME_CLOSED)return GT_EXT_RELEASED;
 gt_ext_session_revoke(r->session);gt_ext_session_stop(r->session);atomic_store_explicit(&r->stopped,true,memory_order_relaxed);
 if(r->context && JS_IsFunction(r->context,r->dispose)){JSValue v=JS_Call(r->context,r->dispose,JS_UNDEFINED,0,NULL);if(JS_IsException(v)){JSValue error=JS_GetException(r->context);JS_FreeValue(r->context,error);r->info.dispose_error=true;}JS_FreeValue(r->context,v);}
 runtime_free_engine(r);
 gt_ext_close_t close=gt_ext_session_close(r->session,0);if(close!=GT_EXT_RELEASED){if(close==GT_EXT_CLOSE_ERROR)r->info.cleanup_error=GT_ERR_IO;return close;}
 gt_ext_session_info_t snapshot;if(gt_ext_session_info(r->session,&snapshot)){r->info.session_end=snapshot.end;r->info.session_error=snapshot.error;}
 gt_status_t status=gt_ext_session_destroy(&r->session);if(status!=GT_OK){r->info.cleanup_error=status;return GT_EXT_CLOSE_ERROR;}r->info.cleanup_error=GT_OK;r->info.phase=GT_RUNTIME_CLOSED;return GT_EXT_RELEASED;
}
gt_status_t gt__bridge_runtime_destroy(gt_bridge_runtime_t **out){if(!out || !*out || (*out)->info.phase!=GT_RUNTIME_CLOSED)return GT_ERR_INVALID_ARG;gt_bridge_runtime_t *r=*out;runtime_free_modules(r);gt__secure_erase(r,sizeof(*r));free(r);*out=NULL;return GT_OK;}
