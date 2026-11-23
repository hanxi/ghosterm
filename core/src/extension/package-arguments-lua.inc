#include "extension/lua-json.h"
#include "extension/lua-vm.h"
#include <lauxlib.h>
#include <lualib.h>

static const unsigned char decoder_source[] = {
#include "gt-lua-cli-decoder.inc"
};

typedef struct {
    gt_lua_vm_t *vm;
    gt_bridge_wire_t *workspace;
    const uint8_t *schema;
    size_t schema_size;
    const char *const *argv;
    size_t argc;
} decoder_input_t;

static int prepare_decoder(lua_State *state) {
    const decoder_input_t *input = lua_touserdata(state, 1);
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaopen_string(state);
    lua_setglobal(state, LUA_STRLIBNAME);
    if (gt__lua_json_decode(state, input->workspace, input->schema, input->schema_size) != GT_OK ||
        !lua_checkstack(input->vm->thread, 2)) {
        return luaL_error(state, "invalid command schema");
    }
    lua_createtable(state, (int)input->argc, 0);
    for (size_t i = 0; i < input->argc; ++i) {
        lua_pushstring(state, input->argv[i]);
        lua_rawseti(state, -2, (lua_Integer)i + 1);
    }
    lua_xmove(state, input->vm->thread, 2);
    return 0;
}

static gt_status_t decode_checked(const gt_package_checked_t *checked,
                                  const char *path,
                                  const char *const *argv,
                                  size_t argc,
                                  char *output,
                                  size_t capacity,
                                  size_t *size) {
    if (argc > 32 || (argc && !argv)) {
        return GT_ERR_INVALID_ARG;
    }
    size_t total = 0;
    for (size_t i = 0; i < argc; ++i) {
        if (!argv[i]) {
            return GT_ERR_INVALID_ARG;
        }
        const size_t length = strnlen(argv[i], 4097);
        if (length > 4096 || length + 1 > 16384 - total ||
            !gt__runtime_source_utf8(argv[i], length)) {
            return GT_ERR_INVALID_ARG;
        }
        total += length + 1;
    }
    gt_package_command_t command;
    bool found = false;
    for (size_t i = 0; i < gt__package_checked_command_count(checked); ++i) {
        if (gt__package_checked_command(checked, i, &command) && !strcmp(command.path, path)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return GT_ERR_NOT_FOUND;
    }
    if (!command.args_schema[0]) {
        return argc ? GT_ERR_INVALID_ARG : GT_OK;
    }
    gt_package_member_t member;
    decoder_input_t input = {.argv = argv, .argc = argc};
    if (!gt__package_checked_member(
            checked, command.args_schema, &member, &input.schema, &input.schema_size) ||
        !input.schema_size || input.schema_size > GT_BRIDGE_FRAME_MAX) {
        return GT_ERR_PROTOCOL;
    }
    const gt_runtime_module_t decoder = {
        .name = "command-argument-decoder",
        .source = decoder_source,
        .size = sizeof(decoder_source),
    };
    gt_lua_vm_t vm;
    if (gt__lua_vm_open(&vm, &decoder, 4 * 1024 * 1024, 25, NULL) != GT_RUNTIME_NONE) {
        return GT_ERR_INTERNAL;
    }
    input.vm = &vm;
    gt_status_t status = gt__bridge_wire_new(NULL, &input.workspace);
    if (status != GT_OK) {
        gt__lua_vm_close(&vm);
        return status;
    }
    lua_pushcfunction(vm.state, prepare_decoder);
    lua_pushlightuserdata(vm.state, &input);
    int count;
    bool yielded;
    status = GT_ERR_INVALID_ARG;
    if (lua_pcall(vm.state, 1, 0, 0) == LUA_OK &&
        gt__lua_vm_resume(&vm, 2, &count, &yielded) == GT_RUNTIME_NONE && !yielded && count == 1) {
        if (lua_isnil(vm.thread, -1)) {
            status = GT_OK;
        } else {
            char json[GT_PLUGIN_ARGUMENT_JSON_MAX + 1];
            size_t length;
            if (gt__lua_json_encode(
                    vm.thread, -1, GT_EXT_JSON_OBJECT, json, sizeof(json), &length) &&
                (!output || capacity >= length)) {
                if (output) {
                    memcpy(output, json, length);
                    if (capacity > length) {
                        output[length] = '\0';
                    }
                }
                *size = length;
                status = GT_OK;
            }
        }
    }
    gt__bridge_wire_free(input.workspace);
    gt__lua_vm_close(&vm);
    return status;
}
