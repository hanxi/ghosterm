#define _POSIX_C_SOURCE 200809L
#include "extension/lua-json.h"
#include "extension/package-json.h"
#include "extension/package-runtime.h"
#include "extension/wire.h"
#include <lauxlib.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EXACT_INTEGER_MAX INT64_C(9007199254740991)

typedef struct {
    char *output;
    size_t capacity, size;
    size_t nodes;
    const void *parents[GT_BRIDGE_WIRE_DEPTH_MAX];
} encoder_t;

static char null_value;
static char array_marker;

static bool marked_array(lua_State *state, int index) {
    if (!lua_getmetatable(state, index)) {
        return false;
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &array_marker);
    const bool marked = lua_rawequal(state, -1, -2);
    lua_pop(state, 2);
    return marked;
}

static void mark_array(lua_State *state) {
    if (lua_rawgetp(state, LUA_REGISTRYINDEX, &array_marker) == LUA_TNIL) {
        lua_pop(state, 1);
        lua_createtable(state, 0, 1);
        lua_pushliteral(state, "JSON array");
        lua_setfield(state, -2, "__metatable");
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &array_marker);
    }
    lua_setmetatable(state, -2);
}

int gt__lua_json_array(lua_State *state) {
    lua_createtable(state, 0, 0);
    mark_array(state);
    return 1;
}

int gt__lua_json_is_array(lua_State *state) {
    lua_pushboolean(state, lua_istable(state, 1) && marked_array(state, 1));
    return 1;
}

void gt__lua_json_push_null(lua_State *state) {
    lua_pushlightuserdata(state, &null_value);
}

static bool write_bytes(encoder_t *out, const char *text, size_t size) {
    if (size >= out->capacity - out->size) {
        return false;
    }
    memcpy(out->output + out->size, text, size);
    out->size += size;
    return true;
}

static bool write_char(encoder_t *out, char value) {
    return write_bytes(out, &value, 1);
}

static bool string(encoder_t *out, const char *text, size_t size) {
    if (size > out->capacity || !write_char(out, '"')) {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char byte = (unsigned char)text[i];
        if (byte >= 32 && byte != '"' && byte != '\\') {
            continue;
        }
        if (!gt__runtime_source_utf8(text + start, i - start) ||
            !write_bytes(out, text + start, i - start)) {
            return false;
        }
        if (byte < 32) {
            const char *hex = "0123456789abcdef";
            const char escaped[] = {'\\', 'u', '0', '0', hex[byte / 16], hex[byte % 16]};
            if (!write_bytes(out, escaped, sizeof(escaped))) {
                return false;
            }
        } else if (!write_char(out, '\\') || !write_char(out, (char)byte)) {
            return false;
        }
        start = i + 1;
    }
    return gt__runtime_source_utf8(text + start, size - start) &&
           write_bytes(out, text + start, size - start) && write_char(out, '"');
}

static bool number(lua_State *state, int index, encoder_t *out) {
    char text[48];
    int size;
    if (lua_isinteger(state, index)) {
        const lua_Integer integer = lua_tointeger(state, index);
        if (integer < -EXACT_INTEGER_MAX || integer > EXACT_INTEGER_MAX) {
            return false;
        }
        size = snprintf(text, sizeof(text), "%lld", (long long)integer);
    } else {
        const lua_Number value = lua_tonumber(state, index);
        if (!isfinite(value) || (fabs(value) > EXACT_INTEGER_MAX && trunc(value) == value)) {
            return false;
        }
        locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        if (!c_locale) {
            return false;
        }
        locale_t previous = uselocale(c_locale);
        if (!previous) {
            freelocale(c_locale);
            return false;
        }
        size = snprintf(text, sizeof(text), "%.17g", (double)value);
        (void)uselocale(previous);
        freelocale(c_locale);
    }
    return size > 0 && (size_t)size < sizeof(text) && write_bytes(out, text, (size_t)size);
}

static bool value(lua_State *, int, encoder_t *, size_t, gt_ext_json_shape_t);

