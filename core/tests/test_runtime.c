#define EXT_RPC_MAIN runtime_rpc_fixture_main
#include "test_ext_rpc.c"
#ifdef GT_RUNTIME_PUBLIC_API
#include "runtime-api-test-alias.h"
#else
#include "extension/runtime.h"
#define GT_RUNTIME_NULL_DESTROY_STATUS GT_ERR_UNSUPPORTED
#endif
#include "base/sha256.h"
#include "extension/memory.h"
static void module(gt_runtime_module_t *m,const char *name,const char *text){m->name=name;m->source=text;m->size=strlen(text);gt_sha256_t hash;gt__sha256_init(&hash);CHECK(gt__sha256_update(&hash,text,m->size) && gt__sha256_final(&hash,m->sha256));}
#ifdef GT_TEST_PLUGIN_JS
static char *read_module(const char *root,const char *name){char path[4096];CHECK(snprintf(path,sizeof(path),"%s/%s",root,name)>0);FILE *f=fopen(path,"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && n<128*1024 && !fseek(f,0,SEEK_SET));char *text=calloc(1,(size_t)n+1);CHECK(text && fread(text,1,(size_t)n,f)==(size_t)n && !fclose(f));return text;}
static gt_runtime_info_t rt_info(gt_bridge_runtime_t *r){gt_runtime_info_t i;CHECK(gt__bridge_runtime_info(r,&i));return i;}
static bool expected_dispose_error;
static void rt_drop(gt_bridge_runtime_t **r){CHECK(gt__bridge_runtime_destroy(r)==GT_ERR_INVALID_ARG && *r);for(unsigned i=0;i<200;i++){gt_ext_close_t c=gt__bridge_runtime_close(*r);CHECK(c==GT_EXT_DRAINING || c==GT_EXT_RELEASED);if(c==GT_EXT_RELEASED){CHECK(gt__bridge_runtime_close(*r)==GT_EXT_RELEASED && rt_info(*r).phase==GT_RUNTIME_CLOSED && rt_info(*r).engine_released);if(expected_dispose_error)CHECK(rt_info(*r).dispose_error && rt_info(*r).error==GT_RUNTIME_NONE);CHECK(gt__bridge_runtime_destroy(r)==GT_OK && !*r);return;}}CHECK(false);}
static void *rt_stop(void *p){gt__bridge_runtime_stop(p);return NULL;}
static const char *echo_plugin=
 "import {object,text,integer} from '@ghosterm/plugin-sdk';"
 "const codec=object({text:text(40),count:integer(0,100)});"
 "export async function activate(ctx){if(ctx.side!=='executor'||!ctx.instanceId||!ctx.epoch||ctx.cancellation.cancelled)throw Error('context');"
 "for(const k of ['process','require','window','document','std','os','fetch','native'])if(k in globalThis)throw Error('ambient');"
 "ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{await Promise.resolve();c.check();return v});return {dispose(){}};}";
static void runtime_case(const char *mode,const char *root){
 gt_runtime_info_t empty;memset(&empty,1,sizeof(empty));CHECK(!gt__bridge_runtime_info(NULL,&empty) && !empty.steps && !empty.jobs && !empty.module_bytes && !empty.engine_released);
 const char *names[]={"index.js","codec.js","session.js","wire-contract.js"};const char *ids[]={"sdk:/index.js","sdk:/codec.js","sdk:/session.js","sdk:/wire-contract.js"};char *scripts[4];gt_runtime_module_t modules[8]={0};
 for(unsigned i=0;i<4;i++){scripts[i]=read_module(root,names[i]);module(&modules[i],ids[i],scripts[i]);}
 const char *plugin=echo_plugin;
 if(!strcmp(mode,"cpu"))plugin="export function activate(){let x=0;for(let i=0;i<20000000;i++)x+=i;return {dispose(){}}}";
 else if(!strcmp(mode,"top-cpu"))plugin="let x=0;for(let i=0;i<20000000;i++)x+=i;export function activate(){return {dispose(){}}}";
 else if(!strcmp(mode,"atomics"))plugin="export function activate(){let refused=false;try{Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0,100)}catch{refused=true}if(!refused)throw Error('blocking wait');return {dispose(){}}}";
 else if(!strcmp(mode,"jobs"))plugin="export function activate(){let count=0;const again=()=>{if(++count<1000)return Promise.resolve().then(again)};again();return {dispose(){}}}";
 else if(!strcmp(mode,"memory"))plugin="export function activate(){globalThis.buffer=new Uint8Array(64*1024*1024);return {dispose(){}}}";
 else if(!strcmp(mode,"caught-memory"))plugin="export function activate(){try{globalThis.buffer=new Uint8Array(64*1024*1024)}catch{}return {dispose(){}}}";
 else if(!strcmp(mode,"global-memory"))plugin="export function activate(){globalThis.buffer=new Uint8Array(1024*1024);return {dispose(){}}}";
 else if(!strcmp(mode,"stack"))plugin="export function activate(){function recur(){return recur()+1}recur();return {dispose(){}}}";
 else if(!strcmp(mode,"reject"))plugin="export async function activate(){throw Error('private payload must not be logged')}";
 else if(!strcmp(mode,"unhandled"))plugin="export function activate(){Promise.reject('private payload');return {dispose(){}}}";
 else if(!strcmp(mode,"handled"))plugin="export function activate(){Promise.reject('handled').catch(()=>{});return {dispose(){}}}";
 else if(!strcmp(mode,"entry"))plugin="export function activate(){return {wrong(){}}}";
 else if(!strcmp(mode,"missing-export"))plugin="export const other=1;";
 else if(!strcmp(mode,"native-import"))plugin="import * as leak from 'host:/native';export function activate(){return {dispose(){}}}";
 else if(!strcmp(mode,"node-import"))plugin="import 'node:fs';export function activate(){return {dispose(){}}}";
 else if(!strcmp(mode,"escape"))plugin="import '../../../escape.js';export function activate(){return {dispose(){}}}";
 else if(!strcmp(mode,"dynamic-import"))plugin="export async function activate(){await import('host:/native');return {dispose(){}}}";
 else if(!strcmp(mode,"dispose-loop"))plugin="export function activate(){return {dispose(){for(;;){}}}}";
 else if(!strcmp(mode,"activation-timeout"))plugin="export async function activate(){await new Promise(()=>{});return {dispose(){}}}";
 else if(!strcmp(mode,"top-await"))plugin="await Promise.resolve();export function activate(){return {dispose(){}}}";
 if(!strcmp(mode,"reverse") || !strcmp(mode,"prototype") || !strcmp(mode,"boundary-outbound"))plugin=
 "import {object,text,integer} from '@ghosterm/plugin-sdk';export async function activate(ctx){"
 "const codec=object({text:text(40),count:integer(0,100)}),call=ctx.session.request({name:'fixture.ask',input:codec,output:codec},{text:'reverse',count:17},1000);"
 "const result=await call.outcome;if(result.kind!=='success'||result.value.text!=='reverse')throw Error('reverse');return {dispose(){}}}";
 char decorated[4096];
 if(!strcmp(mode,"prototype")){CHECK(snprintf(decorated,sizeof(decorated),"Object.defineProperty(Object.prototype,'status',{set(){throw Error('inherited setter')}});%s",plugin)>0);plugin=decorated;}
 module(&modules[4],"plugin:/entry.js",plugin);size_t count=5;
 if(!strcmp(mode,"relative")){module(&modules[4],"plugin:/nested/entry.js","import {value} from '../shared.js';export function activate(){if(value!==42)throw Error('relative');return {dispose(){}}}");module(&modules[5],"plugin:/shared.js","export const value=42;");count=6;}
 rpc_pair_t pair;rpc_open(&pair,"timeout");gt_ext_session_t *borrowed=pair.pair.e;gt_bridge_runtime_t *rt=(gt_bridge_runtime_t *)1;
 gt_runtime_config_t config={.modules=modules,.module_count=count,.entry=modules[4].name,.role=GT_EXT_EXECUTOR,.cpu_ms=(!strcmp(mode,"cpu") || !strcmp(mode,"top-cpu"))?10:500,.jobs_per_step=32,.activation_ms=!strcmp(mode,"activation-timeout")?1:5000};
 if(!strcmp(mode,"invalid")){
  CHECK(gt__bridge_runtime_open(NULL,&config,&rt)==GT_ERR_INVALID_ARG && !rt);config.role=GT_EXT_HOST;CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_ERR_INVALID_ARG && !rt && pair.pair.e);config.role=GT_EXT_EXECUTOR;
  config.memory_bytes=1;CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_ERR_INVALID_ARG && !rt && pair.pair.e);config.memory_bytes=0;
  config.module_count=65;CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_ERR_INVALID_ARG && !rt && pair.pair.e);config.module_count=count;
  strcpy(modules[4].sha256,"invalid");CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_ERR_INVALID_ARG && !rt && pair.pair.e);module(&modules[4],"plugin:/entry.js",plugin);
  modules[4].sha256[0]=modules[4].sha256[0]=='0'?'1':'0';CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_ERR_PROTOCOL && !rt && pair.pair.e);module(&modules[4],"plugin:/entry.js",plugin);
  module(&modules[4],"host:/native",plugin);config.entry="plugin:/missing.js";CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_ERR_INVALID_ARG && !rt && pair.pair.e);rpc_drop(&pair);
 }else{
  size_t total=0;for(size_t i=0;i<count;i++)total+=modules[i].size;
  CHECK(gt__bridge_runtime_open(&pair.pair.e,&config,&rt)==GT_OK && rt && !pair.pair.e);CHECK(rt_info(rt).phase==GT_RUNTIME_CREATED && rt_info(rt).steps==0 && rt_info(rt).module_bytes==total);CHECK(gt__bridge_runtime_destroy(&rt)==GT_ERR_INVALID_ARG && rt);
  void *reservation=NULL;size_t reserved=0;
  if(!strcmp(mode,"global-memory")){gt_plugin_memory_info_t memory=gt__plugin_memory_info();reserved=memory.limit-memory.used-512*1024;reservation=gt__plugin_memory_alloc(NULL,reserved);CHECK(reservation);}
  /* Prove copies, not borrowed caller source or metadata. */
  for(unsigned i=0;i<4;i++){memset(scripts[i],0,modules[i].size);free(scripts[i]);scripts[i]=NULL;}memset(modules,0,sizeof(modules));
  if(!strcmp(mode,"stop")){pthread_t t;CHECK(!pthread_create(&t,NULL,rt_stop,rt) && !pthread_join(t,NULL));CHECK(gt__bridge_runtime_step(rt)==GT_RUNTIME_STOPPED && rt_info(rt).error==GT_RUNTIME_CANCELLED);}
  else{
   unsigned reverse=0;
   for(unsigned i=0;i<20000 && rt_info(rt).phase<GT_RUNTIME_ACTIVE;i++){
    gt_ext_session_step(pair.pair.h,0);gt__bridge_runtime_step(rt);
    if(!strcmp(mode,"reverse") || !strcmp(mode,"prototype")){gt_ext_rpc_request_t q;gt_ext_rpc_status_t status=gt_ext_session_take_request(pair.pair.h,&pair.h,&q);CHECK(status==GT_EXT_RPC_OK || status==GT_EXT_RPC_WAIT);if(status==GT_EXT_RPC_OK){CHECK(!strcmp(q.method,"fixture.ask"));rpc_reply(pair.pair.h,&pair.h,&q);reverse++;}}
   }
   if(!strcmp(mode,"reverse") || !strcmp(mode,"prototype"))CHECK(reverse==1);
   gt_runtime_info_t info=rt_info(rt);
   bool active=!strcmp(mode,"atomics") || !strcmp(mode,"isolation") || !strcmp(mode,"reverse") || !strcmp(mode,"prototype") || !strcmp(mode,"roundtrip") || !strcmp(mode,"relative") || !strcmp(mode,"handled") || !strcmp(mode,"top-await") || !strcmp(mode,"dispose-loop") || !strcmp(mode,"peer-loss");
   CHECK(info.phase==(active?GT_RUNTIME_ACTIVE:GT_RUNTIME_FAILED) && info.steps>0);
   if(!strcmp(mode,"cpu") || !strcmp(mode,"top-cpu"))CHECK(info.error==GT_RUNTIME_CPU);
   if(!strcmp(mode,"memory") || !strcmp(mode,"caught-memory") || !strcmp(mode,"global-memory"))CHECK(info.error==GT_RUNTIME_MEMORY);
   if(!strcmp(mode,"boundary-outbound")){CHECK(info.error==GT_RUNTIME_MEMORY && gt_ext_session_access(borrowed,&pair.e,"ui.form")==GT_EXT_ACCESS_REVOKED);gt_ext_rpc_request_t q;CHECK(gt_ext_session_take_request(pair.pair.h,&pair.h,&q)==GT_EXT_RPC_WAIT);}
   if(!strcmp(mode,"jobs"))CHECK(info.error==GT_RUNTIME_JOBS && info.jobs==config.jobs_per_step);
   if(!strcmp(mode,"activation-timeout"))CHECK(info.error==GT_RUNTIME_EXPIRED);
   if(!strcmp(mode,"roundtrip") || !strcmp(mode,"isolation")){
    rpc_pair_t other={0};gt_bridge_runtime_t *fault=NULL;
    if(!strcmp(mode,"isolation")){
     gt_runtime_module_t isolated[5]={0};char *copies[4];for(unsigned j=0;j<4;j++){copies[j]=read_module(root,names[j]);module(&isolated[j],ids[j],copies[j]);}module(&isolated[4],"plugin:/entry.js","export function activate(){throw Error('isolated failure')}");
     rpc_open(&other,"timeout");gt_runtime_config_t local=config;local.modules=isolated;local.module_count=5;local.entry="plugin:/entry.js";CHECK(gt__bridge_runtime_open(&other.pair.e,&local,&fault)==GT_OK && fault && !other.pair.e);
     for(unsigned j=0;j<4;j++)free(copies[j]);for(unsigned j=0;j<100 && rt_info(fault).phase<GT_RUNTIME_FAILED;j++)gt__bridge_runtime_step(fault);CHECK(rt_info(fault).phase==GT_RUNTIME_FAILED);
    }
    uint64_t h=rpc_start(&pair,false,"{\"text\":\"中文\",\"count\":17}",1000);gt_ext_rpc_result_t result={0};
    for(unsigned i=0;i<2000;i++){gt_ext_session_step(pair.pair.h,0);CHECK(gt__bridge_runtime_step(rt)==GT_RUNTIME_ACTIVE);result=rpc_result(pair.pair.h,&pair.h,h);if(result.confirmed)break;}
    CHECK(result.confirmed && result.state==GT_EXT_RPC_SUCCEEDED && strstr(result.outcome,"中文"));CHECK(gt_ext_session_forget_request(pair.pair.h,&pair.h,h)==GT_EXT_RPC_OK);
    if(fault){CHECK(rt_info(fault).phase==GT_RUNTIME_FAILED && rt_info(rt).phase==GT_RUNTIME_ACTIVE && rt_info(rt).error==GT_RUNTIME_NONE);rt_drop(&fault);ext_drop(&other.pair.h);}
   }
   if(!strcmp(mode,"peer-loss")){ext_drop(&pair.pair.h);for(unsigned i=0;i<100 && gt__bridge_runtime_step(rt)==GT_RUNTIME_ACTIVE;i++){}CHECK(rt_info(rt).phase==GT_RUNTIME_FAILED && rt_info(rt).session_end==GT_EXT_END_LOST);}
  }
  gt__plugin_memory_free(NULL,reservation,reserved);
  expected_dispose_error=!strcmp(mode,"dispose-loop");rt_drop(&rt);if(pair.pair.h)ext_drop(&pair.pair.h);
 }
 for(unsigned i=0;i<4;i++)free(scripts[i]);
}
static void runtime_many(const char *root,unsigned count){
 CHECK(count && count<=10);const char *names[]={"index.js","codec.js","session.js","wire-contract.js"};
 const char *ids[]={"sdk:/index.js","sdk:/codec.js","sdk:/session.js","sdk:/wire-contract.js"};
 char *scripts[4];gt_runtime_module_t modules[5];
 for(unsigned i=0;i<4;i++){scripts[i]=read_module(root,names[i]);module(&modules[i],ids[i],scripts[i]);}
 const char *plugin="import {object,text,integer} from '@ghosterm/plugin-sdk';"
  "const codec=object({text:text(40),count:integer(0,100)});"
  "export async function activate(ctx){if(ctx.side==='executor'){ctx.session.register({name:'fixture.echo',input:codec,output:codec},async v=>v)}"
  "else{const r=await ctx.session.request({name:'fixture.echo',input:codec,output:codec},{text:'parallel',count:17},1000).outcome;"
  "if(r.kind!=='success'||r.value.text!=='parallel')throw Error('parallel outcome')}return {dispose(){}}}";
 module(&modules[4],"plugin:/entry.js",plugin);rpc_pair_t pairs[5];gt_bridge_runtime_t *runtimes[10]={0};unsigned pair_count=(count+1)/2;
 gt_runtime_config_t config={.modules=modules,.module_count=5,.entry="plugin:/entry.js",.role=GT_EXT_EXECUTOR,.cpu_ms=500};
 /* Two runtime roles per socket pair stay within the existing 16-channel cap.
  * Ten runtimes are five real host/executor pairs, not twenty channels. */
 for(unsigned i=0;i<pair_count;i++){
  rpc_open(&pairs[i],"timeout");config.role=GT_EXT_EXECUTOR;CHECK(gt__bridge_runtime_open(&pairs[i].pair.e,&config,&runtimes[i*2])==GT_OK);
  if(i*2+1<count){config.role=GT_EXT_HOST;CHECK(gt__bridge_runtime_open(&pairs[i].pair.h,&config,&runtimes[i*2+1])==GT_OK);}
 }
 for(unsigned tick=0;tick<2000;tick++){
  bool active=true;for(unsigned i=0;i<pair_count;i++)if(pairs[i].pair.h)gt_ext_session_step(pairs[i].pair.h,0);
  for(unsigned i=0;i<count;i++){int phase=gt__bridge_runtime_step(runtimes[i]);CHECK(phase<=GT_RUNTIME_ACTIVE);active=active && phase==GT_RUNTIME_ACTIVE;}
  if(active)break;
 }
 for(unsigned i=0;i<count;i++)CHECK(rt_info(runtimes[i]).phase==GT_RUNTIME_ACTIVE);
 if(count%2){
  rpc_pair_t *last=&pairs[pair_count-1];uint64_t request=rpc_start(last,false,"{\"text\":\"parallel\",\"count\":17}",1000);gt_ext_rpc_result_t result={0};
  for(unsigned tick=0;tick<2000 && !result.confirmed;tick++){gt_ext_session_step(last->pair.h,0);CHECK(gt__bridge_runtime_step(runtimes[count-1])==GT_RUNTIME_ACTIVE);result=rpc_result(last->pair.h,&last->h,request);}
  CHECK(result.confirmed && result.state==GT_EXT_RPC_SUCCEEDED && strstr(result.outcome,"parallel"));CHECK(gt_ext_session_forget_request(last->pair.h,&last->h,request)==GT_EXT_RPC_OK);
 }
 gt_plugin_memory_info_t usage=gt__plugin_memory_info();printf("PROFILE plugin instances=%u current=%zu peak_reserved=%zu limit=%zu bytes\n",count,usage.used,usage.peak,usage.limit);
 for(unsigned i=0;i<count;i++)rt_drop(&runtimes[i]);
 for(unsigned i=0;i<pair_count;i++)if(pairs[i].pair.h)ext_drop(&pairs[i].pair.h);
 for(unsigned i=0;i<4;i++)free(scripts[i]);CHECK(!gt__plugin_memory_info().used);
}
#endif
#ifndef GT_RUNTIME_MAIN
#define GT_RUNTIME_MAIN main
#endif
int GT_RUNTIME_MAIN(int argc,char **argv){CHECK(argc>=2);size_t before=ext_fds();
#ifdef GT_RUNTIME_PUBLIC_API
#ifdef GT_TEST_PLUGIN_JS
 CHECK(gt_ext_runtime_support()==GT_OK);
#else
 CHECK(gt_ext_runtime_support()==GT_ERR_UNSUPPORTED);
#endif
#endif
#ifdef GT_TEST_PLUGIN_JS
 CHECK(argc==3);
 if(!strcmp(argv[1],"repeat-close")){for(unsigned i=0;i<100;i++){runtime_case("roundtrip",argv[2]);CHECK(!gt__plugin_memory_info().used);}}
 else if(!strncmp(argv[1],"many-",5))runtime_many(argv[2],(unsigned)strtoul(argv[1]+5,NULL,10));
 else runtime_case(argv[1],argv[2]);
#else
 (void)module;gt_bridge_runtime_t *rt=(gt_bridge_runtime_t *)1;CHECK(gt__bridge_runtime_open(NULL,NULL,&rt)==GT_ERR_UNSUPPORTED && !rt);gt_runtime_info_t info;memset(&info,1,sizeof(info));CHECK(!gt__bridge_runtime_info(NULL,&info) && !info.steps && !info.module_bytes);CHECK(gt__bridge_runtime_step(NULL)==GT_RUNTIME_CLOSED && gt__bridge_runtime_close(NULL)==GT_EXT_CLOSE_ERROR && gt__bridge_runtime_destroy(NULL)==GT_RUNTIME_NULL_DESTROY_STATUS);gt__bridge_runtime_stop(NULL);
#endif
 CHECK(ext_fds()==before && !gt__plugin_memory_info().used);printf("PASS runtime_%s\n",argv[1]);return 0;}
