/* Lua implementation of the native runtime on the Session's serialized owner. */
#include "base/sha256.h"
#include "base/secure-random.h"
#include "extension/lua-rpc.h"
#include "extension/lua-process.h"
#include "extension/memory.h"
#include "extension/package-runtime.h"
#include "extension/runtime-views.h"
#include "extension/session-owner.h"
#include <inttypes.h>
#include <lauxlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct gt_bridge_runtime {
    gt_ext_session_t *session;
    gt_ext_role_t role;
    gt_package_checked_t *package;
    runtime_views_t *views;
    gt_lua_vm_t vm;
    gt_lua_rpc_t rpc, async_rpc;
    gt_lua_resource_t resource;
    gt_process_jobs_t *processes;
    gt_runtime_info_t info;
    atomic_bool stopped;
    uint32_t activation_ms;
    uint64_t deadline;
    int handlers, context, step_callback;
    char command[520];
    char arguments[16384];
    char decoded_arguments[GT_PLUGIN_ARGUMENT_JSON_MAX + 1];
    char workspace_id[65], pane_id[65], directory[4097];
    size_t offsets[32], argc;
    bool invoking;
};

#include "extension/runtime-views.inc"

static bool view_text(lua_State *state, int index, char *output, size_t capacity, bool method) {
    size_t size;
    const char *text =
        lua_type(state, index) == LUA_TSTRING ? lua_tolstring(state, index, &size) : NULL;
    if (!text || !size || size >= capacity) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const char byte = text[i];
        const bool lower = byte >= 'a' && byte <= 'z';
        const bool alnum = (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
        if (!lower && !((!method || i) && alnum) &&
            !(i && (byte == '.' || byte == '_' || byte == '-'))) {
            return false;
        }
    }
    memcpy(output, text, size);
    output[size] = 0;
    return true;
}

static int open_view(lua_State *state) {
    gt_bridge_runtime_t *r = lua_touserdata(state, lua_upvalueindex(1));
    runtime_view_t candidate = {0};
    if (!r->views || !view_text(state, 1, candidate.kind, sizeof(candidate.kind), false) ||
        !view_text(state, 2, candidate.id, sizeof(candidate.id), false) ||
        !view_text(state, 3, candidate.describe, sizeof(candidate.describe), true) ||
        !view_text(state, 4, candidate.read, sizeof(candidate.read), true) ||
        strcmp(runtime_view_access(r, candidate.kind), "ok")) {
        lua_pushnil(state);
        return 1;
    }
    const bool task = !strcmp(candidate.kind, "task");
    if ((task ? !view_text(state, 5, candidate.cancel, sizeof(candidate.cancel), true)
              : !lua_isnoneornil(state, 5)) ||
        !runtime_view_method(r, candidate.describe) || !runtime_view_method(r, candidate.read) ||
        !strcmp(candidate.describe, candidate.read) ||
        (task && (!runtime_view_method(r, candidate.cancel) ||
                  !strcmp(candidate.describe, candidate.cancel) ||
                  !strcmp(candidate.read, candidate.cancel)))) {
        lua_pushnil(state);
        return 1;
    }
    runtime_view_t *slot = NULL;
    for (size_t i = 0; i < 8; ++i) {
        runtime_view_t *entry = &r->views->entries[i];
        if (entry->handle[0] && !strcmp(entry->id, candidate.id)) {
            lua_pushnil(state);
            return 1;
        }
        if (!entry->handle[0] && !slot) {
            slot = entry;
        }
    }
    if (!slot || r->views->next_id == UINT64_MAX || r->views->revision == UINT64_MAX) {
        lua_pushnil(state);
        return 1;
    }
    (void)snprintf(candidate.handle,
                   sizeof(candidate.handle),
                   "%s.%" PRIu64,
                   r->views->nonce,
                   r->views->next_id);
    lua_pushstring(state, candidate.handle);
    *slot = candidate;
    r->views->next_id++;
    r->views->revision++;
    return 1;
}

static int view_is_open(lua_State *state) {
    gt_bridge_runtime_t *r = lua_touserdata(state, lua_upvalueindex(1));
    char handle[65];
    runtime_view_t *entry =
        view_text(state, 1, handle, sizeof(handle), false) ? runtime_view_find(r, handle) : NULL;
    lua_pushboolean(state, entry && !strcmp(runtime_view_access(r, entry->kind), "ok"));
    return 1;
}

static bool runtime_now(void *opaque, uint64_t *out) {
    (void)opaque;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        return false;
    }
    *out = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    return true;
}

