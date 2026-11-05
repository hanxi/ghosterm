#define GT_RUNTIME_PUBLIC_API 1
#define GT_RUNTIME_MAIN ext_runtime_fixture_main
#include "test_runtime.c"
static gt_ext_resource_store_t *owner_store;
static gt_ext_session_t *resource_adopt(gt_ext_channel_t **channel,gt_ext_role_t role,gt_ext_session_config_t *config,const gt_ext_rpc_method_t *methods,size_t count){
 gt_ext_session_config_t c=*config;c.features|=GT_EXT_FEATURE_RESOURCES;strcpy(c.required[c.required_count++],"resource.receive");strcpy(c.available[c.available_count++],"resource.receive");gt_ext_session_t *s=NULL;CHECK(gt_ext_session_open_resources(channel,role,&c,methods,count,owner_store,1048576,&s)==GT_OK && s && !*channel);return s;
}
int main(int argc,char **argv){
 CHECK(argc==3 && gt_ext_runtime_support()==GT_OK);size_t before=ext_fds();char directory[4096]={0};
 if(!strcmp(argv[1],"resource-owner")){
  CHECK(snprintf(directory,sizeof(directory),"%s/runtime-store-XXXXXX",argv[2])>0 && mkdtemp(directory));int fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0);gt_ext_resource_store_config_t c={fd,2097152,1048576,8};CHECK(gt_ext_resource_store_open(&c,&owner_store)==GT_EXT_RESOURCE_OK && !close(fd));rpc_adopt_hook=resource_adopt;
 }
 rpc_pair_t pair;rpc_open(&pair,"timeout");rpc_adopt_hook=NULL;
 const char *names[]={"index.js","codec.js","session.js","wire-contract.js"},*ids[]={"sdk:/index.js","sdk:/codec.js","sdk:/session.js","sdk:/wire-contract.js"};char *scripts[4];gt_ext_runtime_module_t modules[5]={0};
 for(unsigned i=0;i<4;i++){scripts[i]=read_module(argv[2],names[i]);module(&modules[i],ids[i],scripts[i]);}module(&modules[4],"plugin:/entry.js",echo_plugin);
 gt_ext_runtime_config_t c={.modules=modules,.module_count=5,.entry="plugin:/entry.js",.role=GT_EXT_EXECUTOR,.cpu_ms=500};gt_ext_runtime_t *runtime=NULL;uint64_t handle=0;
 if(!strcmp(argv[1],"outgoing-owner")){
  handle=rpc_start(&pair,true,"{}",1000);CHECK(gt_ext_runtime_open(&pair.pair.e,&c,&runtime)==GT_ERR_INVALID_ARG && !runtime && pair.pair.e);
  CHECK(gt_ext_session_cancel_request(pair.pair.e,&pair.e,handle)==GT_EXT_RPC_OK && gt_ext_session_forget_request(pair.pair.e,&pair.e,handle)==GT_EXT_RPC_OK);
 }else if(!strcmp(argv[1],"taken-owner")){
  handle=rpc_start(&pair,false,"{}",1000);gt_ext_rpc_request_t q=rpc_take(&pair,false);CHECK(gt_ext_runtime_open(&pair.pair.e,&c,&runtime)==GT_ERR_INVALID_ARG && !runtime && pair.pair.e);
  rpc_reply(pair.pair.e,&pair.e,&q);CHECK(rpc_complete(&pair,false,handle).confirmed);CHECK(gt_ext_session_forget_request(pair.pair.h,&pair.h,handle)==GT_EXT_RPC_OK);
 }else if(!strcmp(argv[1],"untaken-peer")){
  handle=rpc_start(&pair,false,"{\"text\":\"early\",\"count\":17}",1000);for(unsigned i=0;i<40;i++)rpc_pump(&pair);
 }else if(!strcmp(argv[1],"resource-owner")){
  gt_ext_resource_descriptor_t d={.size_known=true};strcpy(d.sha256,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");strcpy(d.mime,"application/octet-stream");strcpy(d.display_name,"file.bin");
  CHECK(gt_ext_session_send_resource(pair.pair.e,&pair.e,&d,&handle)==GT_EXT_RESOURCE_OK);CHECK(gt_ext_runtime_open(&pair.pair.e,&c,&runtime)==GT_ERR_INVALID_ARG && !runtime && pair.pair.e);
  CHECK(gt_ext_session_cancel_resource(pair.pair.e,&pair.e,handle)==GT_EXT_RESOURCE_OK && gt_ext_session_forget_resource(pair.pair.e,&pair.e,handle)==GT_EXT_RESOURCE_OK);
 }else CHECK(false);
 CHECK(gt_ext_runtime_open(&pair.pair.e,&c,&runtime)==GT_OK && runtime && !pair.pair.e);
 if(!strcmp(argv[1],"untaken-peer")){
  gt_ext_rpc_result_t result={0};for(unsigned i=0;i<2000;i++){gt_ext_session_step(pair.pair.h,0);gt_ext_runtime_step(runtime);result=rpc_result(pair.pair.h,&pair.h,handle);if(result.confirmed)break;}
  CHECK(result.confirmed && result.state==GT_EXT_RPC_SUCCEEDED && strstr(result.outcome,"early"));CHECK(gt_ext_session_forget_request(pair.pair.h,&pair.h,handle)==GT_EXT_RPC_OK);
 }
 if(owner_store){gt_ext_resource_usage_t usage;CHECK(gt_ext_resource_store_usage(owner_store,&usage) && usage.sessions==2);CHECK(gt_ext_resource_store_destroy(&owner_store)==GT_EXT_RESOURCE_WAIT && owner_store);}
 rt_drop(&runtime);ext_drop(&pair.pair.h);for(unsigned i=0;i<4;i++)free(scripts[i]);
 if(owner_store){CHECK(gt_ext_resource_store_destroy(&owner_store)==GT_EXT_RESOURCE_OK && !owner_store);CHECK(!rmdir(directory));}
 CHECK(ext_fds()==before);printf("PASS ext_runtime_%s\n",argv[1]);return 0;
}
