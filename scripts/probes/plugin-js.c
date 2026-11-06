/* P0 only: same bounded host contract against two pinned engine candidates.
 * No quickjs-libc, native module loader, platform UI, or public Ghosterm ABI.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <quickjs.h>

#ifdef PROBE_NG
typedef bool probe_bool;
#else
typedef JS_BOOL probe_bool;
#endif
struct pending_io;

struct owner {
    JSRuntime *rt;
    JSContext *ctx;
    JSClassID class_id;
    unsigned interrupts, cancel_after, finalized, loaded, denied, rejections;
    int cancelled;
    struct pending_io *pending;
};

static void check(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "ASSERT: %s\n", message);
        exit(1);
    }
}

#ifdef PROBE_NG
static int interrupt(JSContext *ctx, void *opaque) {
    (void)ctx;
#else
static int interrupt(JSRuntime *rt, void *opaque) {
    (void)rt;
#endif
    struct owner *o = opaque;
    ++o->interrupts;
    return o->cancelled || (o->cancel_after && o->interrupts >= o->cancel_after);
}

static void rejection(
    JSContext *ctx, JSValueConst promise, JSValueConst reason, probe_bool handled, void *opaque) {
    (void)ctx;
    (void)promise;
    (void)reason;
    struct owner *o = opaque;
    if (!handled) {
        ++o->rejections;
    }
}

static void discard_exception(JSContext *ctx) {
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
}

static JSValue evaluate(struct owner *o, const char *source) {
    return JS_Eval(o->ctx, source, strlen(source), "plugin:/test.js", JS_EVAL_TYPE_GLOBAL);
}

static void execute(struct owner *o, const char *source) {
    JSValue value = evaluate(o, source);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(o->ctx);
        const char *text = JS_ToCString(o->ctx, error);
        fprintf(stderr, "JS exception: %s\n", text ? text : "unprintable");
        JS_FreeCString(o->ctx, text);
        JS_FreeValue(o->ctx, error);
        check(0, "unexpected JS exception");
    }
    JS_FreeValue(o->ctx, value);
}

static int number(struct owner *o, const char *source) {
    JSValue value = evaluate(o, source);
    int32_t result = 0;
    check(!JS_IsException(value), "number evaluation failed");
    check(JS_ToInt32(o->ctx, &result, value) == 0, "number conversion failed");
    JS_FreeValue(o->ctx, value);
    return result;
}

static int drain(struct owner *o, unsigned budget) {
    unsigned jobs = 0;
    while (JS_IsJobPending(o->rt)) {
        if (jobs++ >= budget) {
            return 1; /* explicit queue budget exhausted */
        }
        JSContext *ctx = NULL;
        int status = JS_ExecutePendingJob(o->rt, &ctx);
        if (status < 0) {
            discard_exception(ctx);
            return -1;
        }
    }
    return 0;
}

static char *normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    struct owner *o = opaque;
    if (strcmp(base, "plugin:/client.js") == 0 && strcmp(name, "./shared.js") == 0) {
        return js_strdup(ctx, "plugin:/shared.js");
    }
    ++o->denied;
    JS_ThrowReferenceError(ctx, "module outside probe allowlist");
    return NULL;
}

static JSModuleDef *load(JSContext *ctx, const char *name, void *opaque) {
    struct owner *o = opaque;
    if (strcmp(name, "plugin:/shared.js") != 0) {
        ++o->denied;
        JS_ThrowReferenceError(ctx, "module unavailable");
        return NULL;
    }
    ++o->loaded;
    const char *source = "globalThis.loads=(globalThis.loads||0)+1; export const answer=42;";
    JSValue value =
        JS_Eval(ctx, source, strlen(source), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(value)) {
        return NULL;
    }
    JSModuleDef *module = JS_VALUE_GET_PTR(value);
    JS_FreeValue(ctx, value);
    return module;
}

static void finalizer(JSRuntime *rt, JSValue value) {
    (void)value;
    struct owner *o = JS_GetRuntimeOpaque(rt);
    ++o->finalized;
}

