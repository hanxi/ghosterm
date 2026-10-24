/* Private P0 owner of one process-global engine initialization and its lease. */
#ifndef GT_P0_GLOBAL_OWNER_H
#define GT_P0_GLOBAL_OWNER_H
#include <ghostty.h>
#include "gui-budget.h"
#include <unistd.h>
typedef struct {p0_gui_reservation *reservation,*threads;bool initialized;} p0_global_owner;
static inline size_t p0_global_thread_capacity(void) {
    long page=sysconf(_SC_PAGESIZE);size_t count=ghostty_p0_global_io_thread_limit(),stack=ghostty_p0_global_io_thread_stack_size();
    if(page<=0 || !count || !stack || (size_t)page>SIZE_MAX/2 || stack>SIZE_MAX-2*(size_t)page)return 0;
    size_t per=stack+2*(size_t)page;if(count>SIZE_MAX/per)return 0;return count*per;
}
/* Caller has destroyed all engine borrowers and joined their threads first.
 * A failed unreserve preserves the lease for cleanup; init cannot be retried. */
static inline bool p0_global_close(p0_global_owner *o) {
    if(!o)return false;
    if(o->initialized) {
        if(!o->reservation || !o->threads || !ghostty_p0_deinit())return false;
        o->initialized=false;
    }
    if(o->reservation) {
        if(p0_gui_unreserve(o->reservation)!=P0_GUI_OK)return false;
        o->reservation=NULL;
    }
    if(o->threads) {
        if(p0_gui_unreserve(o->threads)!=P0_GUI_OK)return false;
        o->threads=NULL;
    }
    return true;
}
static inline bool p0_global_open(p0_gui_pool *pool,size_t capacity,p0_global_owner *o) {
    if(!o || o->initialized || o->reservation || o->threads)return false;
    if(p0_gui_reserve(pool,capacity,&o->reservation)!=P0_GUI_OK)return false;
    size_t native=p0_global_thread_capacity();
    if(!native || p0_gui_reserve(pool,native,&o->threads)!=P0_GUI_OK) {(void)p0_global_close(o);return false;}
    const ghostty_p0_allocator_s *allocator=p0_gui_reserved_allocator(o->reservation);
    if(ghostty_p0_init(0,NULL,allocator)!=0) {(void)p0_global_close(o);return false;}
    o->initialized=true;
    if(!p0_gui_reserved_healthy(o->reservation) || ghostty_p0_global_allocator()!=(uintptr_t)allocator) {
        (void)p0_global_close(o);return false;
    }
    return true;
}
#endif
