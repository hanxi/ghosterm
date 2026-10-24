#define _DEFAULT_SOURCE
#include <ghostty.h>
#include "gui-budget.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct {
    const ghostty_p0_allocator_s *base;
    size_t attempts,deny,live,freed;
} selected_owner;
static uint8_t *selected_alloc(void *ctx,size_t n,uint8_t a,uintptr_t r) {
    selected_owner *s=ctx;
    if (++s->attempts==s->deny) return NULL;
    uint8_t *p=s->base->vtable->alloc(s->base->ctx,n,a,r);
    if(p) s->live++;
    return p;
}
static bool selected_resize(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r) {
    selected_owner *s=ctx;return s->base->vtable->resize(s->base->ctx,p,n,a,to,r);
}
static uint8_t *selected_remap(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r) {
    selected_owner *s=ctx;return s->base->vtable->remap(s->base->ctx,p,n,a,to,r);
}
static void selected_free(void *ctx,uint8_t *p,size_t n,uint8_t a,uintptr_t r) {
    selected_owner *s=ctx;REQUIRE(s->live);s->base->vtable->free(s->base->ctx,p,n,a,r);s->live--;s->freed++;
}
static const ghostty_p0_allocator_vtable_s selected_vtable={selected_alloc,selected_resize,selected_remap,selected_free};
int main(int argc,char **argv) {
    REQUIRE(argc==3);
    const char *mode=argv[1];
    if(!strcmp(mode,"cold")) { REQUIRE(!ghostty_p0_deinit()); REQUIRE(!ghostty_p0_global_allocator()); puts("PASS global-owner/cold: no initialization or teardown"); return 0; }
    REQUIRE(ghostty_p0_init(0,NULL,NULL)!=0);
    REQUIRE(!setenv("GHOSTTY_LOG","false",1));
    REQUIRE(!setenv("GHOSTTY_RESOURCES_DIR",argv[2],1));
    const size_t mib=1024*1024;
    p0_gui_pool *pool=NULL;p0_gui_reservation *domain=NULL,*native=NULL;
    p0_gui_limits limits={.total=64*mib,.general=8*mib,.pages=8*mib,.wire=8*mib,.surfaces=1};
    REQUIRE(p0_gui_pool_create(limits,&pool)==P0_GUI_OK);
    REQUIRE(p0_gui_reserve(pool,!strcmp(mode,"capacity")?(size_t)sysconf(_SC_PAGESIZE):8*mib,&domain)==P0_GUI_OK);
    if(strcmp(mode,"legacy")) {
        size_t count=ghostty_p0_global_io_thread_limit(),stack=ghostty_p0_global_io_thread_stack_size(),page=(size_t)sysconf(_SC_PAGESIZE);
        REQUIRE(count==2 && stack==16*mib && p0_gui_reserve(pool,count*(stack+2*page),&native)==P0_GUI_OK);
    }
    selected_owner owner={.base=p0_gui_reserved_allocator(domain)};
    if(!strcmp(mode,"deny-first"))owner.deny=1;
    if(!strcmp(mode,"deny-second"))owner.deny=2;
    ghostty_p0_allocator_s host={&owner,&selected_vtable};
    if(!strcmp(mode,"legacy")) {
        REQUIRE(ghostty_init(0,NULL)==0);
        REQUIRE(!ghostty_p0_deinit() && !ghostty_p0_global_allocator());
        REQUIRE(ghostty_p0_init(0,NULL,&host)!=0);
        REQUIRE(ghostty_init(0,NULL)!=0);
        REQUIRE(!owner.attempts && !owner.live);
    } else {
        if(!strcmp(mode,"capacity")) {
            char *huge=malloc(128*1024+1);REQUIRE(huge);memset(huge,'x',128*1024);huge[128*1024]=0;
            REQUIRE(!setenv("GHOSTTY_RESOURCES_DIR",huge,1));free(huge);
        }
        int result=ghostty_p0_init(0,NULL,&host);
        if(owner.deny || !strcmp(mode,"capacity")) {
            REQUIRE(result!=0 && !ghostty_p0_global_allocator());
            REQUIRE(owner.attempts>=owner.deny && owner.attempts>0);
            REQUIRE(!owner.live && p0_gui_reserved_idle(domain));
            REQUIRE(!ghostty_p0_deinit());
            REQUIRE(ghostty_p0_init(0,NULL,&host)!=0);
            REQUIRE(ghostty_init(0,NULL)!=0);
            if(!strcmp(mode,"capacity"))REQUIRE(!p0_gui_reserved_healthy(domain));
            else REQUIRE(p0_gui_reserved_healthy(domain));
        } else {
            REQUIRE(result==0 && owner.attempts>=2 && owner.live>0);
            REQUIRE(ghostty_p0_global_allocator()==(uintptr_t)&host);
            REQUIRE(!p0_gui_reserved_idle(domain));
            REQUIRE(p0_gui_unreserve(domain)==P0_GUI_BUSY);
            size_t attempts=owner.attempts,live=owner.live;
            REQUIRE(ghostty_p0_init(0,NULL,&host)!=0);
            REQUIRE(ghostty_init(0,NULL)!=0);
            REQUIRE(owner.attempts==attempts && owner.live==live);
            ghostty_config_t config=ghostty_config_new();REQUIRE(config);
            REQUIRE(owner.live>live);ghostty_config_free(config);
            REQUIRE(owner.live==live);
            REQUIRE(ghostty_p0_deinit());
            REQUIRE(!owner.live && owner.freed>0 && p0_gui_reserved_idle(domain));
            REQUIRE(!ghostty_p0_global_allocator());
            REQUIRE(!ghostty_p0_deinit());
            REQUIRE(ghostty_p0_init(0,NULL,&host)!=0 && ghostty_init(0,NULL)!=0);
        }
    }
    REQUIRE(p0_gui_reserved_idle(domain));
    REQUIRE(p0_gui_unreserve(domain)==P0_GUI_OK);
    if(native)REQUIRE(p0_gui_unreserve(native)==P0_GUI_OK);
    p0_gui_usage usage=p0_gui_pool_read(pool);
    REQUIRE(!usage.live && !usage.committed && !usage.reservations);
    REQUIRE(p0_gui_pool_close(pool)==P0_GUI_OK);
    REQUIRE(!ghostty_p0_deinit());
    printf("PASS global-owner/%s: attempts=%zu freed=%zu; global domain reclaimed\n",mode,owner.attempts,owner.freed);
    return 0;
}
