#include "extension/lua-rpc.h"
#include "base/secure-random.h"
#include "extension/session-owner.h"
#include <lauxlib.h>
#include <string.h>

static int failure(lua_State *state, const char *code, const char *message) {
    lua_pushnil(state);
    lua_createtable(state, 0, 2);
    lua_pushstring(state, code);
    lua_setfield(state, -2, "code");
    lua_pushstring(state, message);
    lua_setfield(state, -2, "message");
    return 2;
}

static int result_value(lua_State *state, gt_lua_rpc_t *rpc) {
    if (!rpc->ready) {
        return failure(state, "RPC_PENDING", "RPC resumed before a result was available");
    }
    rpc->ready = false;
    gt_ext_rpc_state_t outcome = rpc->result.state;
    gt_status_t decoded = GT_ERR_PROTOCOL;
    if (rpc->result.outcome_size) {
        decoded = gt__lua_json_decode(
            state, rpc->workspace, rpc->result.outcome, rpc->result.outcome_size);
        (void)gt__bridge_json_parse(rpc->workspace, NULL, 0);
    }
    gt__secure_erase(&rpc->result, sizeof(rpc->result));

    if (decoded == GT_OK) {
        if (outcome == GT_EXT_RPC_SUCCEEDED) {
            lua_pushliteral(state, "value");
            lua_rawget(state, -2);
            return 1;
        }
        if (outcome == GT_EXT_RPC_FAILED) {
            lua_pushnil(state);
            lua_pushliteral(state, "error");
            lua_rawget(state, -3);
            return 2;
        }
        if (outcome == GT_EXT_RPC_CANCELLED) {
            lua_pushliteral(state, "effect");
            lua_rawget(state, -2);
            failure(state, "RPC_CANCELLED", "RPC was cancelled");
            lua_pushvalue(state, -3);
            lua_setfield(state, -2, "effect");
            return 2;
        }
    }
    if (outcome == GT_EXT_RPC_UNKNOWN) {
        return failure(state, "RPC_UNKNOWN", "RPC outcome could not be confirmed");
    }
    return failure(state, "RPC_INVALID_RESULT", "RPC returned an invalid result");
}

static int resume_call(lua_State *state, int status, lua_KContext context) {
    (void)status;
    (void)context;
    return result_value(state, lua_touserdata(state, lua_upvalueindex(1)));
}

static int queue_call(lua_State *state, gt_lua_rpc_t *rpc) {
    if (rpc->pending || rpc->ready) {
        return failure(state, "RPC_BUSY", "A request is already pending");
    }
    const int count = lua_gettop(state);
    size_t length = 0;
    const char *method =
        lua_type(state, 1) == LUA_TSTRING ? lua_tolstring(state, 1, &length) : NULL;
    const lua_Integer timeout = count == 2 ? 5000 : lua_tointeger(state, 3);
    if (count < 2 || count > 3 || !method || !length || length > 128 ||
        memchr(method, '\0', length) || (count == 3 && !lua_isinteger(state, 3)) || timeout < 1 ||
        timeout > 3600000) {
        return failure(state, "RPC_INVALID_ARGUMENT", "Invalid RPC method or timeout");
    }

    char body[GT_EXT_RPC_BODY_MAX + 1];
    size_t size;
    if (!gt__lua_json_encode(state, 2, GT_EXT_JSON_ANY, body, sizeof(body), &size)) {
        return failure(state, "RPC_INVALID_INPUT", "Input exceeds the RPC JSON contract");
    }
    const gt_ext_rpc_status_t status = gt_ext_session_request(
        rpc->session, &rpc->lease, method, body, size, (uint32_t)timeout, &rpc->pending);
    gt__secure_erase(body, sizeof(body));
    switch (status) {
        case GT_EXT_RPC_OK:
            return 0;
        case GT_EXT_RPC_DENIED:
            return failure(state, "RPC_DENIED", "RPC authorization denied");
        case GT_EXT_RPC_UNSUPPORTED:
            return failure(state, "RPC_UNSUPPORTED", "RPC method is unavailable");
        case GT_EXT_RPC_CLOSED:
            return failure(state, "RPC_CLOSED", "RPC session is closed");
        case GT_EXT_RPC_WAIT:
            return failure(state, "RPC_BUSY", "RPC queue is full");
        case GT_EXT_RPC_INVALID:
            return failure(state, "RPC_INVALID_INPUT", "Invalid RPC input");
    }
    return failure(state, "RPC_UNAVAILABLE", "RPC request could not be queued");
}

static int call(lua_State *state) {
    gt_lua_rpc_t *rpc = lua_touserdata(state, lua_upvalueindex(1));
    if (!lua_isyieldable(state)) {
        return failure(state, "RPC_UNAVAILABLE", "RPC requires a plugin coroutine");
    }
    const int results = queue_call(state, rpc);
    return results ? results : lua_yieldk(state, 0, 0, resume_call);
}

