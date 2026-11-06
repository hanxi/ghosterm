#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/transfer.h"
#include "base/secure-random.h"
#include "base/sha256.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TRANSFER_NONE,
    TRANSFER_CREDIT,
    TRANSFER_CANCEL_OUT,
    TRANSFER_CANCEL_IN,
    TRANSFER_RECEIPT,
    TRANSFER_BEGIN,
    TRANSFER_CHUNK,
    TRANSFER_END
} transfer_tx_t;

typedef enum {
    RECEIPT_SUCCESS,
    RECEIPT_FAILED,
    RECEIPT_CANCELLED,
    RECEIPT_UNKNOWN
} transfer_receipt_t;

typedef struct {
    bool used, needs_send;
    gt_bridge_resource_descriptor_t descriptor;
    transfer_receipt_t kind;
    uint64_t length;
    char code[33];
} transfer_receipt_slot_t;

typedef struct {
    uint8_t bytes[GT_BRIDGE_TRANSFER_CHUNK];
    size_t size, at;
    uint32_t cost;
    uint64_t offset;
} transfer_chunk_t;

struct gt_bridge_transfer {
    gt_allocator_t allocator;
    gt_bridge_session_t *session;
    gt_bridge_wire_t *wire;
    gt_bridge_resource_store_t *store;
    uint64_t scope_budget, owner, counter, rx_ordinal, tx_sequence, rx_sequence;
    gt_bridge_lease_t lease;
    gt_bridge_role_t role;
    bool ended, retired, cleanup_pending, tx_exhausted, rx_exhausted;
    uint32_t tx_credit, rx_credit, credit_pending, grant, cost;
    transfer_tx_t prepared, inflight;
    size_t selected, inflight_slot;

    struct {
        bool used, started, begin_sent, finish_requested, end_committed, cancel_sent;
        gt_bridge_transfer_result_t result;
        gt_bridge_resource_descriptor_t descriptor;
        gt_sha256_t hash;
        uint8_t bytes[GT_BRIDGE_TRANSFER_CHUNK];
        size_t pending;
    } out;

    struct {
        bool active, end_received;
        gt_bridge_resource_descriptor_t descriptor;
        gt_bridge_resource_ref_t ref;
        uint64_t received, length;
        char digest[65];
    } in;

    transfer_chunk_t chunks[64];
    size_t head, chunks_count;
    transfer_receipt_slot_t receipts[GT_BRIDGE_TRANSFER_RECEIPTS];
    size_t next_receipt;
    gt_bridge_snapshot_t snapshots[GT_BRIDGE_TRANSFER_SNAPSHOTS];
    size_t snapshot_head, snapshot_count;
    bool cancel_in_pending;
    char cancel_in_id[33];
    uint64_t cancel_in_generation;
};

static bool transfer_live(gt_bridge_transfer_t *t) {
    gt_bridge_session_info_t i;
    return !t->ended && gt__bridge_session_info(t->session, &i) &&
           i.phase == GT_BRIDGE_SESSION_ACTIVE && i.end == GT_BRIDGE_END_NONE;
}

static bool transfer_access(gt_bridge_transfer_t *t, const gt_bridge_lease_t *l) {
    return gt__bridge_session_access(t->session, l, "resource.receive") == GT_BRIDGE_ACCESS_ALLOWED;
}

static size_t transfer_limit(gt_bridge_transfer_t *t) {
    gt_bridge_session_info_t i;
    (void)gt__bridge_session_info(t->session, &i);
    return i.budgets.message_bytes < 4096 ? i.budgets.message_bytes : 4096;
}

static uint64_t transfer_bytes(gt_bridge_transfer_t *t) {
    gt_bridge_session_info_t i;
    (void)gt__bridge_session_info(t->session, &i);
    return i.budgets.resource_bytes;
}

static bool transfer_string(
    gt_bridge_transfer_t *t, uint16_t object, const char *key, char *out, size_t capacity) {
    const uint8_t *p;
    size_t n;
    if (!gt__bridge_wire_string(t->wire, gt__bridge_wire_member(t->wire, object, key), &p, &n) ||
        n >= capacity) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool transfer_uint(gt_bridge_transfer_t *t, uint16_t object, const char *key, uint64_t *n) {
    return gt__bridge_wire_counter(t->wire, gt__bridge_wire_member(t->wire, object, key), n);
}

static void transfer_id(gt_bridge_transfer_t *t, bool own, uint64_t ordinal, char out[33]) {
    gt_bridge_session_info_t i;
    (void)gt__bridge_session_info(t->session, &i);
    bool host = (t->role == GT_BRIDGE_HOST) == own;
    out[0] = host ? '1' : '9';
    memcpy(out + 1, host ? i.connection : i.instance, 15);
    (void)snprintf(out + 16, 17, "%016" PRIx64, ordinal);
}

static bool transfer_identity(gt_bridge_transfer_t *t,
                              const gt_bridge_message_t *m,
                              bool own,
                              char id[33],
                              uint64_t *generation) {
    char expected[33];
    if (!transfer_string(t, m->payload, "resourceId", id, 33) ||
        !transfer_uint(t, m->payload, "generation", generation) || !*generation) {
        return false;
    }
    transfer_id(t, own, *generation, expected);
    return !memcmp(id, expected, 33);
}

static bool transfer_frame(gt_bridge_transfer_t *t,
                           bool control,
                           uint64_t seq,
                           const char *kind,
                           const char *payload,
                           char *out,
                           size_t capacity,
                           size_t *size) {
    gt_bridge_session_info_t i;
    (void)gt__bridge_session_info(t->session, &i);
    int n = snprintf(
        out,
        capacity,
        "{\"wireVersion\":1,\"connectionId\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64
        "\",\"streamId\":\"%s\",\"seq\":\"%" PRIu64 "\",\"kind\":\"%s\",\"payload\":%s}",
        i.connection,
        i.instance,
        i.epoch,
        control ? "control" : "resource",
        seq,
        kind,
        payload);
    if (n < 0 || (size_t)n >= capacity || (!control && (size_t)n > transfer_limit(t))) {
        return false;
    }
    *size = (size_t)n;
    return true;
}

static bool transfer_quote(const char *p, size_t bound, char *out, size_t capacity) {
    size_t n = strnlen(p, bound), at = 0;
    if (n == bound) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 32 || c == 127) {
            return false;
        }
        if (at + 3 >= capacity) {
            return false;
        }
        if (c == '"' || c == '\\') {
            out[at++] = '\\';
        }
        out[at++] = (char)c;
    }
    out[at] = 0;
    return true;
}

