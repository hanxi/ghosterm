#define _POSIX_C_SOURCE 200809L
#include <quickjs.h>
static JSValue ext_runtime_object_oom(JSContext *, JSValueConst);
#define JS_NewObjectProto ext_runtime_object_oom
#include "../src/extension/runtime.c"
#undef JS_NewObjectProto
#include "../src/extension/runtime-api.c"

static JSValue ext_runtime_object_oom(JSContext *ctx, JSValueConst proto) {
    (void)proto;
    gt_bridge_runtime_t *owner = JS_GetContextOpaque(ctx);
    gt_ext_rpc_result_t result;
    if (!owner ||
        gt_ext_session_request_result(owner->session, &owner->lease, 1, &result) != GT_EXT_RPC_OK ||
        result.state != GT_EXT_RPC_QUEUED) {
        fprintf(stderr, "FAIL public OOM boundary did not observe queued request\n");
        exit(1);
    }
    return JS_ThrowOutOfMemory(ctx);
}

#define GT_RUNTIME_PUBLIC_API 1
#define GT_RUNTIME_MAIN ext_runtime_boundary_fixture_main
#include "test_runtime.c"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    size_t before = ext_fds();
    runtime_case("boundary-outbound", argv[1]);
    CHECK(ext_fds() == before);
    puts("PASS ext_runtime_boundary_outbound");
    return 0;
}
