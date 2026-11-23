#include "extension/lua-process.h"
#include "base/secure-random.h"
#include <lauxlib.h>
#include <string.h>

static gt_process_jobs_t *jobs(lua_State *state) {
    return lua_touserdata(state, lua_upvalueindex(1));
}

static int failure(lua_State *state, gt_status_t status) {
    const char *code = status == GT_ERR_AUTH            ? "DENIED"
                       : status == GT_ERR_INVALID_ARG   ? "INVALID_INPUT"
                       : status == GT_ERR_UNSUPPORTED   ? "UNSUPPORTED"
                       : status == GT_PROCESS_JOBS_WAIT ? "BUSY"
                       : status == GT_ERR_CANCELLED     ? "CANCELLED"
                                                        : "IO";
    lua_pushnil(state);
    lua_createtable(state, 0, 2);
    lua_pushstring(state, code);
    lua_setfield(state, -2, "code");
    lua_pushliteral(state, "Process operation failed");
    lua_setfield(state, -2, "message");
    return 2;
}

static const char *text(lua_State *state, int index, size_t maximum) {
    size_t size;
    const char *value =
        lua_type(state, index) == LUA_TSTRING ? lua_tolstring(state, index, &size) : NULL;
    return value && size <= maximum && !memchr(value, 0, size) ? value : NULL;
}

static uint64_t handle(lua_State *state) {
    const lua_Integer value = lua_isinteger(state, 1) ? lua_tointeger(state, 1) : 0;
    return value > 0 ? (uint64_t)value : 0;
}

static int spawn(lua_State *state) {
    const char *program = text(state, 1, 128), *root = text(state, 2, 64);
    if (!program || !*program || !root || !*root || !lua_istable(state, 3)) {
        return failure(state, GT_ERR_INVALID_ARG);
    }
    if (!lua_isnoneornil(state, 4)) {
        const char *expected = text(state, 4, 128);
        const char *version = gt__process_jobs_version(jobs(state), program);
        if (!expected || !version || strcmp(expected, version)) {
            return failure(state, GT_ERR_UNSUPPORTED);
        }
    }
    const size_t count = lua_rawlen(state, 3);
    if (count >= GT_EXT_PROCESS_ARGUMENTS_MAX) {
        return failure(state, GT_ERR_INVALID_ARG);
    }
    lua_pushnil(state);
    while (lua_next(state, 3)) {
        const lua_Integer key = lua_isinteger(state, -2) ? lua_tointeger(state, -2) : 0;
        if (key < 1 || (uint64_t)key > count) {
            lua_pop(state, 2);
            return failure(state, GT_ERR_INVALID_ARG);
        }
        lua_pop(state, 1);
    }
    const char *argv[GT_EXT_PROCESS_ARGUMENTS_MAX] = {program};
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(state, 3, (lua_Integer)i + 1);
        argv[i + 1] = text(state, -1, 4096);
        lua_pop(state, 1);
        if (!argv[i + 1]) {
            return failure(state, GT_ERR_INVALID_ARG);
        }
    }
    uint64_t id;
    const char *version;
    const gt_status_t status =
        gt__process_jobs_spawn(jobs(state), program, root, argv, count + 1, &id, &version);
    if (status != GT_OK) {
        return failure(state, status);
    }
    lua_pushinteger(state, (lua_Integer)id);
    lua_pushstring(state, version);
    return 2;
}

static int read_pipe(lua_State *state, bool stderr_pipe) {
    const uint64_t id = handle(state);
    const lua_Integer capacity =
        lua_gettop(state) == 1 ? GT_PROCESS_JOBS_IO_MAX : lua_tointeger(state, 2);
    if (!id || capacity < 1 || capacity > GT_PROCESS_JOBS_IO_MAX ||
        (lua_gettop(state) > 1 && !lua_isinteger(state, 2))) {
        return failure(state, GT_ERR_INVALID_ARG);
    }
    char bytes[GT_PROCESS_JOBS_IO_MAX];
    const gt_ext_process_io_t result =
        gt__process_jobs_read(jobs(state), id, stderr_pipe, bytes, (size_t)capacity);
    if (result.state == GT_EXT_PROCESS_DATA) {
        lua_pushlstring(state, bytes, result.count);
        gt__secure_erase(bytes, result.count);
        return 1;
    }
    if (result.state == GT_EXT_PROCESS_WAIT || result.state == GT_EXT_PROCESS_EOF) {
        lua_pushnil(state);
        lua_pushstring(state, result.state == GT_EXT_PROCESS_WAIT ? "wait" : "eof");
        return 2;
    }
    return failure(state, result.error);
}

static int read_stdout(lua_State *state) {
    return read_pipe(state, false);
}

static int read_stderr(lua_State *state) {
    return read_pipe(state, true);
}

static int write_stdin(lua_State *state) {
    size_t size;
    const char *data = lua_type(state, 2) == LUA_TSTRING ? lua_tolstring(state, 2, &size) : NULL;
    if (!data || !size || size > GT_PROCESS_JOBS_IO_MAX) {
        return failure(state, GT_ERR_INVALID_ARG);
    }
    const gt_ext_process_io_t result =
        gt__process_jobs_write(jobs(state), handle(state), data, size);
    if (result.state == GT_EXT_PROCESS_DATA || result.state == GT_EXT_PROCESS_WAIT) {
        lua_pushinteger(state, (lua_Integer)result.count);
        return 1;
    }
    return failure(state, result.error);
}

static int result(lua_State *state, gt_status_t status) {
    if (status != GT_OK) {
        return failure(state, status);
    }
    lua_pushboolean(state, true);
    return 1;
}

static int close_stdin(lua_State *state) {
    return result(state, gt__process_jobs_input_close(jobs(state), handle(state)));
}

static int terminate(lua_State *state) {
    return result(state, gt__process_jobs_signal(jobs(state), handle(state), false));
}

static int release(lua_State *state) {
    return result(state, gt__process_jobs_release(jobs(state), handle(state)));
}

static int poll(lua_State *state) {
    gt_process_job_info_t info;
    const gt_status_t status = gt__process_jobs_poll(jobs(state), handle(state), &info);
    if (status != GT_OK) {
        return failure(state, status);
    }
    lua_createtable(state, 0, 4);
    lua_pushboolean(state, info.process.exited);
    lua_setfield(state, -2, "exited");
    if (info.process.exited) {
        lua_pushinteger(state, info.process.exit_code);
        lua_setfield(state, -2, "exitCode");
        lua_pushinteger(state, info.process.term_signal);
        lua_setfield(state, -2, "termSignal");
    }
    lua_pushstring(state, info.reason);
    lua_setfield(state, -2, "reason");
    return 1;
}

void gt__lua_process_set_methods(lua_State *state, gt_process_jobs_t *owner) {
    static const luaL_Reg methods[] = {
        {"spawn", spawn},
        {"readStdout", read_stdout},
        {"readStderr", read_stderr},
        {"writeStdin", write_stdin},
        {"closeStdin", close_stdin},
        {"terminate", terminate},
        {"poll", poll},
        {"release", release},
        {NULL, NULL},
    };
    lua_pushlightuserdata(state, owner);
    luaL_setfuncs(state, methods, 1);
}
