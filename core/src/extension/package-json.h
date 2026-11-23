/* Private lexical preflight, not schema/trust/authorization. */
#ifndef GT_PRIVATE_PACKAGE_JSON_H
#define GT_PRIVATE_PACKAGE_JSON_H
#include "base/allocator.h"
#include <stdbool.h>
#include <stdint.h>
#define GT_PACKAGE_JSON_BYTES 16777216u
#define GT_PACKAGE_JSON_NODES 16384u
#define GT_PACKAGE_JSON_DEPTH 64u
typedef struct gt_package_json gt_package_json_t;

typedef enum {
    GT_PACKAGE_JSON_OK,
    GT_PACKAGE_JSON_SYNTAX,
    GT_PACKAGE_JSON_UTF8,
    GT_PACKAGE_JSON_DUPLICATE,
    GT_PACKAGE_JSON_LIMIT,
    GT_PACKAGE_JSON_NUMBER,
    GT_PACKAGE_JSON_MEMORY
} gt_package_json_error_t;

/* Reusable small owner; buffers grow on demand within the existing hard limits,
 * and are wiped/released after every check. Allocator context outlives destroy. */
bool gt__package_json_open(const gt_allocator_t *, gt_package_json_t **);
/* Raw UTF-8, decoded-key duplicate rejection, bounded depth/nodes/hash work.
 * Metadata integers must be mathematically exact, nonnegative and JS-safe;
 * prevents JSON.parse rounding fractional schemaVersion/bytes/protocol values.
 * Other JSON documents can request syntax-only checking with integers=false. */
gt_package_json_error_t
gt__package_json_check(gt_package_json_t *, const void *, size_t, bool integers);

typedef enum {
    GT_PACKAGE_JSON_OBJECT,
    GT_PACKAGE_JSON_ARRAY,
    GT_PACKAGE_JSON_STRING,
    GT_PACKAGE_JSON_OTHER
} gt_package_json_kind_t;

typedef struct {
    gt_package_json_kind_t kind;
    uint16_t child, next, count;
    const uint8_t *key, *text;
    size_t key_size, text_size;
} gt_package_json_node_t;

/* Inspect a checked metadata document starting at node 1. The input remains
 * borrowed until reset; node strings remain valid over the same interval. */
gt_package_json_error_t gt__package_json_parse(gt_package_json_t *, const void *, size_t);
bool gt__package_json_node(const gt_package_json_t *, uint16_t, gt_package_json_node_t *);
bool gt__package_json_uint(const gt_package_json_t *, uint16_t, uint64_t *);
void gt__package_json_reset(gt_package_json_t *);
void gt__package_json_destroy(gt_package_json_t **);

#ifdef GT_HAVE_PLUGIN_LUA
#include "extension/wire.h"
/* Syntax-only input for the Lua codec. The caller supplies the byte limit. */
gt_package_json_error_t gt__package_json_parse_lua(gt_package_json_t *, const void *, size_t);
bool gt__package_json_lua_node(const gt_package_json_t *, uint16_t, gt_bridge_json_node_t *);
#endif
#endif