static int start(lua_State *state) {
    gt_lua_rpc_t *rpc = lua_touserdata(state, lua_upvalueindex(1));
    if (rpc->generation == LUA_MAXINTEGER) {
        return failure(state, "RPC_BUSY", "Async request sequence exhausted");
    }
    const int results = queue_call(state, rpc);
    if (results) {
        return results;
    }
    lua_pushinteger(state, ++rpc->generation);
    return 1;
}

static bool current(lua_State *state, const gt_lua_rpc_t *rpc) {
    return lua_isinteger(state, 1) && lua_tointeger(state, 1) == rpc->generation &&
           (rpc->pending || rpc->ready);
}

static int poll(lua_State *state) {
    gt_lua_rpc_t *rpc = lua_touserdata(state, lua_upvalueindex(1));
    if (!current(state, rpc)) {
        return failure(state, "RPC_INVALID_ARGUMENT", "Unknown async request");
    }
    if (!gt__lua_rpc_poll(rpc)) {
        lua_pushnil(state);
        return 1;
    }
    return result_value(state, rpc);
}

static int cancel(lua_State *state) {
    gt_lua_rpc_t *rpc = lua_touserdata(state, lua_upvalueindex(1));
    bool requested = false;
    if (current(state, rpc) && !gt__lua_rpc_poll(rpc)) {
        requested =
            gt_ext_session_cancel_request(rpc->session, &rpc->lease, rpc->pending) == GT_EXT_RPC_OK;
    }
    lua_pushboolean(state, requested);
    return 1;
}

void gt__lua_rpc_set_async(lua_State *state, gt_lua_rpc_t *rpc) {
    const luaL_Reg methods[] = {{"start", start}, {"poll", poll}, {"cancel", cancel}, {NULL, NULL}};
    lua_pushlightuserdata(state, rpc);
    luaL_setfuncs(state, methods, 1);
}

void gt__lua_rpc_push_call(lua_State *state, gt_lua_rpc_t *rpc) {
    lua_pushlightuserdata(state, rpc);
    lua_pushcclosure(state, call, 1);
}

bool gt__lua_rpc_poll(gt_lua_rpc_t *rpc) {
    if (rpc->ready) {
        return true;
    }
    if (!rpc->pending) {
        return false;
    }
    const gt_ext_rpc_status_t status =
        gt_ext_session_request_result(rpc->session, &rpc->lease, rpc->pending, &rpc->result);
    if (status == GT_EXT_RPC_OK) {
        if (rpc->result.state == GT_EXT_RPC_QUEUED || rpc->result.state == GT_EXT_RPC_PENDING) {
            return false;
        }
        (void)gt_ext_session_forget_request(rpc->session, &rpc->lease, rpc->pending);
    } else {
        rpc->result.state = GT_EXT_RPC_UNKNOWN;
    }
    rpc->pending = 0;
    rpc->ready = true;
    return true;
}

typedef struct {
    gt_lua_rpc_t *rpc;
    gt_lua_vm_t *vm;
    const gt_ext_rpc_request_t *request;
} inbound_call_t;

static int prepare_handler(lua_State *state) {
    const inbound_call_t *setup = lua_touserdata(state, 2);
    lua_pushstring(state, setup->request->method);
    if (lua_rawget(state, 1) != LUA_TFUNCTION) {
        return 0;
    }
    if (!lua_checkstack(setup->vm->thread, 2)) {
        return luaL_error(state, "handler stack unavailable");
    }
    const gt_status_t decoded = gt__lua_json_decode(
        state, setup->rpc->workspace, setup->request->input, setup->request->input_size);
    (void)gt__bridge_json_parse(setup->rpc->workspace, NULL, 0);
    if (decoded != GT_OK) {
        return luaL_error(state, "invalid inbound JSON");
    }
    lua_xmove(state, setup->vm->thread, 2);
    lua_pushboolean(state, true);
    return 1;
}

static gt_ext_rpc_status_t respond(gt_lua_rpc_t *rpc, const char *body, size_t size) {
    const uint64_t ticket = rpc->inbound_ticket;
    rpc->inbound_ticket = 0;
    const gt_ext_rpc_status_t status =
        gt_ext_session_check_request(rpc->session, &rpc->lease, ticket);
    if (status != GT_EXT_RPC_OK) {
        return status;
    }
    return gt_ext_session_respond(rpc->session, &rpc->lease, ticket, body, size);
}

static gt_ext_rpc_status_t respond_failure(gt_lua_rpc_t *rpc, bool missing) {
    const char *body = missing
                           ? "{\"kind\":\"failed\",\"error\":{\"code\":\"UNSUPPORTED\","
                             "\"message\":\"No Lua handler for this method\",\"effect\":\"none\"}}"
                           : "{\"kind\":\"failed\",\"error\":{\"code\":\"INTERNAL\","
                             "\"message\":\"Lua handler failed\",\"effect\":\"possible\"}}";
    return respond(rpc, body, strlen(body));
}

