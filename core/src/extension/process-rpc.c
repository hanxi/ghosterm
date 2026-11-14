#define _POSIX_C_SOURCE 200809L
#include "extension/process-rpc.h"
#include "extension/session-owner.h"
#include "extension/session.h"
#include "extension/memory.h"
#include "base/secure-random.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct process_json process_json_t;
#define GT_JSON_TAG process_json
#define GT_JSON_DOC process_json_t
#define GT_JSON_ERROR unsigned
#define GT_JSON_NODES 4096u
#define GT_JSON_BYTES GT_PROCESS_RPC_MESSAGE_MAX
#define GT_JSON_DEPTH 32u
#define GT_JSON_KEYS 8192u
#define GT_JSON_WORK 262144u
#define GT_JSON_LIMIT 1u
#define GT_JSON_SYNTAX 2u
#define GT_JSON_UTF8 3u
#define GT_JSON_DUPLICATE 4u
#include "base/json-reader.inc"

typedef struct {
    uint64_t local, native;
} process_reference_t;

struct gt_process_rpc {
    gt_allocator_t allocator;
    gt_ext_session_t *session;
    gt_ext_lease_t lease;
    gt_runtime_resources_t *resources;
    process_json_t *json;
    uint64_t sequence, reference;
    process_reference_t outgoing[64], incoming[64];
    process_reference_t publications[64], snapshots[64];
    char reply[GT_PROCESS_RPC_MESSAGE_MAX + 1];
    size_t reply_size;
    bool failed, activated;
};

static uint16_t process_member(const process_json_t *d, const char *key) {
    for (uint16_t i = d->nodes[1].child; i; i = d->nodes[i].next) {
        const json_node_t *n = &d->nodes[i];
        if (n->key_size == strlen(key) && !memcmp(d->strings + n->key, key, n->key_size)) {
            return i;
        }
    }
    return 0;
}

static bool process_text(const process_json_t *d, uint16_t id, char *out, size_t cap) {
    const json_node_t *n = &d->nodes[id];
    if (!id || n->kind != J_STRING || !n->size || n->size >= cap ||
        memchr(d->strings + n->text, 0, n->size)) {
        return false;
    }
    memcpy(out, d->strings + n->text, n->size);
    out[n->size] = 0;
    return true;
}

static bool process_counter(const process_json_t *d, uint16_t id, uint64_t *out) {
    char text[21];
    if (!process_text(d, id, text, sizeof(text)) || text[0] == '0') {
        return false;
    }
    uint64_t n = 0;
    for (size_t i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9' || n > (UINT64_MAX - (text[i] - '0')) / 10) {
            return false;
        }
        n = n * 10 + (unsigned)(text[i] - '0');
    }
    *out = n;
    return true;
}

static bool process_keys(const process_json_t *d, const char *const *keys, size_t count) {
    if (d->nodes[1].kind != J_OBJECT || d->nodes[1].count != count) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!process_member(d, keys[i])) {
            return false;
        }
    }
    return true;
}

static process_reference_t *process_find(process_reference_t *map, uint64_t local) {
    for (size_t i = 0; i < 64; i++) {
        if (map[i].local == local) {
            return &map[i];
        }
    }
    return NULL;
}

static process_reference_t *process_empty(process_reference_t *map) {
    return process_find(map, 0);
}

static bool process_append(gt_process_rpc_t *p, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(p->reply + p->reply_size, sizeof(p->reply) - p->reply_size, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(p->reply) - p->reply_size) {
        return false;
    }
    p->reply_size += (size_t)n;
    return true;
}

static bool process_status(gt_process_rpc_t *p, gt_ext_rpc_status_t status) {
    static const char *const names[] = {"ok", "wait", "invalid", "denied", "unsupported", "closed"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) &&
           process_append(
               p, "{\"v\":1,\"id\":\"%" PRIu64 "\",\"status\":\"%s\"", p->sequence, names[status]);
}

static bool process_raw(const process_json_t *d, uint16_t id, const void **bytes, size_t *size) {
    if (!id || d->nodes[id].bytes > GT_EXT_RPC_BODY_MAX) {
        return false;
    }
    *bytes = d->input + d->nodes[id].start;
    *size = d->nodes[id].bytes;
    return true;
}

