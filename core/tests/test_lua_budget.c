#include "extension/lua-json.h"
#include "extension/lua-vm.h"
#include "extension/memory.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "Lua budget assertion at line %d\n", __LINE__);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void exercise(const char *source,
                     size_t instance_limit,
                     uint32_t cpu_ms,
                     gt_runtime_error_t expected,
                     bool verify_global_accounting) {
    const gt_runtime_module_t module = {
        .name = "budget-fixture", .source = source, .size = strlen(source)};
    gt_lua_vm_t vm;
    CHECK(gt__lua_vm_open(&vm, &module, instance_limit, cpu_ms, NULL) == GT_RUNTIME_NONE);
    CHECK(vm.bytes > 0);
    int results;
    bool yielded;
    CHECK(gt__lua_vm_resume(&vm, 0, &results, &yielded) == expected);
    CHECK(!yielded);
    if (verify_global_accounting) {
        CHECK(gt__plugin_memory_info().used == vm.bytes);
    }
    gt__lua_vm_close(&vm);
    CHECK(!vm.state && gt__plugin_memory_info().used == 0);
}

typedef struct {
    bool fail_growth;
    unsigned failures;
} stack_budget_t;

static void *allocate_stack(void *opaque, void *ptr, size_t old_size, size_t new_size) {
    stack_budget_t *budget = opaque;
    if (!new_size) {
        free(ptr);
        return NULL;
    }
    if (budget->fail_growth && (!ptr || new_size > old_size)) {
        ++budget->failures;
        return NULL;
    }
    return realloc(ptr, new_size);
}

static int encode_growing_table(lua_State *state) {
    char encoded[512];
    size_t size = 0;
    void *opaque;
    (void)lua_getallocf(state, &opaque);
    stack_budget_t *budget = opaque;
    const bool inject_failure = lua_toboolean(state, 2);
    budget->fail_growth = inject_failure;
    const bool ok =
        gt__lua_json_encode(state, 1, GT_EXT_JSON_OBJECT, encoded, sizeof(encoded), &size);
    budget->fail_growth = false;
    CHECK(lua_gettop(state) == 2);
    if (inject_failure) {
        CHECK(!ok && budget->failures > 0 && size == 0 && !encoded[0]);
    } else {
        CHECK(ok && size > 0 && strstr(encoded, "\"leaf\":true"));
    }
    return 0;
}

static void exercise_encoding_stack(void) {
    stack_budget_t budget = {0};
    lua_State *state = lua_newstate(allocate_stack, &budget, 0);
    CHECK(state);
    CHECK(luaL_dostring(state, "local t={leaf=true}; for i=1,24 do t={a=t} end; return t") ==
          LUA_OK);
    for (int inject = 1; inject >= 0; --inject) {
        lua_pushcfunction(state, encode_growing_table);
        lua_pushvalue(state, 1);
        lua_pushboolean(state, inject);
        CHECK(lua_pcall(state, 2, 0, 0) == LUA_OK);
        CHECK(lua_gettop(state) == 1 && lua_istable(state, 1));
    }
    lua_close(state);
}

int main(int argc, char **argv) {
    const char *grow = "local entries = {}\nfor i = 1, 40000 do entries[i] = {} end";
    const char *mode = argc == 2 ? argv[1] : "all";
    CHECK(argc <= 2);
    bool all = !strcmp(mode, "all");
    if (all || !strcmp(mode, "cpu")) {
        exercise("local count = 0; for i = 1, 30000000 do count = count + i end; return count",
                 1024 * 1024,
                 10,
                 GT_RUNTIME_CPU,
                 false);
    }
    if (all || !strcmp(mode, "memory")) {
        exercise(grow, 1024 * 1024, 1000, GT_RUNTIME_MEMORY, false);
    }
    if (all || !strcmp(mode, "global")) {
        CHECK(gt__plugin_memory_limit(128 * 1024));
        exercise(grow, 1024 * 1024, 1000, GT_RUNTIME_MEMORY, true);
        CHECK(gt__plugin_memory_info().peak <= 128 * 1024);
        CHECK(gt__plugin_memory_limit(GT_PLUGIN_MEMORY_DEFAULT));
    }
    if (all || !strcmp(mode, "encoding")) {
        exercise_encoding_stack();
    }
    CHECK(all || !strcmp(mode, "encoding") || !strcmp(mode, "cpu") || !strcmp(mode, "memory") ||
          !strcmp(mode, "global") || !strcmp(mode, "control"));
    exercise("return true", 1024 * 1024, 1000, GT_RUNTIME_NONE, false);
    return 0;
}
