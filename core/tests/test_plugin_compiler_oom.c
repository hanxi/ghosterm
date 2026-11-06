/* Exercise the pinned compiler independently of package metadata or Core. */
#include <quickjs.h>
#include <cutils.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL plugin-compiler:%d %s\n", __LINE__, #x); return 1; \
} } while (0)

typedef struct { size_t calls, fail, live; } Memory;
typedef union { max_align_t alignment; size_t size; } Allocation;
#ifdef GT_COMPILER_SWITCH_BOUNDARY
/* The isolated runner observes attempted backfills before the unsafe write. */
extern unsigned gt_test_switch_backfills;
extern unsigned gt_test_force_switch_oom, gt_test_switch_oom_injected;
#endif

static void *allocate(void *opaque, size_t size) {
    Memory *m = opaque;
    if (++m->calls == m->fail || size > SIZE_MAX - sizeof(Allocation)) return NULL;
    Allocation *a = malloc(sizeof(*a) + size);
    if (!a) return NULL;
    a->size = size;
    m->live++;
    return a + 1;
}
static void release(void *opaque, void *ptr) {
    if (ptr) { ((Memory *)opaque)->live--; free((Allocation *)ptr - 1); }
}
static void *resize(void *opaque, void *ptr, size_t size) {
    if (!size) { release(opaque, ptr); return NULL; }
    void *out = allocate(opaque, size);
    if (out && ptr) {
        size_t old = ((Allocation *)ptr - 1)->size;
        memcpy(out, ptr, size < old ? size : old);
        release(opaque, ptr);
    }
    return out;
}
static void *zero_allocate(void *opaque, size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    void *out = allocate(opaque, count * size);
    if (out) memset(out, 0, count * size);
    return out;
}
static size_t usable(const void *ptr) {
    return ptr ? ((const Allocation *)ptr - 1)->size : 0;
}

/* Keep the spare bytes available: a failed wide write must also stop later
 * narrow fast-path writes and direct claims, without changing the prefix. */
static int buffer(void) {
    for (unsigned operation = 0; operation < 7; operation++) {
        Memory m = {0}; DynBuf b;
        dbuf_init2(&b, &m, resize);
        CHECK(!dbuf_claim(&b, 16));
        CHECK(!dbuf_putc(&b, 0x5a));
        m.fail = m.calls + 1;
        CHECK(dbuf_claim(&b, 32) < 0 && dbuf_error(&b));
        size_t calls = m.calls;
        int status = 0;
        switch (operation) {
        case 0: status = dbuf_putc(&b, 1); break;
        case 1: status = dbuf_put_u16(&b, 2); break;
        case 2: status = dbuf_put_u32(&b, 3); break;
        case 3: status = dbuf_put_u64(&b, 4); break;
        case 4: status = dbuf_put(&b, "atom", 4); break;
        case 5: status = dbuf_put_self(&b, 0, 1); break;
        case 6: status = dbuf_claim(&b, 1); break;
        }
        bool stopped = status < 0 && b.size == 1 && b.buf[0] == 0x5a && m.calls == calls;
        dbuf_free(&b);
        CHECK(stopped && !m.live);
    }
    return 0;
}

static int compile(const char *source, size_t fail, size_t *calls) {
    Memory m = {.fail = fail};
    const JSMallocFunctions functions = {
        .js_malloc = allocate, .js_calloc = zero_allocate, .js_free = release,
        .js_realloc = resize, .js_malloc_usable_size = usable
    };
    JSRuntime *rt = JS_NewRuntime2(&functions, &m);
    if (rt) {
        JS_SetMaxStackSize(rt, 524288);
        JSContext *ctx = JS_NewContext(rt);
        if (ctx) {
            JSValue value = JS_Eval(ctx, source, strlen(source), "compiler-oom",
                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
            bool ok = false;
            if (JS_IsException(value)) {
                JSValue error = JS_GetException(ctx);
                JS_FreeValue(ctx, error);
                ok = fail && m.calls >= fail;
#ifdef GT_COMPILER_SWITCH_BOUNDARY
                ok = ok || gt_test_switch_oom_injected;
#endif
            } else {
                int32_t result = 0;
                ok = !JS_ToInt32(ctx, &result, value) && result == 42;
            }
            JS_FreeValue(ctx, value);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            CHECK(ok && !m.live);
        } else { JS_FreeRuntime(rt); CHECK(fail && m.calls >= fail && !m.live); }
    } else CHECK(fail && m.calls >= fail && !m.live);
    *calls = m.calls;
#ifdef GT_COMPILER_SWITCH_BOUNDARY
    CHECK(!gt_test_switch_backfills);
#endif
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "buffer")) return buffer();
#ifdef GT_COMPILER_SWITCH_BOUNDARY
    if (argc == 2 && !strcmp(argv[1], "switch")) {
        size_t calls = 0;
        gt_test_force_switch_oom = 1;
        CHECK(!compile("(()=>{switch(1){case 1:return 42;default:return 0}})()", 0, &calls));
        CHECK(gt_test_switch_oom_injected == 1 && !gt_test_force_switch_oom);
        return 0;
    }
#endif
    CHECK(argc == 2 && !strcmp(argv[1], "sweep"));
    char source[65536]; size_t used = 0;
    const char *prefix = "(()=>{class C{#x=42;constructor(){}get(){return this.#x}}"
        "const captured=42;function outer(){return ()=>captured}"
        "function choose(a,op,b){switch(op){";
    used = strlen(prefix); memcpy(source, prefix, used);
    for (unsigned i = 0; i < 120; i++) {
        int n = snprintf(source + used, sizeof(source) - used,
            "case 'operator%u':if(typeof a==='object'){a=a.value}"
            "if(typeof b==='object'){b=b.value}return a===b;", i);
        CHECK(n > 0 && (size_t)n < sizeof(source) - used); used += (size_t)n;
    }
    const char *suffix = "default:throw new TypeError(`Invalid operator: ${op}`)}}"
        "try{if(!choose(1,'operator119',1))throw Error('bad')}finally{}"
        "return new C().get()+outer()()-42})()";
    CHECK(strlen(suffix) < sizeof(source) - used); strcpy(source + used, suffix);
    size_t calls = 0, baseline = 0;
    CHECK(!compile(source, 0, &baseline));
    for (size_t fail = 1; fail <= baseline; fail++) CHECK(!compile(source, fail, &calls));
    CHECK(!compile(source, 0, &calls) && calls == baseline);
    printf("COMPILER OOM PASS: %zu allocation ordinals; no surviving allocations\n", baseline);
    return 0;
}
