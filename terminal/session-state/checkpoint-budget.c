#include "checkpoint-budget.h"
#include <string.h>
void p0_checkpoint_release(p0_checkpoint *c) {
    if(!c)return;
    ghostty_free(c->allocator,c->canonical,c->canonical_len);
    ghostty_free(c->allocator,c->resources,c->resources_len);
    memset(c,0,sizeof *c);
}
static bool source_ready(p0_budget_scope *s,GhosttyTerminal t) {
    if(p0_budget_result(s,GHOSTTY_SUCCESS)!=GHOSTTY_SUCCESS)return false;
    bool failed=true;
    return ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed)==GHOSTTY_SUCCESS && !failed;
}
static p0_checkpoint_status scratch_error(p0_budget_scope *s,p0_checkpoint_stage stage,int rc) {
    GhosttyResult budget=p0_budget_result(s,GHOSTTY_SUCCESS);
    if(budget==GHOSTTY_LIMIT_EXCEEDED)return P0_CHECKPOINT_LIMIT;
    if(budget==GHOSTTY_OUT_OF_MEMORY)return P0_CHECKPOINT_NO_MEMORY;
    if(budget!=GHOSTTY_SUCCESS)return P0_CHECKPOINT_INVALID;
    if(stage==P0_CHECKPOINT_RESOURCES) {
        if(rc==2)return P0_CHECKPOINT_NO_MEMORY;
        if(rc==3)return P0_CHECKPOINT_UNSUPPORTED;
        if(rc==4)return P0_CHECKPOINT_LIMIT;
    } else {
        if(rc==GHOSTTY_OUT_OF_MEMORY)return P0_CHECKPOINT_NO_MEMORY;
        if(rc==GHOSTTY_LIMIT_EXCEEDED)return P0_CHECKPOINT_LIMIT;
        if(rc==GHOSTTY_INVALID_VALUE)return P0_CHECKPOINT_UNSUPPORTED;
    }
    return P0_CHECKPOINT_ENGINE_ERROR;
}
p0_checkpoint_status p0_checkpoint_capture(p0_budget_scope *source,p0_budget_scope *scratch,
    GhosttyTerminal t,size_t max_wire_bytes,p0_checkpoint *out,p0_checkpoint_error *error) {
    if(out)memset(out,0,sizeof *out);
    if(error)*error=(p0_checkpoint_error){.stage=P0_CHECKPOINT_PREPARE,.engine_code=0};
    if(!out || !source || !scratch || source==scratch || source->budget!=scratch->budget || !source->reserved || !scratch->reserved || !t)return P0_CHECKPOINT_INVALID;
    if(!source_ready(source,t))return P0_CHECKPOINT_SOURCE_FAILED;
    if(p0_budget_result(scratch,GHOSTTY_SUCCESS)!=GHOSTTY_SUCCESS)return scratch_error(scratch,P0_CHECKPOINT_PREPARE,0);
    p0_checkpoint staged={.allocator=p0_budget_allocator(scratch,P0_BUDGET_SCRATCH)};
    p0_checkpoint_stage stage=P0_CHECKPOINT_CANONICAL;
    int rc=ghostty_snapshot_p0_encode_alloc(t,staged.allocator,&staged.canonical,&staged.canonical_len);
    p0_checkpoint_status status=P0_CHECKPOINT_OK;
    if(rc!=GHOSTTY_SUCCESS) {status=scratch_error(scratch,stage,rc);goto rollback;}
    if(staged.canonical_len>max_wire_bytes) {status=P0_CHECKPOINT_LIMIT;goto rollback;}
    stage=P0_CHECKPOINT_RESOURCES;
    rc=ghostty_terminal_p0_resources_encode_alloc(t,staged.allocator,max_wire_bytes-staged.canonical_len,&staged.resources,&staged.resources_len);
    if(rc!=0) {status=scratch_error(scratch,stage,rc);goto rollback;}
    if(staged.resources_len>max_wire_bytes-staged.canonical_len) {status=P0_CHECKPOINT_LIMIT;goto rollback;}
    if(!source_ready(source,t)) {status=P0_CHECKPOINT_SOURCE_FAILED;goto rollback;}
    if(p0_budget_result(scratch,GHOSTTY_SUCCESS)!=GHOSTTY_SUCCESS) {status=scratch_error(scratch,stage,rc);goto rollback;}
    *out=staged;
    if(error)*error=(p0_checkpoint_error){.stage=P0_CHECKPOINT_COMPLETE,.engine_code=0};
    return P0_CHECKPOINT_OK;
rollback:
    if(error)*error=(p0_checkpoint_error){.stage=stage,.engine_code=rc};
    p0_checkpoint_release(&staged);
    if(!source_ready(source,t))return P0_CHECKPOINT_SOURCE_FAILED;
    if(p0_budget_result(scratch,GHOSTTY_SUCCESS)!=GHOSTTY_SUCCESS)return scratch_error(scratch,stage,rc);
    return status;
}
