#define EXT_SOURCE_MAIN runtime_resource_js_source_fixture_main
#include "test_ext_source.c"
#include "base/sha256.h"
#include "ghosterm/extension_runtime.h"
static bool resource_js_expect_oom;
static char *resource_js_module(const char *root,const char *name){char path[4096];CHECK(snprintf(path,sizeof(path),"%s/%s",root,name)>0);FILE *f=fopen(path,"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && n<131072 && !fseek(f,0,SEEK_SET));char *s=calloc(1,(size_t)n+1);CHECK(s && fread(s,1,(size_t)n,f)==(size_t)n && !fclose(f));return s;}
static void resource_js_digest(gt_ext_runtime_module_t *m,const char *id,const char *text){*m=(gt_ext_runtime_module_t){.name=id,.source=text,.size=strlen(text)};gt_sha256_t h;gt__sha256_init(&h);CHECK(gt__sha256_update(&h,text,m->size) && gt__sha256_final(&h,m->sha256));}
static void resource_js_close(gt_ext_runtime_t **r){for(unsigned i=0;i<200;i++){gt_ext_close_t c=gt_ext_runtime_close(*r);CHECK(c==GT_EXT_DRAINING || c==GT_EXT_RELEASED);if(c==GT_EXT_RELEASED){CHECK(gt_ext_runtime_destroy(r)==GT_OK && !*r);return;}}CHECK(false);}
static gt_ext_runtime_info_t resource_js_info(gt_ext_runtime_t *r){gt_ext_runtime_info_t v;CHECK(gt_ext_runtime_info(r,&v));return v;}
static void resource_js_pump(pub_source_fixture_t *f,gt_ext_runtime_t *r){gt_ext_session_step(f->pair.rpc.pair.h,0);gt_ext_runtime_phase_t p=gt_ext_runtime_step(r);CHECK(p<GT_EXT_RUNTIME_FAILED || (resource_js_expect_oom && p==GT_EXT_RUNTIME_FAILED));}
static void resource_js_receive(pub_source_fixture_t *f,gt_ext_runtime_t *r,size_t size){
 int fd=open(f->directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0);gt_ext_source_config_t cfg={fd,"file.bin","application/octet-stream",262144};gt_ext_source_t *s=NULL;CHECK(gt_ext_source_open(f->pair.rpc.pair.h,&f->pair.rpc.h,&cfg,&s)==GT_EXT_SOURCE_OK && !close(fd));
 for(unsigned i=0;i<10000;i++){resource_js_pump(f,r);gt_ext_source_phase_t p=gt_ext_source_step(s);if(p==GT_EXT_SOURCE_READY)CHECK(gt_ext_source_send(s)==GT_EXT_SOURCE_OK);if(p==GT_EXT_SOURCE_SUCCEEDED)break;}
 CHECK(pub_info(s).phase==GT_EXT_SOURCE_SUCCEEDED && pub_info(s).confirmed && pub_info(s).accepted==size);pub_drop(&s);
}
#ifndef RESOURCE_JS_MAIN
#define RESOURCE_JS_MAIN main
#endif
int RESOURCE_JS_MAIN(int argc,char **argv){
 CHECK(argc==3);res_root=argv[2];const char *mode=argv[1];size_t before=ext_fds(),size=!strcmp(mode,"large") || !strcmp(mode,"read-large") || !strcmp(mode,"close-live")?262144:!strcmp(mode,"empty") || !strcmp(mode,"read-empty")?0:3;
 pub_source_fixture_t f={.root=-1};pub_pair(&f.pair,!strcmp(mode,"ungranted")?"ungranted":"roundtrip");pub_file(&f,size,"file.bin");
 const char *names[]={"index.js","codec.js","session.js","wire-contract.js","resources.js"};char *text[5];char ids[5][64];gt_ext_runtime_module_t modules[6]={0};
 for(unsigned i=0;i<5;i++){text[i]=resource_js_module(argv[2],names[i]);CHECK(snprintf(ids[i],sizeof(ids[i]),"sdk:/%s",names[i])>0);resource_js_digest(&modules[i],ids[i],text[i]);}
 bool receive=!strcmp(mode,"read") || !strcmp(mode,"read-large") || !strcmp(mode,"read-empty") || !strcmp(mode,"leak-ref") || !strcmp(mode,"read-oom");
 char plugin[4096];int n=snprintf(plugin,sizeof(plugin),
 "import {object,text,integer} from '@ghosterm/plugin-sdk';export async function activate(ctx){"
 "if(!ctx.resources||ctx.resources.workspaces.join(',')!=='workspace')throw Error('workspace');"
 "const codec=object({text:text(40),count:integer(0,100)});%s"
 "ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{%sreturn {text:'checked',count:17}});return {dispose(){}}}",
 receive?"":"const call=ctx.resources.publish({root:'workspace',relativePath:'file.bin',mediaType:'application/octet-stream'},{byteLimit:'262144',timeoutMs:10000});",
 receive?"const t=ctx.resources.take();if(t.status!=='ok')throw Error('take');if(ctx.resources.open({...t.ref},{byteLimit:'262144',timeoutMs:10000}).status!=='invalid')throw Error('forged');const o=ctx.resources.open(t.ref,{byteLimit:'262144',timeoutMs:10000});if(o.status!=='ok')throw Error('open');let at=0;for(;;){const chunk=await o.stream.read();c.check();if(chunk.kind!=='success')throw Error('read');for(const b of chunk.value.bytes){if(b!==(t.ref.size.bytes==='3'?'abc'.charCodeAt(at):at%251))throw Error('byte');at++}if(chunk.value.done)break}if(String(at)!==t.ref.size.bytes)throw Error('length');if(ctx.resources.release(t.ref)!=='ok')throw Error('release');":"");
 CHECK(n>0 && (size_t)n<sizeof(plugin));
 if(!strncmp(mode,"adapter-",8)){
  /* Fault adapter observations exercise SDK rejection paths. These are NOT
   * peer receipts or disk IO failures. The ordinary native publication below
   * still runs independently once these fault assertions have passed. */
  const char *common="const d={id:'a'.repeat(32),generation:'1',sha256:'b'.repeat(64),displayName:'fixture',mediaType:'application/octet-stream',sizeKnown:true,bytes:'3'};let reads=0;const p={resourceWorkspaces(){return ['workspace']},resourcePublish(){return {status:'ok',handle:'1'}},resourcePublication(){return {phase:'succeeded',error:'ok',expired:false,clockError:false,hasTransfer:true,confirmed:false,transferState:'succeeded',descriptor:d,readBytes:'3',accepted:'3'}},resourceForget(){return 'ok'},resourceCancel(){return 'ok'},resourceTake(){return {status:'ok',handle:'1',descriptor:d}},resourceOpen(){return 'ok'},resourceRead(){reads++;return {status:FAULT,bytes:new Uint8Array()}},resourceRelease(){return 'ok'}};const b=new AdapterBroker(p);";
  const char *checks=!strcmp(mode,"adapter-unconfirmed")?"const a=b.publish({root:'workspace',relativePath:'file.bin',mediaType:'application/octet-stream'},{byteLimit:'3',timeoutMs:1000});b.step();const v=await a.outcome;if(v.kind!=='unknown'||v.reason!=='INVALID_RESOURCE_RECEIPT')throw Error('unconfirmed');b.dispose();":"const ref=b.take().ref,o=b.open(ref,{byteLimit:'3',timeoutMs:1000}),first=o.stream.read();b.step();const a=await first;CHECKS;b.release(ref);b.dispose();";
  char fault[3000];CHECK(snprintf(fault,sizeof(fault),"%s%s",common,checks)>0);char *mark=strstr(fault,"FAULT");CHECK(mark);const char *response=!strcmp(mode,"adapter-read-error")?"reads===1?'io':'ok'":"'ok'";char tail[3000];strcpy(tail,mark+5);CHECK((size_t)(mark-fault)+strlen(response)+strlen(tail)<sizeof(fault));strcpy(mark,response);strcat(mark,tail);
  mark=strstr(fault,"CHECKS");if(mark){strcpy(tail,mark+6);const char *check=!strcmp(mode,"adapter-read-error")?"if(a.kind!=='failed'||a.error.code!=='IO')throw Error('io');const second=o.stream.read();b.step();const v=await second;if(v.kind!=='failed'||v.error.code!=='IO'||reads!==1)throw Error('sticky');":"if(a.kind!=='failed'||a.error.code!=='PROTOCOL')throw Error('truncated');";CHECK((size_t)(mark-fault)+strlen(check)+strlen(tail)<sizeof(fault));strcpy(mark,check);strcat(mark,tail);}
  char original[4096];strcpy(original,plugin);const char *body=strstr(original,"activate(ctx){");CHECK(body);body+=strlen("activate(ctx){");size_t prefix=(size_t)(body-original);CHECK(snprintf(plugin,sizeof(plugin),"import {ResourceBroker as AdapterBroker} from 'sdk:/resources.js';%.*s%s%s",(int)prefix,original,fault,body)>0 && strlen(plugin)<sizeof(plugin)-1);
 }
 /* Inject assertions before handler registration; activate remains asynchronous
  * while native Source/Session proceed, then only a real peer receipt succeeds. */
 if(!receive){char *at=strstr(plugin,"ctx.session.register");CHECK(at);char suffix[2048];strcpy(suffix,at);const char *wait=!strcmp(mode,"ungranted")?"const result=await call.outcome;if(result.kind!=='failed'||result.error.code!=='UNSUPPORTED')throw Error('grant');":!strcmp(mode,"cancel")?"call.cancel();const result=await call.outcome;if(result.kind!=='cancelled'||result.effect!=='none')throw Error('cancel');":!strcmp(mode,"close-live")?"await new Promise(()=>{});":"const result=await call.outcome;if(result.kind!=='success'||!result.value.id||result.value.size.kind!=='known')throw Error('receipt');";CHECK(strlen(plugin)+strlen(wait)<sizeof(plugin));strcpy(at,wait);strcat(at,suffix);}
 if(!strcmp(mode,"leak-ref")){char *at=strstr(plugin,"if(ctx.resources.release(t.ref)");CHECK(at);char *end=strstr(at,"return {text:");CHECK(end);memmove(at,end,strlen(end)+1);}
 resource_js_digest(&modules[5],"plugin:/entry.js",plugin);char root_id[65]="workspace";gt_ext_runtime_root_t root={root_id,f.root,262144};gt_ext_runtime_config_t cfg={.modules=modules,.module_count=6,.entry="plugin:/entry.js",.role=GT_EXT_EXECUTOR,.cpu_ms=500,.activation_ms=10000,.roots=&root,.root_count=1};gt_ext_runtime_t *runtime=NULL;
 if(!strcmp(mode,"invalid-root")){
  root.directory_fd=-1;CHECK(gt_ext_runtime_open(&f.pair.rpc.pair.e,&cfg,&runtime)==GT_ERR_INVALID_ARG && !runtime && f.pair.rpc.pair.e);root.directory_fd=f.root;
  root.byte_limit=16777217;CHECK(gt_ext_runtime_open(&f.pair.rpc.pair.e,&cfg,&runtime)==GT_ERR_INVALID_ARG && !runtime && f.pair.rpc.pair.e);root.byte_limit=262144;
  cfg.root_count=9;CHECK(gt_ext_runtime_open(&f.pair.rpc.pair.e,&cfg,&runtime)==GT_ERR_INVALID_ARG && !runtime && f.pair.rpc.pair.e);cfg.root_count=1;
  strcpy(root_id,"bad/root");CHECK(gt_ext_runtime_open(&f.pair.rpc.pair.e,&cfg,&runtime)==GT_ERR_INVALID_ARG && !runtime && f.pair.rpc.pair.e);for(unsigned i=0;i<5;i++)free(text[i]);pub_fixture_drop(&f);CHECK(ext_fds()==before);puts("PASS runtime_resource_js_invalid-root");return 0;
 }
 CHECK(gt_ext_runtime_open(&f.pair.rpc.pair.e,&cfg,&runtime)==GT_OK && runtime && !f.pair.rpc.pair.e);memset(root_id,0,sizeof(root_id));CHECK(!close(f.root));f.root=-1;
 if(!strcmp(mode,"close-live")){for(unsigned i=0;i<10;i++)resource_js_pump(&f,runtime);CHECK(resource_js_info(runtime).phase==GT_EXT_RUNTIME_STARTING);}
 else{
  for(unsigned i=0;i<10000 && resource_js_info(runtime).phase!=GT_EXT_RUNTIME_ACTIVE;i++)resource_js_pump(&f,runtime);CHECK(resource_js_info(runtime).phase==GT_EXT_RUNTIME_ACTIVE);
  if(receive)resource_js_receive(&f,runtime,size);
  uint64_t request=rpc_start(&f.pair.rpc,false,"{\"text\":\"check\",\"count\":17}",10000);gt_ext_rpc_result_t result={0};for(unsigned i=0;i<10000;i++){resource_js_pump(&f,runtime);if(resource_js_expect_oom && resource_js_info(runtime).phase==GT_EXT_RUNTIME_FAILED)break;result=rpc_result(f.pair.rpc.pair.h,&f.pair.rpc.h,request);if(result.confirmed)break;}
  if(resource_js_expect_oom){CHECK(resource_js_info(runtime).phase==GT_EXT_RUNTIME_FAILED && resource_js_info(runtime).error==GT_EXT_RUNTIME_MEMORY);}
  else{CHECK(result.confirmed && result.state==GT_EXT_RPC_SUCCEEDED && strstr(result.outcome,"checked"));CHECK(gt_ext_session_forget_request(f.pair.rpc.pair.h,&f.pair.rpc.h,request)==GT_EXT_RPC_OK);}
  if(receive){CHECK(res_usage(&f.pair.e).resources==(!strcmp(mode,"leak-ref") || resource_js_expect_oom?1:0));}
  else if(strcmp(mode,"cancel") && strcmp(mode,"ungranted")){gt_ext_resource_snapshot_t ref;CHECK(gt_ext_session_take_resource(f.pair.rpc.pair.h,&f.pair.rpc.h,&ref)==GT_EXT_RESOURCE_OK);pub_verify(&f.pair.h,&f.pair.rpc.h,&ref,size,"file.bin");CHECK(gt_ext_resource_store_release(f.pair.h.s,&f.pair.rpc.h,&ref)==GT_EXT_RESOURCE_OK);}
 }
 resource_js_close(&runtime);CHECK(!res_usage(&f.pair.e).resources);for(unsigned i=0;i<5;i++)free(text[i]);
 /* The executor Session was consumed by runtime; close only the host still
  * owned by this fixture. Keep exact Store/fd assertions for both sides. */
 ext_drop(&f.pair.rpc.pair.h);res_store_drop(&f.pair.h);res_store_drop(&f.pair.e);CHECK(!unlink(f.file) && !rmdir(f.directory));CHECK(ext_fds()==before);printf("PASS runtime_resource_js_%s\n",mode);return 0;
}
