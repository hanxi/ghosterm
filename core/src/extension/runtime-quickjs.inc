/* Included only in the explicitly enabled, content-pinned engine build. */
#include "extension/memory.h"
#include "extension/session-owner.h"
#include "extension/package-runtime.h"
#include "extension/package-cli-decoder.h"
#include "extension/runtime-views.h"
#include "base/sha256.h"
#include "base/secure-random.h"
#include <quickjs.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#define GT_RUNTIME_PROCESS_CHILDREN 16u
#define GT_RUNTIME_PROCESS_LIFETIME_DEFAULT_MS 600000u
#define GT_RUNTIME_PROCESS_OUTPUT_DEFAULT_BYTES (64u * 1024u * 1024u)
#define GT_RUNTIME_PROCESS_STOP_GRACE_MS 250u

typedef enum {
    GT_RUNTIME_PROCESS_STOP_NONE,
    GT_RUNTIME_PROCESS_STOP_DEADLINE,
    GT_RUNTIME_PROCESS_STOP_OUTPUT_LIMIT
} runtime_process_stop_reason_t;

typedef struct {
    uint64_t id;
    gt_ext_process_t *process;
    uint64_t deadline, stop_deadline;
    char verified_version[129];
    JSValue release_promise;
    JSValue release_resolver;
    runtime_process_stop_reason_t stop_reason;
    bool killed;
    bool releasing;
} runtime_process_child_t;

typedef struct {
    char *name;
    char *source;
    size_t size;
} runtime_module_t;

typedef union {
    max_align_t alignment;
    size_t size;
} runtime_allocation_t;

struct gt_bridge_runtime {
    runtime_views_t *views;
    gt_ext_session_t *session;
    gt_ext_transport_process_info_t transport_process;
    gt_status_t transport_process_status;
    gt_ext_lease_t lease;
    gt_ext_role_t role;
    gt_runtime_resources_t *resources;
    gt_ext_process_service_t *process_service;
    bool resources_enabled;
    char *process_environment_block;
    const char *process_environment[GT_EXT_RUNTIME_PROCESS_ENV_MAX];
    size_t process_environment_count, process_environment_bytes;
    uint32_t process_lifetime_ms;
    uint64_t process_output_limit, process_output_bytes;
    runtime_process_child_t process_children[GT_RUNTIME_PROCESS_CHILDREN];
    uint64_t next_process_id, process_stop_deadline;
    runtime_module_t modules[64];
    size_t count;
    char entry[GT_RUNTIME_MODULE_NAME_MAX + 1];
    gt_package_checked_t *package;
    gt_status_t module_cleanup_error;
    char *invocation;
    size_t invocation_size, argument_count, argument_offsets[32];
    char argument_schema[GT_PACKAGE_PATH_BYTES_MAX + 1];
    JSValue decoded_arguments;
    JSRuntime *engine;
    JSContext *context;
    JSValue module, space, pump, dispose, unhandled[64];
    size_t unhandled_count;
    atomic_bool stopped;
    uint64_t deadline, activation_deadline;
    uint32_t cpu_ms, jobs_per_step, activation_ms;
    gt_runtime_info_t info;
    bool started, rejected, interrupted, memory_failed;
    bool process_stopping, process_killed, dispose_called, disposing;
    size_t heap_used, heap_limit;
};

static void *runtime_malloc(void *opaque, size_t n) {
    gt_bridge_runtime_t *r = opaque;
    if (!n) {
        n = 1;
    }
    if (n > SIZE_MAX - sizeof(runtime_allocation_t) ||
        n + sizeof(runtime_allocation_t) > r->heap_limit - r->heap_used) {
        r->memory_failed = true;
        return NULL;
    }
    size_t bytes = n + sizeof(runtime_allocation_t);
    runtime_allocation_t *p = gt__plugin_memory_alloc(NULL, bytes);
    if (!p) {
        r->memory_failed = true;
        return NULL;
    }
    p->size = n;
    r->heap_used += bytes;
    return p + 1;
}

static void runtime_free(void *opaque, void *p) {
    if (!p) {
        return;
    }
    gt_bridge_runtime_t *r = opaque;
    runtime_allocation_t *h = (runtime_allocation_t *)p - 1;
    size_t bytes = h->size + sizeof(*h);
    gt__plugin_memory_free(NULL, h, bytes);
    r->heap_used -= bytes;
}

static void *runtime_calloc(void *opaque, size_t count, size_t n) {
    gt_bridge_runtime_t *r = opaque;
    if (count && n > SIZE_MAX / count) {
        r->memory_failed = true;
        return NULL;
    }
    void *p = runtime_malloc(r, count * n);
    if (p) {
        memset(p, 0, count * n);
    }
    return p;
}

static void *runtime_realloc(void *opaque, void *p, size_t n) {
    if (!p) {
        return runtime_malloc(opaque, n);
    }
    if (!n) {
        runtime_free(opaque, p);
        return NULL;
    }
    size_t old = ((runtime_allocation_t *)p - 1)->size;
    void *next = runtime_malloc(opaque, n);
    if (next) {
        memcpy(next, p, n < old ? n : old);
        runtime_free(opaque, p);
    }
    return next;
}

static size_t runtime_usable(const void *p) {
    return p ? ((const runtime_allocation_t *)p - 1)->size : 0;
}

static const char *runtime_status[] = {"ok", "wait", "invalid", "denied", "unsupported", "closed"};
static const char *runtime_states[] = {
    "queued", "pending", "succeeded", "failed", "cancelled", "unknown"};

static bool runtime_now(uint64_t *out) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return false;
    }
    *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return true;
}

/* Native I/O may block inside a JS host call. Only executing owner-thread CPU
 * consumes the JS budget; activation and resource lifetimes use runtime_now. */
static bool runtime_cpu_now(uint64_t *out) {
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) {
        return false;
    }
    *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return true;
}

static bool runtime_resource_clock(void *opaque, uint64_t *out) {
    (void)opaque;
    return runtime_now(out);
}

static void runtime_fail(gt_bridge_runtime_t *r, gt_runtime_error_t error) {
    if (r->info.phase != GT_RUNTIME_FAILED && r->info.phase != GT_RUNTIME_STOPPED) {
        r->info.phase = error == GT_RUNTIME_CANCELLED ? GT_RUNTIME_STOPPED : GT_RUNTIME_FAILED;
        r->info.error = error;
    }
    gt_ext_session_revoke(r->session);
}

static int runtime_interrupt(JSContext *ctx, void *opaque) {
    (void)ctx;
    gt_bridge_runtime_t *r = opaque;
    uint64_t now;
    if (atomic_load_explicit(&r->stopped, memory_order_relaxed) && !r->disposing) {
        r->interrupted = true;
        return 1;
    }
    if (!runtime_cpu_now(&now) || now >= r->deadline) {
        r->interrupted = true;
        return 1;
    }
    return 0;
}

static void runtime_exception(gt_bridge_runtime_t *r) {
    JSValue e = JS_GetException(r->context);
    JS_FreeValue(r->context, e);
    runtime_fail(r,
                 atomic_load_explicit(&r->stopped, memory_order_relaxed) ? GT_RUNTIME_CANCELLED
                 : r->interrupted                                        ? GT_RUNTIME_CPU
                 : r->memory_failed                                      ? GT_RUNTIME_MEMORY
                                                                         : GT_RUNTIME_SCRIPT);
}

static void runtime_rejection(
    JSContext *ctx, JSValueConst promise, JSValueConst reason, bool handled, void *opaque) {
    (void)reason;
    gt_bridge_runtime_t *r = opaque;
    for (size_t i = 0; i < r->unhandled_count; i++) {
        if (JS_VALUE_GET_PTR(r->unhandled[i]) == JS_VALUE_GET_PTR(promise)) {
            if (handled) {
                JS_FreeValue(ctx, r->unhandled[i]);
                r->unhandled[i] = r->unhandled[--r->unhandled_count];
            }
            return;
        }
    }
    if (!handled) {
        if (r->unhandled_count == 64) {
            r->rejected = true;
        } else {
            r->unhandled[r->unhandled_count++] = JS_DupValue(ctx, promise);
        }
    }
}

static bool runtime_name(const char *name) {
    return gt__runtime_module_name(name);
}

static char *runtime_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    (void)opaque;
    char path[GT_RUNTIME_MODULE_NAME_MAX + 1];
    if (!strcmp(name, "host:/native") && !strcmp(base, "host:/bootstrap.js")) {
        return js_strdup(ctx, name);
    }
    if (!strcmp(name, "@ghosterm/plugin-sdk")) {
        return js_strdup(ctx, "sdk:/index.js");
    }
    if (runtime_name(name)) {
        return js_strdup(ctx, name);
    }
    if (name[0] != '.' || (strncmp(base, "plugin:/", 8) && strncmp(base, "sdk:/", 5))) {
        goto denied;
    }
    const char *slash = strrchr(base, '/');
    if (!slash || (size_t)(slash - base) + 1 + strlen(name) >= sizeof(path)) {
        goto denied;
    }
    memcpy(path, base, (size_t)(slash - base) + 1);
    strcpy(path + (slash - base) + 1, name);
    {
        char result[GT_RUNTIME_MODULE_NAME_MAX + 1];
        size_t prefix = !strncmp(base, "plugin:/", 8) ? 8 : 5, used = prefix;
        memcpy(result, base, prefix);
        const char *at = path + prefix;
        while (*at) {
            const char *end = strchr(at, '/');
            size_t n = end ? (size_t)(end - at) : strlen(at);
            if (n == 1 && *at == '.') {
            } else if (n == 2 && !strncmp(at, "..", 2)) {
                if (used <= prefix) {
                    goto denied;
                }
                used--;
                while (used > prefix && result[used - 1] != '/') {
                    used--;
                }
            } else {
                if (!n || used + n + 1 >= sizeof(result)) {
                    goto denied;
                }
                memcpy(result + used, at, n);
                used += n;
                if (end) {
                    result[used++] = '/';
                }
            }
            if (!end) {
                break;
            }
            at = end + 1;
        }
        result[used] = 0;
        if (!runtime_name(result)) {
            goto denied;
        }
        return js_strdup(ctx, result);
    }
denied:
    JS_ThrowReferenceError(ctx, "module unavailable");
    return NULL;
}

