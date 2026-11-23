#ifndef GT_EXTENSION_LUA_RESOURCE_H
#define GT_EXTENSION_LUA_RESOURCE_H

#include "extension/runtime-resource.h"
#include <lua.h>

typedef struct {
    gt_runtime_resources_t *owner;
    uint64_t publication;
    bool ready;
    gt_runtime_resource_status_t status;
    gt_runtime_publication_t result;
} gt_lua_resource_t;

/* Push inside a protected call. The owner advances the broker before polling. */
void gt__lua_resource_push_publish(lua_State *, gt_lua_resource_t *);
bool gt__lua_resource_poll(gt_lua_resource_t *);

#endif
