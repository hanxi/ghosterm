#include "budget-posix.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t sat_add(size_t a, size_t b) { return b > SIZE_MAX-a ? SIZE_MAX : a+b; }
static void lock(p0_budget *b) { int rc=pthread_mutex_lock(&b->mutex); assert(rc==0); (void)rc; }
static void unlock(p0_budget *b) { int rc=pthread_mutex_unlock(&b->mutex); assert(rc==0); (void)rc; }
static void fault_locked(p0_budget_scope *s, p0_budget_fault why) {
    if(s->fault==P0_BUDGET_HEALTHY) s->fault=why;
    s->usage.denied=sat_add(s->usage.denied,1);
    s->budget->usage.denied=sat_add(s->budget->usage.denied,1);
}
/* Global committed = admitted grants + live bytes of on-demand scopes. */
static void commit_locked(p0_budget_scope *s,size_t value) {
    p0_budget_usage *owner=&s->usage,*global=&s->budget->usage;
    if(value>=owner->committed)global->committed+=value-owner->committed;
    else {assert(global->committed>=owner->committed-value);global->committed-=owner->committed-value;}
    owner->committed=value;
    if(value>owner->peak_committed)owner->peak_committed=value;
    if(global->committed>global->peak_committed)global->peak_committed=global->committed;
}
static void release_locked(p0_budget_endpoint *e,size_t n) {
    p0_budget_usage *a=&e->scope->usage, *b=&e->scope->budget->usage;
    assert(a->live>=n && b->live>=n && a->by_kind[e->kind]>=n && b->by_kind[e->kind]>=n);
    if(!e->scope->reserved)commit_locked(e->scope,a->committed-n);
    a->live-=n;b->live-=n;a->by_kind[e->kind]-=n;b->by_kind[e->kind]-=n;
}
static bool round_size(size_t n,size_t page,size_t *out) {
    if(n==0 || n>SIZE_MAX-(page-1)) return false;
    *out=(n+page-1)/page*page;return true;
}
static bool reserve_locked(p0_budget_endpoint *e,size_t n) {
    p0_budget_scope *s=e->scope;p0_budget *b=s->budget;
    s->usage.attempts=sat_add(s->usage.attempts,1);b->usage.attempts=sat_add(b->usage.attempts,1);
    if(s->closed || s->fault!=P0_BUDGET_HEALTHY) { fault_locked(s,s->fault==P0_BUDGET_HEALTHY?P0_BUDGET_SCOPE_LIMIT:s->fault);return false; }
    if(!s->reserved && n>b->limit-b->usage.committed) { fault_locked(s,P0_BUDGET_GLOBAL_LIMIT);return false; }
    if(n>s->limit-s->usage.live) { fault_locked(s,P0_BUDGET_SCOPE_LIMIT);return false; }
    if(!s->reserved)commit_locked(s,s->usage.committed+n);
    s->usage.live+=n;b->usage.live+=n;s->usage.by_kind[e->kind]+=n;b->usage.by_kind[e->kind]+=n;
    if(s->usage.live>s->usage.peak)s->usage.peak=s->usage.live;
    if(b->usage.live>b->usage.peak)b->usage.peak=b->usage.live;
    return true;
}
static void *allocate(void *ctx,size_t n,uint8_t alignment,uintptr_t ret) {
    (void)ret;p0_budget_endpoint *e=ctx;p0_budget_scope *s=e->scope;p0_budget *b=s->budget;
    size_t needed=0,align=0,extra=0,total=0;
    lock(b);
    if(alignment>=sizeof(size_t)*CHAR_BIT || !round_size(n,b->page_size,&needed)) {
        fault_locked(s,P0_BUDGET_OVERFLOW);unlock(b);return NULL;
    }
    align=(size_t)1<<alignment;extra=align>b->page_size?align-b->page_size:0;
    if(needed>SIZE_MAX-extra) { fault_locked(s,P0_BUDGET_OVERFLOW);unlock(b);return NULL; }
    total=needed+extra;
    bool admitted=reserve_locked(e,total);unlock(b);if(!admitted)return NULL;
    void *raw=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(raw==MAP_FAILED) { lock(b);release_locked(e,total);fault_locked(s,P0_BUDGET_OS_ERROR);unlock(b);return NULL; }
    size_t prefix=(align-((uintptr_t)raw&(align-1)))&(align-1),suffix=total-needed-prefix;
    uint8_t *result=(uint8_t*)raw+prefix;
    /* Every trim is an independently page-aligned mapping range. */
    bool ok=(!prefix || munmap(raw,prefix)==0) && (!suffix || munmap(result+needed,suffix)==0);
    lock(b);
    s->usage.mapped=sat_add(s->usage.mapped,total);b->usage.mapped=sat_add(b->usage.mapped,total);
    if(ok) {
        release_locked(e,total-needed);
        s->usage.unmapped=sat_add(s->usage.unmapped,total-needed);b->usage.unmapped=sat_add(b->usage.unmapped,total-needed);
    } else {
        /* Do not publish an allocation after a failed trim. Keep an unreleased
         * reservation if cleanup itself fails; accounting must not pretend free. */
        if(munmap(raw,total)==0) {
            release_locked(e,total);s->usage.unmapped=sat_add(s->usage.unmapped,total);b->usage.unmapped=sat_add(b->usage.unmapped,total);
        }
        fault_locked(s,P0_BUDGET_OS_ERROR);
    }
    unlock(b);return ok?result:NULL;
}
static bool resize_memory(void *ctx,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {
    (void)ctx;(void)p;(void)n;(void)a;(void)m;(void)r;return false;
}
static void *remap_memory(void *ctx,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {
    (void)ctx;(void)p;(void)n;(void)a;(void)m;(void)r;return NULL;
}
static void free_memory(void *ctx,void *p,size_t n,uint8_t alignment,uintptr_t ret) {
    (void)alignment;(void)ret;if(!p)return;
    p0_budget_endpoint *e=ctx;p0_budget *b=e->scope->budget;size_t mapped=0;
    bool valid=round_size(n,b->page_size,&mapped);assert(valid);if(!valid)return;
    int rc=munmap(p,mapped);lock(b);
    if(rc==0) {
        release_locked(e,mapped);
        e->scope->usage.unmapped=sat_add(e->scope->usage.unmapped,mapped);b->usage.unmapped=sat_add(b->usage.unmapped,mapped);
    } else fault_locked(e->scope,P0_BUDGET_OS_ERROR);
    unlock(b);
}
static const GhosttyAllocatorVtable table={allocate,resize_memory,remap_memory,free_memory};
bool p0_budget_init(p0_budget *b,size_t limit) {
    long page=sysconf(_SC_PAGESIZE);if(!b || page<=0 || ((size_t)page&((size_t)page-1)))return false;
    memset(b,0,sizeof *b);b->limit=limit;b->page_size=(size_t)page;return pthread_mutex_init(&b->mutex,NULL)==0;
}
bool p0_budget_destroy(p0_budget *b) {
    lock(b);bool empty=b->scopes==0 && b->usage.live==0 && b->usage.committed==0;unlock(b);
    return empty && pthread_mutex_destroy(&b->mutex)==0;
}
void p0_budget_scope_init(p0_budget_scope *s,p0_budget *b,size_t limit) {
    memset(s,0,sizeof *s);s->budget=b;s->limit=limit;
    for(unsigned k=0;k<P0_BUDGET_KINDS;k++) {
        s->endpoints[k]=(p0_budget_endpoint){.scope=s,.kind=(p0_budget_kind)k};
        s->allocators[k]=(GhosttyAllocator){.ctx=&s->endpoints[k],.vtable=&table};
    }
    lock(b);assert(b->scopes<SIZE_MAX);b->scopes++;unlock(b);
}
bool p0_budget_scope_admit(p0_budget_scope *s,p0_budget *b,size_t limit) {
    p0_budget_scope_init(s,b,limit);
    lock(b);
    bool admitted=limit<=b->limit-b->usage.committed;
    if(admitted) {s->reserved=true;commit_locked(s,limit);}
    else fault_locked(s,P0_BUDGET_GLOBAL_LIMIT);
    unlock(b);return admitted;
}
bool p0_budget_scope_close(p0_budget_scope *s) {
    p0_budget *b=s->budget;lock(b);bool empty=!s->closed && s->usage.live==0;
    if(empty) {commit_locked(s,0);s->closed=true;assert(b->scopes>0);b->scopes--;}
    unlock(b);return empty;
}
const GhosttyAllocator *p0_budget_allocator(p0_budget_scope *s,p0_budget_kind kind) {
    return kind>=0 && kind<P0_BUDGET_KINDS?&s->allocators[kind]:NULL;
}
bool p0_budget_set_limit(p0_budget *b,size_t n) {
    lock(b);bool valid=n>=b->usage.committed;if(valid)b->limit=n;unlock(b);return valid;
}
bool p0_budget_scope_set_limit(p0_budget_scope *s,size_t n) {
    p0_budget *b=s->budget;lock(b);bool valid=!s->closed && n>=s->usage.live;
    if(valid && s->reserved && n>s->limit && n-s->limit>b->limit-b->usage.committed)valid=false;
    if(valid) {if(s->reserved)commit_locked(s,n);s->limit=n;}
    unlock(b);return valid;
}
p0_budget_usage p0_budget_read(p0_budget *b) {lock(b);p0_budget_usage value=b->usage;unlock(b);return value;}
p0_budget_usage p0_budget_scope_read(p0_budget_scope *s) {lock(s->budget);p0_budget_usage value=s->usage;unlock(s->budget);return value;}
p0_budget_fault p0_budget_scope_fault(p0_budget_scope *s) {lock(s->budget);p0_budget_fault value=s->fault;unlock(s->budget);return value;}
#ifndef GHOSTERM_P0_ALLOCATOR_ONLY
GhosttyResult p0_budget_result(p0_budget_scope *s,GhosttyResult result) {
    if(!s)return GHOSTTY_INVALID_VALUE;
    p0_budget *b=s->budget;lock(b);p0_budget_fault fault=s->fault;bool closed=s->closed;unlock(b);
    if(closed)return GHOSTTY_INVALID_VALUE;
    if(fault==P0_BUDGET_OS_ERROR)return GHOSTTY_OUT_OF_MEMORY;
    if(fault!=P0_BUDGET_HEALTHY)return GHOSTTY_LIMIT_EXCEEDED;
    return result;
}
static GhosttyResult terminal_ready(p0_budget_scope *s,GhosttyTerminal t) {
    if(!t)return GHOSTTY_INVALID_VALUE;
    GhosttyResult rc=p0_budget_result(s,GHOSTTY_SUCCESS);if(rc!=GHOSTTY_SUCCESS)return rc;
    bool failed=false;rc=ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed);
    return rc!=GHOSTTY_SUCCESS?rc:failed?GHOSTTY_REJECTED:GHOSTTY_SUCCESS;
}
GhosttyResult p0_budget_feed(p0_budget_scope *s,GhosttyTerminal t,const uint8_t *data,size_t n) {
    if(!data && n)return GHOSTTY_INVALID_VALUE;
    GhosttyResult rc=terminal_ready(s,t);if(rc!=GHOSTTY_SUCCESS)return rc;
    if(n)ghostty_terminal_vt_write(t,data,n);
    return terminal_ready(s,t);
}
GhosttyResult p0_budget_resize_cells(p0_budget_scope *s,GhosttyTerminal t,uint16_t cols,uint16_t rows,uint32_t width,uint32_t height) {
    GhosttyResult rc=terminal_ready(s,t);if(rc!=GHOSTTY_SUCCESS)return rc;
    rc=ghostty_terminal_resize(t,cols,rows,width,height);
    return p0_budget_result(s,rc);
}

GhosttyResult p0_budget_resize(p0_budget_scope *s,GhosttyTerminal t,uint16_t cols,uint16_t rows) {
    return p0_budget_resize_cells(s,t,cols,rows,0,0);
}

#endif /* engine terminal helpers */