static JSValue runtime_decimal(JSContext *ctx, uint64_t n) {
    char b[32];
    (void)snprintf(b, sizeof(b), "%" PRIu64, n);
    return JS_NewString(ctx, b);
}

static bool runtime_handle(JSContext *ctx, JSValueConst v, uint64_t *out) {
    if (!JS_IsString(v)) {
        return false;
    }
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s) {
        return false;
    }
    uint64_t value = 0;
    bool ok = n && n <= 20 && s[0] >= '1' && s[0] <= '9';
    for (size_t i = 0; i < n && ok; i++) {
        if (s[i] < '0' || s[i] > '9' || value > (UINT64_MAX - (unsigned)(s[i] - '0')) / 10) {
            ok = false;
        } else {
            value = value * 10 + (unsigned)(s[i] - '0');
        }
    }
    JS_FreeCString(ctx, s);
    if (ok) {
        *out = value;
    }
    return ok;
}

static int runtime_set(JSContext *c, JSValue object, const char *key, JSValue value) {
    if (JS_IsException(value)) {
        return -1;
    }
    return JS_SetPropertyStr(c, object, key, value);
}

static JSValue runtime_decode_arguments(gt_bridge_runtime_t *r, JSContext *c, JSValueConst argv) {
    if (!r->argument_schema[0]) {
        return JS_UNDEFINED;
    }
    if (!JS_IsUndefined(r->decoded_arguments)) {
        return JS_DupValue(c, r->decoded_arguments);
    }
    gt_package_member_t member;
    const uint8_t *bytes = NULL;
    size_t size = 0;
    if (!gt__package_checked_member(r->package, r->argument_schema, &member, &bytes, &size) ||
        member.bytes != size) {
        return JS_ThrowInternalError(c, "checked argument schema unavailable");
    }
    char *json = gt__plugin_memory_alloc(NULL, size + 1);
    if (!json) {
        r->memory_failed = true;
        runtime_fail(r, GT_RUNTIME_MEMORY);
        return JS_ThrowOutOfMemory(c);
    }
    memcpy(json, bytes, size);
    json[size] = 0;
    JSValue schema = JS_ParseJSON(c, json, size, "checked-argument-schema");
    gt__plugin_memory_free(NULL, json, size + 1);
    if (JS_IsException(schema)) {
        return JS_EXCEPTION;
    }
    JSValue decoder = JS_Eval(c,
                              gt__package_cli_decoder,
                              sizeof(gt__package_cli_decoder) - 1,
                              "host:/cli-arguments.js",
                              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    if (JS_IsException(decoder)) {
        JS_FreeValue(c, schema);
        return JS_EXCEPTION;
    }
    JSValue values[2] = {schema, JS_DupValue(c, argv)};
    JSValue decoded = JS_Call(c, decoder, JS_UNDEFINED, 2, values);
    JS_FreeValue(c, values[0]);
    JS_FreeValue(c, values[1]);
    JS_FreeValue(c, decoder);
    if (!JS_IsException(decoded)) {
        r->decoded_arguments = JS_DupValue(c, decoded);
    }
    return decoded;
}

/* View registration is local native state; it never enters Session/wire IO. */
#include "extension/runtime-views.inc"

static JSValue
runtime_native(JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int op) {
    (void)self;
    gt_bridge_runtime_t *r = JS_GetContextOpaque(c);
    if (!r || !r->session) {
        return JS_ThrowInternalError(c, "runtime closed");
    }
    if (op == 7) {
        gt_ext_session_revoke(r->session);
        return JS_UNDEFINED;
    }
    if (op == 8) {
        gt_ext_lease_t live;
        return JS_NewBool(
            c,
            !atomic_load_explicit(&r->stopped, memory_order_relaxed) &&
                r->info.phase < GT_RUNTIME_FAILED && gt_ext_session_lease(r->session, &live) &&
                live.epoch == r->lease.epoch && !memcmp(live.token, r->lease.token, 33));
    }
    if (op == 10) {
        if (!r->invocation) {
            return JS_UNDEFINED;
        }
        JSValue result = JS_NewObjectProto(c, JS_NULL), args = JS_NewArray(c);
        if (JS_IsException(result) || JS_IsException(args)) {
            JS_FreeValue(c, result);
            JS_FreeValue(c, args);
            return JS_EXCEPTION;
        }
        for (size_t i = 0; i < r->argument_count; i++) {
            JSValue value = JS_NewString(c, r->invocation + r->argument_offsets[i]);
            if (JS_IsException(value) || JS_SetPropertyUint32(c, args, (uint32_t)i, value) < 0) {
                JS_FreeValue(c, result);
                JS_FreeValue(c, args);
                return JS_EXCEPTION;
            }
        }
        if (runtime_set(c, result, "command", JS_NewString(c, r->invocation)) < 0) {
            JS_FreeValue(c, result);
            JS_FreeValue(c, args);
            return JS_EXCEPTION;
        }
        JSValue decoded = runtime_decode_arguments(r, c, args);
        if (JS_IsException(decoded)) {
            JS_FreeValue(c, result);
            JS_FreeValue(c, args);
            return JS_EXCEPTION;
        }
        if (runtime_set(c, result, "decodedArguments", decoded) < 0) {
            JS_FreeValue(c, result);
            JS_FreeValue(c, args);
            return JS_EXCEPTION;
        }
        if (JS_SetPropertyStr(c, result, "arguments", args) < 0) {
            JS_FreeValue(c, result);
            return JS_EXCEPTION;
        }
        return result;
    }
    if (op == 11) {
        if (gt__runtime_resources_roots(r->resources) != 1) {
            return JS_UNDEFINED;
        }
        JSValue workspace = JS_NewObjectProto(c, JS_NULL);
        if (JS_IsException(workspace) ||
            runtime_set(
                c, workspace, "id", JS_NewString(c, gt__runtime_resources_root(r->resources, 0))) <
                0) {
            JS_FreeValue(c, workspace);
            return JS_EXCEPTION;
        }
        return workspace;
    }
    /* Observe the live scoped grant, never cache it in JS or expose its lease. */
    if (op == 9) {
        if (argc != 1 || !JS_IsString(argv[0])) {
            goto invalid;
        }
        size_t size;
        const char *name = JS_ToCStringLen(c, &size, argv[0]);
        if (!name) {
            return JS_EXCEPTION;
        }
        if (!size || size > 128 || memchr(name, 0, size)) {
            JS_FreeCString(c, name);
            goto invalid;
        }
        gt_ext_access_t access = atomic_load_explicit(&r->stopped, memory_order_relaxed) ||
                                         r->info.phase >= GT_RUNTIME_FAILED
                                     ? GT_EXT_ACCESS_REVOKED
                                     : gt_ext_session_access(r->session, &r->lease, name);
        JS_FreeCString(c, name);
        switch (access) {
            case GT_EXT_ACCESS_UNKNOWN:
                return JS_NewString(c, "unknown");
            case GT_EXT_ACCESS_ALLOWED:
                return JS_NewString(c, "allowed");
            case GT_EXT_ACCESS_DENIED:
                return JS_NewString(c, "denied");
            case GT_EXT_ACCESS_REVOKED:
                return JS_NewString(c, "revoked");
            case GT_EXT_ACCESS_UNSUPPORTED:
                return JS_NewString(c, "unsupported");
        }
        return JS_ThrowInternalError(c, "invalid access observation");
    }
    if ((op == 0 || op == 4 || op == 5 || op == 6) &&
        (atomic_load_explicit(&r->stopped, memory_order_relaxed) ||
         r->info.phase >= GT_RUNTIME_FAILED)) {
        return op == 4 ? JS_NULL : JS_ThrowInternalError(c, "runtime stopped");
    }
    uint64_t h = 0;
    gt_ext_rpc_status_t status;
    if (op == 0) {
        if (argc != 3 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]) || !JS_IsNumber(argv[2])) {
            goto invalid;
        }
        size_t namesize, n;
        const char *name = JS_ToCStringLen(c, &namesize, argv[0]),
                   *json = JS_ToCStringLen(c, &n, argv[1]);
        double ms;
        if (!name || !json) {
            JS_FreeCString(c, name);
            JS_FreeCString(c, json);
            return JS_EXCEPTION;
        }
        if (namesize > 128 || memchr(name, 0, namesize) || JS_ToFloat64(c, &ms, argv[2]) ||
            !(ms >= 1 && ms <= 3600000) || ms != (uint32_t)ms) {
            JS_FreeCString(c, name);
            JS_FreeCString(c, json);
            goto invalid;
        }
        status = gt_ext_session_request(r->session, &r->lease, name, json, n, (uint32_t)ms, &h);
        JS_FreeCString(c, name);
        JS_FreeCString(c, json);
        JSValue out = JS_NewObjectProto(c, JS_NULL);
        if (JS_IsException(out)) {
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return out;
        }
        if (runtime_set(c, out, "status", JS_NewString(c, runtime_status[status])) < 0 ||
            (h && runtime_set(c, out, "handle", runtime_decimal(c, h)) < 0)) {
            JS_FreeValue(c, out);
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return JS_EXCEPTION;
        }
        return out;
    }
    if (op == 4) {
        if (argc != 1 || !JS_IsArray(argv[0])) {
            goto invalid;
        }
        int64_t count = 0;
        if (JS_GetLength(c, argv[0], &count) || count < 0 || count > 16) {
            goto invalid;
        }
        const char *methods[16] = {0};
        size_t method_count = 0;
        for (uint32_t i = 0; i < (uint32_t)count; i++) {
            JSValue item = JS_GetPropertyUint32(c, argv[0], i);
            size_t size = 0;
            if (!JS_IsString(item)) {
                JS_FreeValue(c, item);
                goto invalid;
            }
            methods[method_count] = JS_ToCStringLen(c, &size, item);
            JS_FreeValue(c, item);
            if (!methods[method_count] || !size || size > 128 ||
                memchr(methods[method_count], 0, size)) {
                for (size_t j = 0; j <= method_count; j++) {
                    JS_FreeCString(c, methods[j]);
                }
                goto invalid;
            }
            method_count++;
        }
        gt_ext_rpc_request_t q;
        status =
            gt__ext_session_take_request_methods(r->session, &r->lease, methods, method_count, &q);
        for (size_t i = 0; i < method_count; i++) {
            JS_FreeCString(c, methods[i]);
        }
        if (status == GT_EXT_RPC_WAIT || status == GT_EXT_RPC_CLOSED ||
            status == GT_EXT_RPC_DENIED) {
            return JS_NULL;
        }
        if (status != GT_EXT_RPC_OK) {
            return JS_ThrowInternalError(c, "request unavailable");
        }
        JSValue out = JS_NewObjectProto(c, JS_NULL);
        if (JS_IsException(out)) {
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return out;
        }
        if (runtime_set(c, out, "ticket", runtime_decimal(c, q.ticket)) < 0 ||
            runtime_set(c, out, "method", JS_NewString(c, q.method)) < 0 ||
            runtime_set(c, out, "json", JS_NewStringLen(c, q.input, q.input_size)) < 0) {
            JS_FreeValue(c, out);
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return JS_EXCEPTION;
        }
        return out;
    }
    if (argc < 1 || !runtime_handle(c, argv[0], &h)) {
        goto invalid;
    }
    if (op == 1) {
        gt_ext_rpc_result_t v;
        status = gt_ext_session_request_result(r->session, &r->lease, h, &v);
        if (status != GT_EXT_RPC_OK) {
            return JS_ThrowInternalError(c, "result unavailable");
        }
        JSValue out = JS_NewObjectProto(c, JS_NULL);
        if (JS_IsException(out)) {
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return out;
        }
        if (runtime_set(c, out, "state", JS_NewString(c, runtime_states[v.state])) < 0 ||
            runtime_set(c, out, "confirmed", JS_NewBool(c, v.confirmed)) < 0 ||
            runtime_set(c, out, "outcome", JS_NewStringLen(c, v.outcome, v.outcome_size)) < 0) {
            JS_FreeValue(c, out);
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return JS_EXCEPTION;
        }
        return out;
    }
    if (op == 2) {
        status = gt_ext_session_cancel_request(r->session, &r->lease, h);
    } else if (op == 3) {
        status = gt_ext_session_forget_request(r->session, &r->lease, h);
    } else if (op == 5) {
        status = gt_ext_session_check_request(r->session, &r->lease, h);
    } else if (op == 6) {
        if (argc != 2 || !JS_IsString(argv[1])) {
            goto invalid;
        }
        size_t n;
        const char *json = JS_ToCStringLen(c, &n, argv[1]);
        if (!json) {
            return JS_EXCEPTION;
        }
        status = gt_ext_session_respond(r->session, &r->lease, h, json, n);
        JS_FreeCString(c, json);
    } else {
        goto invalid;
    }
    return JS_NewString(c, runtime_status[status]);