static bool
transfer_begin_payload(const gt_bridge_resource_descriptor_t *d, char *out, size_t capacity) {
    char display[2051], mime[259], size[96];
    if (!transfer_quote(d->display_name, sizeof(d->display_name), display, sizeof(display)) ||
        !transfer_quote(d->mime, sizeof(d->mime), mime, sizeof(mime))) {
        return false;
    }
    if (d->size_known) {
        (void)snprintf(
            size, sizeof(size), "{\"kind\":\"known\",\"bytes\":\"%" PRIu64 "\"}", d->size);
    } else {
        strcpy(size, "{\"kind\":\"unknown\"}");
    }
    int n = snprintf(out,
                     capacity,
                     "{\"resourceId\":\"%s\",\"generation\":\"%" PRIu64
                     "\",\"displayName\":\"%s\",\"mime\":\"%s\",\"sha256\":\"%s\",\"size\":%s}",
                     d->resource_id,
                     d->generation,
                     display,
                     mime,
                     d->sha256,
                     size);
    return n > 0 && (size_t)n < capacity;
}

static bool transfer_descriptor_equal(const gt_bridge_resource_descriptor_t *a,
                                      const gt_bridge_resource_descriptor_t *b) {
    return a->generation == b->generation && a->size_known == b->size_known && a->size == b->size &&
           !memcmp(a->resource_id, b->resource_id, 33) && !strcmp(a->sha256, b->sha256) &&
           !strcmp(a->mime, b->mime) && !strcmp(a->display_name, b->display_name);
}

static transfer_receipt_slot_t *transfer_receipt(gt_bridge_transfer_t *t, const char *id) {
    for (size_t i = 0; i < GT_BRIDGE_TRANSFER_RECEIPTS; i++) {
        if (t->receipts[i].used && !memcmp(t->receipts[i].descriptor.resource_id, id, 33)) {
            return &t->receipts[i];
        }
    }
    return NULL;
}

static gt_status_t transfer_cache(gt_bridge_transfer_t *t,
                                  const gt_bridge_resource_descriptor_t *d,
                                  transfer_receipt_t kind,
                                  uint64_t length,
                                  const char *code) {
    transfer_receipt_slot_t *r = transfer_receipt(t, d->resource_id);
    if (!r) {
        for (size_t j = 0; j < GT_BRIDGE_TRANSFER_RECEIPTS; j++) {
            size_t i = (t->next_receipt + j) % GT_BRIDGE_TRANSFER_RECEIPTS;
            if (!t->receipts[i].needs_send &&
                !(t->inflight == TRANSFER_RECEIPT && t->inflight_slot == i)) {
                r = &t->receipts[i];
                t->next_receipt = (i + 1) % GT_BRIDGE_TRANSFER_RECEIPTS;
                break;
            }
        }
    }
    if (!r) {
        return GT_ERR_PROTOCOL;
    }
    memset(r, 0, sizeof(*r));
    r->used = r->needs_send = true;
    r->descriptor = *d;
    r->kind = kind;
    r->length = length;
    (void)snprintf(r->code, sizeof(r->code), "%s", code);
    return GT_OK;
}

static void transfer_discard_chunks(gt_bridge_transfer_t *t) {
    while (t->chunks_count) {
        transfer_chunk_t *c = &t->chunks[t->head];
        t->credit_pending += c->cost;
        gt__secure_erase(c, sizeof(*c));
        t->head = (t->head + 1) % 64;
        t->chunks_count--;
    }
}

static gt_status_t
transfer_complete_in(gt_bridge_transfer_t *t, transfer_receipt_t kind, const char *code) {
    if (!t->in.active) {
        return GT_OK;
    }
    gt_bridge_resource_descriptor_t d = t->in.descriptor;
    uint64_t length = t->in.received;
    transfer_discard_chunks(t);
    if (kind != RECEIPT_SUCCESS) {
        gt_bridge_resource_status_t status =
            gt__bridge_resource_release(t->store, t->owner, &t->lease, &t->in.ref);
        if (status != GT_BRIDGE_RESOURCE_OK && status != GT_BRIDGE_RESOURCE_NOT_FOUND) {
            return GT_ERR_IO;
        }
    }
    gt__secure_erase(&t->in, sizeof(t->in));
    return transfer_cache(t, &d, kind, length, code);
}

static gt_status_t transfer_bind(gt_bridge_transfer_t *t) {
    if (t->owner) {
        return GT_OK;
    }
    if (!gt__bridge_session_lease(t->session, &t->lease)) {
        return GT_ERR_AUTH;
    }
    gt_bridge_resource_status_t status =
        gt__bridge_resource_bind(t->store, &t->lease, t->scope_budget, &t->owner);
    return status == GT_BRIDGE_RESOURCE_OK   ? GT_OK
           : status == GT_BRIDGE_RESOURCE_IO ? GT_ERR_IO
                                             : GT_ERR_UNSUPPORTED;
}

