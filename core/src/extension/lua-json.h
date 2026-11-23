#ifndef GT_EXTENSION_LUA_JSON_H
#define GT_EXTENSION_LUA_JSON_H

#include "extension/wire.h"
#include "ghosterm/extension_rpc.h"
#include <lua.h>

#define GT_LUA_JSON_MAX 1048576u
/* The null sentinel is distinct from Lua nil. Empty tables use the requested
 * root shape when known, otherwise they are objects. Uses raw table access.
 * The caller supplies a bounded output buffer, including the final NUL. */
void gt__lua_json_push_null(lua_State *);
int gt__lua_json_array(lua_State *);
int gt__lua_json_is_array(lua_State *);
bool gt__lua_json_encode(lua_State *,
                         int index,
                         gt_ext_json_shape_t root_shape,
                         char *output,
                         size_t capacity,
                         size_t *size);

/* Pushes one value on success, leaves the stack unchanged on failure.
 * The caller owns the lexical workspace; its views borrow bytes until reset. */
gt_status_t gt__lua_json_decode(lua_State *, gt_bridge_wire_t *, const void *, size_t);
gt_status_t gt__lua_json_decode_large(lua_State *, const void *, size_t);

#endif
