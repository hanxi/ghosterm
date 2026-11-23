#define _POSIX_C_SOURCE 200809L
#include "extension/lua-vm.h"
#include "extension/memory.h"
#include <lauxlib.h>
#include <lualib.h>
#include <string.h>
#include <time.h>

static void *allocate(void *opaque, void *ptr, size_t old_size, size_t new_size) {
    gt_lua_vm_t *vm = opaque;
    if (!ptr) {
        old_size = 0;
    }
    if (!new_size) {
        gt__plugin_memory_free(NULL, ptr, old_size);
        vm->bytes -= old_size;
        return NULL;
    }
    if (new_size > vm->limit - (vm->bytes - old_size)) {
        return NULL;
    }
    if (ptr && old_size == new_size) {
        return ptr;
    }
    void *next = gt__plugin_memory_alloc(NULL, new_size);
    if (!next) {
        return NULL;
    }
    if (ptr) {
        memcpy(next, ptr, old_size < new_size ? old_size : new_size);
        gt__plugin_memory_free(NULL, ptr, old_size);
    }
    vm->bytes = vm->bytes - old_size + new_size;
    return next;
}

static uint64_t cpu_now(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) {
        return UINT64_MAX;
    }
    return (uint64_t)time.tv_sec * 1000000000 + (uint64_t)time.tv_nsec;
}

static gt_lua_vm_t *vm_for(lua_State *state) {
    return *(gt_lua_vm_t **)lua_getextraspace(state);
}

static gt_runtime_error_t interruption(const gt_lua_vm_t *vm) {
    if (vm->stop && atomic_load(vm->stop)) {
        return GT_RUNTIME_CANCELLED;
    }
    if (cpu_now() >= vm->deadline_ns) {
        return GT_RUNTIME_CPU;
    }
    return GT_RUNTIME_NONE;
}

static void check_budget(lua_State *state, lua_Debug *event) {
    (void)event;
    gt_lua_vm_t *vm = vm_for(state);
    vm->interrupted = interruption(vm);
    if (vm->interrupted != GT_RUNTIME_NONE) {
        lua_yield(state, 0);
    }
}

static int value_type(lua_State *state) {
    luaL_checkany(state, 1);
    lua_pushstring(state, luaL_typename(state, 1));
    return 1;
}

static int prepare(lua_State *state) {
    gt_lua_vm_t *vm = vm_for(state);
    /* Open the pinned base library during setup, then keep only the allowlist
     * in the global table before the plugin chunk receives its environment. */
    luaopen_base(state);
    int base = lua_gettop(state);
    lua_newtable(state);
    static const char *const safe_base[] = {
        "assert",
        "error",
        "ipairs",
        "next",
        "pairs",
        "select",
        "tonumber",
        "tostring",
    };
    for (size_t i = 0; i < sizeof(safe_base) / sizeof(*safe_base); ++i) {
        lua_getfield(state, base, safe_base[i]);
        lua_setfield(state, -2, safe_base[i]);
    }
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pop(state, 1);
    lua_pushcfunction(state, value_type);
    lua_setglobal(state, "type");
    static const luaL_Reg libraries[] = {
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {NULL, NULL},
    };
    for (const luaL_Reg *lib = libraries; lib->name; ++lib) {
        luaL_requiref(state, lib->name, lib->func, 1);
        if (lib->func == luaopen_string) {
            lua_pushnil(state);
            lua_setfield(state, -2, "dump");
            lua_pushnil(state);
            lua_setfield(state, -2, "rep");
            lua_pushnil(state);
            lua_setfield(state, -2, "find");
            lua_pushnil(state);
            lua_setfield(state, -2, "match");
            lua_pushnil(state);
            lua_setfield(state, -2, "gmatch");
            lua_pushnil(state);
            lua_setfield(state, -2, "gsub");
        }
        if (lib->func == luaopen_table) {
            lua_pushnil(state);
            lua_setfield(state, -2, "sort");
        }
        lua_pop(state, 1);
    }
    lua_State *thread = lua_newthread(state);
    const int status = luaL_loadbufferx(state, vm->source, vm->source_size, vm->name, "t");
    if (status != LUA_OK) {
        vm->interrupted = status == LUA_ERRMEM ? GT_RUNTIME_MEMORY : GT_RUNTIME_SCRIPT;
        return lua_error(state);
    }
    lua_xmove(state, thread, 1);
    vm->thread = thread;
    return 1;
}

gt_runtime_error_t gt__lua_vm_open(gt_lua_vm_t *vm,
                                   const gt_runtime_module_t *module,
                                   size_t memory_bytes,
                                   uint32_t cpu_ms,
                                   const atomic_bool *stop) {
    *vm = (gt_lua_vm_t){
        .limit = memory_bytes,
        .cpu_ms = cpu_ms,
        .stop = stop,
        .name = module->name,
        .source = module->source,
        .source_size = module->size,
    };
    vm->state = lua_newstate(allocate, vm, 0);
    if (!vm->state) {
        return GT_RUNTIME_MEMORY;
    }
    *(gt_lua_vm_t **)lua_getextraspace(vm->state) = vm;
    lua_pushcfunction(vm->state, prepare);
    const int status = lua_pcall(vm->state, 0, 1, 0);
    vm->name = NULL;
    vm->source = NULL;
    vm->source_size = 0;
    if (status == LUA_OK) {
        return GT_RUNTIME_NONE;
    }
    const gt_runtime_error_t error = status == LUA_ERRMEM || vm->interrupted == GT_RUNTIME_MEMORY
                                         ? GT_RUNTIME_MEMORY
                                         : GT_RUNTIME_SCRIPT;
    gt__lua_vm_close(vm);
    return error;
}

gt_runtime_error_t gt__lua_vm_resume(gt_lua_vm_t *vm, int nargs, int *results, bool *yielded) {
    *results = 0;
    *yielded = false;
    const uint64_t now = cpu_now();
    const uint64_t budget = (uint64_t)vm->cpu_ms * 1000000;
    vm->deadline_ns = now > UINT64_MAX - budget ? UINT64_MAX : now + budget;
    vm->interrupted = interruption(vm);
    if (vm->interrupted != GT_RUNTIME_NONE) {
        return vm->interrupted;
    }
    lua_sethook(vm->thread, check_budget, LUA_MASKCOUNT, 1000);
    const int status = lua_resume(vm->thread, NULL, nargs, results);
    if (vm->interrupted != GT_RUNTIME_NONE) {
        return vm->interrupted;
    }
    const gt_runtime_error_t after = interruption(vm);
    if (after != GT_RUNTIME_NONE) {
        return after;
    }
    *yielded = status == LUA_YIELD;
    if (status == LUA_OK || *yielded) {
        return GT_RUNTIME_NONE;
    }
    return status == LUA_ERRMEM ? GT_RUNTIME_MEMORY : GT_RUNTIME_SCRIPT;
}

void gt__lua_vm_close(gt_lua_vm_t *vm) {
    if (vm->state) {
        lua_close(vm->state);
    }
    *vm = (gt_lua_vm_t){0};
}