static gt_status_t transfer_pump(gt_bridge_transfer_t *t) {
    if (!t->in.active) {
        return GT_OK;
    }
    if (t->chunks_count) {
        transfer_chunk_t *c = &t->chunks[t->head];
        size_t n = 0;
        gt_bridge_resource_status_t status = gt__bridge_resource_write(t->store,
                                                                       t->owner,
                                                                       &t->lease,
                                                                       &t->in.ref,
                                                                       c->offset + c->at,
                                                                       c->bytes + c->at,
                                                                       c->size - c->at,
                                                                       &n);
        if (status != GT_BRIDGE_RESOURCE_OK && status != GT_BRIDGE_RESOURCE_WAIT) {
            return transfer_complete_in(t, RECEIPT_FAILED, "IO");
        }
        c->at += n;
        if (c->at == c->size) {
            t->credit_pending += c->cost;
            gt__secure_erase(c, sizeof(*c));
            t->head = (t->head + 1) % 64;
            t->chunks_count--;
        }
    }
    if (!transfer_live(t)) {
        return GT_ERR_CANCELLED;
    }
    if (t->in.active && t->in.end_received && !t->chunks_count) {
        gt_bridge_resource_status_t status = gt__bridge_resource_finish(
            t->store, t->owner, &t->lease, &t->in.ref, true, t->in.length, t->in.digest);
        if (!transfer_live(t)) {
            return GT_ERR_CANCELLED;
        }
        if (status != GT_BRIDGE_RESOURCE_OK) {
            return transfer_complete_in(
                t, RECEIPT_FAILED, status == GT_BRIDGE_RESOURCE_PROTOCOL ? "INTEGRITY" : "IO");
        }
        if (t->snapshot_count == GT_BRIDGE_TRANSFER_SNAPSHOTS) {
            return GT_ERR_INTERNAL;
        }
        gt_bridge_snapshot_t *p =
            &t->snapshots[(t->snapshot_head + t->snapshot_count) % GT_BRIDGE_TRANSFER_SNAPSHOTS];
        p->owner = t->owner;
        p->ref = t->in.ref;
        p->descriptor = t->in.descriptor;
        p->descriptor.size_known = true;
        p->descriptor.size = t->in.length;
        t->snapshot_count++;
        return transfer_complete_in(t, RECEIPT_SUCCESS, "");
    }
    return GT_OK;
}

gt_status_t gt__bridge_transfer_new(gt_bridge_session_t *session,
                                    gt_bridge_wire_t *wire,
                                    gt_bridge_resource_store_t *store,
                                    uint64_t budget,
                                    const gt_allocator_t *a,
                                    gt_bridge_transfer_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!session || !wire || !store || !budget || !a || !out) {
        return GT_ERR_INVALID_ARG;
    }
    gt_bridge_transfer_t *t = gt__plugin_memory_alloc(a, sizeof(*t));
    if (!t) {
        return GT_ERR_IO;
    }
    memset(t, 0, sizeof(*t));
    t->allocator = *a;
    t->session = session;
    t->wire = wire;
    t->store = store;
    t->scope_budget = budget;
    size_t count;
    (void)gt__bridge_session_methods(session, &count, &t->role);
    t->credit_pending = GT_BRIDGE_TRANSFER_WINDOW;
    *out = t;
    return GT_OK;
}

void gt__bridge_transfer_free(gt_bridge_transfer_t *t) {
    if (t) {
        gt_allocator_t a = t->allocator;
        gt__secure_erase(t, sizeof(*t));
        gt__plugin_memory_free(&a, t, sizeof(*t));
    }
}

void gt__bridge_transfer_end(gt_bridge_transfer_t *t) {
    if (!t || t->ended) {
        return;
    }
    t->ended = true;
    transfer_discard_chunks(t);
    if (t->out.used && !t->out.result.confirmed &&
        t->out.result.state != GT_BRIDGE_TRANSFER_CANCELLED) {
        t->out.result.state =
            t->out.started ? GT_BRIDGE_TRANSFER_UNKNOWN : GT_BRIDGE_TRANSFER_CANCELLED;
    }
    if (t->owner) {
        if (t->in.active) {
            (void)gt__bridge_resource_release(t->store, t->owner, &t->lease, &t->in.ref);
        }
        t->cleanup_pending =
            gt__bridge_resource_retire(t->store, t->owner, &t->lease) != GT_BRIDGE_RESOURCE_OK;
        t->retired = !t->cleanup_pending;
    }
    gt__secure_erase(&t->in, sizeof(t->in));
}

gt_status_t gt__bridge_transfer_cleanup(gt_bridge_transfer_t *t) {
    if (!t) {
        return GT_OK;
    }
    gt__bridge_transfer_end(t);
    bool failed = false;
    while (t->snapshot_count) {
        gt_bridge_snapshot_t *p = &t->snapshots[t->snapshot_head];
        gt_bridge_resource_status_t status =
            gt__bridge_resource_release(t->store, t->owner, &t->lease, &p->ref);
        gt__secure_erase(p, sizeof(*p));
        t->snapshot_head = (t->snapshot_head + 1) % GT_BRIDGE_TRANSFER_SNAPSHOTS;
        t->snapshot_count--;
        if (status != GT_BRIDGE_RESOURCE_OK) {
            failed = true;
            t->cleanup_pending = true;
            break;
        }
    }
    if (t->cleanup_pending) {
        if (gt__bridge_resource_retire(t->store, t->owner, &t->lease) != GT_BRIDGE_RESOURCE_OK) {
            return GT_ERR_IO;
        }
        t->cleanup_pending = false;
        t->retired = true;
    }
    if (t->snapshot_count || failed) {
        return GT_ERR_IO;
    }
    return GT_OK;
}