static size_t runtime_json_limit(lua_State *state) {
    if (lua_isnoneornil(state, 2)) {
        return GT_EXT_RPC_BODY_MAX;
    }
    if (!lua_isinteger(state, 2)) {
        return 0;
    }
    const lua_Integer limit = lua_tointeger(state, 2);
    return limit > 0 && limit <= GT_LUA_JSON_MAX ? (size_t)limit : 0;
}

static int runtime_decode_json(lua_State *state) {
    gt_bridge_runtime_t *runtime = lua_touserdata(state, lua_upvalueindex(1));
    size_t size = 0;
    const char *text = lua_type(state, 1) == LUA_TSTRING ? lua_tolstring(state, 1, &size) : NULL;
    if (text && size <= runtime_json_limit(state)) {
        gt_status_t status;
        if (size > GT_BRIDGE_FRAME_MAX) {
            status = gt__lua_json_decode_large(state, text, size);
        } else {
            status = gt__lua_json_decode(state, runtime->rpc.workspace, text, size);
            (void)gt__bridge_json_parse(runtime->rpc.workspace, NULL, 0);
        }
        if (status == GT_OK) {
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

static int runtime_encode_json(lua_State *state) {
    const size_t limit = runtime_json_limit(state);
    if (!limit) {
        lua_pushnil(state);
        return 1;
    }
    char small[GT_EXT_RPC_BODY_MAX + 1];
    char *text = limit <= GT_EXT_RPC_BODY_MAX ? small : lua_newuserdatauv(state, limit + 1, 0);
    size_t size;
    if (!gt__lua_json_encode(state, 1, GT_EXT_JSON_ANY, text, limit + 1, &size)) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushlstring(state, text, size);
    return 1;
}

static int runtime_wait_tick(lua_State *state) {
    gt_bridge_runtime_t *runtime = lua_touserdata(state, lua_upvalueindex(1));
    if (!lua_isyieldable(state)) {
        return luaL_error(state, "wait requires a plugin coroutine");
    }
    runtime->rpc.wait_tick = true;
    return lua_yield(state, 0);
}

static int runtime_on_step(lua_State *state) {
    gt_bridge_runtime_t *runtime = lua_touserdata(state, lua_upvalueindex(1));
    if (runtime->info.phase != GT_RUNTIME_STARTING || runtime->step_callback ||
        lua_type(state, 1) != LUA_TFUNCTION || !lua_checkstack(runtime->vm.state, 1)) {
        return luaL_error(state, "onStep requires one startup callback");
    }
    lua_pushvalue(state, 1);
    lua_xmove(state, runtime->vm.state, 1);
    runtime->step_callback = lua_gettop(runtime->vm.state);
    return 0;
}

static int runtime_context(lua_State *state) {
    gt_bridge_runtime_t *runtime = lua_touserdata(state, 1);
    lua_createtable(state, 0, 7);
    lua_pushlightuserdata(state, runtime);
    lua_pushcclosure(state, runtime_on_step, 1);
    lua_setfield(state, -2, "onStep");
    lua_pushlightuserdata(state, runtime);
    lua_pushcclosure(state, runtime_wait_tick, 1);
    lua_setfield(state, -2, "wait");
    lua_pushstring(state, runtime->rpc.lease.instance);
    lua_setfield(state, -2, "instanceId");
    char epoch[21];
    (void)snprintf(epoch, sizeof(epoch), "%" PRIu64, runtime->rpc.lease.epoch);
    lua_pushstring(state, epoch);
    lua_setfield(state, -2, "epoch");
    if (runtime->workspace_id[0] || runtime->pane_id[0] || runtime->directory[0]) {
        lua_createtable(state, 0, 3);
        if (runtime->workspace_id[0]) {
            lua_pushstring(state, runtime->workspace_id);
            lua_setfield(state, -2, "workspaceId");
        }
        if (runtime->pane_id[0]) {
            lua_pushstring(state, runtime->pane_id);
            lua_setfield(state, -2, "paneId");
        }
        if (runtime->directory[0]) {
            lua_pushstring(state, runtime->directory);
            lua_setfield(state, -2, "directory");
        }
        lua_setfield(state, -2, "context");
    }
    lua_createtable(state, 0, 3);
    lua_pushlightuserdata(state, runtime);
    lua_pushcclosure(state, runtime_decode_json, 1);
    lua_setfield(state, -2, "decode");
    lua_pushcfunction(state, runtime_encode_json);
    lua_setfield(state, -2, "encode");
    lua_pushcfunction(state, gt__lua_json_array);
    lua_setfield(state, -2, "array");
    lua_pushcfunction(state, gt__lua_json_is_array);
    lua_setfield(state, -2, "isArray");
    gt__lua_json_push_null(state);
    lua_setfield(state, -2, "null");
    lua_setfield(state, -2, "json");
    if (runtime->decoded_arguments[0]) {
        const gt_status_t decoded = gt__lua_json_decode_large(
            state, runtime->decoded_arguments, strlen(runtime->decoded_arguments));
        if (decoded != GT_OK) {
            return luaL_error(state, "invalid decoded command arguments");
        }
        lua_setfield(state, -2, "arguments");
    }
    lua_createtable(state, 0, 4);
    gt__lua_rpc_set_async(state, &runtime->async_rpc);
    gt__lua_rpc_push_call(state, &runtime->rpc);
    lua_setfield(state, -2, "call");
    lua_setfield(state, -2, "rpc");
    if (runtime->processes) {
        lua_createtable(state, 0, 8);
        gt__lua_process_set_methods(state, runtime->processes);
        lua_setfield(state, -2, "process");
    }
    if (runtime->views) {
        const luaL_Reg methods[] = {{"open", open_view}, {"isOpen", view_is_open}, {NULL, NULL}};
        lua_createtable(state, 0, 2);
        lua_pushlightuserdata(state, runtime);
        luaL_setfuncs(state, methods, 1);
        lua_setfield(state, -2, "views");
    }
    if (runtime->resource.owner) {
        lua_createtable(state, 0, 1);
        gt__lua_resource_push_publish(state, &runtime->resource);
        lua_setfield(state, -2, "publish");
        lua_setfield(state, -2, "resources");
    }
    return 1;
}

static bool runtime_copy_context_field(char *destination, size_t capacity, const char *source) {
    if (!source) {
        return true;
    }
    const size_t size = strnlen(source, capacity);
    if (!size || size == capacity || !gt__runtime_source_utf8(source, size)) {
        return false;
    }
    memcpy(destination, source, size + 1);
    return true;
}

static gt_status_t runtime_copy_invocation(gt_bridge_runtime_t *runtime,
                                           const gt_ext_invocation_t *invocation) {
    if (!invocation) {
        return GT_OK;
    }
    if (!invocation->command || invocation->argc > 32 || (invocation->argc && !invocation->argv)) {
        return GT_ERR_INVALID_ARG;
    }
    if (invocation->workspace_id || invocation->pane_id || invocation->directory) {
        if (runtime->role != GT_EXT_HOST ||
            (invocation->directory && invocation->directory[0] != '/') ||
            !runtime_copy_context_field(
                runtime->workspace_id, sizeof(runtime->workspace_id), invocation->workspace_id) ||
            !runtime_copy_context_field(
                runtime->pane_id, sizeof(runtime->pane_id), invocation->pane_id) ||
            !runtime_copy_context_field(
                runtime->directory, sizeof(runtime->directory), invocation->directory)) {
            return GT_ERR_INVALID_ARG;
        }
    }
    const size_t length = strnlen(invocation->command, sizeof(runtime->command));
    if (!length || length == sizeof(runtime->command)) {
        return GT_ERR_INVALID_ARG;
    }
    size_t tokens = 1, token = 0;
    for (size_t i = 0; i < length; ++i) {
        const char byte = invocation->command[i];
        if (byte == ' ') {
            if (!token || ++tokens > 8) {
                return GT_ERR_INVALID_ARG;
            }
            token = 0;
        } else {
            const bool letter = byte >= 'a' && byte <= 'z';
            if ((!token && !letter) || !(letter || (byte >= '0' && byte <= '9') || byte == '-') ||
                ++token > 64) {
                return GT_ERR_INVALID_ARG;
            }
        }
    }
    if (!token) {
        return GT_ERR_INVALID_ARG;
    }
    size_t used = 0;
    for (size_t i = 0; i < invocation->argc; ++i) {
        const char *argument = invocation->argv[i];
        if (!argument) {
            return GT_ERR_INVALID_ARG;
        }
        const size_t size = strnlen(argument, 4097);
        if (size > 4096 || size + 1 > sizeof(runtime->arguments) - used ||
            !gt__runtime_source_utf8(argument, size)) {
            return GT_ERR_INVALID_ARG;
        }
        runtime->offsets[i] = used;
        memcpy(runtime->arguments + used, argument, size + 1);
        used += size + 1;
    }
    runtime->argc = invocation->argc;
    memcpy(runtime->command, invocation->command, length + 1);
    return GT_OK;
}

static int prepare_command(lua_State *state) {
    gt_bridge_runtime_t *runtime = lua_touserdata(state, 1);
    lua_pushstring(state, runtime->command);
    if (lua_rawget(state, 2) != LUA_TFUNCTION || !lua_checkstack(runtime->vm.thread, 3)) {
        return 0;
    }
    lua_pushvalue(state, 3);
    if (runtime->decoded_arguments[0]) {
        const gt_status_t decoded = gt__lua_json_decode_large(
            state, runtime->decoded_arguments, strlen(runtime->decoded_arguments));
        if (decoded != GT_OK) {
            return luaL_error(state, "invalid decoded command arguments");
        }
    } else {
        lua_createtable(state, (int)runtime->argc, 0);
        for (size_t i = 0; i < runtime->argc; ++i) {
            lua_pushstring(state, runtime->arguments + runtime->offsets[i]);
            lua_rawseti(state, -2, (lua_Integer)i + 1);
        }
    }
    lua_xmove(state, runtime->vm.thread, 3);
    lua_pushboolean(state, true);
    return 1;
}

static void runtime_release_vm(gt_bridge_runtime_t *runtime) {
    gt__lua_vm_close(&runtime->vm);
    gt__bridge_wire_free(runtime->rpc.workspace);
    runtime->rpc.workspace = NULL;
}

static gt_runtime_phase_t runtime_end(gt_bridge_runtime_t *runtime, gt_runtime_error_t error) {
    gt_ext_session_revoke(runtime->session);
    gt_ext_session_stop(runtime->session);
    gt__process_jobs_stop(runtime->processes);
    gt__runtime_resources_stop(runtime->resource.owner);
    runtime_release_vm(runtime);
    runtime->info.error = error;
    runtime->info.phase = error == GT_RUNTIME_CANCELLED ? GT_RUNTIME_STOPPED : GT_RUNTIME_FAILED;
    return runtime->info.phase;
}

static gt_status_t runtime_open_processes(gt_bridge_runtime_t *runtime,
                                          const gt_runtime_config_t *config) {
    if (!config->process_package) {
        return GT_OK;
    }
    if (!config->process_program_count || !config->process_programs ||
        config->process_environment_count > GT_EXT_PROCESS_ENVIRONMENT_MAX ||
        (config->process_environment_count && !config->process_environment)) {
        return GT_ERR_UNSUPPORTED;
    }
    char environment[GT_EXT_PROCESS_ENVIRONMENT_BYTES_MAX];
    const char *entries[GT_EXT_PROCESS_ENVIRONMENT_MAX];
    size_t used = 0;
    for (size_t i = 0; i < config->process_environment_count; ++i) {
        const gt_ext_process_environment_t *entry = &config->process_environment[i];
        if (!entry->name || !entry->value) {
            return GT_ERR_INVALID_ARG;
        }
        const size_t name = strnlen(entry->name, 129);
        const size_t value = strnlen(entry->value, 4097);
        if (!name || name > 128 || value > 4096 || memchr(entry->name, '=', name) ||
            name + value + 2 > sizeof(environment) - used) {
            return GT_ERR_INVALID_ARG;
        }
        entries[i] = environment + used;
        memcpy(environment + used, entry->name, name);
        used += name;
        environment[used++] = '=';
        memcpy(environment + used, entry->value, value + 1);
        used += value + 1;
    }
    const gt_process_jobs_config_t jobs = {
        .programs = config->process_programs,
        .program_count = config->process_program_count,
        .environment = entries,
        .environment_count = config->process_environment_count,
        .lifetime_ms = config->process_lifetime_ms,
        .output_bytes = config->process_output_bytes,
    };
    const gt_status_t status = gt__process_jobs_open(runtime->session,
                                                     runtime->resource.owner,
                                                     config->process_package,
                                                     &jobs,
                                                     &runtime->processes);
    gt__secure_erase(environment, used);
    return status;
}

gt_status_t gt__bridge_runtime_open(gt_ext_session_t **session,
                                    const gt_runtime_config_t *config,
                                    gt_bridge_runtime_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!session || !*session || !config || !out || !config->modules || !config->module_count ||
        !config->entry || (config->role != GT_EXT_HOST && config->role != GT_EXT_EXECUTOR) ||
        config->root_count > GT_RUNTIME_ROOTS_MAX || (config->root_count && !config->roots)) {
        return GT_ERR_INVALID_ARG;
    }
    if (config->module_count != 1 || config->stack_bytes || config->jobs_per_step ||
        (!config->process_package &&
         (config->process_program_count || config->process_environment_count ||
          config->process_lifetime_ms || config->process_output_bytes))) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_ext_session_t *owner = *session;
    gt_ext_session_info_t info;
    gt_bridge_role_t role;
    size_t method_count;
    gt__bridge_session_methods(owner->inner, &method_count, &role);
    if (!gt_ext_session_info(owner, &info) || info.phase != GT_EXT_SESSION_ACTIVE ||
        !owner->requests || owner->sources ||
        !gt__bridge_session_runtime_admissible(owner->inner) ||
        (unsigned)role != (unsigned)config->role) {
        return GT_ERR_INVALID_ARG;
    }
    if (config->root_count && !owner->store && !config->process_package) {
        return GT_ERR_UNSUPPORTED;
    }
    const gt_runtime_module_t *module = config->modules;
    const size_t memory = config->memory_bytes ? config->memory_bytes : 16u * 1024u * 1024u;
    const uint32_t cpu = config->cpu_ms ? config->cpu_ms : 25;
    if (!gt__runtime_module_name(module->name) || strncmp(module->name, "plugin:/", 8) ||
        strcmp(module->name, config->entry) || !module->source || !module->size ||
        module->size > 128u * 1024u || !gt__runtime_source_utf8(module->source, module->size) ||
        strnlen(module->sha256, sizeof(module->sha256)) != 64 || memory < 1024u * 1024u ||
        memory > GT_PLUGIN_MEMORY_DEFAULT || cpu > 1000 || config->activation_ms > 60000) {
        return GT_ERR_INVALID_ARG;
    }
    gt_sha256_t hash;
    char digest[65];
    gt__sha256_init(&hash);
    if (!gt__sha256_update(&hash, module->source, module->size) ||
        !gt__sha256_final(&hash, digest) || strcmp(module->sha256, digest)) {
        return GT_ERR_PROTOCOL;
    }
    gt_bridge_runtime_t *runtime = gt__plugin_memory_calloc(1, sizeof(*runtime));
    if (!runtime) {
        return GT_ERR_IO;
    }
    atomic_init(&runtime->stopped, false);
    runtime->session = owner;
    runtime->role = config->role;
    runtime->rpc.session = owner;
    runtime->rpc.resource = &runtime->resource;
    runtime->activation_ms = config->activation_ms ? config->activation_ms : 5000;
    gt_status_t status = runtime_copy_invocation(runtime, config->invocation);
    if (status != GT_OK) {
        goto fail;
    }
    if (config->decoded_arguments) {
        const size_t size = strnlen(config->decoded_arguments, sizeof(runtime->decoded_arguments));
        if (size == sizeof(runtime->decoded_arguments)) {
            status = GT_ERR_INVALID_ARG;
            goto fail;
        }
        memcpy(runtime->decoded_arguments, config->decoded_arguments, size + 1);
    }
    status = GT_ERR_INVALID_ARG;
    if (!gt_ext_session_lease(owner, &runtime->rpc.lease)) {
        goto fail;
    }
    const gt_runtime_error_t opened =
        gt__lua_vm_open(&runtime->vm, module, memory, cpu, &runtime->stopped);
    if (opened != GT_RUNTIME_NONE) {
        status = opened == GT_RUNTIME_MEMORY ? GT_ERR_IO : GT_ERR_PROTOCOL;
        goto fail;
    }
    status = gt__bridge_wire_new(NULL, &runtime->rpc.workspace);
    if (status != GT_OK) {
        goto fail;
    }
    runtime->async_rpc.session = owner;
    runtime->async_rpc.lease = runtime->rpc.lease;
    runtime->async_rpc.workspace = runtime->rpc.workspace;
    if (owner->store || config->process_package) {
        status = gt__runtime_resources_open(owner,
                                            owner->store,
                                            &runtime->rpc.lease,
                                            config->roots,
                                            config->root_count,
                                            NULL,
                                            runtime_now,
                                            &runtime->resource.owner);
        if (status != GT_OK) {
            goto fail;
        }
    }
    status = runtime_open_processes(runtime, config);
    if (status != GT_OK) {
        goto fail;
    }
    runtime->info.module_bytes = module->size;
    *session = NULL;
    *out = runtime;
    return GT_OK;

fail:
    if (runtime->resource.owner) {
        (void)gt__runtime_resources_destroy(&runtime->resource.owner);
    }
    runtime_release_vm(runtime);
    gt__plugin_memory_free(NULL, runtime, sizeof(*runtime));
    return status;
}

gt_status_t gt__bridge_runtime_open_package(gt_ext_session_t **session,
                                            gt_package_checked_t *package,
                                            const gt_runtime_config_t *config,
                                            bool associated,
                                            gt_bridge_runtime_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!session || !*session || !package || !config || !out || config->modules ||
        config->module_count || config->entry) {
        return GT_ERR_INVALID_ARG;
    }
    gt_ext_lease_t native_lease;
    if (!gt_ext_session_lease(*session, &native_lease)) {
        return GT_ERR_AUTH;
    }
    gt_bridge_lease_t lease;
    memcpy(&lease, &native_lease, sizeof(lease));
    char entry[GT_RUNTIME_MODULE_NAME_MAX + 1];
    gt_status_t status = gt__runtime_package_entry(package,
                                                   (*session)->inner,
                                                   (gt_bridge_role_t)config->role,
                                                   &lease,
                                                   associated,
                                                   entry,
                                                   sizeof(entry));
    if (status != GT_OK) {
        return status;
    }
    char arguments[GT_PLUGIN_ARGUMENT_JSON_MAX + 1] = {0};
    if (config->invocation) {
        size_t size;
        status = gt__package_decode_arguments(package,
                                              config->invocation->command,
                                              config->invocation->argv,
                                              config->invocation->argc,
                                              arguments,
                                              sizeof(arguments),
                                              &size);
        if (status != GT_OK) {
            return status;
        }
    }
    gt_runtime_source_t source;
    status = gt__runtime_package_source(package, entry, &source);
    if (status != GT_OK) {
        return status;
    }
    gt_runtime_module_t module = {.name = entry, .source = source.bytes, .size = source.size};
    memcpy(module.sha256, source.sha256, sizeof(module.sha256));
    gt_runtime_config_t selected = *config;
    selected.modules = &module;
    selected.module_count = 1;
    selected.entry = entry;
    if (selected.role == GT_EXT_EXECUTOR) {
        /* The command is validated above. Its handler on the Executor is
         * selected by the authorized RPC method, after the Client invokes it. */
        selected.invocation = NULL;
    }
    selected.decoded_arguments = arguments;
    gt_package_check_info_t metadata;
    if (!gt__package_checked_info(package, &metadata)) {
        return GT_ERR_PROTOCOL;
    }
    selected.process_package = metadata.process_program_count ? package : NULL;
    if (selected.process_package && (!selected.root_count || !selected.process_program_count)) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_package_checked_t *retained = NULL;
    if (!gt__package_checked_retain(package, &retained)) {
        return GT_ERR_IO;
    }
    status = gt__bridge_runtime_open(session, &selected, out);
    if (status == GT_OK) {
        (*out)->package = retained;
    } else {
        (void)gt__package_checked_destroy(&retained);
    }
    return status;
}

static bool command_exported(const gt_bridge_runtime_t *runtime, const char *name) {
    if (runtime->role != GT_EXT_HOST) {
        return false;
    }
    if (!strcmp(runtime->command, name)) {
        return true;
    }
    const size_t count = gt__package_checked_command_count(runtime->package);
    for (size_t i = 0; i < count; ++i) {
        gt_package_command_t command;
        if (gt__package_checked_command(runtime->package, i, &command) &&
            !strcmp(command.path, name)) {
            return true;
        }
    }
    return false;
}

static bool register_exports(gt_bridge_runtime_t *runtime) {
    lua_State *state = runtime->vm.state;
    const int top = lua_gettop(state);
    size_t method_count;
    gt_bridge_role_t role;
    const gt_bridge_rpc_method_t *methods =
        gt__bridge_session_methods(runtime->session->inner, &method_count, &role);
    gt_package_check_info_t metadata = {0};
    if (runtime->package && !gt__package_checked_info(runtime->package, &metadata)) {
        return false;
    }
    size_t exports_found = 0;
    if (!lua_checkstack(state, 2)) {
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, runtime->handlers)) {
        size_t size;
        const char *name =
            lua_type(state, -2) == LUA_TSTRING ? lua_tolstring(state, -2, &size) : NULL;
        if (!name || !size || size > 519 || memchr(name, 0, size) ||
            lua_type(state, -1) != LUA_TFUNCTION) {
            lua_settop(state, top);
            return false;
        }
        const char *method = NULL;
        for (size_t i = 0; i < method_count; ++i) {
            if (methods[i].receiver == role && !strcmp(methods[i].method, name)) {
                method = methods[i].method;
                break;
            }
        }
        bool declared = false;
        for (size_t i = 0; i < metadata.rpc_handler_count; ++i) {
            if (!strcmp(metadata.rpc_handlers[i], name)) {
                declared = true;
                exports_found++;
                break;
            }
        }
        if (!runtime->package) {
            declared = method != NULL;
        }
        if (!declared && !command_exported(runtime, name)) {
            lua_settop(state, top);
            return false;
        }
        if (declared && method) {
            runtime->rpc.handlers[runtime->rpc.handler_count++] = method;
        }
        lua_pop(state, 1);
    }
    return exports_found == metadata.rpc_handler_count;
}

