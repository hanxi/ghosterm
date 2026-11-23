#include "extension/lua-resource.h"
#include <string.h>

static int failed(lua_State *state, const char *code, const char *message, bool possible) {
    lua_pushnil(state);
    lua_createtable(state, 0, 3);
    lua_pushstring(state, code);
    lua_setfield(state, -2, "code");
    lua_pushstring(state, message);
    lua_setfield(state, -2, "message");
    lua_pushstring(state, possible ? "possible" : "none");
    lua_setfield(state, -2, "effect");
    return 2;
}

static void field(lua_State *state, const char *key, const char *value) {
    lua_pushstring(state, value);
    lua_setfield(state, -2, key);
}

static int finish(lua_State *state, int status, lua_KContext context) {
    (void)status;
    (void)context;
    gt_lua_resource_t *resource = lua_touserdata(state, lua_upvalueindex(1));
    if (!resource->ready) {
        return failed(state, "INTERNAL", "Publication pending", true);
    }
    resource->ready = false;
    const gt_ext_source_info_t *source = &resource->result.source;
    if (resource->status != GT_RUNTIME_RESOURCE_OK || source->phase == GT_EXT_SOURCE_UNCERTAIN) {
        return failed(state, "IO", "Publication outcome could not be confirmed", true);
    }
    if (source->phase != GT_EXT_SOURCE_SUCCEEDED || !source->confirmed) {
        const char *code = resource->result.expired                       ? "EXPIRED"
                           : source->phase == GT_EXT_SOURCE_STOPPED       ? "CANCELLED"
                           : source->error == GT_EXT_SOURCE_LIMIT_REACHED ? "RESOURCE_LIMIT"
                                                                          : "IO";
        return failed(state, code, "Resource publication failed", source->has_transfer);
    }
    lua_createtable(state, 0, 4);
    field(state, "resourceId", source->resource_id);
    field(state, "sha256", source->descriptor.sha256);
    field(state, "mime", source->descriptor.mime);
    lua_pushinteger(state, (lua_Integer)source->descriptor.size);
    lua_setfield(state, -2, "size");
    return 1;
}

static const char *argument(lua_State *state, int index, size_t limit) {
    if (lua_type(state, index) != LUA_TSTRING) {
        return NULL;
    }
    size_t size;
    const char *text = lua_tolstring(state, index, &size);
    return size && size <= limit && !memchr(text, '\0', size) ? text : NULL;
}

static int publish(lua_State *state) {
    gt_lua_resource_t *resource = lua_touserdata(state, lua_upvalueindex(1));
    if (!resource->owner) {
        return failed(state, "UNSUPPORTED", "Resource publication unavailable", false);
    }
    if (!lua_isyieldable(state) || resource->publication || resource->ready) {
        return failed(state, "BUSY", "Publication requires an idle coroutine", false);
    }
    const char *root = argument(state, 1, 64);
    const char *path = argument(state, 2, 4096);
    const char *mime = argument(state, 3, 128);
    const lua_Integer bytes = lua_tointeger(state, 4);
    const lua_Integer timeout = lua_gettop(state) == 4 ? 30000 : lua_tointeger(state, 5);
    if (lua_gettop(state) < 4 || lua_gettop(state) > 5 || !root || !path || !mime ||
        !lua_isinteger(state, 4) || bytes < 1 ||
        (lua_gettop(state) == 5 && !lua_isinteger(state, 5)) || timeout < 1 || timeout > 3600000) {
        return failed(state, "INVALID_INPUT", "Invalid resource publication arguments", false);
    }
    const gt_runtime_resource_status_t status =
        gt__runtime_resources_publish(resource->owner,
                                      root,
                                      path,
                                      mime,
                                      (uint64_t)bytes,
                                      (uint32_t)timeout,
                                      &resource->publication);
    switch (status) {
        case GT_RUNTIME_RESOURCE_OK:
            return lua_yieldk(state, 0, 0, finish);
        case GT_RUNTIME_RESOURCE_DENIED:
            return failed(state, "DENIED", "Resource authorization denied", false);
        case GT_RUNTIME_RESOURCE_UNSUPPORTED:
            return failed(state, "UNSUPPORTED", "Resource publication unavailable", false);
        case GT_RUNTIME_RESOURCE_LIMIT:
            return failed(state, "RESOURCE_LIMIT", "Resource limit reached", false);
        case GT_RUNTIME_RESOURCE_INVALID:
            return failed(state, "INVALID_INPUT", "Invalid resource publication arguments", false);
        case GT_RUNTIME_RESOURCE_BUSY:
            return failed(state, "BUSY", "Resource queue is full", false);
        default:
            return failed(state, "IO", "Resource publication could not start", false);
    }
}

void gt__lua_resource_push_publish(lua_State *state, gt_lua_resource_t *resource) {
    lua_pushlightuserdata(state, resource);
    lua_pushcclosure(state, publish, 1);
}

bool gt__lua_resource_poll(gt_lua_resource_t *resource) {
    if (!resource->publication) {
        return resource->ready;
    }
    resource->status = gt__runtime_resources_publication(
        resource->owner, resource->publication, &resource->result);
    if (resource->status == GT_RUNTIME_RESOURCE_WAIT ||
        (resource->status == GT_RUNTIME_RESOURCE_OK &&
         resource->result.source.phase < GT_EXT_SOURCE_SUCCEEDED)) {
        return false;
    }
    const gt_runtime_resource_status_t forgotten =
        gt__runtime_resources_forget(resource->owner, resource->publication);
    if (forgotten == GT_RUNTIME_RESOURCE_WAIT || forgotten == GT_RUNTIME_RESOURCE_BUSY) {
        return false;
    }
    if (forgotten != GT_RUNTIME_RESOURCE_OK && resource->status == GT_RUNTIME_RESOURCE_OK) {
        resource->status = forgotten;
    }
    resource->publication = 0;
    resource->ready = true;
    return true;
}
