/* Conformance host only: the actual public Core Session and pinned QuickJS-NG.
 * No production engine dependency, module loader or JS native adapter yet. */
#define EXT_RPC_MAIN sdk_fixture_unused_main
#include "../../core/tests/test_ext_rpc.c"
#include <inttypes.h>
#include <quickjs.h>
static rpc_pair_t pair;
static unsigned requests, effects;
static const char *module_root;
static const char *statuses[] = {"ok", "wait", "invalid", "denied", "unsupported", "closed"};
static const char *states[] = {"queued", "pending", "succeeded", "failed", "cancelled", "unknown"};

static void set(JSContext *c, JSValue object, const char *key, JSValue value) {
    CHECK(JS_SetPropertyStr(c, object, key, value) >= 0);
}

static JSValue decimal(JSContext *c, uint64_t n) {
    char b[32];
    CHECK(snprintf(b, sizeof(b), "%" PRIu64, n) > 0);
    return JS_NewString(c, b);
}

static uint64_t handle(JSContext *c, JSValueConst v) {
    const char *s = JS_ToCString(c, v);
    CHECK(s && s[0]);
    char *end = NULL;
    errno = 0;
    uint64_t n = strtoull(s, &end, 10);
    CHECK(!errno && end && !*end && n);
    JS_FreeCString(c, s);
    return n;
}

static JSValue native(JSContext *c, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    CHECK(argc >= 2);
    const char *op = JS_ToCString(c, argv[0]);
    CHECK(op);
    int32_t side = 0;
    CHECK(!JS_ToInt32(c, &side, argv[1]) && (side == 0 || side == 1));
    gt_ext_session_t *s = side ? pair.pair.e : pair.pair.h;
    gt_ext_lease_t *l = side ? &pair.e : &pair.h;
    JSValue out = JS_UNDEFINED;
    if (!strcmp(op, "request")) {
        CHECK(argc == 5);
        const char *method = JS_ToCString(c, argv[2]);
        size_t n;
        const char *json = JS_ToCStringLen(c, &n, argv[3]);
        uint32_t ms;
        CHECK(method && json && !JS_ToUint32(c, &ms, argv[4]));
        uint64_t h = 0;
        gt_ext_rpc_status_t status = gt_ext_session_request(s, l, method, json, n, ms, &h);
        requests++;
        out = JS_NewObject(c);
        set(c, out, "status", JS_NewString(c, statuses[status]));
        if (h) {
            set(c, out, "handle", decimal(c, h));
        }
        JS_FreeCString(c, method);
        JS_FreeCString(c, json);
    } else if (!strcmp(op, "result")) {
        CHECK(argc == 3);
        gt_ext_rpc_result_t r;
        CHECK(gt_ext_session_request_result(s, l, handle(c, argv[2]), &r) == GT_EXT_RPC_OK);
        out = JS_NewObject(c);
        set(c, out, "state", JS_NewString(c, states[r.state]));
        set(c, out, "confirmed", JS_NewBool(c, r.confirmed));
        set(c, out, "outcome", JS_NewStringLen(c, r.outcome, r.outcome_size));
    } else if (!strcmp(op, "cancel") || !strcmp(op, "forget") || !strcmp(op, "check")) {
        CHECK(argc == 3);
        uint64_t h = handle(c, argv[2]);
        gt_ext_rpc_status_t status = !strcmp(op, "cancel") ? gt_ext_session_cancel_request(s, l, h)
                                     : !strcmp(op, "forget")
                                         ? gt_ext_session_forget_request(s, l, h)
                                         : gt_ext_session_check_request(s, l, h);
        out = JS_NewString(c, statuses[status]);
    } else if (!strcmp(op, "take")) {
        gt_ext_rpc_request_t q;
        gt_ext_rpc_status_t status = gt_ext_session_take_request(s, l, &q);
        CHECK(status == GT_EXT_RPC_OK || status == GT_EXT_RPC_WAIT || status == GT_EXT_RPC_CLOSED ||
              status == GT_EXT_RPC_DENIED);
        if (status != GT_EXT_RPC_OK) {
            out = JS_NULL;
        } else {
            out = JS_NewObject(c);
            set(c, out, "ticket", decimal(c, q.ticket));
            set(c, out, "method", JS_NewString(c, q.method));
            set(c, out, "json", JS_NewStringLen(c, q.input, q.input_size));
        }
    } else if (!strcmp(op, "respond")) {
        CHECK(argc == 4);
        size_t n;
        const char *json = JS_ToCStringLen(c, &n, argv[3]);
        CHECK(json);
        gt_ext_rpc_status_t status = gt_ext_session_respond(s, l, handle(c, argv[2]), json, n);
        out = JS_NewString(c, statuses[status]);
        JS_FreeCString(c, json);
    } else if (!strcmp(op, "revoke")) {
        gt_ext_session_revoke(s);
    } else if (!strcmp(op, "stop")) {
        gt_ext_session_stop(s);
    } else if (!strcmp(op, "advance")) {
        uint32_t ms;
        CHECK(argc == 3 && !JS_ToUint32(c, &ms, argv[2]));
        pair.pair.clock += ms;
    } else if (!strcmp(op, "effect")) {
        effects++;
        out = JS_NewUint32(c, effects);
    } else if (!strcmp(op, "effects")) {
        out = JS_NewUint32(c, effects);
    } else if (!strcmp(op, "requests")) {
        out = JS_NewUint32(c, requests);
    } else if (!strcmp(op, "stale")) {
        l->epoch++;
    } else if (!strcmp(op, "reply")) {
        CHECK(argc == 3);
        gt_ext_rpc_request_t q = rpc_take(&pair, false);
        size_t n;
        const char *json = JS_ToCStringLen(c, &n, argv[2]);
        CHECK(json);
        CHECK(gt_ext_session_respond(pair.pair.e, &pair.e, q.ticket, json, n) == GT_EXT_RPC_OK);
        JS_FreeCString(c, json);
    } else {
        CHECK(false);
    }
    JS_FreeCString(c, op);
    return out;
}