static gt_runtime_phase_t activate(gt_bridge_runtime_t *runtime) {
    gt_lua_vm_t *vm = &runtime->vm;
    uint64_t now;
    if (!runtime_now(NULL, &now)) {
        return runtime_end(runtime, GT_RUNTIME_IO);
    }
    int nargs = 0;
    if (runtime->info.phase == GT_RUNTIME_CREATED) {
        runtime->info.phase = GT_RUNTIME_STARTING;
        runtime->deadline = now + runtime->activation_ms;
        lua_pushcfunction(vm->state, runtime_context);
        lua_pushlightuserdata(vm->state, runtime);
        if (lua_pcall(vm->state, 1, 1, 0) != LUA_OK || !lua_checkstack(vm->thread, 1)) {
            return runtime_end(runtime, GT_RUNTIME_MEMORY);
        }
        runtime->context = lua_gettop(vm->state);
        if (!lua_checkstack(vm->state, 1)) {
            return runtime_end(runtime, GT_RUNTIME_MEMORY);
        }
        lua_pushvalue(vm->state, runtime->context);
        lua_xmove(vm->state, vm->thread, 1);
        nargs = 1;
    } else {
        if (now >= runtime->deadline) {
            return runtime_end(runtime, GT_RUNTIME_EXPIRED);
        }
        if (!runtime->rpc.wait_tick && !gt__lua_rpc_poll(&runtime->rpc) &&
            !gt__lua_resource_poll(&runtime->resource)) {
            return GT_RUNTIME_STARTING;
        }
    }
    int results;
    bool yielded;
resume:
    runtime->rpc.wait_tick = false;
    const gt_runtime_error_t error = gt__lua_vm_resume(vm, nargs, &results, &yielded);
    if (error != GT_RUNTIME_NONE) {
        return runtime_end(runtime, error);
    }
    if (yielded) {
        return runtime->rpc.wait_tick || runtime->rpc.pending || runtime->resource.publication
                   ? GT_RUNTIME_STARTING
                   : runtime_end(runtime, GT_RUNTIME_PROTOCOL);
    }
    if (runtime->invoking) {
        if (results && lua_isnil(vm->thread, -results)) {
            return runtime_end(runtime, GT_RUNTIME_SCRIPT);
        }
    } else {
        if (results != 1 || !lua_istable(vm->thread, -1) || !lua_checkstack(vm->state, 5)) {
            return runtime_end(runtime, GT_RUNTIME_SCRIPT);
        }
        lua_xmove(vm->thread, vm->state, 1);
        runtime->handlers = lua_gettop(vm->state);
        if (!register_exports(runtime)) {
            return runtime_end(runtime, GT_RUNTIME_PROTOCOL);
        }
        if (runtime->command[0]) {
            if (lua_resetthread(vm->thread) != LUA_OK) {
                return runtime_end(runtime, GT_RUNTIME_SCRIPT);
            }
            lua_pushcfunction(vm->state, prepare_command);
            lua_pushlightuserdata(vm->state, runtime);
            lua_pushvalue(vm->state, runtime->handlers);
            lua_pushvalue(vm->state, runtime->context);
            if (lua_pcall(vm->state, 3, 1, 0) != LUA_OK || !lua_toboolean(vm->state, -1)) {
                return runtime_end(runtime, GT_RUNTIME_SCRIPT);
            }
            lua_pop(vm->state, 1);
            runtime->invoking = true;
            nargs = 2;
            goto resume;
        }
    }
    runtime->info.phase = GT_RUNTIME_ACTIVE;
    return GT_RUNTIME_ACTIVE;
}