gt_bridge_resource_status_t
gt__bridge_transfer_start(gt_bridge_transfer_t *t,
                          const gt_bridge_lease_t *lease,
                          const gt_bridge_resource_descriptor_t *metadata,
                          uint64_t *handle) {
    if (handle) {
        *handle = 0;
    }
    if (!t || !metadata || !handle) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    if (!transfer_live(t)) {
        return GT_BRIDGE_RESOURCE_CLOSED;
    }
    if (!gt__bridge_session_has_capability(t->session, "resource.receive")) {
        return GT_BRIDGE_RESOURCE_UNSUPPORTED;
    }
    if (!transfer_access(t, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (t->out.used) {
        return GT_BRIDGE_RESOURCE_WAIT;
    }
    if (metadata->resource_id[0] || metadata->generation ||
        (!metadata->size_known && metadata->size) ||
        (metadata->size_known && metadata->size > transfer_bytes(t))) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    if (t->counter == UINT64_MAX) {
        return GT_BRIDGE_RESOURCE_UNSUPPORTED;
    }
    gt_bridge_resource_descriptor_t d = *metadata;
    d.generation = t->counter + 1;
    transfer_id(t, true, d.generation, d.resource_id);
    char payload[4097], frame[4097];
    size_t n;
    gt_bridge_message_t m;
    if (!transfer_begin_payload(&d, payload, sizeof(payload)) ||
        !transfer_frame(
            t, false, UINT64_MAX, "resource.begin", payload, frame, sizeof(frame), &n) ||
        gt__bridge_wire_parse(t->wire, frame, n, &m) != GT_OK) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    t->counter++;
    t->out.used = true;
    t->out.descriptor = d;
    t->out.result = (gt_bridge_transfer_result_t){
        .handle = t->counter, .state = GT_BRIDGE_TRANSFER_QUEUED, .generation = d.generation};
    memcpy(t->out.result.resource_id, d.resource_id, 33);
    gt__sha256_init(&t->out.hash);
    *handle = t->counter;
    return GT_BRIDGE_RESOURCE_OK;
}

gt_bridge_resource_status_t gt__bridge_transfer_write(gt_bridge_transfer_t *t,
                                                      const gt_bridge_lease_t *lease,
                                                      uint64_t handle,
                                                      const void *data,
                                                      size_t n,
                                                      size_t *accepted) {
    if (accepted) {
        *accepted = 0;
    }
    if (!t || !accepted || !data || !n) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    if (!transfer_live(t)) {
        return GT_BRIDGE_RESOURCE_CLOSED;
    }
    if (!transfer_access(t, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (!t->out.used || handle != t->out.result.handle) {
        return GT_BRIDGE_RESOURCE_NOT_FOUND;
    }
    if (t->out.result.confirmed || t->out.result.cancel_requested || t->out.finish_requested ||
        t->out.result.state == GT_BRIDGE_TRANSFER_CANCELLED) {
        return GT_BRIDGE_RESOURCE_CLOSED;
    }
    if (t->out.pending) {
        return GT_BRIDGE_RESOURCE_WAIT;
    }
    gt_bridge_session_info_t i;
    (void)gt__bridge_session_info(t->session, &i);
    size_t limit = i.budgets.fragment_bytes;
    if (limit > GT_BRIDGE_TRANSFER_CHUNK) {
        limit = GT_BRIDGE_TRANSFER_CHUNK;
    }
    if (limit > (transfer_limit(t) - 512) / 4 * 3) {
        limit = (transfer_limit(t) - 512) / 4 * 3;
    }
    if (n > limit) {
        n = limit;
    }
    uint64_t maximum = t->out.descriptor.size_known ? t->out.descriptor.size : transfer_bytes(t);
    if (n > maximum - t->out.result.accepted) {
        return GT_BRIDGE_RESOURCE_LIMIT;
    }
    memcpy(t->out.bytes, data, n);
    if (!gt__sha256_update(&t->out.hash, data, n)) {
        return GT_BRIDGE_RESOURCE_IO;
    }
    t->out.pending = n;
    t->out.result.accepted += n;
    *accepted = n;
    return GT_BRIDGE_RESOURCE_OK;
}

gt_bridge_resource_status_t gt__bridge_transfer_finish(gt_bridge_transfer_t *t,
                                                       const gt_bridge_lease_t *lease,
                                                       uint64_t handle,
                                                       bool complete) {
    if (!t) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    if (!transfer_live(t)) {
        return GT_BRIDGE_RESOURCE_CLOSED;
    }
    if (!transfer_access(t, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (!t->out.used || handle != t->out.result.handle) {
        return GT_BRIDGE_RESOURCE_NOT_FOUND;
    }
    if (t->out.result.confirmed || t->out.result.cancel_requested ||
        t->out.result.state == GT_BRIDGE_TRANSFER_CANCELLED) {
        return GT_BRIDGE_RESOURCE_CLOSED;
    }
    if (!complete ||
        (t->out.descriptor.size_known && t->out.result.accepted != t->out.descriptor.size)) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    gt_sha256_t copy = t->out.hash;
    char digest[65];
    bool valid = gt__sha256_final(&copy, digest) && !memcmp(digest, t->out.descriptor.sha256, 65);
    gt__secure_erase(digest, sizeof(digest));
    if (!valid) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    t->out.finish_requested = true;
    return GT_BRIDGE_RESOURCE_OK;
}

gt_bridge_resource_status_t gt__bridge_transfer_cancel(gt_bridge_transfer_t *t,
                                                       const gt_bridge_lease_t *lease,
                                                       uint64_t handle) {
    if (!t || !gt__bridge_session_scope(t->session, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (!t->out.used || handle != t->out.result.handle) {
        return GT_BRIDGE_RESOURCE_NOT_FOUND;
    }
    if (t->out.result.confirmed) {
        return GT_BRIDGE_RESOURCE_OK;
    }
    t->out.result.cancel_requested = true;
    if (!t->out.started) {
        t->out.result.state = GT_BRIDGE_TRANSFER_CANCELLED;
    }
    return GT_BRIDGE_RESOURCE_OK;
}

bool gt__bridge_transfer_result(gt_bridge_transfer_t *t,
                                const gt_bridge_lease_t *lease,
                                uint64_t handle,
                                gt_bridge_transfer_result_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!t || !out || !gt__bridge_session_scope(t->session, lease) || !t->out.used ||
        handle != t->out.result.handle) {
        return false;
    }
    *out = t->out.result;
    if (!transfer_live(t) && !out->confirmed) {
        out->state = t->out.started ? GT_BRIDGE_TRANSFER_UNKNOWN : GT_BRIDGE_TRANSFER_CANCELLED;
    }
    return true;
}

gt_bridge_resource_status_t gt__bridge_transfer_forget(gt_bridge_transfer_t *t,
                                                       const gt_bridge_lease_t *lease,
                                                       uint64_t handle) {
    if (!t || !gt__bridge_session_scope(t->session, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (!t->out.used || handle != t->out.result.handle) {
        return GT_BRIDGE_RESOURCE_NOT_FOUND;
    }
    if (!t->out.result.confirmed && t->out.result.state != GT_BRIDGE_TRANSFER_CANCELLED &&
        transfer_live(t)) {
        return GT_BRIDGE_RESOURCE_WAIT;
    }
    if (t->inflight >= TRANSFER_BEGIN) {
        return GT_BRIDGE_RESOURCE_WAIT;
    }
    gt__secure_erase(&t->out, sizeof(t->out));
    return GT_BRIDGE_RESOURCE_OK;
}

gt_bridge_resource_status_t gt__bridge_transfer_take(gt_bridge_transfer_t *t,
                                                     const gt_bridge_lease_t *lease,
                                                     gt_bridge_snapshot_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!t || !out) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    if (!gt__bridge_session_scope(t->session, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (!t->snapshot_count) {
        return GT_BRIDGE_RESOURCE_WAIT;
    }
    *out = t->snapshots[t->snapshot_head];
    gt__secure_erase(&t->snapshots[t->snapshot_head], sizeof(*out));
    t->snapshot_head = (t->snapshot_head + 1) % GT_BRIDGE_TRANSFER_SNAPSHOTS;
    t->snapshot_count--;
    return GT_BRIDGE_RESOURCE_OK;
}

gt_bridge_resource_status_t gt__bridge_transfer_reject(gt_bridge_transfer_t *t,
                                                       const gt_bridge_lease_t *lease) {
    if (!t) {
        return GT_BRIDGE_RESOURCE_INVALID;
    }
    if (!transfer_live(t)) {
        return GT_BRIDGE_RESOURCE_CLOSED;
    }
    if (!transfer_access(t, lease)) {
        return GT_BRIDGE_RESOURCE_DENIED;
    }
    if (!t->in.active) {
        return GT_BRIDGE_RESOURCE_NOT_FOUND;
    }
    t->cancel_in_pending = true;
    memcpy(t->cancel_in_id, t->in.descriptor.resource_id, 33);
    t->cancel_in_generation = t->in.descriptor.generation;
    return transfer_complete_in(t, RECEIPT_CANCELLED, "CANCELLED") == GT_OK ? GT_BRIDGE_RESOURCE_OK
                                                                            : GT_BRIDGE_RESOURCE_IO;
}

static bool transfer_decode64(const char *p, size_t n, uint8_t *out, size_t *size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    *size = 0;
    if (!n || n % 4 || n > 1024) {
        return false;
    }
    for (size_t i = 0; i < n; i += 4) {
        uint32_t value = 0;
        unsigned pad = 0;
        for (size_t j = 0; j < 4; j++) {
            const char *at = strchr(alphabet, p[i + j]);
            if (p[i + j] == '=') {
                pad++;
                value <<= 6;
            } else {
                if (pad || !at) {
                    return false;
                }
                value = (value << 6) | (uint32_t)(at - alphabet);
            }
        }
        if (pad > 2 || (pad && i + 4 != n) || (pad == 2 && (value & 0xffff)) ||
            (pad == 1 && (value & 0xff))) {
            return false;
        }
        out[(*size)++] = (uint8_t)(value >> 16);
        if (pad < 2) {
            out[(*size)++] = (uint8_t)(value >> 8);
        }
        if (!pad) {
            out[(*size)++] = (uint8_t)value;
        }
    }
    return true;
}

static void transfer_encode64(const uint8_t *p, size_t n, char out[1025]) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)p[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= p[i + 2];
        }
        out[at++] = a[v >> 18];
        out[at++] = a[(v >> 12) & 63];
        out[at++] = i + 1 < n ? a[(v >> 6) & 63] : '=';
        out[at++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[at] = 0;
}

static gt_status_t transfer_result_received(gt_bridge_transfer_t *t, const gt_bridge_message_t *m) {
    char id[33], kind[32], code[33] = "";
    uint64_t generation;
    if (!transfer_identity(t, m, true, id, &generation) || generation > t->counter) {
        return GT_ERR_PROTOCOL;
    }
    if (!t->out.used || memcmp(t->out.descriptor.resource_id, id, 33)) {
        return GT_OK;
    }
    uint16_t outcome = gt__bridge_wire_member(t->wire, m->payload, "outcome");
    if (!transfer_string(t, outcome, "kind", kind, sizeof(kind))) {
        return GT_ERR_PROTOCOL;
    }
    gt_bridge_transfer_state_t state;
    if (!strcmp(kind, "success")) {
        uint16_t v = gt__bridge_wire_member(t->wire, outcome, "value");
        uint64_t size;
        char digest[65];
        if (!t->out.end_committed || !transfer_uint(t, v, "byteLength", &size) ||
            !transfer_string(t, v, "sha256", digest, 65) || size != t->out.result.accepted ||
            memcmp(digest, t->out.descriptor.sha256, 65)) {
            return GT_ERR_PROTOCOL;
        }
        state = GT_BRIDGE_TRANSFER_SUCCEEDED;
    } else if (!strcmp(kind, "failed")) {
        uint16_t e = gt__bridge_wire_member(t->wire, outcome, "error");
        char effect[16];
        if (!transfer_string(t, e, "code", code, 33) ||
            !transfer_string(t, e, "effect", effect, sizeof(effect)) || strcmp(effect, "none")) {
            return GT_ERR_PROTOCOL;
        }
        state = GT_BRIDGE_TRANSFER_FAILED;
    } else if (!strcmp(kind, "cancelled")) {
        char effect[16];
        if (!transfer_string(t, outcome, "effect", effect, sizeof(effect)) ||
            strcmp(effect, "none")) {
            return GT_ERR_PROTOCOL;
        }
        state = GT_BRIDGE_TRANSFER_CANCELLED;
        strcpy(code, "CANCELLED");
    } else {
        state = GT_BRIDGE_TRANSFER_UNKNOWN;
    }
    if (t->out.result.confirmed) {
        return t->out.result.state == state && !strcmp(t->out.result.code, code) ? GT_OK
                                                                                 : GT_ERR_PROTOCOL;
    }
    t->out.result.state = state;
    t->out.result.confirmed = true;
    strcpy(t->out.result.code, code);
    gt__secure_erase(t->out.bytes, sizeof(t->out.bytes));
    t->out.pending = 0;
    return GT_OK;
}

gt_status_t
gt__bridge_transfer_receive(gt_bridge_transfer_t *t, const gt_bridge_message_t *m, size_t size) {
    if (!t || !transfer_live(t)) {
        return GT_ERR_CANCELLED;
    }
    if (transfer_bind(t) != GT_OK) {
        return GT_ERR_UNSUPPORTED;
    }
    if (!strcmp(m->stream, "control")) {
        if (m->kind == GT_BRIDGE_CREDIT) {
            char stream[129];
            uint64_t n;
            if (!transfer_string(t, m->payload, "streamId", stream, 129) ||
                strcmp(stream, "resource") ||
                !gt__bridge_wire_uint(
                    t->wire, gt__bridge_wire_member(t->wire, m->payload, "bytes"), &n) ||
                n > GT_BRIDGE_TRANSFER_WINDOW - t->tx_credit) {
                return GT_ERR_PROTOCOL;
            }
            t->tx_credit += (uint32_t)n;
            return GT_OK;
        }
        if (m->kind == GT_BRIDGE_RESOURCE_RESULT) {
            return transfer_result_received(t, m);
        }
        if (m->kind == GT_BRIDGE_RESOURCE_CANCEL) {
            char id[33];
            uint64_t generation;
            if (transfer_identity(t, m, true, id, &generation)) {
                if (generation > t->counter) {
                    return GT_ERR_PROTOCOL;
                }
                if (t->out.used && !memcmp(t->out.descriptor.resource_id, id, 33) &&
                    !t->out.result.confirmed) {
                    t->out.result.cancel_requested = true;
                    t->out.cancel_sent = true;
                }
                return GT_OK;
            }
            if (!transfer_identity(t, m, false, id, &generation) || generation > t->rx_ordinal) {
                return GT_ERR_PROTOCOL;
            }
            if (t->in.active && !memcmp(id, t->in.descriptor.resource_id, 33)) {
                return transfer_complete_in(t, RECEIPT_CANCELLED, "CANCELLED");
            }
            transfer_receipt_slot_t *r = transfer_receipt(t, id);
            if (r) {
                r->needs_send = true;
            }
            return GT_OK;
        }
        return GT_ERR_UNSUPPORTED;
    }
    if (strcmp(m->stream, "resource") ||
        (m->kind != GT_BRIDGE_RESOURCE_BEGIN && m->kind != GT_BRIDGE_RESOURCE_CHUNK &&
         m->kind != GT_BRIDGE_RESOURCE_END)) {
        return GT_ERR_UNSUPPORTED;
    }
    if (size > transfer_limit(t) || size + 4 > t->rx_credit || t->rx_exhausted ||
        m->sequence != t->rx_sequence) {
        return GT_ERR_PROTOCOL;
    }
    if (t->rx_sequence == UINT64_MAX) {
        t->rx_exhausted = true;
    } else {
        t->rx_sequence++;
    }
    t->rx_credit -= (uint32_t)(size + 4);
    char id[33];
    uint64_t generation;
    if (!transfer_identity(t, m, false, id, &generation)) {
        return GT_ERR_PROTOCOL;
    }
    transfer_receipt_slot_t *cached = transfer_receipt(t, id);
    if (m->kind == GT_BRIDGE_RESOURCE_BEGIN) {
        gt_bridge_resource_descriptor_t d = {.generation = generation};
        memcpy(d.resource_id, id, 33);
        if (!transfer_string(
                t, m->payload, "displayName", d.display_name, sizeof(d.display_name)) ||
            !transfer_string(t, m->payload, "mime", d.mime, sizeof(d.mime)) ||
            !transfer_string(t, m->payload, "sha256", d.sha256, 65)) {
            return GT_ERR_PROTOCOL;
        }
        d.size_known =
            transfer_uint(t, gt__bridge_wire_member(t->wire, m->payload, "size"), "bytes", &d.size);
        t->credit_pending += (uint32_t)(size + 4);
        if (cached) {
            if (!transfer_descriptor_equal(&cached->descriptor, &d)) {
                return GT_ERR_PROTOCOL;
            }
            cached->needs_send = true;
            return GT_OK;
        }
        if (t->in.active) {
            return transfer_descriptor_equal(&t->in.descriptor, &d) ? GT_OK : GT_ERR_PROTOCOL;
        }
        if (generation <= t->rx_ordinal) {
            return transfer_cache(t, &d, RECEIPT_UNKNOWN, 0, "RETIRED");
        }
        t->rx_ordinal = generation;
        if (!gt__bridge_session_has_capability(t->session, "resource.receive")) {
            return transfer_cache(t, &d, RECEIPT_FAILED, 0, "UNSUPPORTED");
        }
        if (!transfer_access(t, &t->lease)) {
            return transfer_cache(t, &d, RECEIPT_FAILED, 0, "DENIED");
        }
        if ((d.size_known && d.size > transfer_bytes(t))) {
            return transfer_cache(t, &d, RECEIPT_FAILED, 0, "RESOURCE_LIMIT");
        }
        if (t->snapshot_count == GT_BRIDGE_TRANSFER_SNAPSHOTS) {
            return transfer_cache(t, &d, RECEIPT_FAILED, 0, "BUSY");
        }
        gt_bridge_resource_status_t status =
            gt__bridge_resource_begin(t->store, t->owner, &t->lease, &d, &t->in.ref);
        if (status != GT_BRIDGE_RESOURCE_OK) {
            return transfer_cache(t,
                                  &d,
                                  RECEIPT_FAILED,
                                  0,
                                  status == GT_BRIDGE_RESOURCE_WAIT    ? "BUSY"
                                  : status == GT_BRIDGE_RESOURCE_LIMIT ? "RESOURCE_LIMIT"
                                  : status == GT_BRIDGE_RESOURCE_IO    ? "IO"
                                                                       : "INVALID_INPUT");
        }
        t->in.active = true;
        t->in.descriptor = d;
        return GT_OK;
    }
    if (!t->in.active || memcmp(t->in.descriptor.resource_id, id, 33)) {
        if (!cached) {
            return GT_ERR_PROTOCOL;
        }
        t->credit_pending += (uint32_t)(size + 4);
        if (m->kind == GT_BRIDGE_RESOURCE_END) {
            uint64_t length;
            char digest[65];
            if (!transfer_uint(t, m->payload, "byteLength", &length) ||
                !transfer_string(t, m->payload, "sha256", digest, 65) ||
                (cached->kind == RECEIPT_SUCCESS &&
                 (length != cached->length || memcmp(digest, cached->descriptor.sha256, 65)))) {
                return GT_ERR_PROTOCOL;
            }
            cached->needs_send = true;
        }
        return GT_OK;
    }
    if (m->kind == GT_BRIDGE_RESOURCE_CHUNK) {
        if (t->in.end_received || t->chunks_count == 64) {
            return GT_ERR_PROTOCOL;
        }
        uint64_t offset;
        char encoded[1025];
        transfer_chunk_t *c = &t->chunks[(t->head + t->chunks_count) % 64];
        if (!transfer_uint(t, m->payload, "offset", &offset) || offset != t->in.received ||
            !transfer_string(t, m->payload, "dataBase64", encoded, sizeof(encoded)) ||
            !transfer_decode64(encoded, strlen(encoded), c->bytes, &c->size)) {
            return GT_ERR_PROTOCOL;
        }
        gt_bridge_session_info_t info;
        (void)gt__bridge_session_info(t->session, &info);
        uint64_t limit = t->in.descriptor.size_known ? t->in.descriptor.size : transfer_bytes(t);
        if (c->size > info.budgets.fragment_bytes || c->size > limit - t->in.received) {
            t->credit_pending += (uint32_t)(size + 4);
            gt__secure_erase(c, sizeof(*c));
            return transfer_complete_in(t, RECEIPT_FAILED, "RESOURCE_LIMIT");
        }
        c->offset = offset;
        c->cost = (uint32_t)(size + 4);
        t->in.received += c->size;
        t->chunks_count++;
        return GT_OK;
    }
    uint64_t length;
    char digest[65];
    if (!transfer_uint(t, m->payload, "byteLength", &length) ||
        !transfer_string(t, m->payload, "sha256", digest, 65)) {
        return GT_ERR_PROTOCOL;
    }
    t->credit_pending += (uint32_t)(size + 4);
    if (t->in.end_received && (t->in.length != length || memcmp(t->in.digest, digest, 65))) {
        return GT_ERR_PROTOCOL;
    }
    t->in.end_received = true;
    t->in.length = length;
    memcpy(t->in.digest, digest, 65);
    return GT_OK;
}

static bool transfer_receipt_payload(const transfer_receipt_slot_t *r, char *out, size_t capacity) {
    char outcome[512];
    if (r->kind == RECEIPT_SUCCESS) {
        (void)snprintf(outcome,
                       sizeof(outcome),
                       "{\"kind\":\"success\",\"value\":{\"byteLength\":\"%" PRIu64
                       "\",\"sha256\":\"%s\"}}",
                       r->length,
                       r->descriptor.sha256);
    } else if (r->kind == RECEIPT_CANCELLED) {
        strcpy(outcome, "{\"kind\":\"cancelled\",\"effect\":\"none\"}");
    } else if (r->kind == RECEIPT_UNKNOWN) {
        strcpy(
            outcome,
            "{\"kind\":\"unknown\",\"reason\":\"receipt retired; resource not published again\"}");
    } else {
        (void)snprintf(outcome,
                       sizeof(outcome),
                       "{\"kind\":\"failed\",\"error\":{\"code\":\"%s\",\"message\":\"resource not "
                       "published\",\"effect\":\"none\"}}",
                       r->code);
    }
    int n = snprintf(out,
                     capacity,
                     "{\"resourceId\":\"%s\",\"generation\":\"%" PRIu64 "\",\"outcome\":%s}",
                     r->descriptor.resource_id,
                     r->descriptor.generation,
                     outcome);
    return n > 0 && (size_t)n < capacity;
}

gt_status_t gt__bridge_transfer_prepare(gt_bridge_transfer_t *t,
                                        uint64_t control_seq,
                                        char *out,
                                        size_t capacity,
                                        size_t *size,
                                        bool *control) {
    *size = 0;
    *control = false;
    if (!t || !transfer_live(t) || t->inflight != TRANSFER_NONE) {
        return GT_ERR_INVALID_ARG;
    }
    gt_status_t status = transfer_bind(t);
    if (status != GT_OK) {
        return status;
    }
    if ((status = transfer_pump(t)) != GT_OK) {
        return status;
    }
    t->prepared = TRANSFER_NONE;
    char payload[4097];
    const char *kind = NULL;
    if (t->credit_pending) {
        t->prepared = TRANSFER_CREDIT;
        t->grant = t->credit_pending;
        (void)snprintf(
            payload, sizeof(payload), "{\"bytes\":%u,\"streamId\":\"resource\"}", t->grant);
        kind = "credit";
        *control = true;
    } else if (t->cancel_in_pending) {
        t->prepared = TRANSFER_CANCEL_IN;
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"resourceId\":\"%s\",\"generation\":\"%" PRIu64 "\"}",
                       t->cancel_in_id,
                       t->cancel_in_generation);
        kind = "resource.cancel";
        *control = true;
    } else if (t->out.used && t->out.started && t->out.result.cancel_requested &&
               !t->out.cancel_sent && !t->out.result.confirmed) {
        t->prepared = TRANSFER_CANCEL_OUT;
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"resourceId\":\"%s\",\"generation\":\"%" PRIu64 "\"}",
                       t->out.descriptor.resource_id,
                       t->out.descriptor.generation);
        kind = "resource.cancel";
        *control = true;
    } else {
        for (size_t i = 0; i < GT_BRIDGE_TRANSFER_RECEIPTS; i++) {
            if (t->receipts[i].needs_send) {
                t->prepared = TRANSFER_RECEIPT;
                t->selected = i;
                if (!transfer_receipt_payload(&t->receipts[i], payload, sizeof(payload))) {
                    return GT_ERR_INTERNAL;
                }
                kind = "resource.result";
                *control = true;
                break;
            }
        }
    }
    if (!kind && t->out.used && !t->out.result.confirmed && !t->out.result.cancel_requested &&
        t->out.result.state != GT_BRIDGE_TRANSFER_CANCELLED) {
        if (!t->out.started) {
            t->prepared = TRANSFER_BEGIN;
            if (!transfer_begin_payload(&t->out.descriptor, payload, sizeof(payload))) {
                return GT_ERR_INTERNAL;
            }
            kind = "resource.begin";
        } else if (t->out.pending) {
            char encoded[1025];
            transfer_encode64(t->out.bytes, t->out.pending, encoded);
            t->prepared = TRANSFER_CHUNK;
            (void)snprintf(payload,
                           sizeof(payload),
                           "{\"resourceId\":\"%s\",\"generation\":\"%" PRIu64
                           "\",\"offset\":\"%" PRIu64 "\",\"dataBase64\":\"%s\"}",
                           t->out.descriptor.resource_id,
                           t->out.descriptor.generation,
                           t->out.result.accepted - t->out.pending,
                           encoded);
            kind = "resource.chunk";
        } else if (t->out.finish_requested && !t->out.end_committed) {
            t->prepared = TRANSFER_END;
            (void)snprintf(payload,
                           sizeof(payload),
                           "{\"resourceId\":\"%s\",\"generation\":\"%" PRIu64
                           "\",\"byteLength\":\"%" PRIu64 "\",\"sha256\":\"%s\"}",
                           t->out.descriptor.resource_id,
                           t->out.descriptor.generation,
                           t->out.result.accepted,
                           t->out.descriptor.sha256);
            kind = "resource.end";
        }
    }
    if (!kind) {
        return GT_OK;
    }
    if (!*control && t->tx_exhausted) {
        return GT_ERR_UNSUPPORTED;
    }
    if (!transfer_frame(t,
                        *control,
                        *control ? control_seq : t->tx_sequence,
                        kind,
                        payload,
                        out,
                        capacity,
                        size)) {
        return GT_ERR_PROTOCOL;
    }
    t->cost = (uint32_t)(*size + 4);
    if (!*control && t->cost > t->tx_credit) {
        *size = 0;
        t->prepared = TRANSFER_NONE;
    }
    return GT_OK;
}

void gt__bridge_transfer_commit(gt_bridge_transfer_t *t) {
    t->inflight = t->prepared;
    t->inflight_slot = t->selected;
    if (t->prepared == TRANSFER_CREDIT) {
        t->credit_pending -= t->grant;
    } else if (t->prepared == TRANSFER_CANCEL_OUT) {
        t->out.cancel_sent = true;
    } else if (t->prepared == TRANSFER_CANCEL_IN) {
        t->cancel_in_pending = false;
    } else if (t->prepared == TRANSFER_RECEIPT) {
        t->receipts[t->selected].needs_send = false;
    } else {
        t->tx_credit -= t->cost;
        if (t->tx_sequence == UINT64_MAX) {
            t->tx_exhausted = true;
        } else {
            t->tx_sequence++;
        }
        if (t->prepared == TRANSFER_BEGIN) {
            t->out.started = true;
            t->out.result.state = GT_BRIDGE_TRANSFER_SENDING;
        }
        if (t->prepared == TRANSFER_END) {
            t->out.end_committed = true;
            t->out.result.state = GT_BRIDGE_TRANSFER_PENDING;
        }
    }
}

void gt__bridge_transfer_sent(gt_bridge_transfer_t *t) {
    if (t->inflight == TRANSFER_CREDIT) {
        t->rx_credit += t->grant;
    }
    if (t->inflight == TRANSFER_BEGIN) {
        t->out.begin_sent = true;
    }
    if (t->inflight == TRANSFER_CHUNK) {
        gt__secure_erase(t->out.bytes, sizeof(t->out.bytes));
        t->out.pending = 0;
    }
    t->inflight = t->prepared = TRANSFER_NONE;
}

bool gt__bridge_transfer_has_owner(const gt_bridge_transfer_t *t) {
    return t && t->out.used;
}