invalid:
    return JS_ThrowTypeError(c, "invalid native request");
}

#include "extension/runtime-resource-js.inc"

static runtime_process_child_t *runtime_process_find(gt_bridge_runtime_t *r, uint64_t id) {
    for (size_t i = 0; i < GT_RUNTIME_PROCESS_CHILDREN; i++) {
        if (r->process_children[i].process && r->process_children[i].id == id) {
            return &r->process_children[i];
        }
    }
    return NULL;
}

static JSValue runtime_process_state(JSContext *c, const char *state) {
    JSValue result = JS_NewObjectProto(c, JS_NULL);
    if (JS_IsException(result) || runtime_set(c, result, "state", JS_NewString(c, state)) < 0) {
        JS_FreeValue(c, result);
        return JS_EXCEPTION;
    }
    return result;
}

static JSValue runtime_process_error(JSContext *c, gt_status_t status) {
    JSValue error = JS_NewError(c);
    if (JS_IsException(error) ||
        runtime_set(c, error, "message", JS_NewString(c, "process pipe I/O failed")) < 0 ||
        runtime_set(c, error, "code", JS_NewInt32(c, status)) < 0) {
        JS_FreeValue(c, error);
        return JS_EXCEPTION;
    }
    return error;
}

static JSValue runtime_process_limit_error(JSContext *c) {
    JSValue error = JS_NewError(c);
    if (JS_IsException(error) ||
        runtime_set(c, error, "message", JS_NewString(c, "process output quota exceeded")) < 0 ||
        runtime_set(c, error, "code", JS_NewString(c, "RESOURCE_LIMIT")) < 0) {
        JS_FreeValue(c, error);
        return JS_EXCEPTION;
    }
    return error;
}

static bool runtime_process_stop(runtime_process_child_t *slot,
                                 runtime_process_stop_reason_t reason,
                                 uint64_t now) {
    if (slot->stop_reason != GT_RUNTIME_PROCESS_STOP_NONE) {
        return true;
    }
    if (now > UINT64_MAX - GT_RUNTIME_PROCESS_STOP_GRACE_MS) {
        return false;
    }
    if (!gt__ext_process_reaping(slot->process) &&
        gt__ext_process_signal(slot->process, SIGTERM) != GT_OK) {
        gt_ext_process_info_t info;
        if (gt__ext_process_poll(slot->process, &info) != GT_OK ||
            (!info.exited && !gt__ext_process_reaping(slot->process))) {
            return false;
        }
    }
    slot->stop_reason = reason;
    slot->stop_deadline = now + GT_RUNTIME_PROCESS_STOP_GRACE_MS;
    return true;
}

static JSValue runtime_process_resolved_promise(JSContext *c, JSValue value) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(c, resolving);
    if (JS_IsException(promise)) {
        return promise;
    }
    JSValue result = JS_Call(c, resolving[0], JS_UNDEFINED, 1, (JSValueConst *)&value);
    JS_FreeValue(c, resolving[0]);
    JS_FreeValue(c, resolving[1]);
    if (JS_IsException(result)) {
        JS_FreeValue(c, promise);
        JS_FreeValue(c, JS_GetException(c));
        return JS_EXCEPTION;
    }
    JS_FreeValue(c, result);
    return promise;
}

static JSValue
runtime_process_method(JSContext *c, JSCFunctionData *function, int op, uint64_t id) {
    JSValue data = JS_NewInt64(c, (int64_t)id);
    if (JS_IsException(data)) {
        return data;
    }
    JSValue method = JS_NewCFunctionData(c, function, 0, op, 1, &data);
    JS_FreeValue(c, data);
    return method;
}

