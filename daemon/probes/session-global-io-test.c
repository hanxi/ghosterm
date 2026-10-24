#define _DARWIN_C_SOURCE
#include <ghostty.h>
#include "gui-budget.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {uint8_t *p;size_t n;} range;
typedef struct {
    pthread_mutex_t lock;const ghostty_p0_allocator_s *base;ghostty_p0_allocator_s allocator;
    range ranges[1024];size_t attempts,deny,live,freed;bool freed_registered;
    atomic_uint started,finished;atomic_bool release_exit,deinit_returned;bool hold_exit;size_t per;uintptr_t stack_floor;
} owner;
static uint8_t *selected_alloc(void *ctx,size_t n,uint8_t a,uintptr_t r) {
    owner *o=ctx;REQUIRE(!pthread_mutex_lock(&o->lock));
    if(++o->attempts==o->deny){REQUIRE(!pthread_mutex_unlock(&o->lock));return NULL;}
    uint8_t *p=o->base->vtable->alloc(o->base->ctx,n,a,r);
    if(p){size_t slot=0;while(slot<1024&&o->ranges[slot].p)slot++;REQUIRE(slot<1024);o->ranges[slot]=(range){p,n};o->live++;}
    REQUIRE(!pthread_mutex_unlock(&o->lock));return p;
}
static bool selected_resize(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r){(void)ctx;(void)p;(void)n;(void)a;(void)to;(void)r;return false;}
static uint8_t *selected_remap(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r){(void)ctx;(void)p;(void)n;(void)a;(void)to;(void)r;return NULL;}
static void selected_free(void *ctx,uint8_t *p,size_t n,uint8_t a,uintptr_t r) {
    owner *o=ctx;REQUIRE(!pthread_mutex_lock(&o->lock));size_t slot=0;
    while(slot<1024&&o->ranges[slot].p!=p)slot++;REQUIRE(slot<1024&&o->ranges[slot].n==n&&o->live);
    stack_t signal;REQUIRE(!sigaltstack(NULL,&signal));uintptr_t sp=(uintptr_t)signal.ss_sp;
    if(!(signal.ss_flags&SS_DISABLE)&&sp>=(uintptr_t)p&&sp-(uintptr_t)p<n)o->freed_registered=true;
    o->ranges[slot]=(range){0};o->live--;o->freed++;o->base->vtable->free(o->base->ctx,p,n,a,r);
    REQUIRE(!pthread_mutex_unlock(&o->lock));
}
static size_t live_count(owner *o) {REQUIRE(!pthread_mutex_lock(&o->lock));size_t result=o->live;REQUIRE(!pthread_mutex_unlock(&o->lock));return result;}
static const ghostty_p0_allocator_vtable_s vtable={selected_alloc,selected_resize,selected_remap,selected_free};
static void observed(void *ctx,uint32_t role,uint32_t phase) {
    owner *o=ctx;REQUIRE(!pthread_main_np()&&role<2&&phase<=1);
    size_t stack=pthread_get_stacksize_np(pthread_self());REQUIRE(stack>=o->stack_floor&&stack<o->per);
    uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());REQUIRE(top>stack);
    mach_vm_address_t guard=top-stack-1,address=guard;mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    REQUIRE(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
    REQUIRE(address<=guard&&address+size>guard&&info.protection==VM_PROT_NONE&&size>=(size_t)sysconf(_SC_PAGESIZE)&&size<=o->per-stack);
    if(object)REQUIRE(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
    stack_t signal;REQUIRE(!sigaltstack(NULL,&signal)&&!(signal.ss_flags&SS_DISABLE)&&signal.ss_size==256*1024);
    bool found=false;uintptr_t sp=(uintptr_t)signal.ss_sp;
    REQUIRE(!pthread_mutex_lock(&o->lock));
    for(size_t i=0;i<1024;i++)if(o->ranges[i].p&&sp>=(uintptr_t)o->ranges[i].p&&sp-(uintptr_t)o->ranges[i].p<=o->ranges[i].n&&signal.ss_size<=o->ranges[i].n-(sp-(uintptr_t)o->ranges[i].p))found=true;
    REQUIRE(!pthread_mutex_unlock(&o->lock));REQUIRE(found);
    if(!phase){uintptr_t allocator=0;REQUIRE(ghostty_p0_global_io_thread_allocator(role,&allocator)&&allocator==(uintptr_t)&o->allocator);}
    atomic_uint *mask=phase?&o->finished:&o->started;REQUIRE(!(atomic_fetch_or(mask,1u<<role)&(1u<<role)));
    printf("METRIC global-io role=%u finished=%u stack=%zu guard=%llu reserve=%zu\n",role,phase,stack,(unsigned long long)size,o->per);
    if(phase && o->hold_exit) {
        unsigned tries=0;while(!atomic_load(&o->release_exit) && tries++<10000)usleep(1000);
        REQUIRE(atomic_load(&o->release_exit));
    }
}
static void *close_engine(void *context) {
    owner *o=context;REQUIRE(ghostty_p0_deinit());atomic_store(&o->deinit_returned,true);return NULL;
}
int main(int argc,char **argv) {
    REQUIRE(argc==3);const size_t mib=1024*1024;
    REQUIRE(ghostty_p0_global_io_thread_limit()==2&&ghostty_p0_global_io_thread_stack_size()==16*mib);
    REQUIRE(!setenv("GHOSTTY_LOG","false",1)&&!setenv("GHOSTTY_RESOURCES_DIR",argv[2],1));
    ghostty_p0_io_stats_s stats={.threads=99};REQUIRE(!ghostty_p0_global_io_stats(&stats)&&stats.threads==99);
    p0_gui_limits limits={.total=64*mib,.general=mib,.pages=mib,.wire=mib,.surfaces=1};
    p0_gui_pool *pool=NULL;p0_gui_reservation *general=NULL,*threads=NULL;
    REQUIRE(p0_gui_pool_create(limits,&pool)==P0_GUI_OK&&p0_gui_reserve(pool,8*mib,&general)==P0_GUI_OK);
    owner o={.base=p0_gui_reserved_allocator(general),.per=16*mib+2*(size_t)sysconf(_SC_PAGESIZE),.stack_floor=16*mib};
    REQUIRE(!pthread_mutex_init(&o.lock,NULL));o.allocator=(ghostty_p0_allocator_s){&o,&vtable};
    REQUIRE(p0_gui_reserve(pool,2*o.per,&threads)==P0_GUI_OK);
    REQUIRE(!ghostty_p0_init(0,NULL,&o.allocator)&&ghostty_p0_global_io_set_observer(&o,observed));
    REQUIRE(ghostty_p0_global_io_stats(&stats)&&!stats.threads&&!stats.started&&!stats.joined);
    REQUIRE(!ghostty_p0_global_io_thread_allocator(0,NULL));
    size_t initial=live_count(&o);bool idle=!strcmp(argv[1],"idle"),join=!strcmp(argv[1],"join");o.hold_exit=join;
    unsigned kind=(unsigned)atoi(argv[1]);bool denial=!strcmp(argv[1],"deny"),reuse=!strcmp(argv[1],"reuse");
    if(denial){o.deny=o.attempts+2;REQUIRE(!ghostty_p0_global_io_probe(0));REQUIRE(ghostty_p0_global_io_stats(&stats)&&!stats.threads&&!stats.started&&stats.failures==1&&live_count(&o)==initial);o.deny=0;kind=0;}
    unsigned expected=idle?0u:(kind==4)?1u:2u,mask=(1u<<expected)-1;
    if(reuse){for(unsigned i=0;i<400;i++)REQUIRE(ghostty_p0_global_io_probe(i%4));}
    else if(!idle)REQUIRE(ghostty_p0_global_io_probe(kind));
    REQUIRE(atomic_load(&o.started)==mask&&!atomic_load(&o.finished));
    if(!idle)REQUIRE(!ghostty_p0_global_io_set_observer(NULL,NULL));
    REQUIRE(ghostty_p0_global_io_stats(&stats)&&stats.threads==expected&&stats.started==expected&&!stats.finished&&!stats.joined&&stats.failures==(denial?1u:0u));
    REQUIRE(live_count(&o)>=initial+expected&&p0_gui_unreserve(general)==P0_GUI_BUSY);
    if(join) {
        pthread_t closer;REQUIRE(!pthread_create(&closer,NULL,close_engine,&o));
        unsigned tries=0;while(atomic_load(&o.finished)!=mask && tries++<2000)usleep(1000);
        REQUIRE(atomic_load(&o.finished)==mask);
        for(tries=0;tries<2000 && !atomic_load(&o.deinit_returned);tries++)usleep(1000);
        REQUIRE(!atomic_load(&o.deinit_returned));
        atomic_store(&o.release_exit,true);REQUIRE(!pthread_join(closer,NULL)&&atomic_load(&o.deinit_returned));
    } else REQUIRE(ghostty_p0_deinit());
    REQUIRE(ghostty_p0_global_io_stats(&stats)&&!stats.threads&&stats.started==expected&&stats.finished==expected&&stats.joined==expected);
    REQUIRE(atomic_load(&o.finished)==mask&&!o.live&&!o.freed_registered&&p0_gui_reserved_idle(general));
    REQUIRE(p0_gui_unreserve(general)==P0_GUI_OK&&p0_gui_unreserve(threads)==P0_GUI_OK);
    p0_gui_usage usage=p0_gui_pool_read(pool);REQUIRE(!usage.live&&!usage.committed&&!usage.reservations&&p0_gui_pool_close(pool)==P0_GUI_OK);
    REQUIRE(!pthread_mutex_destroy(&o.lock));printf("PASS global-io/%s: selected frames, real native stack/guard, capacity, joined teardown and zero owner\n",argv[1]);return 0;
}
