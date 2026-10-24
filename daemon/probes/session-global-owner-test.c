#define _DEFAULT_SOURCE
#include <ghostty.h>
#include "session-global-owner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(int argc,char **argv) {
    REQUIRE(argc==3);const char *mode=argv[1];
    const size_t mib=1024*1024;
    if(!strcmp(mode,"invalid")) {
        REQUIRE(!p0_global_open(NULL,8*mib,NULL));REQUIRE(!p0_global_close(NULL));
        p0_global_owner invalid={.initialized=true};
        REQUIRE(!p0_global_open(NULL,8*mib,&invalid));REQUIRE(!p0_global_close(&invalid));
        puts("PASS global-lease/invalid: rejected without engine initialization");return 0;
    }
    REQUIRE(!setenv("GHOSTTY_LOG","false",1));
    REQUIRE(!setenv("GHOSTTY_RESOURCES_DIR",argv[2],1));
    p0_gui_limits limits={.total=64*mib,.general=mib,.pages=mib,.wire=mib,.surfaces=1};
    p0_gui_pool *pool=NULL;REQUIRE(p0_gui_pool_create(limits,&pool)==P0_GUI_OK);
    p0_global_owner owner={0};REQUIRE(p0_global_close(&owner));
    if(!strcmp(mode,"admission")) {
        p0_gui_reservation *other=NULL;REQUIRE(p0_gui_reserve(pool,64*mib,&other)==P0_GUI_OK);
        REQUIRE(!p0_global_open(pool,8*mib,&owner));
        REQUIRE(!owner.initialized && !owner.reservation && !owner.threads && !ghostty_p0_global_allocator());
        REQUIRE(p0_gui_pool_read(pool).committed==64*mib && p0_gui_pool_read(pool).reservations==1);
        REQUIRE(p0_gui_unreserve(other)==P0_GUI_OK);
    }
    if(!strcmp(mode,"capacity")) {
        char *huge=malloc(128*1024+1);REQUIRE(huge);memset(huge,'x',128*1024);huge[128*1024]=0;
        REQUIRE(!setenv("GHOSTTY_RESOURCES_DIR",huge,1));free(huge);
        REQUIRE(!p0_global_open(pool,(size_t)sysconf(_SC_PAGESIZE),&owner));
        REQUIRE(!owner.initialized && !owner.reservation && !owner.threads && !ghostty_p0_global_allocator());
        REQUIRE(!p0_global_open(pool,8*mib,&owner));
        REQUIRE(!owner.initialized && !owner.reservation && !owner.threads);
    } else {
        REQUIRE(p0_global_open(pool,8*mib,&owner));
        REQUIRE(owner.initialized && owner.reservation && owner.threads);
        const ghostty_p0_allocator_s *selected=p0_gui_reserved_allocator(owner.reservation);
        REQUIRE(ghostty_p0_global_allocator()==(uintptr_t)selected);
        REQUIRE(!p0_gui_reserved_idle(owner.reservation) && p0_gui_reserved_healthy(owner.reservation));
        REQUIRE(p0_gui_unreserve(owner.reservation)==P0_GUI_BUSY);
        REQUIRE(p0_gui_pool_close(pool)==P0_GUI_BUSY);
        REQUIRE(!p0_global_open(pool,8*mib,&owner));
        p0_global_owner duplicate={0};
        REQUIRE(!p0_global_open(pool,8*mib,&duplicate));
        REQUIRE(!duplicate.initialized && !duplicate.reservation && !duplicate.threads);
        REQUIRE(p0_gui_pool_read(pool).committed==8*mib+p0_global_thread_capacity() && p0_gui_pool_read(pool).reservations==2);
        if(!strcmp(mode,"busy")) {
            uint8_t *borrowed=selected->vtable->alloc(selected->ctx,64,0,0);REQUIRE(borrowed);
            REQUIRE(!p0_global_close(&owner));
            REQUIRE(!owner.initialized && owner.reservation && !ghostty_p0_global_allocator());
            REQUIRE(p0_gui_unreserve(owner.reservation)==P0_GUI_BUSY);
            selected->vtable->free(selected->ctx,borrowed,64,0,0);
        }
        REQUIRE(p0_global_close(&owner));
        REQUIRE(!owner.initialized && !owner.reservation && !owner.threads && !ghostty_p0_global_allocator());
        REQUIRE(p0_global_close(&owner));
    }
    p0_gui_usage usage=p0_gui_pool_read(pool);
    REQUIRE(!usage.live && !usage.committed && !usage.reservations);
    REQUIRE(p0_gui_pool_close(pool)==P0_GUI_OK);
    printf("PASS global-lease/%s: selected owner, explicit result and complete pool reclamation\n",mode);
    return 0;
}