static gt_runtime_error_t run_step_callback(gt_bridge_runtime_t *runtime) {
    gt_lua_vm_t *vm = &runtime->vm;
    if (lua_resetthread(vm->thread) != LUA_OK || !lua_checkstack(vm->state, 1) ||
        !lua_checkstack(vm->thread, 1)) {
        return GT_RUNTIME_MEMORY;
    }
    lua_pushvalue(vm->state, runtime->step_callback);
    lua_xmove(vm->state, vm->thread, 1);
    int results;
    bool yielded;
    const gt_runtime_error_t error = gt__lua_vm_resume(vm, 0, &results, &yielded);
    if (error == GT_RUNTIME_NONE && (yielded || results)) {
        return GT_RUNTIME_PROTOCOL;
    }
    return error;
}

gt_runtime_phase_t gt__bridge_runtime_step(gt_bridge_runtime_t *runtime) {
    if (!runtime) {
        return GT_RUNTIME_CLOSED;
    }
    if (runtime->info.phase >= GT_RUNTIME_FAILED) {
        return runtime->info.phase;
    }
    if (atomic_load(&runtime->stopped)) {
        return runtime_end(runtime, GT_RUNTIME_CANCELLED);
    }
    runtime->info.steps++;
    if (gt_ext_session_step(runtime->session, 0) != GT_EXT_SESSION_ACTIVE) {
        gt_ext_session_info_t info;
        if (gt_ext_session_info(runtime->session, &info)) {
            runtime->info.session_end = info.end;
            runtime->info.session_error = info.error;
        }
        return runtime_end(runtime, GT_RUNTIME_PROTOCOL);
    }
    if (gt__process_jobs_step(runtime->processes) != GT_OK) {
        return runtime_end(runtime, GT_RUNTIME_IO);
    }
    gt__runtime_resources_step(runtime->resource.owner);
    if (runtime->info.phase != GT_RUNTIME_ACTIVE) {
        return activate(runtime);
    }
    gt_runtime_error_t error;
    const gt_ext_rpc_status_t status =
        gt__lua_rpc_dispatch(&runtime->rpc, &runtime->vm, runtime->handlers, &error);
    if (error != GT_RUNTIME_NONE) {
        return runtime_end(runtime, error);
    }
    if (status != GT_EXT_RPC_OK && status != GT_EXT_RPC_WAIT) {
        return runtime_end(runtime, GT_RUNTIME_PROTOCOL);
    }
    if (runtime->step_callback && !runtime->rpc.inbound_ticket) {
        const gt_runtime_error_t stepped = run_step_callback(runtime);
        if (stepped != GT_RUNTIME_NONE) {
            return runtime_end(runtime, stepped);
        }
    }
    return GT_RUNTIME_ACTIVE;
}

