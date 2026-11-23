#include "extension/package-json.h"
#include "extension/memory.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
#define GT_JSON_TAG gt_package_json
#define GT_JSON_DOC gt_package_json_t
#define GT_JSON_ERROR gt_package_json_error_t
#define GT_JSON_NODES GT_PACKAGE_JSON_NODES
#define GT_JSON_BYTES GT_PACKAGE_JSON_BYTES
#define GT_JSON_DEPTH GT_PACKAGE_JSON_DEPTH
#define GT_JSON_KEYS 32768u
#define GT_JSON_WORK 1048576u
#define GT_JSON_LIMIT GT_PACKAGE_JSON_LIMIT
#define GT_JSON_SYNTAX GT_PACKAGE_JSON_SYNTAX
#define GT_JSON_UTF8 GT_PACKAGE_JSON_UTF8
#define GT_JSON_DUPLICATE GT_PACKAGE_JSON_DUPLICATE
#define GT_JSON_DYNAMIC 1
#define GT_JSON_MEMORY GT_PACKAGE_JSON_MEMORY
#include "base/json-reader.inc"

bool gt__package_json_open(const gt_allocator_t *allocator, gt_package_json_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return false;
    }
    gt_allocator_t a = allocator ? *allocator : (gt_allocator_t){NULL, allocate, release};
    if (!a.allocate || !a.release) {
        return false;
    }
    gt_package_json_t *d = gt__plugin_memory_alloc(&a, sizeof(*d));
    if (!d) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    d->allocator = a;
    *out = d;
    return true;
}

static gt_package_json_error_t
json_check(gt_package_json_t *d, const void *bytes, size_t size, bool integers) {
    json_buffers_free(d);
    gt_allocator_t a = d->allocator;
    gt__secure_erase(d, sizeof(*d));
    d->allocator = a;
    d->input = bytes;
    d->input_size = size;
    if (!bytes && size) {
        return d->error = GT_PACKAGE_JSON_SYNTAX;
    }
    if (size > GT_PACKAGE_JSON_BYTES) {
        return d->error = GT_PACKAGE_JSON_LIMIT;
    }
    if (!value(d, 0, 0, 0, 0)) {
        return d->error;
    }
    space(d);
    if (d->at != size) {
        return d->error = GT_PACKAGE_JSON_SYNTAX;
    }
    if (integers) {
        for (uint16_t id = 1; id <= d->used; id++) {
            if (d->nodes[id].kind == J_NUMBER) {
                uint64_t n;
                if (!node_uint(d, id, &n) || n > UINT64_C(9007199254740991)) {
                    return d->error = GT_PACKAGE_JSON_NUMBER;
                }
            }
        }
    }
    d->valid = true;
    return GT_PACKAGE_JSON_OK;
}

gt_package_json_error_t
gt__package_json_check(gt_package_json_t *d, const void *bytes, size_t size, bool integers) {
    if (!d) {
        return GT_PACKAGE_JSON_SYNTAX;
    }
    gt_package_json_error_t result = json_check(d, bytes, size, integers);
    gt__package_json_reset(d);
    return result;
}

gt_package_json_error_t
gt__package_json_parse(gt_package_json_t *d, const void *bytes, size_t size) {
    if (!d) {
        return GT_PACKAGE_JSON_SYNTAX;
    }
    gt_package_json_error_t result = json_check(d, bytes, size, true);
    if (result != GT_PACKAGE_JSON_OK) {
        gt__package_json_reset(d);
    }
    return result;
}

#ifdef GT_HAVE_PLUGIN_LUA
gt_package_json_error_t
gt__package_json_parse_lua(gt_package_json_t *d, const void *bytes, size_t size) {
    return json_check(d, bytes, size, false);
}

bool gt__package_json_lua_node(const gt_package_json_t *d,
                               uint16_t id,
                               gt_bridge_json_node_t *out) {
    if (!d->valid || !id || id > d->used) {
        return false;
    }
    const json_node_t *node = &d->nodes[id];
    const gt_bridge_json_kind_t kinds[] = {
        [J_OBJECT] = GT_BRIDGE_JSON_OBJECT,
        [J_ARRAY] = GT_BRIDGE_JSON_ARRAY,
        [J_STRING] = GT_BRIDGE_JSON_STRING,
        [J_NUMBER] = GT_BRIDGE_JSON_NUMBER,
        [J_TRUE] = GT_BRIDGE_JSON_TRUE,
        [J_FALSE] = GT_BRIDGE_JSON_FALSE,
        [J_NULL] = GT_BRIDGE_JSON_NULL,
    };
    const uint8_t *strings = d->strings ? d->strings : (const uint8_t *)"";
    *out = (gt_bridge_json_node_t){
        .kind = kinds[node->kind],
        .child = node->child,
        .next = node->next,
        .count = node->count,
        .key = strings + node->key,
        .key_size = node->key_size,
        .text = node->kind == J_NUMBER ? d->input + node->start : strings + node->text,
        .text_size = node->kind == J_NUMBER ? node->bytes : node->size,
    };
    return true;
}
#endif

bool gt__package_json_node(const gt_package_json_t *d, uint16_t id, gt_package_json_node_t *out) {
    if (!d || !d->valid || !id || id > d->used || !out) {
        return false;
    }
    const json_node_t *node = &d->nodes[id];
    *out = (gt_package_json_node_t){
        .kind = node->kind == J_OBJECT   ? GT_PACKAGE_JSON_OBJECT
                : node->kind == J_ARRAY  ? GT_PACKAGE_JSON_ARRAY
                : node->kind == J_STRING ? GT_PACKAGE_JSON_STRING
                                         : GT_PACKAGE_JSON_OTHER,
        .child = node->child,
        .next = node->next,
        .count = node->count,
        .key = node->key_size ? d->strings + node->key : NULL,
        .text = node->size ? d->strings + node->text : NULL,
        .key_size = node->key_size,
        .text_size = node->size,
    };
    return true;
}

bool gt__package_json_uint(const gt_package_json_t *d, uint16_t id, uint64_t *out) {
    return d && d->valid && id && id <= d->used && out && node_uint(d, id, out);
}

void gt__package_json_reset(gt_package_json_t *d) {
    if (d) {
        json_buffers_free(d);
        d->valid = false;
        d->input = NULL;
        d->input_size = 0;
    }
}

void gt__package_json_destroy(gt_package_json_t **out) {
    if (out && *out) {
        gt_package_json_t *d = *out;
        *out = NULL;
        gt_allocator_t a = d->allocator;
        json_buffers_free(d);
        gt__plugin_memory_free(&a, d, sizeof(*d));
    }
}
