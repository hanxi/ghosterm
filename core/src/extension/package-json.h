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
void gt__package_json_destroy(gt_package_json_t **);
#endif
