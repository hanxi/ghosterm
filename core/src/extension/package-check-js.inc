/* Trusted metadata-only evaluator; compiled only for the explicit JS backend. */
#include "extension/memory.h"
#include "extension/package-json.h"
#include <quickjs.h>
#include <time.h>
#include "extension/package-validator-code.inc"
typedef union {max_align_t alignment;struct {size_t size;} value;} package_js_allocation_t;
typedef struct {
 gt_allocator_t allocator;const gt_package_check_config_t *config;
 size_t used,limit;uint64_t deadline;bool memory,expired,cancelled,clock_error;
} package_check_owner_t;
static void *check_allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void check_release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static void check_wipe_free(package_check_owner_t *s,void *p,size_t n){if(p){gt__secure_erase(p,n);gt__plugin_memory_free(&s->allocator,p,n);}}
static void *check_malloc(void *opaque,size_t n){
 package_check_owner_t *s=opaque;if(!n)n=1;if(n>SIZE_MAX-sizeof(package_js_allocation_t) || n+sizeof(package_js_allocation_t)>s->limit-s->used){s->memory=true;return NULL;}
 size_t size=n+sizeof(package_js_allocation_t);package_js_allocation_t *a=gt__plugin_memory_alloc(&s->allocator,size);if(!a){s->memory=true;return NULL;}a->value.size=n;s->used+=size;return a+1;
}
static void check_free(void *opaque,void *p){if(!p)return;package_check_owner_t *s=opaque;package_js_allocation_t *a=(package_js_allocation_t *)p-1;size_t n=a->value.size+sizeof(*a);s->used-=n;check_wipe_free(s,a,n);}
static void *check_calloc(void *opaque,size_t count,size_t size){package_check_owner_t *s=opaque;if(count && size>SIZE_MAX/count){s->memory=true;return NULL;}size_t n=count*size;void *p=check_malloc(s,n);if(p)memset(p,0,n);return p;}
static void *check_realloc(void *opaque,void *old,size_t n){
 if(!old)return check_malloc(opaque,n);if(!n){check_free(opaque,old);return NULL;}package_js_allocation_t *a=(package_js_allocation_t *)old-1;size_t previous=a->value.size;void *p=check_malloc(opaque,n);if(p){memcpy(p,old,n<previous?n:previous);check_free(opaque,old);}return p;
}
static size_t check_usable(const void *p){return p?((const package_js_allocation_t *)p-1)->value.size:0;}
static bool check_clock(package_check_owner_t *s,uint64_t *out){
 if(s->config->clock)return s->config->clock(s->config->clock_context,out);struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;
}
static bool check_budget(package_check_owner_t *s){
 if(s->config->stop && atomic_load_explicit(s->config->stop,memory_order_relaxed)){s->cancelled=true;return false;}uint64_t now;if(!check_clock(s,&now)){s->clock_error=true;return false;}if(now>=s->deadline){s->expired=true;return false;}return true;
}
static int check_interrupt(JSContext *ctx,void *opaque){(void)ctx;return !check_budget(opaque);}
static gt_package_check_status_t check_exception(package_check_owner_t *s,gt_package_check_report_t *out){
 if(s->cancelled)return check_report(out,GT_PACKAGE_CHECK_CANCELLED,"CANCELLED");if(s->clock_error)return check_report(out,GT_PACKAGE_CHECK_IO,"CLOCK_IO");if(s->expired)return check_report(out,GT_PACKAGE_CHECK_LIMIT,"CPU_BUDGET");if(s->memory)return check_report(out,GT_PACKAGE_CHECK_MEMORY,"ENGINE_MEMORY");return check_report(out,GT_PACKAGE_CHECK_INTERNAL,"VALIDATOR_FAILURE");
}
static int check_set(JSContext *c,JSValue object,const char *name,JSValue value){if(JS_IsException(value))return -1;return JS_SetPropertyStr(c,object,name,value);}
static bool check_string(JSContext *c,JSValueConst object,const char *name,char *out,size_t capacity){
 JSValue value=JS_GetPropertyStr(c,object,name);size_t size=0;const char *p=JS_IsString(value)?JS_ToCStringLen(c,&size,value):NULL;bool ok=p && size<capacity && !memchr(p,0,size);if(ok){memcpy(out,p,size);out[size]=0;}JS_FreeCString(c,p);JS_FreeValue(c,value);return ok;
}
static bool check_strings(JSContext *c,JSValueConst object,const char *name,char out[16][129],size_t *count){
 JSValue array=JS_GetPropertyStr(c,object,name),length=JS_GetPropertyStr(c,array,"length");uint32_t n=0;bool ok=JS_IsArray(array) && !JS_ToUint32(c,&n,length) && n<=16;JS_FreeValue(c,length);
 for(uint32_t i=0;i<n && ok;i++){JSValue value=JS_GetPropertyUint32(c,array,i);size_t size=0;const char *p=JS_IsString(value)?JS_ToCStringLen(c,&size,value):NULL;ok=p && size<=128 && !memchr(p,0,size);if(ok){memcpy(out[i],p,size);out[i][size]=0;}JS_FreeCString(c,p);JS_FreeValue(c,value);}JS_FreeValue(c,array);if(ok)*count=n;return ok;
}
static JSValue check_json(package_check_owner_t *s,JSContext *c,const void *bytes,size_t size){
 char *copy=gt__plugin_memory_alloc(&s->allocator,size+1);if(!copy){s->memory=true;return JS_EXCEPTION;}if(size)memcpy(copy,bytes,size);copy[size]=0;JSValue value=JS_ParseJSON(c,copy,size,"package-metadata");check_wipe_free(s,copy,size+1);return value;
}
static bool check_protocols(JSContext *c,JSValueConst result,gt_package_check_info_t *out){
 JSValue array=JS_GetPropertyStr(c,result,"protocols"),length=JS_GetPropertyStr(c,array,"length");uint32_t n=0;bool ok=JS_IsArray(array) && !JS_ToUint32(c,&n,length) && n>=1 && n<=8;JS_FreeValue(c,length);
 for(uint32_t i=0;i<n && ok;i++){JSValue v=JS_GetPropertyUint32(c,array,i);uint32_t protocol=0;ok=!JS_ToUint32(c,&protocol,v) && protocol>=1 && protocol<=65535;JS_FreeValue(c,v);if(ok)out->protocols[i]=(uint16_t)protocol;}JS_FreeValue(c,array);if(ok)out->protocol_count=n;return ok;
}
static bool check_command(JSContext *c,JSValueConst commands,uint32_t index,gt_package_command_t *out){
 JSValue command=JS_GetPropertyUint32(c,commands,index),path=JS_GetPropertyStr(c,command,"path"),length=JS_GetPropertyStr(c,path,"length");uint32_t count=0;
 bool ok=JS_IsArray(path) && !JS_ToUint32(c,&count,length) && count>=1 && count<=8;JS_FreeValue(c,length);size_t used=0;
 for(uint32_t j=0;j<count && ok;j++){
  JSValue value=JS_GetPropertyUint32(c,path,j);size_t n=0;const char *token=JS_IsString(value)?JS_ToCStringLen(c,&n,value):NULL;
  ok=token && n>=1 && n<=64 && token[0]>='a' && token[0]<='z';
  for(size_t k=0;k<n && ok;k++)ok=(token[k]>='a' && token[k]<='z') || (token[k]>='0' && token[k]<='9') || token[k]=='-';
  if(ok){if(j)out->path[used++]=' ';memcpy(out->path+used,token,n);used+=n;out->path[used]=0;}JS_FreeCString(c,token);JS_FreeValue(c,value);
 }
 JS_FreeValue(c,path);ok=ok && check_string(c,command,"title",out->title,sizeof(out->title)) && check_string(c,command,"argsSchema",out->args_schema,sizeof(out->args_schema));
 JSValue views=JS_GetPropertyStr(c,command,"views");count=0;
 if(!JS_IsUndefined(views)){length=JS_GetPropertyStr(c,views,"length");ok=ok && JS_IsArray(views) && !JS_ToUint32(c,&count,length) && count<=8;JS_FreeValue(c,length);}
 for(uint32_t j=0;j<count && ok;j++){JSValue value=JS_GetPropertyUint32(c,views,j);size_t n=0;const char *v=JS_IsString(value)?JS_ToCStringLen(c,&n,value):NULL;ok=v && n<sizeof(out->views[j]) && !memchr(v,0,n);if(ok){memcpy(out->views[j],v,n);out->views[j][n]=0;}JS_FreeCString(c,v);JS_FreeValue(c,value);}
 if(ok)out->view_count=count;JS_FreeValue(c,views);JS_FreeValue(c,command);return ok;
}
gt_package_check_status_t gt__package_check(gt_package_snapshot_t **snapshot,const gt_package_check_config_t *config,const gt_allocator_t *allocator,gt_package_checked_t **out,gt_package_check_report_t *report){
 if(out)*out=NULL;check_report(report,GT_PACKAGE_CHECK_INVALID,"INVALID_ARGUMENT");
 if(!snapshot || !*snapshot || !config || !out || !report || (!config->host_json && config->host_size) || (!config->registry_json && config->registry_size))return GT_PACKAGE_CHECK_INVALID;
 size_t count=0;uint64_t bytes=0;if(!gt__package_snapshot_count(*snapshot,&count,&bytes))return check_report(report,GT_PACKAGE_CHECK_INVALID,"CAPTURE_NOT_READY");
 if(config->host_size>65536 || config->registry_size>1048576)return check_report(report,GT_PACKAGE_CHECK_LIMIT,"FACT_BYTES");
 size_t memory=config->memory_bytes?config->memory_bytes:67108864,stack=config->stack_bytes?config->stack_bytes:524288;uint32_t cpu=config->cpu_ms?config->cpu_ms:1000;
 if(memory<4194304 || memory>134217728 || stack<65536 || stack>1048576 || cpu>5000)return GT_PACKAGE_CHECK_INVALID;
 package_check_owner_t state={.allocator=allocator?*allocator:(gt_allocator_t){NULL,check_allocate,check_release},.config=config,.limit=memory};
 if(!state.allocator.allocate || !state.allocator.release)return GT_PACKAGE_CHECK_INVALID;
 uint64_t now;if(!check_clock(&state,&now) || now>UINT64_MAX-cpu)return check_report(report,GT_PACKAGE_CHECK_IO,"CLOCK_IO");state.deadline=now+cpu;if(!check_budget(&state))return check_exception(&state,report);
 gt_package_json_t *json=NULL;gt_package_checked_t *checked=NULL;JSRuntime *engine=NULL;JSContext *c=NULL;JSValue api=JS_UNDEFINED,fn=JS_UNDEFINED,answer=JS_UNDEFINED,args[5]={JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};char *code=NULL;
 if(!gt__package_json_open(&state.allocator,&json)){check_report(report,GT_PACKAGE_CHECK_MEMORY,"NATIVE_MEMORY");goto done;}
 const char *names[2]={"plugin.json","integrity.json"};gt_package_member_t metadata[2];const uint8_t *raw[2]={NULL,NULL};size_t lengths[2]={0,0};
 for(size_t i=0;i<2;i++){
  size_t index;if(gt__package_snapshot_find(*snapshot,names[i],&index)!=GT_PACKAGE_OK || !gt__package_snapshot_member(*snapshot,index,&metadata[i]) || !gt__package_snapshot_view(*snapshot,index,&raw[i],&lengths[i])){check_report(report,GT_PACKAGE_CHECK_INVALID,"REQUIRED_PACKAGE_MEMBER");goto done;}
  gt_package_json_error_t error=gt__package_json_check(json,raw[i],lengths[i],true);if(error!=GT_PACKAGE_JSON_OK){check_report(report,error==GT_PACKAGE_JSON_MEMORY?GT_PACKAGE_CHECK_MEMORY:error==GT_PACKAGE_JSON_LIMIT?GT_PACKAGE_CHECK_LIMIT:GT_PACKAGE_CHECK_INVALID,error==GT_PACKAGE_JSON_MEMORY?"NATIVE_MEMORY":error==GT_PACKAGE_JSON_DUPLICATE?"JSON_DUPLICATE":error==GT_PACKAGE_JSON_UTF8?"JSON_UTF8":error==GT_PACKAGE_JSON_NUMBER?"JSON_INTEGER":error==GT_PACKAGE_JSON_LIMIT?"JSON_LIMIT":"JSON_SYNTAX");goto done;}if(!check_budget(&state)){check_exception(&state,report);goto done;}
 }
 for(size_t i=0;i<2;i++){
  const void *p=i?config->registry_json:config->host_json;size_t n=i?config->registry_size:config->host_size;
  if(p){gt_package_json_error_t error=gt__package_json_check(json,p,n,true);if(error!=GT_PACKAGE_JSON_OK){check_report(report,error==GT_PACKAGE_JSON_MEMORY?GT_PACKAGE_CHECK_MEMORY:error==GT_PACKAGE_JSON_LIMIT?GT_PACKAGE_CHECK_LIMIT:GT_PACKAGE_CHECK_INVALID,error==GT_PACKAGE_JSON_MEMORY?"NATIVE_MEMORY":"FACT_JSON");goto done;}}
 }
 if(!check_budget(&state)){check_exception(&state,report);goto done;}
 {const JSMallocFunctions functions={check_calloc,check_malloc,check_free,check_realloc,check_usable};engine=JS_NewRuntime2(&functions,&state);}if(!engine){check_report(report,GT_PACKAGE_CHECK_MEMORY,"ENGINE_MEMORY");goto done;}
 JS_SetCanBlock(engine,false);JS_SetMaxStackSize(engine,stack);JS_SetInterruptHandler(engine,check_interrupt,&state);c=JS_NewContext(engine);if(!c){check_exception(&state,report);goto done;}
 code=gt__plugin_memory_alloc(&state.allocator,GT_PACKAGE_VALIDATOR_BYTES+1);if(!code){check_report(report,GT_PACKAGE_CHECK_MEMORY,"NATIVE_MEMORY");goto done;}
 {size_t at=0;for(size_t i=0;i<sizeof(package_validator_chunks)/sizeof(package_validator_chunks[0]);i++){size_t n=strlen(package_validator_chunks[i]);if(n>GT_PACKAGE_VALIDATOR_BYTES-at){check_report(report,GT_PACKAGE_CHECK_INTERNAL,"VALIDATOR_SIZE");goto done;}memcpy(code+at,package_validator_chunks[i],n);at+=n;}if(at!=GT_PACKAGE_VALIDATOR_BYTES){check_report(report,GT_PACKAGE_CHECK_INTERNAL,"VALIDATOR_SIZE");goto done;}code[at]=0;}
 api=JS_Eval(c,code,GT_PACKAGE_VALIDATOR_BYTES,"trusted-package-validator",JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_STRICT);if(JS_IsException(api)){check_exception(&state,report);goto done;}fn=JS_GetPropertyStr(c,api,"check");if(!JS_IsFunction(c,fn)){check_exception(&state,report);goto done;}
 args[0]=check_json(&state,c,raw[0],lengths[0]);args[1]=check_json(&state,c,raw[1],lengths[1]);args[2]=JS_NewArray(c);args[3]=config->host_json?check_json(&state,c,config->host_json,config->host_size):JS_NULL;args[4]=config->registry_json?check_json(&state,c,config->registry_json,config->registry_size):JS_NewArray(c);
 for(size_t i=0;i<5;i++)if(JS_IsException(args[i])){check_exception(&state,report);goto done;}
 for(size_t i=0;i<count;i++){
  gt_package_member_t member;if(!gt__package_snapshot_member(*snapshot,i,&member)){check_report(report,GT_PACKAGE_CHECK_INTERNAL,"CATALOG_FAILURE");goto done;}
  JSValue entry=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(entry)){check_exception(&state,report);goto done;}
  if(check_set(c,entry,"path",JS_NewString(c,member.path))<0 || check_set(c,entry,"bytes",JS_NewFloat64(c,(double)member.bytes))<0 || check_set(c,entry,"sha256",JS_NewString(c,member.sha256))<0 || check_set(c,entry,"kind",JS_NewString(c,"regular"))<0){JS_FreeValue(c,entry);check_exception(&state,report);goto done;}
  if(JS_SetPropertyUint32(c,args[2],(uint32_t)i,entry)<0){check_exception(&state,report);goto done;}
 }
 answer=JS_Call(c,fn,api,5,args);if(JS_IsException(answer)){check_exception(&state,report);goto done;}if(!check_budget(&state)){check_exception(&state,report);goto done;}if(state.memory){check_report(report,GT_PACKAGE_CHECK_MEMORY,"ENGINE_MEMORY");goto done;}
 {JSValue ok=JS_GetPropertyStr(c,answer,"ok");int good=JS_ToBool(c,ok);JS_FreeValue(c,ok);if(good!=1){char error[65],kind[32];if(!check_string(c,answer,"code",error,sizeof(error)) || !check_string(c,answer,"state",kind,sizeof(kind))){check_exception(&state,report);goto done;}gt_package_check_status_t status=!strcmp(kind,"invalid")?GT_PACKAGE_CHECK_INVALID:!strcmp(kind,"unknown")?GT_PACKAGE_CHECK_UNKNOWN:!strcmp(kind,"unsupported")?GT_PACKAGE_CHECK_UNSUPPORTED:!strcmp(kind,"denied")?GT_PACKAGE_CHECK_DENIED:GT_PACKAGE_CHECK_INTERNAL;check_report(report,status,error);goto done;}}
 {JSValue commands=JS_GetPropertyStr(c,args[0],"commands"),length=JS_GetPropertyStr(c,commands,"length");uint32_t n=0;bool valid=JS_IsArray(commands) && !JS_ToUint32(c,&n,length) && n>=1 && n<=64;JS_FreeValue(c,length);
  if(!valid){JS_FreeValue(c,commands);check_exception(&state,report);goto done;}
  size_t allocation=sizeof(*checked)+n*sizeof(gt_package_command_t);checked=gt__plugin_memory_alloc(&state.allocator,allocation);
  if(!checked){JS_FreeValue(c,commands);check_report(report,GT_PACKAGE_CHECK_MEMORY,"NATIVE_MEMORY");goto done;}
  memset(checked,0,allocation);checked->allocation_size=allocation;checked->command_count=n;checked->allocator=state.allocator;atomic_init(&checked->references,1);
  for(uint32_t i=0;i<n && valid;i++)valid=check_command(c,commands,i,&checked->commands[i]);JS_FreeValue(c,commands);
  if(!valid){check_exception(&state,report);goto done;}
 }
 {gt_package_check_info_t *info=&checked->info;JSValue identity=JS_GetPropertyStr(c,answer,"identity"),result=JS_GetPropertyStr(c,answer,"result"),association=JS_GetPropertyStr(c,answer,"associationRequired");char side[16]={0},authorization[32]={0};JSValue authenticated=JS_GetPropertyStr(c,result,"publisherAuthenticated");
  bool ok=check_string(c,args[3],"sdkVersion",info->sdk_version,sizeof(info->sdk_version)) && check_string(c,identity,"id",info->id,sizeof(info->id)) && check_string(c,identity,"version",info->version,sizeof(info->version)) && check_string(c,identity,"publisher",info->publisher,sizeof(info->publisher)) && check_string(c,identity,"license",info->license,sizeof(info->license)) && check_string(c,identity,"runtime",info->runtime,sizeof(info->runtime)) && check_string(c,result,"entry",info->entry,sizeof(info->entry)) && check_string(c,result,"side",side,sizeof(side)) && check_string(c,result,"authorization",authorization,sizeof(authorization)) && !strcmp(authorization,"not-granted") && JS_IsBool(authenticated) && JS_ToBool(c,authenticated)==0 && JS_IsBool(association) && check_strings(c,identity,"requires",info->requires,&info->requires_count) && check_strings(c,result,"optionalMissing",info->optional_missing,&info->optional_missing_count) && check_protocols(c,result,info);
  ok=ok && (!strcmp(side,"client") || !strcmp(side,"executor"));info->executor=!strcmp(side,"executor");info->association_required=JS_ToBool(c,association)==1;
  if(info->executor)ok=ok && check_string(c,result,"variantId",info->variant,sizeof(info->variant));
  JS_FreeValue(c,identity);JS_FreeValue(c,result);JS_FreeValue(c,association);JS_FreeValue(c,authenticated);if(!ok){check_exception(&state,report);goto done;}memcpy(info->manifest_digest,metadata[0].sha256,65);memcpy(info->package_digest,metadata[1].sha256,65);
 }
 check_report(report,GT_PACKAGE_CHECK_COMPATIBLE,"COMPATIBLE");
done:
 if(c){for(size_t i=0;i<5;i++)JS_FreeValue(c,args[i]);JS_FreeValue(c,answer);JS_FreeValue(c,fn);JS_FreeValue(c,api);JS_FreeContext(c);}if(engine)JS_FreeRuntime(engine);
 check_wipe_free(&state,code,GT_PACKAGE_VALIDATOR_BYTES+1);gt__package_json_destroy(&json);
 if(report->status==GT_PACKAGE_CHECK_COMPATIBLE && !check_budget(&state))check_exception(&state,report);
 if(report->status==GT_PACKAGE_CHECK_COMPATIBLE){checked->snapshot=*snapshot;*snapshot=NULL;*out=checked;}
 else if(checked)check_wipe_free(&state,checked,checked->allocation_size);
 return report->status;
}
