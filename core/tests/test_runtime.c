#define EXT_RPC_MAIN runtime_rpc_fixture_main
#include "test_ext_rpc.c"
#ifdef GT_RUNTIME_PUBLIC_API
#include "runtime-api-test-alias.h"
#else
#include "extension/runtime.h"
#define GT_RUNTIME_NULL_DESTROY_STATUS GT_ERR_UNSUPPORTED
#endif
#include "extension/memory.h"

int main(int argc, char **argv) {
    CHECK(argc == 2 && !strcmp(argv[1], "disabled"));
    size_t before = ext_fds();
#ifdef GT_RUNTIME_PUBLIC_API
    CHECK(gt_ext_runtime_support() == GT_ERR_UNSUPPORTED);
#endif
    gt_bridge_runtime_t *rt = (gt_bridge_runtime_t *)1;
    CHECK(gt__bridge_runtime_open(NULL, NULL, &rt) == GT_ERR_UNSUPPORTED && !rt);
    gt_runtime_info_t info;
    memset(&info, 1, sizeof(info));
    CHECK(!gt__bridge_runtime_info(NULL, &info) && !info.steps && !info.module_bytes);
    CHECK(gt__bridge_runtime_step(NULL) == GT_RUNTIME_CLOSED &&
          gt__bridge_runtime_close(NULL) == GT_EXT_CLOSE_ERROR &&
          gt__bridge_runtime_destroy(NULL) == GT_RUNTIME_NULL_DESTROY_STATUS);
    gt__bridge_runtime_stop(NULL);
    CHECK(ext_fds() == before && !gt__plugin_memory_info().used);
    printf("PASS runtime_%s\n", argv[1]);
    return 0;
}