static bool
table(lua_State *state, int index, encoder_t *out, size_t depth, gt_ext_json_shape_t expected) {
    if (depth == GT_BRIDGE_WIRE_DEPTH_MAX || !lua_checkstack(state, 3)) {
        return false;
    }
    const void *identity = lua_topointer(state, index);
    for (size_t i = 0; i < depth; ++i) {
        if (out->parents[i] == identity) {
            return false;
        }
    }
    out->parents[depth] = identity;
    size_t count = 0, maximum = 0;
    bool object = false, array = false;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        if (++count > GT_BRIDGE_WIRE_NODES_MAX || count >= out->capacity) {
            return false;
        }
        if (lua_type(state, -2) == LUA_TSTRING) {
            object = true;
        } else if (lua_isinteger(state, -2)) {
            const lua_Integer key = lua_tointeger(state, -2);
            if (key < 1 || key > GT_BRIDGE_WIRE_NODES_MAX) {
                return false;
            }
            array = true;
            if ((size_t)key > maximum) {
                maximum = (size_t)key;
            }
        } else {
            return false;
        }
        lua_pop(state, 1);
    }
    if (array && (object || count != maximum)) {
        return false;
    }
    const bool marked = marked_array(state, index);
    if (marked && object) {
        return false;
    }
    if (!count) {
        array = marked || expected == GT_EXT_JSON_ARRAY;
    }
    if ((array && expected == GT_EXT_JSON_OBJECT) || (!array && expected == GT_EXT_JSON_ARRAY)) {
        return false;
    }
    if (array) {
        if (!write_char(out, '[')) {
            return false;
        }
        for (size_t i = 1; i <= count; ++i) {
            lua_rawgeti(state, index, (lua_Integer)i);
            if ((i > 1 && !write_char(out, ',')) ||
                !value(state, -1, out, depth + 1, GT_EXT_JSON_ANY)) {
                return false;
            }
            lua_pop(state, 1);
        }
        return write_char(out, ']');
    }
    if (!write_char(out, '{')) {
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, index)) {
        size_t size;
        const char *key = lua_tolstring(state, -2, &size);
        if (!string(out, key, size) || !write_char(out, ':') ||
            !value(state, -1, out, depth + 1, GT_EXT_JSON_ANY)) {
            return false;
        }
        lua_pop(state, 1);
        if (--count && !write_char(out, ',')) {
            return false;
        }
    }
    return write_char(out, '}');
}

static bool
value(lua_State *state, int index, encoder_t *out, size_t depth, gt_ext_json_shape_t expected) {
    if (++out->nodes > GT_BRIDGE_WIRE_NODES_MAX) {
        return false;
    }
    const int type = lua_type(state, index);
    if (type == LUA_TTABLE) {
        return (expected == GT_EXT_JSON_ANY || expected == GT_EXT_JSON_OBJECT ||
                expected == GT_EXT_JSON_ARRAY) &&
               table(state, lua_absindex(state, index), out, depth, expected);
    }
    switch (type) {
        case LUA_TBOOLEAN:
            return (expected == GT_EXT_JSON_ANY || expected == GT_EXT_JSON_BOOLEAN) &&
                   (lua_toboolean(state, index) ? write_bytes(out, "true", 4)
                                                : write_bytes(out, "false", 5));
        case LUA_TNUMBER:
            return (expected == GT_EXT_JSON_ANY || expected == GT_EXT_JSON_NUMBER) &&
                   number(state, index, out);
        case LUA_TSTRING: {
            size_t size;
            const char *text = lua_tolstring(state, index, &size);
            return (expected == GT_EXT_JSON_ANY || expected == GT_EXT_JSON_STRING) &&
                   string(out, text, size);
        }
        case LUA_TLIGHTUSERDATA:
            return (expected == GT_EXT_JSON_ANY || expected == GT_EXT_JSON_NULL) &&
                   lua_touserdata(state, index) == &null_value && write_bytes(out, "null", 4);
        default:
            return false;
    }
}

bool gt__lua_json_encode(lua_State *state,
                         int index,
                         gt_ext_json_shape_t root_shape,
                         char *output,
                         size_t capacity,
                         size_t *size) {
    *size = 0;
    if (!capacity) {
        return false;
    }
    const int top = lua_gettop(state);
    encoder_t encoder = {.output = output, .capacity = capacity};
    const bool success = value(state, index, &encoder, 0, root_shape);
    lua_settop(state, top);
    if (success) {
        *size = encoder.size;
    }
    output[*size] = '\0';
    return success;
}

static bool decode_number(lua_State *state, const gt_bridge_json_node_t *node) {
    char text[128];
    if (node->text_size >= sizeof(text)) {
        return false;
    }
    memcpy(text, node->text, node->text_size);
    text[node->text_size] = '\0';
    if (!lua_stringtonumber(state, text)) {
        return false;
    }
    if (lua_isinteger(state, -1)) {
        const lua_Integer integer = lua_tointeger(state, -1);
        return integer >= -EXACT_INTEGER_MAX && integer <= EXACT_INTEGER_MAX;
    }
    const lua_Number value = lua_tonumber(state, -1);
    return isfinite(value) && (fabs(value) <= EXACT_INTEGER_MAX || trunc(value) != value);
}