static JSValue runtime_process_child_call(
    JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int op, JSValueConst *data) {
    (void)self;
    gt_bridge_runtime_t *r = JS_GetContextOpaque(c);
    int64_t raw_id = 0;
    if (!r || !JS_IsNumber(data[0]) || JS_ToInt64(c, &raw_id, data[0]) || raw_id <= 0) {
        return JS_ThrowInternalError(c, "process handle unavailable");
    }
    runtime_process_child_t *slot = runtime_process_find(r, (uint64_t)raw_id);
    if (!slot) {
        if (op == 7) {
            return JS_UNDEFINED;
        }
        return JS_ThrowInternalError(c, "process closed");
    }
    if (op == 1 || op == 2) {
        double requested = 0;
        if (argc != 1 || !JS_IsNumber(argv[0]) || JS_ToFloat64(c, &requested, argv[0]) ||
            requested < 1 || requested > GT_EXT_PROCESS_IO_MAX || requested != (size_t)requested) {
            return JS_ThrowTypeError(c, "invalid process read size");
        }
        uint8_t bytes[GT_EXT_PROCESS_IO_MAX];
        gt_ext_process_io_t io =
            op == 1 ? gt__ext_process_read_stdout(slot->process, bytes, (size_t)requested)
                    : gt__ext_process_read_stderr(slot->process, bytes, (size_t)requested);
        const char *state = io.state == GT_EXT_PROCESS_DATA     ? "data"
                            : io.state == GT_EXT_PROCESS_WAIT   ? "wait"
                            : io.state == GT_EXT_PROCESS_EOF    ? "eof"
                            : io.state == GT_EXT_PROCESS_CLOSED ? "closed"
                                                                : "error";
        JSValue result = runtime_process_state(c, state);
        if (JS_IsException(result)) {
            return result;
        }
        if (io.state == GT_EXT_PROCESS_DATA) {
            if (io.count > r->process_output_limit - r->process_output_bytes) {
                uint64_t now;
                if (!runtime_now(&now) ||
                    !runtime_process_stop(slot, GT_RUNTIME_PROCESS_STOP_OUTPUT_LIMIT, now)) {
                    JS_FreeValue(c, result);
                    return JS_ThrowInternalError(c, "cannot stop process after output quota");
                }
                JSValue error = runtime_process_limit_error(c);
                if (JS_IsException(error)) {
                    JS_FreeValue(c, result);
                    return JS_EXCEPTION;
                }
                if (runtime_set(c, result, "state", JS_NewString(c, "error")) < 0) {
                    JS_FreeValue(c, error);
                    JS_FreeValue(c, result);
                    return JS_EXCEPTION;
                }
                if (runtime_set(c, result, "error", error) < 0) {
                    JS_FreeValue(c, result);
                    return JS_EXCEPTION;
                }
            } else {
                r->process_output_bytes += io.count;
                if (runtime_set(c, result, "bytes", JS_NewUint8ArrayCopy(c, bytes, io.count)) < 0) {
                    JS_FreeValue(c, result);
                    return JS_EXCEPTION;
                }
            }
        }
        if (io.state == GT_EXT_PROCESS_ERROR &&
            runtime_set(c, result, "error", runtime_process_error(c, io.error)) < 0) {
            JS_FreeValue(c, result);
            return JS_EXCEPTION;
        }
        return result;
    }
    if (op == 3) {
        if (argc != 1 || JS_GetTypedArrayType(argv[0]) != JS_TYPED_ARRAY_UINT8) {
            return JS_ThrowTypeError(c, "expected Uint8Array");
        }
        size_t offset = 0, size = 0, element = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(c, argv[0], &offset, &size, &element);
        if (JS_IsException(buffer)) {
            return JS_EXCEPTION;
        }
        size_t buffer_size = 0;
        uint8_t *base = JS_GetArrayBuffer(c, &buffer_size, buffer);
        if (!base || element != 1 || offset > buffer_size || size > buffer_size - offset) {
            JS_FreeValue(c, buffer);
            return JS_ThrowTypeError(c, "invalid process bytes");
        }
        gt_ext_process_io_t io = gt__ext_process_write(slot->process, base + offset, size);
        JS_FreeValue(c, buffer);
        const char *state = io.state == GT_EXT_PROCESS_DATA     ? "data"
                            : io.state == GT_EXT_PROCESS_WAIT   ? "wait"
                            : io.state == GT_EXT_PROCESS_CLOSED ? "closed"
                                                                : "error";
        JSValue result = runtime_process_state(c, state);
        if (JS_IsException(result) ||
            (io.state == GT_EXT_PROCESS_DATA &&
             runtime_set(c, result, "count", JS_NewUint32(c, (uint32_t)io.count)) < 0) ||
            (io.state == GT_EXT_PROCESS_ERROR &&
             runtime_set(c, result, "error", runtime_process_error(c, io.error)) < 0)) {
            JS_FreeValue(c, result);
            return JS_EXCEPTION;
        }
        return result;
    }
    if (op == 4) {
        return argc == 0 && gt__ext_process_close_stdin(slot->process) == GT_OK
                   ? JS_UNDEFINED
                   : JS_ThrowInternalError(c, "cannot close process stdin");
    }
    if (op == 5) {
        if (argc != 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(c, "invalid process signal");
        }
        const char *signal_name = JS_ToCString(c, argv[0]);
        if (!signal_name) {
            return JS_EXCEPTION;
        }
        int signal_number = !strcmp(signal_name, "terminate") ? SIGTERM
                            : !strcmp(signal_name, "kill")    ? SIGKILL
                                                              : 0;
        JS_FreeCString(c, signal_name);
        if (!signal_number || gt__ext_process_signal(slot->process, signal_number) != GT_OK) {
            return JS_ThrowTypeError(c, "unsupported process signal");
        }
        return JS_UNDEFINED;
    }
    if (op == 6 || op == 7) {
        if (argc) {
            return JS_ThrowTypeError(c, "invalid process operation");
        }
        gt_ext_process_info_t info;
        if (gt__ext_process_poll(slot->process, &info) != GT_OK) {
            return JS_ThrowInternalError(c, "cannot observe process");
        }
        if (op == 6) {
            JSValue result = runtime_process_state(c, info.exited ? "exited" : "running");
            if (!JS_IsException(result) && info.exited &&
                (runtime_set(c,
                             result,
                             "code",
                             info.term_signal ? JS_NULL : JS_NewInt32(c, info.exit_code)) < 0 ||
                 runtime_set(c,
                             result,
                             "signal",
                             info.term_signal ? JS_NewInt32(c, info.term_signal) : JS_NULL) < 0)) {
                JS_FreeValue(c, result);
                return JS_EXCEPTION;
            }
            const char *reason = slot->stop_reason == GT_RUNTIME_PROCESS_STOP_DEADLINE ? "deadline"
                                 : slot->stop_reason == GT_RUNTIME_PROCESS_STOP_OUTPUT_LIMIT
                                     ? "output-limit"
                                     : NULL;
            if (reason &&
                runtime_set(c, result, "terminationReason", JS_NewString(c, reason)) < 0) {
                JS_FreeValue(c, result);
                return JS_EXCEPTION;
            }
            return result;
        }
        if (info.exited) {
            if (gt__ext_process_destroy(&slot->process) != GT_OK) {
                return JS_ThrowInternalError(c, "cannot release process");
            }
            JS_FreeValue(c, slot->release_resolver);
            JS_FreeValue(c, slot->release_promise);
            memset(slot, 0, sizeof(*slot));
            slot->release_resolver = JS_UNDEFINED;
            slot->release_promise = JS_UNDEFINED;
            return runtime_process_resolved_promise(c, JS_UNDEFINED);
        }
        if (slot->releasing) {
            return JS_DupValue(c, slot->release_promise);
        }
        JSValue resolvers[2];
        JSValue promise = JS_NewPromiseCapability(c, resolvers);
        if (JS_IsException(promise)) {
            return promise;
        }
        slot->release_promise = JS_DupValue(c, promise);
        slot->release_resolver = resolvers[0];
        JS_FreeValue(c, resolvers[1]);
        slot->releasing = true;
        return promise;
    }
    return JS_ThrowTypeError(c, "invalid process operation");
}

static JSValue runtime_process_spawn_call(
    JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int magic) {
    (void)self;
    (void)magic;
    gt_bridge_runtime_t *r = JS_GetContextOpaque(c);
    if (!r || !r->process_service || argc != 1 || !JS_IsObject(argv[0]) ||
        atomic_load_explicit(&r->stopped, memory_order_relaxed)) {
        return JS_ThrowInternalError(c, "process service unavailable");
    }
    const char *program = NULL, *cwd = NULL, *policy = NULL;
    JSValue value = JS_GetPropertyStr(c, argv[0], "program");
    bool ok = resource_string(c, value, 128, &program);
    JS_FreeValue(c, value);
    value = JS_GetPropertyStr(c, argv[0], "cwd");
    JSValue cwd_id = JS_IsObject(value) ? JS_GetPropertyStr(c, value, "id") : JS_UNDEFINED;
    ok = ok && resource_string(c, cwd_id, 64, &cwd);
    JS_FreeValue(c, cwd_id);
    JS_FreeValue(c, value);
    value = JS_GetPropertyStr(c, argv[0], "envPolicy");
    ok = ok && resource_string(c, value, 64, &policy) &&
         !strcmp(policy, "selected-user-environment");
    JS_FreeValue(c, value);
    if (!ok) {
        JS_FreeCString(c, program);
        JS_FreeCString(c, cwd);
        JS_FreeCString(c, policy);
        return JS_ThrowTypeError(c, "invalid process spawn options");
    }
    int cwd_fd = -1;
    for (size_t i = 0; i < gt__runtime_resources_roots(r->resources); i++) {
        if (!strcmp(cwd, gt__runtime_resources_root(r->resources, i))) {
            cwd_fd = gt__runtime_resources_root_fd(r->resources, i);
            break;
        }
    }
    JS_FreeCString(c, cwd);
    JS_FreeCString(c, policy);
    if (cwd_fd < 0) {
        JS_FreeCString(c, program);
        return JS_ThrowTypeError(c, "workspace is outside the authorized roots");
    }
    JSValue args_value = JS_GetPropertyStr(c, argv[0], "argv");
    JSValue length_value = JS_GetPropertyStr(c, args_value, "length");
    uint32_t count = 0;
    bool valid = JS_IsArray(args_value) && !JS_ToUint32(c, &count, length_value) && count > 0 &&
                 count <= GT_EXT_PROCESS_ARGUMENTS_MAX;
    JS_FreeValue(c, length_value);
    const char *args[GT_EXT_PROCESS_ARGUMENTS_MAX] = {0};
    JSValue held[GT_EXT_PROCESS_ARGUMENTS_MAX];
    memset(held, 0, sizeof(held));
    for (uint32_t i = 0; valid && i < count; i++) {
        held[i] = JS_GetPropertyUint32(c, args_value, i);
        args[i] = JS_ToCString(c, held[i]);
        valid = args[i] != NULL;
    }
    JS_FreeValue(c, args_value);
    if (!valid) {
        for (uint32_t i = 0; i < count; i++) {
            if (args[i]) {
                JS_FreeCString(c, args[i]);
            }
            JS_FreeValue(c, held[i]);
        }
        JS_FreeCString(c, program);
        return JS_ThrowTypeError(c, "invalid process argv");
    }
    if (r->next_process_id >= INT64_MAX) {
        for (uint32_t i = 0; i < count; i++) {
            JS_FreeCString(c, args[i]);
            JS_FreeValue(c, held[i]);
        }
        JS_FreeCString(c, program);
        return JS_ThrowInternalError(c, "process handle space exhausted");
    }
    runtime_process_child_t *slot = NULL;
    for (size_t i = 0; i < GT_RUNTIME_PROCESS_CHILDREN; i++) {
        if (!r->process_children[i].process) {
            slot = &r->process_children[i];
            break;
        }
    }
    gt_status_t status = slot ? gt__ext_process_service_spawn(r->process_service,
                                                              program,
                                                              args,
                                                              count,
                                                              cwd_fd,
                                                              r->process_environment,
                                                              r->process_environment_count,
                                                              &slot->process)
                              : GT_ERR_UNSUPPORTED;
    for (uint32_t i = 0; i < count; i++) {
        JS_FreeCString(c, args[i]);
        JS_FreeValue(c, held[i]);
    }
    const char *version = gt__ext_process_service_version(r->process_service, program);
    JS_FreeCString(c, program);
    if (status != GT_OK || !slot || !version) {
        if (slot && slot->process) {
            (void)gt__ext_process_signal(slot->process, SIGKILL);
            (void)gt__ext_process_destroy(&slot->process);
        }
        /* Program denial and spawn IO are recoverable operation failures.
         * QuickJS-NG InternalError is uncatchable and would kill the Runtime. */
        return JS_ThrowPlainError(c, "authorized process could not start");
    }
    uint64_t now;
    if (!runtime_now(&now) || now > UINT64_MAX - r->process_lifetime_ms) {
        (void)gt__ext_process_signal(slot->process, SIGKILL);
        (void)gt__ext_process_destroy(&slot->process);
        return JS_ThrowInternalError(c, "process deadline clock unavailable");
    }
    r->next_process_id++;
    slot->id = r->next_process_id;
    slot->deadline = now + r->process_lifetime_ms;
    strcpy(slot->verified_version, version);
    slot->release_promise = JS_UNDEFINED;
    slot->release_resolver = JS_UNDEFINED;
    slot->releasing = false;
    JSValue child = JS_NewObjectProto(c, JS_NULL);
    if (JS_IsException(child) ||
        runtime_set(c, child, "verifiedVersion", JS_NewString(c, slot->verified_version)) < 0) {
        JS_FreeValue(c, child);
        return JS_EXCEPTION;
    }
    const char *names[] = {
        "readStdout", "readStderr", "writeStdin", "closeStdin", "signal", "poll", "release"};
    for (int i = 0; i < 7; i++) {
        JSValue method = runtime_process_method(c, runtime_process_child_call, i + 1, slot->id);
        if (runtime_set(c, child, names[i], method) < 0) {
            JS_FreeValue(c, child);
            return JS_EXCEPTION;
        }
    }
    JSValue promise = runtime_process_resolved_promise(c, child);
    JS_FreeValue(c, child);
    return promise;
}

