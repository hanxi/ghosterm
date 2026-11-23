#ifndef GT_EXTENSION_LUA_VM_H
#define GT_EXTENSION_LUA_VM_H

#include "extension/runtime.h"
#include <lua.h>
#include <stdatomic.h>

typedef struct {
    lua_State *state;
    lua_State *thread;
    size_t bytes, limit;
    uint64_t deadline_ns;
    uint32_t cpu_ms;
    const atomic_bool *stop;
    const char *name;
    const void *source;
    size_t source_size;
    gt_runtime_error_t interrupted;
} gt_lua_vm_t;

/* The caller keeps the VM at a stable address on the original owner thread.
 * Source is borrowed only during open. The stop flag outlives the VM. */
gt_runtime_error_t gt__lua_vm_open(gt_lua_vm_t *,
                                   const gt_runtime_module_t *,
                                   size_t memory_bytes,
                                   uint32_t cpu_ms,
                                   const atomic_bool *stop);
gt_runtime_error_t gt__lua_vm_resume(gt_lua_vm_t *, int nargs, int *results, bool *yielded);
void gt__lua_vm_close(gt_lua_vm_t *);

#endif
