#define PACKAGE_RUNTIME_MAIN package_loader_fixture_main
#include "test_package_runtime.c"
#undef PACKAGE_RUNTIME_MAIN
int main(int argc,char **argv){
 CHECK(argc>=2);const char *mode=argv[1];gt_ext_package_t *package=NULL;
 if(!strcmp(mode,"disabled")){gt_ext_registry_t *registry=NULL;CHECK(gt_ext_registry_open(&registry)==GT_EXT_REGISTRY_UNSUPPORTED && !registry);CHECK(gt_ext_package_open(NULL,&package)==GT_ERR_UNSUPPORTED && !package);gt_ext_runtime_t *r=NULL;CHECK(gt_ext_runtime_open_package(NULL,NULL,NULL,false,&r)==GT_ERR_UNSUPPORTED && !r);return 0;}
 CHECK(argc==3);size_t fds=ext_fds();gt_plugin_memory_info_t before=gt__plugin_memory_info();load_fixture_t f;load_fixture(&f,"roundtrip",argv[2]);
 gt_ext_package_config_t capture={f.fd,1024,4096,16777216,67108864};CHECK(gt_ext_package_open(&capture,&package)==GT_OK && package);
 gt_ext_package_metadata_t metadata;memset(&metadata,0x55,sizeof(metadata));CHECK(!gt_ext_package_metadata(package,&metadata) && !metadata.id[0]);
 if(!strcmp(mode,"capture-stop"))gt_ext_package_stop(package);
 gt_ext_package_phase_t phase=GT_EXT_PACKAGE_CAPTURING;for(unsigned i=0;i<50000 && phase==GT_EXT_PACKAGE_CAPTURING;i++)phase=gt_ext_package_step(package);
 gt_ext_package_info_t info;CHECK(gt_ext_package_info(package,&info));
 if(!strcmp(mode,"capture-stop")){CHECK(phase==GT_EXT_PACKAGE_FAILED && info.error==GT_EXT_PACKAGE_CANCELLED);goto done;}
 CHECK(phase==GT_EXT_PACKAGE_CAPTURED && info.members>0 && info.bytes>0);
 if(!strcmp(mode,"check-stop"))gt_ext_package_stop(package);
 const char *host="{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],\"runtimes\":[\"portable-js\"],\"capabilities\":[\"session.rpc\"]}";
 gt_ext_package_check_config_t config={.host_json=!strcmp(mode,"unknown")?NULL:host};
 phase=gt_ext_package_check(package,&config);CHECK(gt_ext_package_info(package,&info));
 if(!strcmp(mode,"unknown")){CHECK(phase==GT_EXT_PACKAGE_FAILED && info.error==GT_EXT_PACKAGE_UNKNOWN && info.code[0]);goto done;}
 if(!strcmp(mode,"check-stop")){CHECK(phase==GT_EXT_PACKAGE_FAILED && info.error==GT_EXT_PACKAGE_CANCELLED);goto done;}
 CHECK(phase==GT_EXT_PACKAGE_CHECKED && gt_ext_package_metadata(package,&metadata));
 CHECK(!strcmp(metadata.id,f.info.id) && !strcmp(metadata.package_digest,f.info.package_digest) && metadata.requires_count==1 && !metadata.authorization_granted && !metadata.publisher_authenticated);
 if(!strcmp(mode,"registry")){
  uint32_t count=0;gt_ext_package_command_t command;CHECK(gt_ext_package_command_count(package,&count) && count==1);
  CHECK(gt_ext_package_command(package,0,&command) && !strcmp(command.path,"fixture") && !strcmp((char *)command.args_schema,"schemas/input.json"));
  memset(&command,0x55,sizeof(command));CHECK(!gt_ext_package_command(package,1,&command) && !command.path[0]);
  gt_ext_registry_t *registry=NULL;CHECK(gt_ext_registry_open(&registry)==GT_EXT_REGISTRY_OK);
  CHECK(gt_ext_registry_put(registry,package,GT_EXT_REGISTRY_SOURCE_UNKNOWN,NULL)==GT_EXT_REGISTRY_UNKNOWN);
  CHECK(gt_ext_registry_put(registry,package,GT_EXT_REGISTRY_SOURCE_DENIED,NULL)==GT_EXT_REGISTRY_DENIED);
  CHECK(gt_ext_registry_put(registry,package,GT_EXT_REGISTRY_SOURCE_ALLOWED,NULL)==GT_EXT_REGISTRY_OK);
  gt_ext_package_stop(package);CHECK(gt_ext_registry_put(registry,package,GT_EXT_REGISTRY_SOURCE_ALLOWED,NULL)==GT_EXT_REGISTRY_CANCELLED);
  gt_ext_package_t *selected=NULL;CHECK(gt_ext_registry_resolve(registry,"fixture",true,metadata.id,metadata.package_digest,&selected,&command)==GT_EXT_REGISTRY_OK);
  gt_ext_package_info_t selected_info;CHECK(gt_ext_package_info(selected,&selected_info) && selected_info.members==info.members && selected_info.bytes==info.bytes);
  CHECK(gt_ext_registry_remove(registry,metadata.id,true,metadata.package_digest)==GT_EXT_REGISTRY_OK);
  CHECK(gt_ext_registry_destroy(&registry)==GT_EXT_REGISTRY_OK && !registry);
  CHECK(gt_ext_package_destroy(&package)==GT_OK);package=selected;
 }
 rpc_pair_t pair;load_pair(&pair,&f,!strcmp(mode,"mismatch")?"digest":"roundtrip");gt_ext_session_t *original=pair.pair.e;
 gt_ext_runtime_config_t runtime={.role=GT_EXT_EXECUTOR,.cpu_ms=1000,.activation_ms=10000,.jobs_per_step=128};gt_ext_runtime_t *r=NULL;
 if(!strcmp(mode,"override"))runtime.entry="plugin:/override.js";
 if(!strcmp(mode,"load-stop"))gt_ext_package_stop(package);
 gt_status_t status=gt_ext_runtime_open_package(&pair.pair.e,package,&runtime,false,&r);
 if(!strcmp(mode,"override") || !strcmp(mode,"load-stop") || !strcmp(mode,"mismatch")){
  CHECK(status==(!strcmp(mode,"override")?GT_ERR_INVALID_ARG:!strcmp(mode,"load-stop")?GT_ERR_CANCELLED:GT_ERR_PROTOCOL) && !r && pair.pair.e==original);rpc_drop(&pair);goto done;
 }
 CHECK(status==GT_OK && r && !pair.pair.e);CHECK(gt_ext_package_destroy(&package)==GT_OK && !package);
 gt_ext_runtime_info_t ri;for(unsigned i=0;i<20000;i++){gt_ext_session_step(pair.pair.h,0);gt_ext_runtime_step(r);CHECK(gt_ext_runtime_info(r,&ri));if(ri.phase>=GT_EXT_RUNTIME_ACTIVE)break;}
 CHECK(ri.phase==GT_EXT_RUNTIME_ACTIVE);
 uint64_t request=rpc_start(&pair,false,"{\"text\":\"public\",\"count\":17}",1000);gt_ext_rpc_result_t result={0};
 for(unsigned i=0;i<2000;i++){gt_ext_session_step(pair.pair.h,0);CHECK(gt_ext_runtime_step(r)==GT_EXT_RUNTIME_ACTIVE);result=rpc_result(pair.pair.h,&pair.h,request);if(result.confirmed)break;}
 CHECK(result.confirmed && result.state==GT_EXT_RPC_SUCCEEDED && strstr(result.outcome,"public"));CHECK(gt_ext_session_forget_request(pair.pair.h,&pair.h,request)==GT_EXT_RPC_OK);
 gt_ext_close_t closed=GT_EXT_DRAINING;for(unsigned i=0;i<200 && closed==GT_EXT_DRAINING;i++)closed=gt_ext_runtime_close(r);CHECK(closed==GT_EXT_RELEASED && gt_ext_runtime_destroy(&r)==GT_OK && !r);ext_drop(&pair.pair.h);
done:
 if(package)CHECK(gt_ext_package_destroy(&package)==GT_OK && !package);load_clear(&f);CHECK(ext_fds()==fds && gt__plugin_memory_info().used==before.used);return 0;
}