static JSValue native_object(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    (void)argc;
    (void)argv;
    struct owner *o = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return JS_NewObjectClass(ctx, o->class_id);
}

static void open_owner(struct owner *o) {
    memset(o, 0, sizeof(*o));
    o->rt = JS_NewRuntime();
    check(o->rt != NULL, "runtime allocation failed");
    JS_SetMemoryLimit(o->rt, 16 * 1024 * 1024);
    JS_SetMaxStackSize(o->rt, 128 * 1024);
    JS_SetRuntimeOpaque(o->rt, o);
    JS_SetInterruptHandler(o->rt, interrupt, o);
    JS_SetHostPromiseRejectionTracker(o->rt, rejection, o);
    JS_SetModuleLoaderFunc(o->rt, normalize, load, o);
    o->ctx = JS_NewContext(o->rt);
    check(o->ctx != NULL, "context allocation failed");
#ifdef PROBE_NG
    JS_NewClassID(o->rt, &o->class_id);
#else
    JS_NewClassID(&o->class_id);
#endif
    JSClassDef definition = {.class_name = "ProbeResource", .finalizer = finalizer};
    check(JS_NewClass(o->rt, o->class_id, &definition) == 0, "class registration failed");
    JSValue global = JS_GetGlobalObject(o->ctx);
    check(JS_SetPropertyStr(o->ctx,
                            global,
                            "nativeObject",
                            JS_NewCFunction(o->ctx, native_object, "nativeObject", 0)) >= 0,
          "host function installation failed");
    JS_FreeValue(o->ctx, global);
}

static void close_owner(struct owner *o) {
    JS_FreeContext(o->ctx);
    JS_FreeRuntime(o->rt);
}

static int64_t now_ms(void) {
    struct timespec ts;
    check(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "monotonic clock unavailable");
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct io_request {
    int fd, result;
    int64_t deadline;
    atomic_int cancelled, entered;
};

struct pending_io {
    JSValue callbacks[2];
    int settled;
};

static JSValue start_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    (void)argc;
    (void)argv;
    struct owner *o = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!o->pending) {
        return JS_ThrowInternalError(ctx, "no pending IO slot");
    }
    return JS_NewPromiseCapability(ctx, o->pending->callbacks);
}

/* Engine interrupts cannot interrupt blocking C. This native capability owns
 * its deadline and cancellation, including the no-data / never-EOF case. */
static int bounded_read(struct io_request *request) {
    for (;;) {
        if (atomic_load(&request->cancelled)) {
            return -2;
        }
        int64_t remaining = request->deadline - now_ms();
        if (remaining <= 0) {
            return -1;
        }
        struct pollfd fd = {.fd = request->fd, .events = POLLIN};
        atomic_store(&request->entered, 1);
        int status = poll(&fd, 1, remaining < 10 ? (int)remaining : 10);
        if (status < 0 && errno == EINTR) {
            continue;
        }
        if (status < 0) {
            return -3;
        }
        if (status > 0) {
            unsigned char byte;
            return read(request->fd, &byte, 1) == 1 ? byte : -3;
        }
    }
}

static void *io_worker(void *opaque) {
    struct io_request *request = opaque;
    request->result = bounded_read(request);
    return NULL;
}

static void settle_io(struct owner *o, struct pending_io *pending, int result) {
    if (pending->settled) {
        return;
    }
    pending->settled = 1;
    JSValue argument;
    if (result >= 0) {
        argument = JS_NewInt32(o->ctx, result);
    } else {
        argument = JS_NewError(o->ctx);
        check(JS_SetPropertyStr(o->ctx,
                                argument,
                                "message",
                                JS_NewString(o->ctx,
                                             result == -2   ? "CANCELLED"
                                             : result == -1 ? "EXPIRED"
                                                            : "IO")) >= 0,
              "native error construction failed");
    }
    JSValue call = JS_Call(o->ctx, pending->callbacks[result < 0], JS_UNDEFINED, 1, &argument);
    check(!JS_IsException(call), "native promise settlement failed");
    JS_FreeValue(o->ctx, call);
    JS_FreeValue(o->ctx, argument);
    JS_FreeValue(o->ctx, pending->callbacks[0]);
    JS_FreeValue(o->ctx, pending->callbacks[1]);
}