static char *normalize(JSContext *c, const char *base, const char *name, void *unused) {
    (void)unused;
    if (!strcmp(base, "portable-session.js") && !strncmp(name, "../portable/", 12)) {
        name += 12;
    }
    const char *allowed[] = {
        "codec.js", "wire-contract.js", "session.js", "portable-session.js", "index.js"};
    for (size_t i = 0; i < 5; i++) {
        const char *n = !strncmp(name, "./", 2) ? name + 2 : name;
        if (!strcmp(n, allowed[i])) {
            return js_strdup(c, n);
        }
    }
    JS_ThrowReferenceError(c, "module unavailable");
    return NULL;
}

static JSModuleDef *load(JSContext *c, const char *name, void *unused) {
    (void)unused;
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/%s", module_root, name) > 0);
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    CHECK(n > 0 && n < 262144 && !fseek(f, 0, SEEK_SET));
    char *buf = calloc(1, (size_t)n + 1);
    CHECK(buf && fread(buf, 1, (size_t)n, f) == (size_t)n && !fclose(f));
    JSValue v = JS_Eval(c, buf, (size_t)n, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(buf);
    if (JS_IsException(v)) {
        return NULL;
    }
    JSModuleDef *m = JS_VALUE_GET_PTR(v);
    JS_FreeValue(c, v);
    return m;
}

static void execute(JSContext *c, const char *text, int flags) {
    JSValue v = JS_Eval(c, text, strlen(text), "fixture.js", flags);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(c);
        const char *str = JS_ToCString(c, e);
        fprintf(stderr, "JS: %s\n", str ? str : "exception");
        JS_FreeCString(c, str);
        JS_FreeValue(c, e);
        CHECK(false);
    }
    JS_FreeValue(c, v);
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    size_t before = ext_fds();
    module_root = argv[1];
    rpc_open(&pair, "timeout");
    JSRuntime *rt = JS_NewRuntime();
    CHECK(rt);
    JS_SetMemoryLimit(rt, 16 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 256 * 1024);
    JSContext *c = JS_NewContext(rt);
    CHECK(c);
    JS_SetModuleLoaderFunc(rt, normalize, load, NULL);
    JSValue global = JS_GetGlobalObject(c);
    set(c, global, "native", JS_NewCFunction(c, native, "native", 2));
    set(c, global, "testCase", JS_NewString(c, argv[2]));
    JS_FreeValue(c, global);
    execute(c,
            "import {run} from './portable-session.js'; globalThis.done=0; "
            "run(globalThis.testCase).then(()=>{globalThis.done=1},e=>{globalThis.failure=String(e)"
            ";globalThis.done=-1});",
            JS_EVAL_TYPE_MODULE);
    bool done = false;
    uint64_t end = ext_now() + 10000;
    for (unsigned i = 0; i < 4000 && ext_now() < end; i++) {
        rpc_pump(&pair);
        execute(c, "if(globalThis.tick)globalThis.tick()", JS_EVAL_TYPE_GLOBAL);
        for (unsigned j = 0; j < 128 && JS_IsJobPending(rt); j++) {
            JSContext *job = NULL;
            CHECK(JS_ExecutePendingJob(rt, &job) >= 0);
        }
        global = JS_GetGlobalObject(c);
        JSValue value = JS_GetPropertyStr(c, global, "done");
        int32_t status;
        CHECK(!JS_ToInt32(c, &status, value));
        JS_FreeValue(c, value);
        if (status < 0) {
            value = JS_GetPropertyStr(c, global, "failure");
            const char *str = JS_ToCString(c, value);
            fprintf(stderr, "ASSERT: %s\n", str ? str : "SDK failed");
            JS_FreeCString(c, str);
            JS_FreeValue(c, value);
            JS_FreeValue(c, global);
            return 1;
        }
        JS_FreeValue(c, global);
        if (status == 1) {
            done = true;
            break;
        }
    }
    CHECK(done);
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    rpc_drop(&pair);
    CHECK(ext_fds() == before);
    printf("PASS sdk_%s\n", argv[2]);
    return 0;
}
