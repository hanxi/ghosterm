/* Production checker with a deterministic finite JS workload at the Call
 * boundary. Fails normally if interrupts are bypassed; no timeout counts red. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <quickjs.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "extension/package-snapshot.h"
static bool test_in_js, test_loop_finished, test_caught_oom, test_injected_oom, test_destroy_error;

static JSValue package_test_call(
    JSContext *ctx, JSValueConst fn, JSValueConst self, int argc, JSValueConst *argv) {
    if (test_destroy_error) {
        return JS_Call(ctx, fn, self, argc, argv);
    }
    if (test_caught_oom) {
        void *p = js_malloc(ctx, 16777217);
        if (p) {
            js_free(ctx, p);
        } else {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            test_injected_oom = true;
        }
        return JS_Call(ctx, fn, self, argc, argv);
    }
    test_in_js = true;
    const char script[] = "for(let i=0;i<200000;i++){}";
    JSValue result =
        JS_Eval(ctx, script, strlen(script), "test-finite-js-workload", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        return result;
    }
    JS_FreeValue(ctx, result);
    test_loop_finished = true;
    return JS_Call(ctx, fn, self, argc, argv);
}

static gt_package_status_t package_test_destroy(gt_package_snapshot_t **out) {
    gt_package_status_t status = gt__package_snapshot_destroy(out);
    return status == GT_PACKAGE_OK && test_destroy_error ? GT_PACKAGE_IO : status;
}

#define JS_Call package_test_call
#define gt__package_snapshot_destroy package_test_destroy
#include "../src/extension/package-check.c"
#undef JS_Call
#undef gt__package_snapshot_destroy
#define GT_PACKAGE_CHECK_BOUNDARY 1
#include "test_package_check.c"