static void native_io(struct owner *o) {
    int fds[2];
    check(pipe(fds) == 0, "pipe failed");
    JSValue global = JS_GetGlobalObject(o->ctx);
    check(JS_SetPropertyStr(
              o->ctx, global, "startRead", JS_NewCFunction(o->ctx, start_read, "startRead", 0)) >=
              0,
          "native IO capability installation failed");
    JS_FreeValue(o->ctx, global);
    check(write(fds[1], "X", 1) == 1, "pipe control write failed");
    struct io_request control = {.fd = fds[0], .deadline = now_ms() + 500};
    check(bounded_read(&control) == 'X', "native IO control failed");
    for (int cancelled = 0; cancelled <= 1; ++cancelled) {
        struct pending_io pending = {0};
        o->pending = &pending;
        execute(o,
                "globalThis.status='waiting';startRead().then(n=>status='data:'+n,e=>status=e."
                "message);");
        check(number(o, "status==='waiting'") == 1, "native IO resolved inline");
        struct io_request request = {.fd = fds[0], .deadline = now_ms() + (cancelled ? 1000 : 20)};
        int result;
        if (cancelled) {
            pthread_t worker;
            check(pthread_create(&worker, NULL, io_worker, &request) == 0,
                  "IO worker creation failed");
            while (!atomic_load(&request.entered)) {
                struct timespec delay = {.tv_nsec = 1000000};
                nanosleep(&delay, NULL);
            }
            atomic_store(&request.cancelled, 1);
            check(pthread_join(worker, NULL) == 0, "IO worker join failed");
            result = request.result;
        } else {
            result = bounded_read(&request);
        }
        check(result == (cancelled ? -2 : -1), "native IO deadline/cancel collapsed");
        settle_io(o, &pending, result);
        settle_io(o, &pending, 'X'); /* late completion cannot turn failure into success */
        check(drain(o, 32) == 0 &&
                  number(o, cancelled ? "status==='CANCELLED'" : "status==='EXPIRED'") == 1,
              "native promise outcome/late completion failed");
        o->pending = NULL;
    }
    close(fds[0]);
    close(fds[1]);
}