bool gt__bridge_runtime_info(const gt_bridge_runtime_t *runtime, gt_runtime_info_t *out) {
    if (!out) {
        return false;
    }
    *out = runtime ? runtime->info : (gt_runtime_info_t){0};
    if (runtime) {
        out->engine_released = runtime->vm.state == NULL;
    }
    return runtime != NULL;
}

void gt__bridge_runtime_stop(gt_bridge_runtime_t *runtime) {
    if (runtime) {
        atomic_store(&runtime->stopped, true);
    }
}

gt_ext_close_t gt__bridge_runtime_close(gt_bridge_runtime_t *runtime) {
    if (!runtime) {
        return GT_EXT_CLOSE_ERROR;
    }
    if (runtime->info.phase == GT_RUNTIME_CLOSED) {
        return GT_EXT_RELEASED;
    }
    if (runtime->info.phase < GT_RUNTIME_FAILED) {
        (void)runtime_end(runtime, GT_RUNTIME_CANCELLED);
    }
    (void)gt_ext_session_step(runtime->session, 0);
    if (runtime->processes) {
        const gt_status_t stepped = gt__process_jobs_step(runtime->processes);
        const gt_status_t destroyed = gt__process_jobs_destroy(&runtime->processes);
        if (stepped != GT_OK && destroyed != GT_PROCESS_JOBS_WAIT) {
            runtime->info.cleanup_error = stepped;
            return GT_EXT_CLOSE_ERROR;
        }
        if (destroyed == GT_PROCESS_JOBS_WAIT) {
            return GT_EXT_DRAINING;
        }
        if (destroyed != GT_OK) {
            runtime->info.cleanup_error = destroyed;
            return GT_EXT_CLOSE_ERROR;
        }
    }
    if (runtime->resource.owner) {
        const gt_runtime_resource_status_t status =
            gt__runtime_resources_destroy(&runtime->resource.owner);
        if (status == GT_RUNTIME_RESOURCE_WAIT) {
            return GT_EXT_DRAINING;
        }
        if (status != GT_RUNTIME_RESOURCE_OK) {
            runtime->info.cleanup_error = GT_ERR_IO;
            return GT_EXT_CLOSE_ERROR;
        }
    }
    const gt_ext_close_t closed = gt_ext_session_close(runtime->session, 0);
    if (closed == GT_EXT_RELEASED) {
        runtime->info.phase = GT_RUNTIME_CLOSED;
    }
    return closed;
}

gt_status_t gt__bridge_runtime_destroy(gt_bridge_runtime_t **runtime) {
    if (!runtime || !*runtime || (*runtime)->info.phase != GT_RUNTIME_CLOSED) {
        return GT_ERR_INVALID_ARG;
    }
    const gt_status_t status = gt_ext_session_destroy(&(*runtime)->session);
    if (status == GT_OK) {
        if ((*runtime)->package) {
            (void)gt__package_checked_destroy(&(*runtime)->package);
        }
        gt__plugin_memory_free(NULL, (*runtime)->views, sizeof(runtime_views_t));
        gt__plugin_memory_free(NULL, *runtime, sizeof(**runtime));
        *runtime = NULL;
    }
    return status;
}

gt_status_t gt__bridge_runtime_transport_process_info(const gt_bridge_runtime_t *runtime,
                                                      gt_ext_transport_process_info_t *out) {
    return gt_ext_session_process_info(runtime ? runtime->session : NULL, out);
}