static bool runtime_process_step(gt_bridge_runtime_t *r) {
    for (size_t i = 0; i < GT_RUNTIME_PROCESS_CHILDREN; i++) {
        runtime_process_child_t *slot = &r->process_children[i];
        if (!slot->process) {
            continue;
        }
        gt_ext_process_info_t info;
        gt_status_t status = gt__ext_process_poll(slot->process, &info);
        if (status != GT_OK) {
            return false;
        }
        uint64_t now;
        if (!runtime_now(&now)) {
            return false;
        }
        if (!info.exited && slot->stop_reason == GT_RUNTIME_PROCESS_STOP_NONE &&
            now >= slot->deadline &&
            !runtime_process_stop(slot, GT_RUNTIME_PROCESS_STOP_DEADLINE, now)) {
            return false;
        }
        if (!info.exited && slot->stop_reason != GT_RUNTIME_PROCESS_STOP_NONE && !slot->killed &&
            now >= slot->stop_deadline) {
            if (!gt__ext_process_reaping(slot->process) &&
                gt__ext_process_signal(slot->process, SIGKILL) != GT_OK) {
                return false;
            }
            slot->killed = true;
        }
        if ((info.exited && slot->releasing) || (r->process_stopping && info.exited)) {
            if (slot->releasing) {
                JSValue result = JS_Call(r->context,
                                         slot->release_resolver,
                                         JS_UNDEFINED,
                                         1,
                                         (JSValueConst[]){JS_UNDEFINED});
                if (JS_IsException(result)) {
                    JS_FreeValue(r->context, JS_GetException(r->context));
                }
                JS_FreeValue(r->context, result);
                JS_FreeValue(r->context, slot->release_resolver);
                JS_FreeValue(r->context, slot->release_promise);
            }
            if (gt__ext_process_destroy(&slot->process) != GT_OK) {
                return false;
            }
            memset(slot, 0, sizeof(*slot));
            slot->release_resolver = JS_UNDEFINED;
            slot->release_promise = JS_UNDEFINED;
        }
    }
    return true;
}

static bool runtime_process_live(const gt_bridge_runtime_t *r) {
    for (size_t i = 0; i < GT_RUNTIME_PROCESS_CHILDREN; i++) {
        if (r->process_children[i].process) {
            return true;
        }
    }
    return false;
}

