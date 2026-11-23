#ifndef GT_EXTENSION_LUA_RPC_H
#define GT_EXTENSION_LUA_RPC_H

#include "extension/lua-json.h"
#include "extension/lua-vm.h"
#include "extension/lua-resource.h"

typedef struct {
    gt_ext_session_t *session;
    gt_ext_lease_t lease;
    gt_bridge_wire_t *workspace;
    gt_lua_resource_t *resource;
    const char *handlers[GT_EXT_RPC_METHODS_MAX];
    size_t handler_count;
    uint64_t pending, inbound_ticket;
    lua_Integer generation;
    gt_ext_rpc_result_t result;
    bool ready, wait_tick;
} gt_lua_rpc_t;

/* One outstanding call per coroutine. The owner supplies a stable binding
 * and services the Session before polling. Push inside a Lua protected call. */
void gt__lua_rpc_push_call(lua_State *, gt_lua_rpc_t *);
/* Adds start/poll/cancel closures to the table at the top. The async binding
 * has its own request slot, separate from the blocking coroutine binding. */
void gt__lua_rpc_set_async(lua_State *, gt_lua_rpc_t *);
bool gt__lua_rpc_poll(gt_lua_rpc_t *);

/* Dispatches one inbound method from the raw handler table on vm->state.
 * The entry coroutine must have completed before first use. WAIT covers an
 * empty receive queue or a handler suspended in its outbound RPC. A terminal
 * VM error requires the Session owner to stop the runtime. */
gt_ext_rpc_status_t
gt__lua_rpc_dispatch(gt_lua_rpc_t *, gt_lua_vm_t *, int handlers, gt_runtime_error_t *);

#endif
