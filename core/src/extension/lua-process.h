#ifndef GT_EXTENSION_LUA_PROCESS_H
#define GT_EXTENSION_LUA_PROCESS_H

#include "extension/process-jobs.h"
#include <lua.h>

/* Adds the bounded process methods to the table at the top of the stack.
 * The jobs owner outlives all closures and is stepped by the Session worker. */
void gt__lua_process_set_methods(lua_State *, gt_process_jobs_t *);

#endif