static int runtime_bind(JSContext *c, JSModuleDef *module) {
    const char *names[] = {"request",
                           "result",
                           "cancel",
                           "forget",
                           "take",
                           "check",
                           "respond",
                           "revoke",
                           "alive",
                           "access",
                           "invocation",
                           "workspaceHandle"};
    for (int i = 0; i < 12; i++) {
        if (JS_SetModuleExport(
                c,
                module,
                names[i],
                JS_NewCFunctionMagic(c, runtime_native, names[i], 0, JS_CFUNC_generic_magic, i))) {
            return -1;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (JS_SetModuleExport(c,
                               module,
                               runtime_view_exports[i],
                               JS_NewCFunctionMagic(c,
                                                    runtime_view_native,
                                                    runtime_view_exports[i],
                                                    0,
                                                    JS_CFUNC_generic_magic,
                                                    i))) {
            return -1;
        }
    }
    for (int i = 0; i < 9; i++) {
        if (JS_SetModuleExport(c,
                               module,
                               runtime_resource_exports[i],
                               JS_NewCFunctionMagic(c,
                                                    runtime_resource_native,
                                                    runtime_resource_exports[i],
                                                    0,
                                                    JS_CFUNC_generic_magic,
                                                    i))) {
            return -1;
        }
    }
    if (JS_SetModuleExport(
            c,
            module,
            "processSpawn",
            JS_NewCFunctionMagic(
                c, runtime_process_spawn_call, "processSpawn", 1, JS_CFUNC_generic_magic, 0))) {
        return -1;
    }
    return 0;
}

static JSModuleDef *runtime_package_load(JSContext *ctx, const char *name, gt_bridge_runtime_t *r) {
    gt_runtime_source_t source;
    if (gt__runtime_package_source(r->package, name, &source) != GT_OK) {
        JS_ThrowReferenceError(ctx, "module unavailable");
        return NULL;
    }
    bool known = false;
    for (size_t i = 0; i < r->count; i++) {
        if (!strcmp(r->modules[i].name, name)) {
            known = true;
        }
    }
    if (source.size > 128 * 1024 ||
        (!known && (r->count >= 64 || source.size > 1024 * 1024 - r->info.module_bytes))) {
        goto memory;
    }
    char *copy = gt__plugin_memory_alloc(NULL, source.size + 1);
    if (!copy) {
        goto memory;
    }
    size_t used = 0;
    if (source.chunks) {
        for (size_t i = 0; i < source.chunk_count; i++) {
            size_t n = strlen(source.chunks[i]);
            if (n > source.size - used) {
                goto invalid;
            }
            memcpy(copy + used, source.chunks[i], n);
            used += n;
        }
    } else {
        if (source.size) {
            memcpy(copy, source.bytes, source.size);
        }
        used = source.size;
    }
    copy[used] = 0;
    gt_sha256_t hash;
    char digest[65];
    gt__sha256_init(&hash);
    if (used != source.size || !gt__runtime_source_utf8(copy, used) ||
        !gt__sha256_update(&hash, copy, used) || !gt__sha256_final(&hash, digest) ||
        strcmp(digest, source.sha256)) {
        goto invalid;
    }
    if (!known) {
        runtime_module_t *m = &r->modules[r->count];
        m->name = gt__plugin_memory_alloc(NULL, strlen(name) + 1);
        if (!m->name) {
            gt__plugin_memory_free(NULL, copy, source.size + 1);
            goto memory;
        }
        strcpy(m->name, name);
        m->size = source.size;
        r->count++;
        r->info.module_bytes += source.size;
    }
    JSValue value = JS_Eval(ctx, copy, used, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    gt__plugin_memory_free(NULL, copy, source.size + 1);
    if (JS_IsException(value)) {
        return NULL;
    }
    JSModuleDef *module = JS_VALUE_GET_PTR(value);
    JS_FreeValue(ctx, value);
    return module;
invalid:
    gt__plugin_memory_free(NULL, copy, source.size + 1);
    runtime_fail(r, GT_RUNTIME_SCRIPT);
    JS_ThrowReferenceError(ctx, "module unavailable");
    return NULL;
memory:
    r->memory_failed = true;
    runtime_fail(r, GT_RUNTIME_MEMORY);
    JS_ThrowOutOfMemory(ctx);
    return NULL;
}

static JSModuleDef *runtime_load(JSContext *ctx, const char *name, void *opaque) {
    gt_bridge_runtime_t *r = opaque;
    if (!strcmp(name, "host:/native")) {
        JSModuleDef *m = JS_NewCModule(ctx, name, runtime_bind);
        if (!m) {
            return NULL;
        }
        const char *names[] = {"request",
                               "result",
                               "cancel",
                               "forget",
                               "take",
                               "check",
                               "respond",
                               "revoke",
                               "alive",
                               "access",
                               "invocation",
                               "workspaceHandle",
                               "processSpawn"};
        for (int i = 0; i < 13; i++) {
            if (JS_AddModuleExport(ctx, m, names[i])) {
                return NULL;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (JS_AddModuleExport(ctx, m, runtime_view_exports[i])) {
                return NULL;
            }
        }
        for (int i = 0; i < 9; i++) {
            if (JS_AddModuleExport(ctx, m, runtime_resource_exports[i])) {
                return NULL;
            }
        }
        return m;
    }
    if (r->package) {
        return runtime_package_load(ctx, name, r);
    }
    for (size_t i = 0; i < r->count; i++) {
        if (!strcmp(name, r->modules[i].name)) {
            JSValue v = JS_Eval(ctx,
                                r->modules[i].source,
                                r->modules[i].size,
                                name,
                                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(v)) {
                return NULL;
            }
            JSModuleDef *m = JS_VALUE_GET_PTR(v);
            JS_FreeValue(ctx, v);
            return m;
        }
    }
    JS_ThrowReferenceError(ctx, "module unavailable");
    return NULL;
}

static void runtime_free_engine(gt_bridge_runtime_t *r) {
    if (r->views) {
        gt__secure_erase(r->views, sizeof(*r->views));
        runtime_free(r, r->views);
        r->views = NULL;
    }
    if (r->engine) {
        JS_SetHostPromiseRejectionTracker(r->engine, NULL, NULL);
    }
    for (size_t i = 0; i < r->unhandled_count; i++) {
        JS_FreeValue(r->context, r->unhandled[i]);
    }
    r->unhandled_count = 0;
    if (r->context) {
        JS_FreeValue(r->context, r->decoded_arguments);
        JS_FreeValue(r->context, r->module);
        JS_FreeValue(r->context, r->space);
        JS_FreeValue(r->context, r->pump);
        JS_FreeValue(r->context, r->dispose);
        JS_FreeContext(r->context);
        r->context = NULL;
    }
    if (r->engine) {
        JS_FreeRuntime(r->engine);
        r->engine = NULL;
    }
}

static bool runtime_free_modules(gt_bridge_runtime_t *r) {
    for (size_t i = 0; i < r->count; i++) {
        gt__plugin_memory_free(NULL, r->modules[i].source, r->modules[i].size + 1);
        r->modules[i].source = NULL;
        if (r->modules[i].name) {
            gt__plugin_memory_free(NULL, r->modules[i].name, strlen(r->modules[i].name) + 1);
            r->modules[i].name = NULL;
        }
    }
    gt__plugin_memory_free(NULL, r->invocation, r->invocation_size);
    r->invocation = NULL;
    r->invocation_size = 0;
    if (r->process_environment_block) {
        gt__secure_erase(r->process_environment_block, r->process_environment_bytes);
        gt__plugin_memory_free(NULL, r->process_environment_block, r->process_environment_bytes);
        r->process_environment_block = NULL;
        r->process_environment_bytes = r->process_environment_count = 0;
        memset(r->process_environment, 0, sizeof(r->process_environment));
    }
    if (r->package && gt__package_checked_destroy(&r->package) != GT_PACKAGE_OK) {
        r->module_cleanup_error = GT_ERR_IO;
        return false;
    }
    return true;
}

static gt_status_t runtime_invocation(gt_bridge_runtime_t *r,
                                      const gt_ext_invocation_t *input,
                                      gt_package_checked_t *package) {
    if (!input) {
        return GT_OK;
    }
    if (!input->command || input->argc > 32 || (input->argc && !input->argv)) {
        return GT_ERR_INVALID_ARG;
    }
    size_t command_size = strnlen(input->command, 520), token = 0, tokens = 1;
    if (!command_size || command_size >= 520) {
        return GT_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < command_size; i++) {
        unsigned char c = (unsigned char)input->command[i];
        if (c == ' ') {
            if (!token || ++tokens > 8) {
                return GT_ERR_INVALID_ARG;
            }
            token = 0;
        } else {
            if ((!token && !(c >= 'a' && c <= 'z')) ||
                !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || ++token > 64) {
                return GT_ERR_INVALID_ARG;
            }
        }
    }
    if (!token) {
        return GT_ERR_INVALID_ARG;
    }
    if (package) {
        bool found = false;
        gt_package_command_t declared;
        for (size_t i = 0; i < gt__package_checked_command_count(package); i++) {
            if (gt__package_checked_command(package, i, &declared) &&
                !strcmp(declared.path, input->command)) {
                found = true;
                memcpy(r->argument_schema, declared.args_schema, strlen(declared.args_schema) + 1);
                break;
            }
        }
        if (!found) {
            return GT_ERR_NOT_FOUND;
        }
    }
    size_t sizes[32], total = 0;
    for (size_t i = 0; i < input->argc; i++) {
        if (!input->argv[i]) {
            return GT_ERR_INVALID_ARG;
        }
        sizes[i] = strnlen(input->argv[i], 4097);
        if (sizes[i] > 4096 || sizes[i] + 1 > 16384 - total ||
            !gt__runtime_source_utf8(input->argv[i], sizes[i])) {
            return GT_ERR_INVALID_ARG;
        }
        total += sizes[i] + 1;
    }
    r->invocation_size = command_size + 1 + total;
    r->invocation = gt__plugin_memory_alloc(NULL, r->invocation_size);
    if (!r->invocation) {
        return GT_ERR_IO;
    }
    memcpy(r->invocation, input->command, command_size + 1);
    size_t at = command_size + 1;
    r->argument_count = input->argc;
    for (size_t i = 0; i < input->argc; i++) {
        r->argument_offsets[i] = at;
        memcpy(r->invocation + at, input->argv[i], sizes[i] + 1);
        at += sizes[i] + 1;
    }
    return GT_OK;
}

static gt_status_t runtime_process_environment_copy(gt_bridge_runtime_t *r,
                                                    const gt_runtime_config_t *config) {
    if (config->process_environment_count > GT_EXT_RUNTIME_PROCESS_ENV_MAX ||
        (config->process_environment_count && !config->process_environment)) {
        return GT_ERR_INVALID_ARG;
    }
    size_t total = 0;
    size_t name_sizes[GT_EXT_RUNTIME_PROCESS_ENV_MAX], value_sizes[GT_EXT_RUNTIME_PROCESS_ENV_MAX];
    for (size_t i = 0; i < config->process_environment_count; i++) {
        const gt_ext_process_environment_t *entry = &config->process_environment[i];
        if (!entry->name || !entry->value) {
            return GT_ERR_INVALID_ARG;
        }
        name_sizes[i] = strnlen(entry->name, 129);
        value_sizes[i] = strnlen(entry->value, GT_EXT_PROCESS_ENVIRONMENT_VALUE_MAX + 1);
        if (!name_sizes[i] || name_sizes[i] > 128 ||
            value_sizes[i] > GT_EXT_PROCESS_ENVIRONMENT_VALUE_MAX ||
            name_sizes[i] + value_sizes[i] + 2 > GT_EXT_PROCESS_ENVIRONMENT_VALUE_MAX) {
            return GT_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < name_sizes[i]; j++) {
            char c = entry->name[j];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                  (j > 0 && c >= '0' && c <= '9'))) {
                return GT_ERR_INVALID_ARG;
            }
        }
        for (size_t previous = 0; previous < i; previous++) {
            if (name_sizes[previous] == name_sizes[i] &&
                !memcmp(config->process_environment[previous].name, entry->name, name_sizes[i])) {
                return GT_ERR_INVALID_ARG;
            }
        }
        if (name_sizes[i] + value_sizes[i] + 2 > GT_EXT_PROCESS_ENVIRONMENT_BYTES_MAX - total) {
            return GT_ERR_INVALID_ARG;
        }
        total += name_sizes[i] + value_sizes[i] + 2;
    }
    if (!total) {
        return GT_OK;
    }
    r->process_environment_block = gt__plugin_memory_alloc(NULL, total);
    if (!r->process_environment_block) {
        return GT_ERR_IO;
    }
    r->process_environment_bytes = total;
    size_t at = 0;
    for (size_t i = 0; i < config->process_environment_count; i++) {
        const gt_ext_process_environment_t *entry = &config->process_environment[i];
        r->process_environment[i] = r->process_environment_block + at;
        memcpy(r->process_environment_block + at, entry->name, name_sizes[i]);
        at += name_sizes[i];
        r->process_environment_block[at++] = '=';
        memcpy(r->process_environment_block + at, entry->value, value_sizes[i]);
        at += value_sizes[i];
        r->process_environment_block[at++] = 0;
    }
    r->process_environment_count = config->process_environment_count;
    return GT_OK;
}

static gt_status_t runtime_open(gt_ext_session_t **session,
                                const gt_runtime_config_t *c,
                                gt_package_checked_t *package,
                                bool associated,
                                gt_bridge_runtime_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out || !session || !*session || !c ||
        (!package && (!c->modules || !c->module_count || c->module_count > 64 ||
                      !runtime_name(c->entry) || strncmp(c->entry, "plugin:/", 8))) ||
        (package && (c->modules || c->module_count || c->entry)) ||
        (c->role != GT_EXT_HOST && c->role != GT_EXT_EXECUTOR) ||
        c->root_count > GT_RUNTIME_ROOTS_MAX || (c->root_count && !c->roots) ||
        c->process_program_count > GT_EXT_PROCESS_PROGRAMS_MAX ||
        (c->process_program_count && !c->process_programs) ||
        c->process_environment_count > GT_EXT_RUNTIME_PROCESS_ENV_MAX ||
        (c->process_environment_count && !c->process_environment) ||
        c->process_lifetime_ms > GT_EXT_RUNTIME_PROCESS_LIFETIME_MS_MAX ||
        c->process_output_bytes > GT_EXT_RUNTIME_PROCESS_OUTPUT_BYTES_MAX) {
        return GT_ERR_INVALID_ARG;
    }
    gt_ext_session_info_t info;
    if (!gt_ext_session_info(*session, &info) || info.phase != GT_EXT_SESSION_ACTIVE ||
        !(*session)->requests || (*session)->sources ||
        !gt__bridge_session_runtime_admissible((*session)->inner)) {
        return GT_ERR_INVALID_ARG;
    }
    if (!package && c->root_count && !(*session)->store) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_bridge_role_t actual_role;
    size_t method_count;
    gt__bridge_session_methods((*session)->inner, &method_count, &actual_role);
    if ((unsigned)actual_role != (unsigned)c->role) {
        return GT_ERR_INVALID_ARG;
    }
    size_t memory = c->memory_bytes ? c->memory_bytes : 16 * 1024 * 1024,
           stack = c->stack_bytes ? c->stack_bytes : 256 * 1024;
    uint32_t cpu = c->cpu_ms ? c->cpu_ms : 25, jobs = c->jobs_per_step ? c->jobs_per_step : 32;
    if (c->activation_ms > 60000) {
        return GT_ERR_INVALID_ARG;
    }
    if (memory < 1024 * 1024 || memory > 128 * 1024 * 1024 || stack < 64 * 1024 ||
        stack > 1024 * 1024 || cpu > 1000 || jobs > 128) {
        return GT_ERR_INVALID_ARG;
    }
    gt_bridge_runtime_t *r = gt__plugin_memory_calloc(1, sizeof(*r));
    if (!r) {
        return GT_ERR_IO;
    }
    atomic_init(&r->stopped, false);
    r->cpu_ms = cpu;
    r->jobs_per_step = jobs;
    r->activation_ms = c->activation_ms ? c->activation_ms : 5000;
    r->role = c->role;
    if (!package) {
        strcpy(r->entry, c->entry);
    }
    r->module = r->space = r->pump = r->dispose = r->decoded_arguments = JS_UNDEFINED;
    gt_status_t status = runtime_invocation(r, c->invocation, package);
    if (status == GT_OK) {
        status = runtime_process_environment_copy(r, c);
    }
    bool process_required = false, pipes_required = false;
    if (status != GT_OK) {
        goto fail;
    }
    status = GT_ERR_INVALID_ARG;
    bool entry = package != NULL;
    for (size_t i = 0; i < c->module_count; i++) {
        const gt_runtime_module_t *m = &c->modules[i];
        if (!runtime_name(m->name) || !m->source || !m->size || m->size > 128 * 1024 ||
            memchr(m->source, 0, m->size) || r->info.module_bytes + m->size > 1024 * 1024 ||
            strnlen(m->sha256, 65) != 64) {
            goto fail;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(m->name, r->modules[j].name)) {
                goto fail;
            }
        }
        gt_sha256_t hash;
        char hex[65];
        gt__sha256_init(&hash);
        if (!gt__sha256_update(&hash, m->source, m->size) || !gt__sha256_final(&hash, hex) ||
            strcmp(hex, m->sha256)) {
            status = GT_ERR_PROTOCOL;
            goto fail;
        }
        runtime_module_t *copy = &r->modules[r->count++];
        copy->size = m->size;
        copy->name = gt__plugin_memory_alloc(NULL, strlen(m->name) + 1);
        if (!copy->name) {
            status = GT_ERR_IO;
            goto fail;
        }
        strcpy(copy->name, m->name);
        copy->source = gt__plugin_memory_alloc(NULL, m->size + 1);
        if (!copy->source) {
            status = GT_ERR_IO;
            goto fail;
        }
        memcpy(copy->source, m->source, m->size);
        copy->source[m->size] = 0;
        r->info.module_bytes += m->size;
        if (!strcmp(m->name, c->entry)) {
            entry = true;
        }
    }
    /* The allocator is the authoritative local and shared limit. An additional
  * QuickJS limit could reject before our allocator observes the failure, hiding
  * a caught OOM from the native sticky error / lease revocation. */
    if (!entry) {
        goto fail;
    }
    r->heap_limit = memory;
    const JSMallocFunctions functions = {
        runtime_calloc, runtime_malloc, runtime_free, runtime_realloc, runtime_usable};
    r->engine = JS_NewRuntime2(&functions, r);
    if (!r->engine) {
        status = GT_ERR_IO;
        goto fail;
    }
    /* The native owner serializes calls, but a dispatch queue may move them
     * between OS threads. QuickJS caches the stack top at runtime creation. */
    JS_UpdateStackTop(r->engine);
    JS_SetCanBlock(r->engine, false);
    JS_SetMaxStackSize(r->engine, stack);
    r->context = JS_NewContext(r->engine);
    if (!r->context) {
        status = GT_ERR_IO;
        goto fail;
    }
    JS_SetContextOpaque(r->context, r);
    JS_SetModuleLoaderFunc(r->engine, runtime_normalize, runtime_load, r);
    JS_SetInterruptHandler(r->engine, runtime_interrupt, r);
    JS_SetHostPromiseRejectionTracker(r->engine, runtime_rejection, r);
    if (!gt_ext_session_lease(*session, &r->lease)) {
        goto fail;
    }
    if (package) {
        gt_bridge_lease_t lease;
        memcpy(&lease, &r->lease, sizeof(lease));
        status = gt__runtime_package_entry(package,
                                           (*session)->inner,
                                           (gt_bridge_role_t)c->role,
                                           &lease,
                                           associated,
                                           r->entry,
                                           sizeof(r->entry));
        if (status != GT_OK) {
            goto fail;
        }
        gt_package_check_info_t package_info;
        if (!gt__package_checked_info(package, &package_info)) {
            goto fail;
        }
        for (size_t i = 0; i < package_info.requires_count; i++) {
            const char *name = package_info.requires[i];
            if ((!strcmp(name, "resource.receive") || !strcmp(name, "file.readScoped")) &&
                !(*session)->store) {
                status = GT_ERR_UNSUPPORTED;
                goto fail;
            }
            if (!strcmp(name, "file.readScoped") &&
                (!c->root_count || gt_ext_session_access(*session, &r->lease, "resource.receive") !=
                                       GT_EXT_ACCESS_ALLOWED)) {
                status = GT_ERR_UNSUPPORTED;
                goto fail;
            }
            if (!strcmp(name, "process.spawn") || !strcmp(name, "process.pipes")) {
                if (!package_info.executor || !package_info.process_program_count ||
                    !c->process_program_count || !c->root_count) {
                    status = GT_ERR_UNSUPPORTED;
                    goto fail;
                }
                process_required |= !strcmp(name, "process.spawn");
                pipes_required |= !strcmp(name, "process.pipes");
            }
        }
        if (process_required != pipes_required ||
            ((c->process_program_count || c->process_environment_count || c->process_lifetime_ms ||
              c->process_output_bytes) &&
             (!process_required || !pipes_required))) {
            status = GT_ERR_UNSUPPORTED;
            goto fail;
        }
        if (process_required) {
            r->process_lifetime_ms = c->process_lifetime_ms
                                         ? c->process_lifetime_ms
                                         : GT_RUNTIME_PROCESS_LIFETIME_DEFAULT_MS;
            r->process_output_limit = c->process_output_bytes
                                          ? c->process_output_bytes
                                          : GT_RUNTIME_PROCESS_OUTPUT_DEFAULT_BYTES;
            status = gt__ext_process_service_open(*session,
                                                  &r->lease,
                                                  package,
                                                  c->process_programs,
                                                  c->process_program_count,
                                                  &r->process_service);
            if (status != GT_OK) {
                goto fail;
            }
        }
        if (!gt__package_checked_retain(package, &r->package)) {
            status = GT_ERR_IO;
            goto fail;
        }
    }
    if ((*session)->store || process_required) {
        status = gt__runtime_resources_open(*session,
                                            (*session)->store,
                                            &r->lease,
                                            c->roots,
                                            c->root_count,
                                            NULL,
                                            runtime_resource_clock,
                                            &r->resources);
        if (status != GT_OK) {
            goto fail;
        }
        r->resources_enabled = (*session)->store != NULL;
    }
    r->session = *session;
    *session = NULL;
    *out = r;
    return GT_OK;