static gt_ext_rpc_status_t
begin_dispatch(gt_lua_rpc_t *rpc, gt_lua_vm_t *vm, int handlers, gt_runtime_error_t *error) {
    if (!rpc->handler_count) {
        return GT_EXT_RPC_WAIT;
    }
    gt_ext_rpc_request_t request;
    const gt_ext_rpc_status_t taken = gt__ext_session_take_request_methods(
        rpc->session, &rpc->lease, rpc->handlers, rpc->handler_count, &request);
    if (taken != GT_EXT_RPC_OK) {
        return taken;
    }
    rpc->inbound_ticket = request.ticket;
    const gt_ext_rpc_status_t checked =
        gt_ext_session_check_request(rpc->session, &rpc->lease, rpc->inbound_ticket);
    if (checked != GT_EXT_RPC_OK) {
        rpc->inbound_ticket = 0;
        return checked;
    }
    lua_State *state = vm->state;
    if (lua_resetthread(vm->thread) != LUA_OK || !lua_checkstack(state, 3)) {
        *error = GT_RUNTIME_MEMORY;
        return respond_failure(rpc, false);
    }
    handlers = lua_absindex(state, handlers);
    const inbound_call_t setup = {.rpc = rpc, .vm = vm, .request = &request};
    lua_pushcfunction(state, prepare_handler);
    lua_pushvalue(state, handlers);
    lua_pushlightuserdata(state, (void *)&setup);
    const int status = lua_pcall(state, 2, 1, 0);
    const bool found = status == LUA_OK && lua_toboolean(state, -1);
    lua_pop(state, 1);
    gt__secure_erase(&request, sizeof(request));
    if (!found) {
        if (status == LUA_ERRMEM) {
            *error = GT_RUNTIME_MEMORY;
        }
        const gt_ext_rpc_status_t sent = respond_failure(rpc, status == LUA_OK);
        return sent == GT_EXT_RPC_OK ? GT_EXT_RPC_WAIT : sent;
    }
    return GT_EXT_RPC_OK;
}

static gt_ext_rpc_status_t finish_dispatch(gt_lua_rpc_t *rpc, lua_State *thread, int results) {
    static const char success[] = "{\"kind\":\"success\",\"value\":";
    static const char failed[] = "{\"kind\":\"failed\",\"error\":";
    const bool business_failure = results == 2 && lua_isnil(thread, -2);
    const char *prefix = business_failure ? failed : success;
    const size_t prefix_size = strlen(prefix);
    char body[GT_EXT_RPC_BODY_MAX + 1];
    size_t value_size;
    if ((!business_failure && results != 1) ||
        !gt__lua_json_encode(thread,
                             -1,
                             business_failure ? GT_EXT_JSON_OBJECT : GT_EXT_JSON_ANY,
                             body + prefix_size,
                             sizeof(body) - prefix_size - 1,
                             &value_size)) {
        return respond_failure(rpc, false);
    }
    memcpy(body, prefix, prefix_size);
    body[prefix_size + value_size] = '}';
    const uint64_t ticket = rpc->inbound_ticket;
    const gt_ext_rpc_status_t status = respond(rpc, body, prefix_size + value_size + 1);
    gt__secure_erase(body, sizeof(body));
    if (status == GT_EXT_RPC_INVALID) {
        rpc->inbound_ticket = ticket;
        return respond_failure(rpc, false);
    }
    return status;
}

gt_ext_rpc_status_t
gt__lua_rpc_dispatch(gt_lua_rpc_t *rpc, gt_lua_vm_t *vm, int handlers, gt_runtime_error_t *error) {
    *error = GT_RUNTIME_NONE;
    int nargs = 0;
    if (!rpc->inbound_ticket) {
        const gt_ext_rpc_status_t status = begin_dispatch(rpc, vm, handlers, error);
        if (status != GT_EXT_RPC_OK || *error != GT_RUNTIME_NONE) {
            return status;
        }
        nargs = 1;
    } else {
        const gt_ext_rpc_status_t status =
            gt_ext_session_check_request(rpc->session, &rpc->lease, rpc->inbound_ticket);
        if (status != GT_EXT_RPC_OK) {
            *error = GT_RUNTIME_CANCELLED;
            return status;
        }
        if (!rpc->wait_tick && !gt__lua_rpc_poll(rpc) &&
            (!rpc->resource || !gt__lua_resource_poll(rpc->resource))) {
            return GT_EXT_RPC_WAIT;
        }
    }
    rpc->wait_tick = false;
    int results;
    bool yielded;
    *error = gt__lua_vm_resume(vm, nargs, &results, &yielded);
    if (*error != GT_RUNTIME_NONE) {
        return respond_failure(rpc, false);
    }
    if (yielded) {
        if (!rpc->wait_tick && !rpc->pending && (!rpc->resource || !rpc->resource->publication)) {
            *error = GT_RUNTIME_PROTOCOL;
            return respond_failure(rpc, false);
        }
        return GT_EXT_RPC_WAIT;
    }
    return finish_dispatch(rpc, vm->thread, results);
}
