#include "extension/memory.h"
#define main ext_session_fixture_main
#include "test_ext_session.c"
#undef main
#include "extension/session.h"
#include "extension/channel-owner.h"
static bool facade_allocation_failure,facade_open_failure,facade_close_error,facade_draining,facade_destroy_error;
static void *facade_calloc(size_t count,size_t size){if(facade_allocation_failure)return NULL;return gt__plugin_memory_calloc(count,size);}
static gt_status_t facade_open(gt_bridge_stream_t **p,gt_bridge_role_t role,const gt_bridge_session_config_t *c,const gt_allocator_t *a,gt_bridge_session_t **s){if(facade_open_failure){*s=NULL;return GT_ERR_IO;}return gt__bridge_session_open(p,role,c,a,s);}
static gt_bridge_close_t facade_close(gt_bridge_session_t *s,int ms){if(facade_close_error)return GT_BRIDGE_CLOSE_ERROR;if(facade_draining)return GT_BRIDGE_DRAINING;return gt__bridge_session_close(s,ms);}
static gt_status_t facade_destroy(gt_bridge_session_t *s){return facade_destroy_error?GT_ERR_IO:gt__bridge_session_destroy(s);}
/* Actual facade; inject only allocator and private provider failure boundaries. */
#define gt__plugin_memory_calloc facade_calloc
#define gt__bridge_session_open facade_open
#define gt__bridge_session_close facade_close
#define gt__bridge_session_destroy facade_destroy
#include "../src/extension/session-api.c"
#undef gt__plugin_memory_calloc
#undef gt__bridge_session_open
#undef gt__bridge_session_close
#undef gt__bridge_session_destroy
int main(int argc,char **argv){
 CHECK(argc==2);size_t before=ext_fds();int f[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,f));gt_ext_channel_t *c=ext_channel(f[0]);gt_ext_session_config_t config=ext_config();gt_ext_session_t *s=NULL;
 if(!strcmp(argv[1],"allocation") || !strcmp(argv[1],"inner-open")){
  facade_allocation_failure=!strcmp(argv[1],"allocation");facade_open_failure=!strcmp(argv[1],"inner-open");
  CHECK(gt_ext_session_open(&c,GT_EXT_HOST,&config,&s)==GT_ERR_IO && !s && c);gt_ext_channel_info_t i;CHECK(gt_ext_channel_info(c,&i) && i.phase==GT_EXT_READY && !i.accepted && !i.received);facade_allocation_failure=facade_open_failure=false;
 }
 s=ext_adopt(&c,GT_EXT_HOST,&config);CHECK(gt_ext_session_destroy(&s)==GT_ERR_INVALID_ARG && s);
 if(!strcmp(argv[1],"close-error")){
  facade_close_error=true;CHECK(gt_ext_session_close(s,0)==GT_EXT_CLOSE_ERROR && ext_info(s).cleanup_error==GT_ERR_IO && ext_info(s).end==GT_EXT_END_NONE);
  CHECK(gt_ext_session_destroy(&s)==GT_ERR_INVALID_ARG && s);facade_close_error=false;
 }else if(!strcmp(argv[1],"draining")){
  facade_draining=true;CHECK(gt_ext_session_close(s,0)==GT_EXT_DRAINING && !ext_info(s).cleanup_error && gt_ext_session_destroy(&s)==GT_ERR_INVALID_ARG && s);facade_draining=false;
 }else if(!strcmp(argv[1],"destroy-error")){
  CHECK(gt_ext_session_close(s,0)==GT_EXT_RELEASED);facade_destroy_error=true;CHECK(gt_ext_session_destroy(&s)==GT_ERR_IO && s && ext_info(s).cleanup_error==GT_ERR_IO);
  CHECK(gt_ext_session_close(s,0)==GT_EXT_RELEASED && ext_info(s).cleanup_error==GT_ERR_IO);facade_destroy_error=false;
 }else CHECK(!strcmp(argv[1],"allocation") || !strcmp(argv[1],"inner-open"));
 ext_drop(&s);CHECK(!close(f[1]));ext_native("capacity");CHECK(ext_fds()==before);printf("PASS ext_session_boundary_%s\n",argv[1]);return 0;
}