fail:
    runtime_free_engine(r);
    if (r->process_service) {
        (void)gt__ext_process_service_destroy(&r->process_service);
    }
    if (r->resources) {
        (void)gt__runtime_resources_destroy(&r->resources);
    }
    runtime_free_modules(r);
    gt__secure_erase(r, sizeof(*r));
    gt__plugin_memory_free(NULL, r, sizeof(*r));
    return status;
}

gt_status_t gt__bridge_runtime_open(gt_ext_session_t **s,
                                    const gt_runtime_config_t *c,
                                    gt_bridge_runtime_t **out) {
    return runtime_open(s, c, NULL, false, out);
}

gt_status_t gt__bridge_runtime_open_package(gt_ext_session_t **s,
                                            gt_package_checked_t *p,
                                            const gt_runtime_config_t *c,
                                            bool associated,
                                            gt_bridge_runtime_t **out) {
    if (!p) {
        if (out) {
            *out = NULL;
        }
        return GT_ERR_INVALID_ARG;
    }
    return runtime_open(s, c, p, associated, out);
}

static bool runtime_start(gt_bridge_runtime_t *r) {
    char quoted[GT_RUNTIME_MODULE_NAME_MAX * 2 + 3];
    size_t at = 0;
    quoted[at++] = '"';
    for (const char *p = r->entry; *p; p++) {
        if (*p == '"' || *p == '\\') {
            quoted[at++] = '\\';
        }
        quoted[at++] = *p;
    }
    quoted[at++] = '"';
    quoted[at] = 0;
    char script[GT_RUNTIME_MODULE_NAME_MAX * 2 + 4096];
    int n = snprintf(
        script,
        sizeof(script),
        "import {SessionChannel} from 'sdk:/index.js';import * as port from 'host:/native';import "
        "{activate} from %s;"
        "const session=new SessionChannel(port);let state=0,disposable;const "
        "cancellation=Object.freeze({get cancelled(){return !port.alive()}});"
        "%sconst process=%s;%sconst "
        "invocation=port.invocation();if(invocation){Object.freeze(invocation.arguments);const "
        "workspace=port.workspaceHandle();if(workspace)invocation.workspaceHandle=Object.freeze("
        "workspace);"
        "Object.freeze(invocation)}const "
        "context=Object.freeze({invocation,session,side:'%s',instanceId:'%s',epoch:'%" PRIu64
        "',cancellation,capabilities:Object.freeze({query(name){return "
        "port.access(name)}})%s%s%s});"
        "export function pump(){session.step();resources?.step();if(state>0&&typeof "
        "disposable.step==='function')disposable.step();return state}export function "
        "dispose(){views?.dispose();session.dispose();resources?.dispose();disposable?.dispose()}"
        "Promise.resolve().then(()=>activate(context)).then(value=>{if(!value || typeof "
        "value.dispose!=='function')throw "
        "Error('entry');disposable=value;state=1},()=>{state=-1}).catch(()=>{state=-1});",
        quoted,
        r->resources_enabled ? "const {ResourceBroker}=await import('sdk:/resources.js');const "
                               "resources=new ResourceBroker(port);"
                             : "const resources=undefined;",
        r->process_service ? "Object.freeze({spawn:port.processSpawn})" : "undefined",
        r->views ? "const {ViewRegistry}=await import('sdk:/views.js');const views=new "
                   "ViewRegistry(port);"
                 : "const views=undefined;",
        r->role == GT_EXT_HOST ? "client" : "executor",
        r->lease.instance,
        r->lease.epoch,
        r->resources_enabled ? ",resources" : "",
        r->process_service ? ",process" : "",
        r->views ? ",views" : "");
    if (n < 0 || (size_t)n >= sizeof(script)) {
        runtime_fail(r, GT_RUNTIME_PROTOCOL);
        return false;
    }
    JSValue compiled = JS_Eval(r->context,
                               script,
                               (size_t)n,
                               "host:/bootstrap.js",
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        runtime_exception(r);
        return false;
    }
    JSModuleDef *m = JS_VALUE_GET_PTR(compiled);
    r->module = JS_EvalFunction(r->context, compiled);
    if (JS_IsException(r->module)) {
        r->module = JS_UNDEFINED;
        runtime_exception(r);
        return false;
    }
    r->space = JS_GetModuleNamespace(r->context, m);
    if (JS_IsException(r->space)) {
        r->space = JS_UNDEFINED;
        runtime_exception(r);
        return false;
    }
    JS_PromiseMarkAsHandled(r->context, r->module);
    return true;
}

static bool runtime_jobs(gt_bridge_runtime_t *r, uint32_t *used) {
    while (JS_IsJobPending(r->engine)) {
        if (*used >= r->jobs_per_step) {
            runtime_fail(r, GT_RUNTIME_JOBS);
            return false;
        }
        JSContext *ctx = NULL;
        if (JS_ExecutePendingJob(r->engine, &ctx) < 0) {
            runtime_exception(r);
            return false;
        }
        (*used)++;
        r->info.jobs++;
        if (r->memory_failed) {
            runtime_fail(r, GT_RUNTIME_MEMORY);
            return false;
        }
        if (r->info.phase >= GT_RUNTIME_FAILED) {
            return false;
        }
    }
    return true;
}

