#define _POSIX_C_SOURCE 200809L
#include <quickjs.h>
/* The owning C request has already been admitted before this exact native-to-JS
 * handoff allocation. Real QuickJS OOM exception; no fake protocol outcome. */
static JSValue runtime_object_oom(JSContext *ctx,JSValueConst proto);
#define JS_NewObjectProto runtime_object_oom
#include "../src/extension/runtime.c"
#undef JS_NewObjectProto
static JSValue runtime_object_oom(JSContext *ctx,JSValueConst proto){
 (void)proto;gt_bridge_runtime_t *owner=JS_GetContextOpaque(ctx);gt_ext_rpc_result_t result;
 /* Fixture's first request is actually queued, before the owning handoff fails. */
 if(!owner || gt_ext_session_request_result(owner->session,&owner->lease,1,&result)!=GT_EXT_RPC_OK || result.state!=GT_EXT_RPC_QUEUED){fprintf(stderr,"FAIL OOM boundary did not observe queued native request\n");exit(1);}
 return JS_ThrowOutOfMemory(ctx);
}
#define GT_RUNTIME_MAIN runtime_boundary_fixture_main
#include "test_runtime.c"
int main(int argc,char **argv){CHECK(argc==2);size_t before=ext_fds();runtime_case("boundary-outbound",argv[1]);CHECK(ext_fds()==before);puts("PASS runtime_boundary_outbound");return 0;}