typedef struct {
    const gt_bridge_wire_t *wire;
    const gt_package_json_t *large;
} decoder_t;

static bool read_node(const decoder_t *document, uint16_t id, gt_bridge_json_node_t *node) {
    return id <= GT_BRIDGE_WIRE_NODES_MAX &&
           (document->wire ? gt__bridge_json_node(document->wire, id, node)
                           : gt__package_json_lua_node(document->large, id, node));
}

static bool decode_node(lua_State *state, const decoder_t *document, uint16_t id, unsigned depth) {
    gt_bridge_json_node_t node;
    if (!lua_checkstack(state, 4) || !read_node(document, id, &node)) {
        return false;
    }
    switch (node.kind) {
        case GT_BRIDGE_JSON_STRING:
            lua_pushlstring(state, (const char *)node.text, node.text_size);
            return true;
        case GT_BRIDGE_JSON_NUMBER:
            return decode_number(state, &node);
        case GT_BRIDGE_JSON_TRUE:
        case GT_BRIDGE_JSON_FALSE:
            lua_pushboolean(state, node.kind == GT_BRIDGE_JSON_TRUE);
            return true;
        case GT_BRIDGE_JSON_NULL:
            gt__lua_json_push_null(state);
            return true;
        case GT_BRIDGE_JSON_ARRAY:
        case GT_BRIDGE_JSON_OBJECT:
            break;
    }
    if (depth >= GT_BRIDGE_WIRE_DEPTH_MAX) {
        return false;
    }
    const bool array = node.kind == GT_BRIDGE_JSON_ARRAY;
    lua_createtable(state, array ? node.count : 0, array ? 0 : node.count);
    if (array) {
        mark_array(state);
    }
    uint16_t child = node.child;
    for (lua_Integer i = 1; child; ++i) {
        gt_bridge_json_node_t item;
        if (!read_node(document, child, &item)) {
            return false;
        }
        if (!array) {
            lua_pushlstring(state, (const char *)item.key, item.key_size);
        }
        if (!decode_node(state, document, child, depth + 1)) {
            return false;
        }
        if (array) {
            lua_rawseti(state, -2, i);
        } else {
            lua_rawset(state, -3);
        }
        child = item.next;
    }
    return true;
}

static int decode_protected(lua_State *state) {
    const decoder_t *document = lua_touserdata(state, 1);
    if (!decode_node(state, document, 1, 0)) {
        return luaL_error(state, "invalid RPC JSON value");
    }
    return 1;
}

static gt_status_t decode_document(lua_State *state, decoder_t *document) {
    if (!lua_checkstack(state, 2)) {
        return GT_ERR_INTERNAL;
    }
    lua_pushcfunction(state, decode_protected);
    lua_pushlightuserdata(state, document);
    const int status = lua_pcall(state, 1, 1, 0);
    if (status == LUA_OK) {
        return GT_OK;
    }
    lua_pop(state, 1);
    return status == LUA_ERRMEM ? GT_ERR_INTERNAL : GT_ERR_PROTOCOL;
}

gt_status_t
gt__lua_json_decode(lua_State *state, gt_bridge_wire_t *workspace, const void *bytes, size_t size) {
    const gt_status_t parsed = gt__bridge_json_parse(workspace, bytes, size);
    decoder_t document = {.wire = workspace};
    return parsed == GT_OK ? decode_document(state, &document) : parsed;
}

gt_status_t gt__lua_json_decode_large(lua_State *state, const void *bytes, size_t size) {
    if (size > GT_LUA_JSON_MAX) {
        return GT_ERR_PROTOCOL;
    }
    gt_package_json_t *workspace = NULL;
    if (!gt__package_json_open(NULL, &workspace)) {
        return GT_ERR_INTERNAL;
    }
    const gt_package_json_error_t parsed = gt__package_json_parse_lua(workspace, bytes, size);
    decoder_t document = {.large = workspace};
    const gt_status_t result = parsed == GT_PACKAGE_JSON_OK ? decode_document(state, &document)
                               : parsed == GT_PACKAGE_JSON_MEMORY ? GT_ERR_INTERNAL
                                                                  : GT_ERR_PROTOCOL;
    gt__package_json_destroy(&workspace);
    return result;
}