gt_runtime_phase_t gt__bridge_runtime_step(gt_bridge_runtime_t *r) {
    if (!r) {
        return GT_RUNTIME_CLOSED;
    }
    if (r->info.phase >= GT_RUNTIME_FAILED) {
        return r->info.phase;
    }
    JS_UpdateStackTop(r->engine);
    if (atomic_load_explicit(&r->stopped, memory_order_relaxed)) {
        runtime_fail(r, GT_RUNTIME_CANCELLED);
        return r->info.phase;
    }
    if (!runtime_process_step(r)) {
        runtime_fail(r, GT_RUNTIME_IO);
        return r->info.phase;
    }
    uint64_t now, cpu_now;
    if (!runtime_cpu_now(&cpu_now)) {
        runtime_fail(r, GT_RUNTIME_IO);
        return r->info.phase;
    }
    r->deadline = cpu_now + r->cpu_ms;
    r->interrupted = false;
    r->info.steps++;
    gt_ext_session_phase_t native = gt_ext_session_step(r->session, 0);
    gt_ext_session_info_t info;
    if (gt_ext_session_info(r->session, &info)) {
        r->info.session_end = info.end;
        r->info.session_error = info.error;
    }
    if (native != GT_EXT_SESSION_ACTIVE) {
        runtime_fail(r, GT_RUNTIME_PROTOCOL);
        return r->info.phase;
    }
    gt__runtime_resources_step(r->resources);
    if (!runtime_now(&now) || !runtime_cpu_now(&cpu_now)) {
        runtime_fail(r, GT_RUNTIME_IO);
        return r->info.phase;
    }
    r->deadline = cpu_now + r->cpu_ms;
    if (!r->started) {
        r->started = true;
        r->activation_deadline = now + r->activation_ms;
        r->info.phase = GT_RUNTIME_STARTING;
        if (!runtime_start(r)) {
            return r->info.phase;
        }
    }
    uint32_t used = 0;
    if (!runtime_jobs(r, &used)) {
        return r->info.phase;
    }
    JSPromiseStateEnum module = JS_PromiseState(r->context, r->module);
    if (module == JS_PROMISE_REJECTED) {
        runtime_fail(r,
                     atomic_load_explicit(&r->stopped, memory_order_relaxed) ? GT_RUNTIME_CANCELLED
                     : r->interrupted                                        ? GT_RUNTIME_CPU
                                                                             : GT_RUNTIME_SCRIPT);
        return r->info.phase;
    }
    if (module != JS_PROMISE_PENDING) {
        if (JS_IsUndefined(r->pump)) {
            r->pump = JS_GetPropertyStr(r->context, r->space, "pump");
            r->dispose = JS_GetPropertyStr(r->context, r->space, "dispose");
            if (JS_IsException(r->pump) || JS_IsException(r->dispose)) {
                runtime_exception(r);
                return r->info.phase;
            }
        }
        JSValue value = JS_Call(r->context, r->pump, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(value)) {
            runtime_exception(r);
            return r->info.phase;
        }
        int32_t state;
        if (JS_ToInt32(r->context, &state, value)) {
            JS_FreeValue(r->context, value);
            runtime_exception(r);
            return r->info.phase;
        }
        JS_FreeValue(r->context, value);
        if (state < 0) {
            runtime_fail(r, GT_RUNTIME_SCRIPT);
            return r->info.phase;
        }
        if (state > 0) {
            r->info.phase = GT_RUNTIME_ACTIVE;
        }
        if (!runtime_jobs(r, &used)) {
            return r->info.phase;
        }
    }
    if (r->memory_failed) {
        runtime_fail(r, GT_RUNTIME_MEMORY);
    } else if (r->rejected || r->unhandled_count) {
        runtime_fail(r, GT_RUNTIME_SCRIPT);
    } else if (r->info.phase == GT_RUNTIME_STARTING &&
               (!runtime_now(&now) || now >= r->activation_deadline)) {
        runtime_fail(r, GT_RUNTIME_EXPIRED);
    }
    return r->info.phase;
}

bool gt__bridge_runtime_info(const gt_bridge_runtime_t *r, gt_runtime_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!r || !out) {
        return false;
    }
    *out = r->info;
    out->engine_released = r->engine == NULL;
    return true;
}

void gt__bridge_runtime_stop(gt_bridge_runtime_t *r) {
    if (r) {
        atomic_store_explicit(&r->stopped, true, memory_order_relaxed);
    }
}

gt_ext_close_t gt__bridge_runtime_close(gt_bridge_runtime_t *r) {
    if (!r) {
        return GT_EXT_CLOSE_ERROR;
    }
    if (r->info.phase == GT_RUNTIME_CLOSED) {
        return GT_EXT_RELEASED;
    }
    if (r->engine) {
        JS_UpdateStackTop(r->engine);
    }
    gt_ext_session_revoke(r->session);
    gt_ext_session_stop(r->session);
    gt__runtime_resources_stop(r->resources);
    atomic_store_explicit(&r->stopped, true, memory_order_relaxed);
    if (!r->process_stopping) {
        uint64_t now;
        if (!runtime_now(&now)) {
            r->info.cleanup_error = GT_ERR_IO;
            return GT_EXT_CLOSE_ERROR;
        }
        r->process_stopping = true;
        r->process_stop_deadline = now + 250;
        for (size_t i = 0; i < GT_RUNTIME_PROCESS_CHILDREN; i++) {
            runtime_process_child_t *slot = &r->process_children[i];
            if (slot->process) {
                gt_ext_process_info_t info;
                if (gt__ext_process_poll(slot->process, &info) != GT_OK ||
                    (!info.exited && !gt__ext_process_reaping(slot->process) &&
                     gt__ext_process_signal(slot->process, SIGTERM) != GT_OK)) {
                    r->info.cleanup_error = GT_ERR_IO;
                    return GT_EXT_CLOSE_ERROR;
                }
            }
        }
    }
    if (!r->dispose_called && r->context && JS_IsFunction(r->context, r->dispose)) {
        uint64_t dispose_cpu_now;
        if (!runtime_cpu_now(&dispose_cpu_now)) {
            r->info.cleanup_error = GT_ERR_IO;
            return GT_EXT_CLOSE_ERROR;
        }
        /* Grants and alive() remain revoked. Only this synchronous cleanup
         * call ignores stop, with a fresh copy of the same configured CPU
         * quantum; an unbounded disposer must still be interrupted. */
        r->deadline = dispose_cpu_now + r->cpu_ms;
        r->interrupted = false;
        r->disposing = true;
        JSValue v = JS_Call(r->context, r->dispose, JS_UNDEFINED, 0, NULL);
        r->disposing = false;
        if (JS_IsException(v)) {
            JSValue error = JS_GetException(r->context);
            JS_FreeValue(r->context, error);
            r->info.dispose_error = true;
        }
        JS_FreeValue(r->context, v);
    }
    r->dispose_called = true;
    if (!runtime_process_step(r)) {
        r->info.cleanup_error = GT_ERR_IO;
        return GT_EXT_CLOSE_ERROR;
    }
    if (runtime_process_live(r)) {
        uint64_t now;
        if (!runtime_now(&now)) {
            r->info.cleanup_error = GT_ERR_IO;
            return GT_EXT_CLOSE_ERROR;
        }
        if (!r->process_killed && now >= r->process_stop_deadline) {
            for (size_t i = 0; i < GT_RUNTIME_PROCESS_CHILDREN; i++) {
                runtime_process_child_t *slot = &r->process_children[i];
                if (slot->process) {
                    gt_ext_process_info_t info;
                    if (gt__ext_process_poll(slot->process, &info) != GT_OK ||
                        (!info.exited && !gt__ext_process_reaping(slot->process) &&
                         gt__ext_process_signal(slot->process, SIGKILL) != GT_OK)) {
                        r->info.cleanup_error = GT_ERR_IO;
                        return GT_EXT_CLOSE_ERROR;
                    }
                }
            }
            r->process_killed = true;
        }
        return GT_EXT_DRAINING;
    }
    if (r->process_service) {
        gt_status_t process_status = gt__ext_process_service_destroy(&r->process_service);
        if (process_status != GT_OK) {
            r->info.cleanup_error = process_status;
            return GT_EXT_CLOSE_ERROR;
        }
    }
    runtime_free_engine(r);
    if (!runtime_free_modules(r)) {
        r->info.cleanup_error = r->module_cleanup_error;
        return GT_EXT_CLOSE_ERROR;
    }
    gt_ext_close_t close = gt_ext_session_close(r->session, 0);
    if (r->resources) {
        gt_runtime_resource_status_t status = gt__runtime_resources_destroy(&r->resources);
        if (status != GT_RUNTIME_RESOURCE_OK) {
            if (status == GT_RUNTIME_RESOURCE_WAIT) {
                return GT_EXT_DRAINING;
            }
            r->info.cleanup_error = GT_ERR_IO;
            return GT_EXT_CLOSE_ERROR;
        }
        if (close == GT_EXT_DRAINING) {
            close = gt_ext_session_close(r->session, 0);
        }
    }
    if (close != GT_EXT_RELEASED) {
        if (close == GT_EXT_CLOSE_ERROR) {
            r->info.cleanup_error = GT_ERR_IO;
        }
        return close;
    }
    gt_ext_session_info_t snapshot;
    if (gt_ext_session_info(r->session, &snapshot)) {
        r->info.session_end = snapshot.end;
        r->info.session_error = snapshot.error;
    }
    r->transport_process_status = gt_ext_session_process_info(r->session, &r->transport_process);
    gt_status_t status = gt_ext_session_destroy(&r->session);
    if (status != GT_OK) {
        r->info.cleanup_error = status;
        return GT_EXT_CLOSE_ERROR;
    }
    r->info.cleanup_error = r->module_cleanup_error;
    r->info.phase = GT_RUNTIME_CLOSED;
    return GT_EXT_RELEASED;
}

gt_status_t gt__bridge_runtime_transport_process_info(const gt_bridge_runtime_t *r,
                                                      gt_ext_transport_process_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!r || !out) {
        return GT_ERR_INVALID_ARG;
    }
    if (r->session) {
        return gt_ext_session_process_info(r->session, out);
    }
    if (r->transport_process_status == GT_OK) {
        *out = r->transport_process;
    }
    return r->transport_process_status;
}

gt_status_t gt__bridge_runtime_destroy(gt_bridge_runtime_t **out) {
    if (!out || !*out || (*out)->info.phase != GT_RUNTIME_CLOSED) {
        return GT_ERR_INVALID_ARG;
    }
    gt_bridge_runtime_t *r = *out;
    runtime_free_modules(r);
    gt__secure_erase(r, sizeof(*r));
    gt__plugin_memory_free(NULL, r, sizeof(*r));
    *out = NULL;
    return GT_OK;
}
