#ifndef GTERM_TEST_ECHO_OUTCOME_H
#define GTERM_TEST_ECHO_OUTCOME_H

#include "extension/wire.h"
#include "ghosterm.h"
#include <string.h>

static bool echo_string(const gt_bridge_wire_t *doc, uint16_t node, const char *expected) {
    const uint8_t *bytes;
    size_t size;
    return gt__bridge_wire_string(doc, node, &bytes, &size) && size == strlen(expected) &&
           !memcmp(bytes, expected, size);
}

static bool echo_uint(const gt_bridge_wire_t *doc, uint16_t node, uint64_t expected) {
    uint64_t actual;
    return gt__bridge_wire_uint(doc, node, &actual) && actual == expected;
}

static bool echo_matches(const gt_ext_rpc_result_t *result,
                         const char *text,
                         uint64_t count,
                         uint64_t owner,
                         uint64_t calls) {
    gt_bridge_wire_t *doc = NULL;
    if (result->state != GT_EXT_RPC_SUCCEEDED || gt__bridge_wire_new(NULL, &doc) != GT_OK) {
        return false;
    }
    const char *const envelope_fields[] = {"kind", "value"};
    const char *const value_fields[] = {"text", "count", "owner", "calls"};
    bool valid = gt__bridge_json_parse(doc, result->outcome, result->outcome_size) == GT_OK &&
                 gt__bridge_wire_object_fields(doc, 1, envelope_fields, 2) &&
                 echo_string(doc, gt__bridge_wire_member(doc, 1, "kind"), "success");
    if (valid) {
        const uint16_t value = gt__bridge_wire_member(doc, 1, "value");
        valid = gt__bridge_wire_object_fields(doc, value, value_fields, 4) &&
                echo_string(doc, gt__bridge_wire_member(doc, value, "text"), text) &&
                echo_uint(doc, gt__bridge_wire_member(doc, value, "count"), count) &&
                echo_uint(doc, gt__bridge_wire_member(doc, value, "owner"), owner) &&
                echo_uint(doc, gt__bridge_wire_member(doc, value, "calls"), calls);
    }
    gt__bridge_wire_free(doc);
    return valid;
}
#endif