static void run_case(const char *name) {
    struct owner o;
    open_owner(&o);
    if (strcmp(name, "portable") == 0) {
        check(number(&o, "JSON.parse(JSON.stringify({s:'终端',n:new Uint8Array([42])[0]})).n") ==
                  42,
              "portable typed array/JSON failed");
        check(number(&o,
                     "[typeof std,typeof os,typeof process,typeof require,typeof "
                     "fetch].every(x=>x==='undefined')") == 1,
              "ambient host capability exposed");
    } else if (strcmp(name, "modules") == 0) {
        const char *source = "import {answer} from './shared.js'; globalThis.result=answer;";
        JSValue value =
            JS_Eval(o.ctx, source, strlen(source), "plugin:/client.js", JS_EVAL_TYPE_MODULE);
        check(!JS_IsException(value), "allowed module evaluation failed");
        JS_FreeValue(o.ctx, value);
        check(drain(&o, 32) == 0 && o.rejections == 0, "module job failed");
        check(number(&o, "result") == 42 && number(&o, "loads") == 1 && o.loaded == 1,
              "module closure missing");
        const char *denied[] = {"std",
                                "os",
                                "node:fs",
                                "../escape.js",
                                "/etc/passwd",
                                "evil.so",
                                "https://example.invalid/x.js"};
        for (unsigned i = 0; i < sizeof(denied) / sizeof(*denied); ++i) {
            char input[256];
            snprintf(input, sizeof(input), "import '%s';", denied[i]);
            value = JS_Eval(o.ctx, input, strlen(input), "plugin:/client.js", JS_EVAL_TYPE_MODULE);
            check(JS_IsException(value), "forbidden module accepted");
            discard_exception(o.ctx);
        }
        check(o.denied == 7, "module rejection did not use allowlist");
    } else if (strcmp(name, "promise") == 0) {
        execute(&o, "globalThis.result=0; Promise.resolve(40).then(n=>n+2).then(n=>result=n);");
        check(number(&o, "result") == 0 && JS_IsJobPending(o.rt), "Promise ran inline");
        check(drain(&o, 32) == 0 && number(&o, "result") == 42, "Promise jobs not delivered");
        execute(&o, "Promise.reject(new Error('probe rejection'));");
        check(drain(&o, 32) == 0 && o.rejections == 1, "unhandled rejection hidden");
    } else if (strcmp(name, "job-budget") == 0) {
        execute(&o,
                "globalThis.ticks=0; function again(){ticks++;Promise.resolve().then(again)}; "
                "again();");
        check(drain(&o, 8) == 1 && number(&o, "ticks") == 9, "job budget not enforced");
    } else if (strcmp(name, "interrupt") == 0) {
        o.cancel_after = 4;
        JSValue value = evaluate(&o, "for(;;){}");
        check(JS_IsException(value) && o.interrupts >= 4, "CPU interrupt failed");
        discard_exception(o.ctx);
        o.cancel_after = 0;
        check(number(&o, "21*2") == 42, "runtime unusable after CPU interruption");
    } else if (strcmp(name, "cancel") == 0) {
        o.cancelled = 1;
        JSValue value = evaluate(&o, "for(;;){}");
        check(JS_IsException(value) && o.interrupts > 0, "owner cancellation failed");
        discard_exception(o.ctx);
        o.cancelled = 0;
        check(number(&o, "21*2") == 42, "runtime unusable after cancel");
    } else if (strcmp(name, "memory") == 0) {
        JSValue value = evaluate(&o, "new Uint8Array(32*1024*1024)");
        check(JS_IsException(value), "memory limit failed");
        discard_exception(o.ctx);
        check(number(&o, "21*2") == 42, "runtime unusable after memory rejection");
    } else if (strcmp(name, "stack") == 0) {
        JSValue value =
            evaluate(&o, "function recurse(n){return n ? recurse(n-1)+1 : 0}; recurse(1000)");
        check(JS_IsException(value), "stack limit failed");
        discard_exception(o.ctx);
        check(number(&o, "recurse(3)") == 3, "runtime unusable after stack rejection");
    } else if (strcmp(name, "gc") == 0) {
        execute(&o, "(()=>{let a=nativeObject();a.self=a})();");
        check(o.finalized == 0, "cycle finalized before GC");
        JS_RunGC(o.rt);
        check(o.finalized == 1, "GC did not finalize owned cycle");
        execute(&o, "globalThis.held=nativeObject();");
        close_owner(&o);
        check(o.finalized == 2, "dispose did not release held resource");
        return;
    } else if (strcmp(name, "isolation") == 0) {
        struct owner other;
        open_owner(&other);
        execute(&o,
                "globalThis.privateValue=42;globalThis.result=0;Promise.resolve().then(()=>result="
                "42);");
        check(number(&other, "typeof privateValue === 'undefined'") == 1 &&
                  !JS_IsJobPending(other.rt),
              "runtime globals/jobs crossed owners");
        JSValue value = evaluate(&other, "new Uint8Array(32*1024*1024)");
        check(JS_IsException(value), "second owner memory limit missing");
        discard_exception(other.ctx);
        close_owner(&other);
        check(drain(&o, 32) == 0 && number(&o, "result") == 42,
              "failed owner damaged healthy owner");
    } else if (strcmp(name, "native-io") == 0) {
        native_io(&o);
    } else {
        check(0, "unknown case");
    }
    close_owner(&o);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    run_case(argv[1]);
    printf("PASS %s\n", argv[1]);
    return 0;
}
