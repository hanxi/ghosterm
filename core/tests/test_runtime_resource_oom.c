/* Actual snapshot has been taken into the native owner before QuickJS refuses
 * its descriptor POD. No fake malloc/success; close must release the real pin. */
#define _DARWIN_C_SOURCE 1
#include "../src/extension/runtime-resource.c"
#include <quickjs.h>
static JSValue resource_take_oom(JSContext *, JSValueConst);
#define JS_NewObjectProto resource_take_oom
#include "../src/extension/runtime.c"
#undef JS_NewObjectProto
static bool resource_oom_observed;

static JSValue resource_take_oom(JSContext *c, JSValueConst proto) {
    gt_bridge_runtime_t *r = JS_GetContextOpaque(c);
    if (r && r->resources && !resource_oom_observed) {
        for (size_t i = 0; i < GT_RUNTIME_PINS_MAX; i++) {
            if (r->resources->pins[i].id) {
                resource_oom_observed = true;
                return JS_ThrowOutOfMemory(c);
            }
        }
    }
    return JS_NewObjectProto(c, proto);
}

#define RESOURCE_JS_MAIN resource_oom_fixture_main
#include "test_runtime_resource_js.c"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    resource_js_expect_oom = true;
    char *args[] = {argv[0], "read-oom", argv[1]};
    CHECK(!resource_oom_fixture_main(3, args) && resource_oom_observed);
    puts("PASS resource_actual_snapshot_oom_cleanup");
    return 0;
}
