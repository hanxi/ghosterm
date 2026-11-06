/* Private S4 wire candidate: bounded UTF-8 JSON + generated envelope schema.
 * Validation is not host authentication, consent, method permission or delivery.
 * No allocation during parse. Owner-thread only; raw payload views are borrowed
 * until the next parse/free. Any failed parse invalidates the previous view.
 * No general JSON Schema API or public ABI is promised by this implementation. */
#ifndef GT_BRIDGE_WIRE_H
#define GT_BRIDGE_WIRE_H
#include "extension/frame.h"
#define GT_BRIDGE_WIRE_DEPTH_MAX 32u
#define GT_BRIDGE_WIRE_NODES_MAX 4096u
typedef struct gt_bridge_wire gt_bridge_wire_t;

typedef enum {
    GT_BRIDGE_WIRE_VALID,
    GT_BRIDGE_WIRE_SYNTAX,
    GT_BRIDGE_WIRE_UTF8,
    GT_BRIDGE_WIRE_DUPLICATE,
    GT_BRIDGE_WIRE_LIMIT,
    GT_BRIDGE_WIRE_SCHEMA
} gt_bridge_wire_error_t;

typedef enum {
    GT_BRIDGE_OFFER,
    GT_BRIDGE_REQUEST,
    GT_BRIDGE_RESPONSE,
    GT_BRIDGE_EVENT,
    GT_BRIDGE_CANCEL,
    GT_BRIDGE_CREDIT,
    GT_BRIDGE_RESOURCE_BEGIN,
    GT_BRIDGE_RESOURCE_CHUNK,
    GT_BRIDGE_RESOURCE_END,
    GT_BRIDGE_CLOSE,
    GT_BRIDGE_HELLO,
    GT_BRIDGE_PROBE,
    GT_BRIDGE_ACCEPT,
    GT_BRIDGE_READY,
    GT_BRIDGE_RESOURCE_RESULT,
    GT_BRIDGE_RESOURCE_CANCEL
} gt_bridge_kind_t;

typedef struct {
    char connection[33], instance[33], stream[129];
    uint64_t epoch, sequence;
    gt_bridge_kind_t kind;
    uint16_t payload;
} gt_bridge_message_t;

/* One fixed workspace allocation, charged until free; byte limit is 64 KiB,
 * including whitespace. 32 nested containers, 4096 value nodes; hash work is
 * bounded too. Reject duplicate decoded keys at every object depth, lone UTF-16
 * surrogates, malformed UTF-8, BOM, trailing data and non-JSON number grammar.
 * Counters remain canonical checked decimal strings, not floating point.
 * Numeric protocol fields use exact decimal integer/range checks, without
 * locale, rounding, exponent overflow or a dependency on IEEE-754 conversion. */
gt_status_t gt__bridge_wire_new(const gt_allocator_t *, gt_bridge_wire_t **);
gt_status_t gt__bridge_wire_parse(gt_bridge_wire_t *, const void *, size_t, gt_bridge_message_t *);
gt_bridge_wire_error_t gt__bridge_wire_error(const gt_bridge_wire_t *);
uint16_t gt__bridge_wire_member(const gt_bridge_wire_t *, uint16_t, const char *);
bool gt__bridge_wire_string(const gt_bridge_wire_t *, uint16_t, const uint8_t **, size_t *);
bool gt__bridge_wire_raw(const gt_bridge_wire_t *, uint16_t, const uint8_t **, size_t *);
/* Typed accessors for validated numeric fields and capability arrays. */
bool gt__bridge_wire_uint(const gt_bridge_wire_t *, uint16_t, uint64_t *);
bool gt__bridge_wire_counter(const gt_bridge_wire_t *, uint16_t, uint64_t *);
size_t gt__bridge_wire_count(const gt_bridge_wire_t *, uint16_t);
uint16_t gt__bridge_wire_at(const gt_bridge_wire_t *, uint16_t, size_t);
void gt__bridge_wire_free(gt_bridge_wire_t *);
#endif