static bool process_name(const char *text) {
    if (text[0] < 'a' || text[0] > 'z') {
        return false;
    }
    for (size_t i = 1; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

#include "extension/process-resource.inc"

/* Validate every field before any native mutation. Replies have fixed overhead
 * plus at most one 3072-byte raw JSON value; never serialize plugin strings. */
static bool process_dispatch(gt_process_rpc_t *p, const char *op) {
    process_json_t *d = p->json;
    static const char *const basic[] = {"v", "id", "op"};
    static const char *const reference[] = {"v", "id", "op", "ref"};
    static const char *const request[] = {"v", "id", "op", "method", "input", "timeoutMs"};
    static const char *const take[] = {"v", "id", "op", "methods"};
    static const char *const respond[] = {"v", "id", "op", "ref", "outcome"};
    static const char *const access[] = {"v", "id", "op", "capability"};
    if (!strcmp(op, "resource")) {
        return process_resource_dispatch(p);
    }
    if (!strcmp(op, "activated")) {
        if (!process_keys(d, basic, 3) || p->activated) {
            return false;
        }
        p->activated = true;
        return process_status(p, GT_EXT_RPC_OK);
    }
    if (!strcmp(op, "request")) {
        char method[129];
        uint64_t timeout;
        const void *json;
        size_t size;
        if (!process_keys(d, request, 6) ||
            !process_text(d, process_member(d, "method"), method, sizeof(method)) ||
            !process_name(method) || !node_uint(d, process_member(d, "timeoutMs"), &timeout) ||
            !timeout || timeout > 3600000 ||
            !process_raw(d, process_member(d, "input"), &json, &size)) {
            return false;
        }
        process_reference_t *slot = process_empty(p->outgoing);
        if (!slot || p->reference == UINT64_MAX) {
            return process_status(p, GT_EXT_RPC_WAIT);
        }
        uint64_t handle = 0;
        gt_ext_rpc_status_t status = gt_ext_session_request(
            p->session, &p->lease, method, json, size, (uint32_t)timeout, &handle);
        if (!process_status(p, status)) {
            return false;
        }
        if (status == GT_EXT_RPC_OK) {
            *slot = (process_reference_t){++p->reference, handle};
            return process_append(p, ",\"handle\":\"%" PRIu64 "\"", slot->local);
        }
        return true;
    }
    if (!strcmp(op, "take")) {
        uint16_t array = process_member(d, "methods");
        char storage[16][129];
        const char *methods[16];
        if (!process_keys(d, take, 4) || d->nodes[array].kind != J_ARRAY ||
            d->nodes[array].count > 16) {
            return false;
        }
        size_t count = 0;
        for (uint16_t i = d->nodes[array].child; i; i = d->nodes[i].next) {
            if (!process_text(d, i, storage[count], sizeof(storage[count])) ||
                !process_name(storage[count])) {
                return false;
            }
            methods[count] = storage[count];
            for (size_t j = 0; j < count; j++) {
                if (!strcmp(methods[j], methods[count])) {
                    return false;
                }
            }
            count++;
        }
        process_reference_t *slot = process_empty(p->incoming);
        if (!slot || p->reference == UINT64_MAX || !count) {
            return process_status(p, GT_EXT_RPC_WAIT);
        }
        gt_ext_rpc_request_t q;
        gt_ext_rpc_status_t status =
            gt__ext_session_take_request_methods(p->session, &p->lease, methods, count, &q);
        if (!process_status(p, status)) {
            return false;
        }
        if (status == GT_EXT_RPC_OK) {
            *slot = (process_reference_t){++p->reference, q.ticket};
            return process_append(p,
                                  ",\"ticket\":\"%" PRIu64 "\",\"method\":\"%s\",\"input\":%.*s",
                                  slot->local,
                                  q.method,
                                  (int)q.input_size,
                                  q.input);
        }
        return true;
    }
    if (!strcmp(op, "revoke")) {
        if (!process_keys(d, basic, 3)) {
            return false;
        }
        gt__process_rpc_revoke(p);
        return process_status(p, GT_EXT_RPC_OK);
    }
    if (!strcmp(op, "access")) {
        char capability[129];
        if (!process_keys(d, access, 4) ||
            !process_text(d, process_member(d, "capability"), capability, sizeof(capability)) ||
            !process_name(capability)) {
            return false;
        }
        gt_ext_access_t status = gt_ext_session_access(p->session, &p->lease, capability);
        static const char *const names[] = {
            "unknown", "allowed", "denied", "revoked", "unsupported"};
        return (unsigned)status < sizeof(names) / sizeof(names[0]) &&
               process_status(p, GT_EXT_RPC_OK) &&
               process_append(p, ",\"access\":\"%s\"", names[status]);
    }
    bool response = !strcmp(op, "respond"), incoming = response || !strcmp(op, "check");
    if (!incoming && strcmp(op, "result") && strcmp(op, "cancel") && strcmp(op, "forget")) {
        return false;
    }
    uint64_t local;
    const void *outcome = NULL;
    size_t outcome_size = 0;
    if (!process_keys(d, response ? respond : reference, response ? 5 : 4) ||
        !process_counter(d, process_member(d, "ref"), &local) ||
        (response && !process_raw(d, process_member(d, "outcome"), &outcome, &outcome_size))) {
        return false;
    }
    process_reference_t *slot = process_find(incoming ? p->incoming : p->outgoing, local);
    if (!slot) {
        return process_status(p, GT_EXT_RPC_INVALID);
    }
    gt_ext_rpc_status_t status;
    if (!strcmp(op, "result")) {
        gt_ext_rpc_result_t result;
        status = gt_ext_session_request_result(p->session, &p->lease, slot->native, &result);
        if (!process_status(p, status)) {
            return false;
        }
        if (status == GT_EXT_RPC_OK) {
            static const char *const states[] = {
                "queued", "pending", "succeeded", "failed", "cancelled", "unknown"};
            return (unsigned)result.state < sizeof(states) / sizeof(states[0]) &&
                   process_append(p,
                                  ",\"state\":\"%s\",\"confirmed\":%s,\"cancelRequested\":%s,"
                                  "\"deadlineElapsed\":%s,\"outcome\":%.*s",
                                  states[result.state],
                                  result.confirmed ? "true" : "false",
                                  result.cancel_requested ? "true" : "false",
                                  result.deadline_elapsed ? "true" : "false",
                                  result.outcome_size ? (int)result.outcome_size : 4,
                                  result.outcome_size ? result.outcome : "null");
        }
        return true;
    }
    bool interrupted = false;
    if (response) {
        status = gt_ext_session_check_request(p->session, &p->lease, slot->native);
        if (status == GT_EXT_RPC_OK) {
            status =
                gt_ext_session_respond(p->session, &p->lease, slot->native, outcome, outcome_size);
        } else if (status == GT_EXT_RPC_CLOSED || status == GT_EXT_RPC_DENIED) {
            /* An asynchronously delivered cancellation/grant observation cannot
             * certify that the child had no effect. Never forward stale success. */
            static const char unknown[] =
                "{\"kind\":\"unknown\",\"reason\":\"HANDLER_INTERRUPTED\"}";
            interrupted = true;
            status = gt_ext_session_respond(
                p->session, &p->lease, slot->native, unknown, sizeof(unknown) - 1);
        }
    } else if (incoming) {
        status = gt_ext_session_check_request(p->session, &p->lease, slot->native);
    } else if (!strcmp(op, "cancel")) {
        status = gt_ext_session_cancel_request(p->session, &p->lease, slot->native);
    } else {
        status = gt_ext_session_forget_request(p->session, &p->lease, slot->native);
    }
    if (status == GT_EXT_RPC_OK && (response || !strcmp(op, "forget"))) {
        gt__secure_erase(slot, sizeof(*slot));
    }
    return process_status(p, status) &&
           (!interrupted || process_append(p, ",\"interrupted\":true"));
}

gt_status_t gt__process_rpc_open(gt_ext_session_t *session,
                                 const gt_allocator_t *input,
                                 gt_process_rpc_t **out) {
    return gt__process_rpc_open_resources(session, NULL, input, out);
}

gt_status_t gt__process_rpc_open_resources(gt_ext_session_t *session,
                                           gt_runtime_resources_t *resources,
                                           const gt_allocator_t *input,
                                           gt_process_rpc_t **out) {
    if (out) {
        *out = NULL;
    }
    gt_ext_session_info_t info;
    gt_ext_lease_t lease;
    gt_bridge_role_t role;
    size_t count;
    if (!out || !session || (resources && !gt__runtime_resources_matches(resources, session)) ||
        (input && (!input->allocate || !input->release)) || !gt_ext_session_info(session, &info) ||
        info.phase != GT_EXT_SESSION_ACTIVE || !session->requests || session->sources ||
        !gt_ext_session_lease(session, &lease) ||
        !gt__bridge_session_runtime_admissible(session->inner)) {
        return GT_ERR_INVALID_ARG;
    }
    (void)gt__bridge_session_methods(session->inner, &count, &role);
    if (role != GT_BRIDGE_EXECUTOR) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_allocator_t a = input ? *input : (gt_allocator_t){NULL, allocate, release};
    gt_process_rpc_t *p = gt__plugin_memory_alloc(&a, sizeof(*p));
    if (!p) {
        return GT_ERR_IO;
    }
    memset(p, 0, sizeof(*p));
    p->json = gt__plugin_memory_alloc(&a, sizeof(*p->json));
    if (!p->json) {
        gt__plugin_memory_free(&a, p, sizeof(*p));
        return GT_ERR_IO;
    }
    memset(p->json, 0, sizeof(*p->json));
    p->allocator = a;
    p->session = session;
    p->lease = lease;
    p->resources = resources;
    session->sources++;
    *out = p;
    return GT_OK;
}

gt_process_rpc_status_t gt__process_rpc_call(gt_process_rpc_t *p, const void *bytes, size_t size) {
    if (!p || (!bytes && size)) {
        return GT_PROCESS_RPC_INVALID;
    }
    if (p->failed) {
        return GT_PROCESS_RPC_PROTOCOL;
    }
    if (p->reply_size) {
        return GT_PROCESS_RPC_WAIT;
    }
    process_json_t *d = p->json;
    gt__secure_erase(d, sizeof(*d));
    d->input = bytes;
    d->input_size = size;
    uint64_t version, sequence;
    char op[16];
    bool valid = size && size <= GT_PROCESS_RPC_MESSAGE_MAX && value(d, 0, 0, 0, 0);
    if (valid) {
        space(d);
        valid = d->at == size && d->nodes[1].kind == J_OBJECT &&
                node_uint(d, process_member(d, "v"), &version) && version == 1 &&
                process_counter(d, process_member(d, "id"), &sequence) &&
                p->sequence != UINT64_MAX && sequence == p->sequence + 1 &&
                process_text(d, process_member(d, "op"), op, sizeof(op));
    }
    if (valid) {
        p->sequence = sequence;
        valid = process_dispatch(p, op) && process_append(p, "}");
    }
    gt__secure_erase(d, sizeof(*d));
    if (!valid) {
        gt__secure_erase(p->reply, sizeof(p->reply));
        p->reply_size = 0;
        p->failed = true;
        gt__process_rpc_revoke(p);
        return GT_PROCESS_RPC_PROTOCOL;
    }
    return GT_PROCESS_RPC_OK;
}

bool gt__process_rpc_reply(const gt_process_rpc_t *p, const uint8_t **bytes, size_t *size) {
    if (bytes) {
        *bytes = NULL;
    }
    if (size) {
        *size = 0;
    }
    if (!p || !bytes || !size || !p->reply_size) {
        return false;
    }
    *bytes = (const uint8_t *)p->reply;
    *size = p->reply_size;
    return true;
}

gt_process_rpc_status_t gt__process_rpc_consume(gt_process_rpc_t *p) {
    if (!p || !p->reply_size) {
        return GT_PROCESS_RPC_INVALID;
    }
    gt__secure_erase(p->reply, sizeof(p->reply));
    p->reply_size = 0;
    return GT_PROCESS_RPC_OK;
}

void gt__process_rpc_revoke(gt_process_rpc_t *p) {
    if (p) {
        gt_ext_session_revoke(p->session);
        gt__runtime_resources_stop(p->resources);
    }
}

bool gt__process_rpc_activated(const gt_process_rpc_t *p) {
    return p && p->activated && !p->failed;
}

gt_status_t gt__process_rpc_destroy(gt_process_rpc_t **out) {
    if (!out || !*out) {
        return GT_ERR_INVALID_ARG;
    }
    gt_process_rpc_t *p = *out;
    *out = NULL;
    gt__process_rpc_revoke(p);
    gt_status_t result = GT_OK;
    for (size_t i = 0; i < 64; i++) {
        if (p->outgoing[i].local) {
            gt_ext_rpc_status_t status =
                gt_ext_session_forget_request(p->session, &p->lease, p->outgoing[i].native);
            if (status != GT_EXT_RPC_OK && status != GT_EXT_RPC_CLOSED) {
                result = GT_ERR_IO;
            }
        }
    }
    p->session->sources--;
    gt_allocator_t a = p->allocator;
    gt__plugin_memory_free(&a, p->json, sizeof(*p->json));
    gt__plugin_memory_free(&a, p, sizeof(*p));
    return result;
}
