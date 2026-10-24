#include "gui-budget.h"
#include "budget-posix.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
struct p0_gui_pool {
    p0_gui_metadata_allocator allocator;
    pthread_mutex_t admission;
    p0_budget budget;
    p0_budget_scope metadata_capacity;
    p0_gui_limits limits;
    size_t surfaces,reservations;
};
struct p0_gui_budget {
    p0_gui_pool *pool;
    p0_budget_scope general,pages,wire,threads;
};
struct p0_gui_reservation {p0_gui_pool *pool;p0_budget_scope scope;};
static void *metadata_default_allocate(void *c,size_t n){(void)c;return malloc(n);}
static void metadata_default_release(void *c,void *p,size_t n){(void)c;(void)n;free(p);}
static void *metadata_new(p0_gui_pool *p,size_t n) {
    void *v=p->allocator.allocate(p->allocator.context,n);if(v)memset(v,0,n);return v;
}
static void metadata_free(p0_gui_pool *p,void *v,size_t n){p->allocator.release(p->allocator.context,v,n);}
p0_gui_status p0_gui_pool_create(p0_gui_limits limits,p0_gui_pool **out) {
    return p0_gui_pool_create_with_allocator(limits,NULL,out);
}
p0_gui_status p0_gui_pool_create_with_allocator(p0_gui_limits limits,const p0_gui_metadata_allocator *allocator,p0_gui_pool **out) {
    p0_gui_metadata_allocator a=allocator?*allocator:(p0_gui_metadata_allocator){NULL,metadata_default_allocate,metadata_default_release};
    if(out)*out=NULL;
    if((limits.metadata && !allocator) || !a.allocate || !a.release || !out || !limits.general || !limits.pages || !limits.wire || !limits.surfaces ||
       limits.general>SIZE_MAX-limits.pages || limits.general+limits.pages>SIZE_MAX-limits.wire ||
       limits.general+limits.pages+limits.wire>SIZE_MAX-limits.threads)
        return P0_GUI_INVALID;
    p0_gui_pool *p=a.allocate(a.context,sizeof(*p));if(!p)return P0_GUI_NO_MEMORY;
    memset(p,0,sizeof(*p));p->allocator=a;
    if(pthread_mutex_init(&p->admission,NULL)) {a.release(a.context,p,sizeof(*p));return P0_GUI_ERROR;}
    if(!p0_budget_init(&p->budget,limits.total)) {
        pthread_mutex_destroy(&p->admission);a.release(a.context,p,sizeof(*p));return P0_GUI_ERROR;
    }
    if(limits.metadata && !p0_budget_scope_admit(&p->metadata_capacity,&p->budget,limits.metadata)) {
        (void)p0_budget_scope_close(&p->metadata_capacity);(void)p0_budget_destroy(&p->budget);
        pthread_mutex_destroy(&p->admission);a.release(a.context,p,sizeof(*p));return P0_GUI_LIMIT;
    }
    p->limits=limits;*out=p;return P0_GUI_OK;
}
p0_gui_status p0_gui_reserve(p0_gui_pool *p,size_t bytes,p0_gui_reservation **out) {
    if(out)*out=NULL;
    if(!p || !bytes || !out)return P0_GUI_INVALID;
    pthread_mutex_lock(&p->admission);
    p0_gui_reservation *r=metadata_new(p,sizeof(*r));
    if(!r){pthread_mutex_unlock(&p->admission);return P0_GUI_NO_MEMORY;}
    r->pool=p;
    if(!p0_budget_scope_admit(&r->scope,&p->budget,bytes)) {
        (void)p0_budget_scope_close(&r->scope);metadata_free(p,r,sizeof(*r));
        pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;
    }
    p->reservations++;*out=r;pthread_mutex_unlock(&p->admission);return P0_GUI_OK;
}
p0_gui_status p0_gui_unreserve(p0_gui_reservation *r) {
    if(!r)return P0_GUI_INVALID;
    p0_gui_pool *p=r->pool;pthread_mutex_lock(&p->admission);
    if(!p0_gui_reserved_idle(r)){pthread_mutex_unlock(&p->admission);return P0_GUI_BUSY;}
    if(!p0_budget_scope_close(&r->scope)){pthread_mutex_unlock(&p->admission);return P0_GUI_ERROR;}
    p->reservations--;metadata_free(p,r,sizeof(*r));pthread_mutex_unlock(&p->admission);return P0_GUI_OK;
}
const void *p0_gui_reserved_allocator(p0_gui_reservation *r) {
    return r?p0_budget_allocator(&r->scope,P0_BUDGET_GENERAL):NULL;
}
bool p0_gui_reserved_healthy(p0_gui_reservation *r) {
    return r && p0_budget_result(&r->scope,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS;
}
bool p0_gui_reserved_idle(p0_gui_reservation *r) {
    if(!r)return false;
    p0_budget_usage u=p0_budget_scope_read(&r->scope);
    return !u.live && u.mapped==u.unmapped;
}
p0_gui_status p0_gui_admit(p0_gui_pool *p,p0_gui_budget **out) {
    if(out)*out=NULL;
    if(!p || !out)return P0_GUI_INVALID;
    pthread_mutex_lock(&p->admission);
    if(p->surfaces>=p->limits.surfaces) {
        pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;
    }
    p0_gui_budget *b=metadata_new(p,sizeof(*b));
    if(!b) {pthread_mutex_unlock(&p->admission);return P0_GUI_NO_MEMORY;}
    b->pool=p;
    p0_budget_scope *scopes[]={&b->general,&b->pages,&b->wire};
    const size_t limits[]={p->limits.general,p->limits.pages,p->limits.wire};
    for(unsigned i=0;i<3;i++) {
        if(!p0_budget_scope_admit(scopes[i],&p->budget,limits[i])) {
            /* Even the failed admit creates a faulted, open scope. */
            for(unsigned j=0;j<=i;j++)(void)p0_budget_scope_close(scopes[j]);
            metadata_free(p,b,sizeof(*b));pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;
        }
    }
    if(p->limits.threads) {
        if(!p0_budget_scope_admit(&b->threads,&p->budget,p->limits.threads)) {
            (void)p0_budget_scope_close(&b->threads);
            for(unsigned j=0;j<3;j++)(void)p0_budget_scope_close(scopes[j]);
            metadata_free(p,b,sizeof(*b));pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;
        }
    }
    p->surfaces++;*out=b;pthread_mutex_unlock(&p->admission);return P0_GUI_OK;
}
p0_gui_usage p0_gui_pool_read(p0_gui_pool *p) {
    if(!p)return (p0_gui_usage){0};
    pthread_mutex_lock(&p->admission);p0_budget_usage u=p0_budget_read(&p->budget);
    p0_gui_usage result={p->surfaces,u.committed,u.live,u.peak,u.by_kind[P0_BUDGET_SCRATCH],p->reservations};
    pthread_mutex_unlock(&p->admission);return result;
}
const void *p0_gui_general(p0_gui_budget *b) {return b ? p0_budget_allocator(&b->general,P0_BUDGET_GENERAL) : NULL;}
const void *p0_gui_pages(p0_gui_budget *b) {return b ? p0_budget_allocator(&b->pages,P0_BUDGET_PAGES) : NULL;}
size_t p0_gui_thread_capacity(p0_gui_budget *b) {return b && b->pool->limits.threads?p0_budget_scope_read(&b->threads).committed:0;}
bool p0_gui_healthy(void *context) {
    p0_gui_budget *b=context;
    return b && p0_budget_result(&b->general,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS &&
           p0_budget_result(&b->pages,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS &&
           p0_budget_result(&b->wire,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS;
}
bool p0_gui_idle(p0_gui_budget *b) {
    if(!b)return false;
    p0_budget_scope *scopes[]={&b->general,&b->pages,&b->wire};
    for(unsigned i=0;i<3;i++) {
        p0_budget_usage u=p0_budget_scope_read(scopes[i]);
        if(u.live || u.mapped!=u.unmapped)return false;
    }
    return true;
}
void *p0_gui_buffer_new(p0_gui_budget *b,size_t n) {
    if(!b || !n || !p0_gui_healthy(b))return NULL;
    const GhosttyAllocator *a=p0_budget_allocator(&b->wire,P0_BUDGET_SCRATCH);
    return a->vtable->alloc(a->ctx,n,0,0);
}
void p0_gui_buffer_free(p0_gui_budget *b,void *bytes,size_t n) {
    if(!b || !bytes)return;
    const GhosttyAllocator *a=p0_budget_allocator(&b->wire,P0_BUDGET_SCRATCH);
    a->vtable->free(a->ctx,bytes,n,0,0);
}
p0_gui_status p0_gui_release(p0_gui_budget *b) {
    if(!b)return P0_GUI_INVALID;
    p0_gui_pool *p=b->pool;pthread_mutex_lock(&p->admission);
    if(!p0_gui_idle(b)) {pthread_mutex_unlock(&p->admission);return P0_GUI_BUSY;}
    if(!p0_budget_scope_close(&b->general) || !p0_budget_scope_close(&b->pages) || !p0_budget_scope_close(&b->wire)) {
        pthread_mutex_unlock(&p->admission);return P0_GUI_ERROR;
    }
    if(p->limits.threads && !p0_budget_scope_close(&b->threads)){pthread_mutex_unlock(&p->admission);return P0_GUI_ERROR;}
    p->surfaces--;metadata_free(p,b,sizeof(*b));pthread_mutex_unlock(&p->admission);return P0_GUI_OK;
}
p0_gui_status p0_gui_pool_close(p0_gui_pool *p) {
    if(!p)return P0_GUI_INVALID;
    pthread_mutex_lock(&p->admission);
    if(p->surfaces || p->reservations) {pthread_mutex_unlock(&p->admission);return P0_GUI_BUSY;}
    if(p->limits.metadata && !p0_budget_scope_close(&p->metadata_capacity)) {pthread_mutex_unlock(&p->admission);return P0_GUI_ERROR;}
    if(!p0_budget_destroy(&p->budget)) {pthread_mutex_unlock(&p->admission);return P0_GUI_ERROR;}
    pthread_mutex_unlock(&p->admission);
    if(pthread_mutex_destroy(&p->admission))return P0_GUI_ERROR;
    p0_gui_metadata_allocator a=p->allocator;a.release(a.context,p,sizeof(*p));return P0_GUI_OK;
}
