#define _POSIX_C_SOURCE 200809L
#include "gui-budget.h"
#include "budget-posix.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
typedef struct {p0_gui_pool *pool;atomic_bool *go;p0_gui_budget *lease;p0_gui_reservation *reservation;size_t runtime_bytes;p0_gui_status result;} attempt;
static void *admit_thread(void *context) {
    attempt *a=context;
    while(!atomic_load(a->go)) {const struct timespec pause={0,100000};nanosleep(&pause,NULL);}
    a->result=a->runtime_bytes?p0_gui_reserve(a->pool,a->runtime_bytes,&a->reservation):p0_gui_admit(a->pool,&a->lease);return NULL;
}
typedef struct {unsigned attempts,fail,live;void *p[8];size_t sizes[8];} metadata_fixture;
static void *meta_allocate(void *context,size_t n) {
    metadata_fixture *f=context;if(++f->attempts==f->fail)return NULL;
    void *p=malloc(n);assert(p);memset(p,0xa5,n);
    for(unsigned i=0;i<8;i++)if(!f->p[i]){f->p[i]=p;f->sizes[i]=n;f->live++;return p;}
    abort();
}
static void meta_release(void *context,void *p,size_t n) {
    metadata_fixture *f=context;
    for(unsigned i=0;i<8;i++)if(f->p[i]==p){assert(n==f->sizes[i] && f->live);f->p[i]=NULL;f->live--;free(p);return;}
    abort();
}
static void metadata_case(p0_gui_limits limits,size_t page) {
    for(unsigned fail=1;fail<=4;fail++) {
        metadata_fixture f={.fail=fail};p0_gui_metadata_allocator a={&f,meta_allocate,meta_release};p0_gui_pool *p=(void *)1;
        p0_gui_status rc=p0_gui_pool_create_with_allocator(limits,&a,&p);
        if(fail==1){assert(rc==P0_GUI_NO_MEMORY && !p && !f.live);continue;}
        assert(rc==P0_GUI_OK && f.live==1);
        p0_gui_reservation *r=(void *)1;rc=p0_gui_reserve(p,page,&r);
        assert(rc==(fail==2?P0_GUI_NO_MEMORY:P0_GUI_OK) && (!!r)==(fail!=2));
        p0_gui_budget *b=(void *)1;rc=p0_gui_admit(p,&b);
        assert(rc==(fail==3?P0_GUI_NO_MEMORY:P0_GUI_OK) && (!!b)==(fail!=3));
        assert(f.live==1u+(r!=NULL)+(b!=NULL));
        assert(p0_gui_pool_close(p)==P0_GUI_BUSY);
        if(r)assert(p0_gui_unreserve(r)==P0_GUI_OK);
        if(b)assert(p0_gui_release(b)==P0_GUI_OK);
        assert(f.live==1 && p0_gui_pool_close(p)==P0_GUI_OK && !f.live);
    }
    metadata_fixture f={0};p0_gui_metadata_allocator a={&f,meta_allocate,meta_release};p0_gui_pool *p=NULL;limits.total=0;
    assert(p0_gui_pool_create_with_allocator(limits,&a,&p)==P0_GUI_OK);
    p0_gui_reservation *r=(void *)1;assert(p0_gui_reserve(p,page,&r)==P0_GUI_LIMIT && !r && f.live==1);
    p0_gui_budget *b=(void *)1;assert(p0_gui_admit(p,&b)==P0_GUI_LIMIT && !b && f.live==1);
    assert(p0_gui_pool_close(p)==P0_GUI_OK && !f.live);
}
static void combined_case(p0_gui_limits limits,size_t page) {
    metadata_fixture f={0};p0_gui_metadata_allocator a={&f,meta_allocate,meta_release};p0_gui_pool *p=(void *)1;
    limits.metadata=7*page;
    assert(p0_gui_pool_create(limits,&p)==P0_GUI_INVALID && !p);
    assert(p0_gui_pool_create_with_allocator(limits,&a,&p)==P0_GUI_OK && f.live==1);
    p0_gui_usage u=p0_gui_pool_read(p);assert(u.committed==7*page && !u.surfaces);
    p0_gui_budget *b=(void *)1;assert(p0_gui_admit(p,&b)==P0_GUI_LIMIT && !b && f.live==1);
    p0_gui_reservation *r=(void *)1;assert(p0_gui_reserve(p,6*page,&r)==P0_GUI_LIMIT && !r && f.live==1);
    assert(p0_gui_reserve(p,5*page,&r)==P0_GUI_OK);
    u=p0_gui_pool_read(p);assert(u.committed==12*page && u.reservations==1);
    assert(p0_gui_unreserve(r)==P0_GUI_OK && p0_gui_pool_read(p).committed==7*page);
    assert(p0_gui_pool_close(p)==P0_GUI_OK && !f.live);
    limits.metadata=13*page;p=(void *)1;
    assert(p0_gui_pool_create_with_allocator(limits,&a,&p)==P0_GUI_LIMIT && !p && !f.live);
}
static void combined_race_case(p0_gui_limits limits,size_t page) {
    metadata_fixture f={0};p0_gui_metadata_allocator allocator={&f,meta_allocate,meta_release};p0_gui_pool *pool=NULL;
    limits.metadata=6*page;assert(p0_gui_pool_create_with_allocator(limits,&allocator,&pool)==P0_GUI_OK);
    for(unsigned i=0;i<32;i++) {
        atomic_bool go=false;attempt a[2]={{.pool=pool,.go=&go},{.pool=pool,.go=&go,.runtime_bytes=6*page}};
        pthread_t threads[2];
        for(unsigned j=0;j<2;j++){unsigned k=(i+j)%2;assert(!pthread_create(&threads[k],NULL,admit_thread,&a[k]));}
        atomic_store(&go,true);
        for(unsigned j=0;j<2;j++)assert(!pthread_join(threads[j],NULL));
        assert((a[0].result==P0_GUI_OK)+(a[1].result==P0_GUI_OK)==1);
        assert((a[0].result==P0_GUI_LIMIT)+(a[1].result==P0_GUI_LIMIT)==1);
        assert((a[0].lease!=NULL)==(a[0].result==P0_GUI_OK));
        assert((a[1].reservation!=NULL)==(a[1].result==P0_GUI_OK));
        p0_gui_usage u=p0_gui_pool_read(pool);
        assert(u.committed==12*page && u.surfaces==(a[0].lease!=NULL) && u.reservations==(a[1].reservation!=NULL) && f.live==2);
        assert(p0_gui_pool_close(pool)==P0_GUI_BUSY);
        if(a[0].lease)assert(p0_gui_release(a[0].lease)==P0_GUI_OK);
        if(a[1].reservation)assert(p0_gui_unreserve(a[1].reservation)==P0_GUI_OK);
        u=p0_gui_pool_read(pool);assert(u.committed==6*page && !u.surfaces && !u.reservations && f.live==1);
        /* Both rejection directions are mandatory, independent of race winner. */
        p0_gui_budget *surface=NULL;assert(p0_gui_admit(pool,&surface)==P0_GUI_OK);
        p0_gui_reservation *runtime=(void *)1;
        assert(p0_gui_reserve(pool,6*page,&runtime)==P0_GUI_LIMIT && !runtime && f.live==2);
        assert(p0_gui_release(surface)==P0_GUI_OK);
        assert(p0_gui_reserve(pool,6*page,&runtime)==P0_GUI_OK);surface=(void *)1;
        assert(p0_gui_admit(pool,&surface)==P0_GUI_LIMIT && !surface && f.live==2);
        assert(p0_gui_unreserve(runtime)==P0_GUI_OK);
        u=p0_gui_pool_read(pool);assert(u.committed==6*page && !u.live && !u.surfaces && !u.reservations && f.live==1);
    }
    assert(p0_gui_pool_close(pool)==P0_GUI_OK && !f.live);
}
static void threads_case(size_t page) {
    p0_gui_limits limits={14*page,2*page,2*page,2*page,3,0,2*page};
    p0_gui_pool *pool=NULL;assert(p0_gui_pool_create(limits,&pool)==P0_GUI_OK);
    p0_gui_budget *first=NULL;assert(p0_gui_admit(pool,&first)==P0_GUI_OK);
    assert(p0_gui_thread_capacity(first)==2*page && p0_gui_pool_read(pool).committed==8*page);
    for(unsigned i=0;i<4;i++) {
        p0_gui_budget *second=(void *)1;assert(p0_gui_admit(pool,&second)==P0_GUI_LIMIT && !second);
        p0_gui_usage u=p0_gui_pool_read(pool);assert(u.surfaces==1 && u.committed==8*page && !u.live);
        assert(p0_gui_healthy(first));
    }
    assert(p0_gui_release(first)==P0_GUI_OK && !p0_gui_pool_read(pool).committed);
    assert(p0_gui_pool_close(pool)==P0_GUI_OK);
    limits.threads=SIZE_MAX;assert(p0_gui_pool_create(limits,&pool)==P0_GUI_INVALID && !pool);
}
int main(int argc,char **argv) {
    assert(argc==2);size_t page=(size_t)sysconf(_SC_PAGESIZE);assert(page && page<=SIZE_MAX/100);
    p0_gui_limits limits={12*page,2*page,2*page,2*page,3,0,0};
    if(!strcmp(argv[1],"threads")){threads_case(page);return 0;}
    if(!strcmp(argv[1],"combined-race")){combined_race_case(limits,page);return 0;}
    if(!strcmp(argv[1],"combined")){combined_case(limits,page);return 0;}
    if(!strcmp(argv[1],"metadata")){metadata_case(limits,page);return 0;}
    if(!strcmp(argv[1],"slots"))limits.surfaces=1;
    if(!strcmp(argv[1],"race"))limits.total=6*page;
    if(!strcmp(argv[1],"rollback"))limits.total=4*page;
    p0_gui_pool *pool=NULL;assert(p0_gui_pool_create(limits,&pool)==P0_GUI_OK && pool);
    if(!strcmp(argv[1],"runtime-allocator")) {
        assert(!p0_gui_reserved_allocator(NULL) && !p0_gui_reserved_healthy(NULL) && !p0_gui_reserved_idle(NULL));
        p0_gui_reservation *r=NULL;assert(p0_gui_reserve(pool,4*page,&r)==P0_GUI_OK);
        const GhosttyAllocator *a=p0_gui_reserved_allocator(r);assert(a && p0_gui_reserved_healthy(r) && p0_gui_reserved_idle(r));
        void *p=a->vtable->alloc(a->ctx,page+1,0,0);assert(p);memset(p,0x5a,page+1);
        assert(p0_gui_pool_read(pool).live==2*page && !p0_gui_reserved_idle(r));
        assert(p0_gui_unreserve(r)==P0_GUI_BUSY && ((uint8_t *)p)[page]==0x5a);
        p0_gui_budget *b=NULL;assert(p0_gui_admit(pool,&b)==P0_GUI_OK && p0_gui_healthy(b));
        assert(!a->vtable->alloc(a->ctx,4*page+1,0,0));
        assert(!p0_gui_reserved_healthy(r) && p0_gui_healthy(b) && p0_gui_pool_read(pool).committed==10*page);
        a->vtable->free(a->ctx,p,page+1,0,0);
        assert(p0_gui_reserved_idle(r) && !p0_gui_reserved_healthy(r));
        assert(p0_gui_unreserve(r)==P0_GUI_OK && p0_gui_release(b)==P0_GUI_OK);
        assert(!p0_gui_pool_read(pool).live && !p0_gui_pool_read(pool).committed);
        assert(p0_gui_pool_close(pool)==P0_GUI_OK);return 0;
    }
    if(!strcmp(argv[1],"runtime")) {
        p0_gui_reservation *r=(void *)1;
        assert(p0_gui_reserve(NULL,page,&r)==P0_GUI_INVALID && !r);
        assert(p0_gui_reserve(pool,0,&r)==P0_GUI_INVALID && !r);
        assert(p0_gui_reserve(pool,7*page,&r)==P0_GUI_OK && r);
        p0_gui_usage u=p0_gui_pool_read(pool);assert(u.reservations==1 && u.committed==7*page && !u.live && !u.surfaces);
        assert(p0_gui_pool_close(pool)==P0_GUI_BUSY);
        p0_gui_budget *b=(void *)1;assert(p0_gui_admit(pool,&b)==P0_GUI_LIMIT && !b);
        p0_gui_reservation *bad=(void *)1;assert(p0_gui_reserve(pool,6*page,&bad)==P0_GUI_LIMIT && !bad);
        u=p0_gui_pool_read(pool);assert(u.reservations==1 && u.committed==7*page);
        assert(p0_gui_unreserve(r)==P0_GUI_OK);
        u=p0_gui_pool_read(pool);assert(!u.reservations && !u.committed);
        assert(p0_gui_admit(pool,&b)==P0_GUI_OK);
        assert(p0_gui_reserve(pool,7*page,&bad)==P0_GUI_LIMIT && !bad);
        assert(p0_gui_release(b)==P0_GUI_OK && p0_gui_pool_close(pool)==P0_GUI_OK);return 0;
    }
    if(!strcmp(argv[1],"invalid")) {
        p0_gui_pool *bad=(void *)1;limits.general=SIZE_MAX;
        assert(p0_gui_pool_create(limits,&bad)==P0_GUI_INVALID && !bad);
        limits.general=0;assert(p0_gui_pool_create(limits,&bad)==P0_GUI_INVALID && !bad);
        p0_gui_budget *lease=(void *)1;assert(p0_gui_admit(NULL,&lease)==P0_GUI_INVALID && !lease);
        assert(p0_gui_release(NULL)==P0_GUI_INVALID);
        assert(p0_gui_pool_close(pool)==P0_GUI_OK);return 0;
    }
    if(!strcmp(argv[1],"rollback")) {
        for(unsigned i=0;i<5;i++) {
            p0_gui_budget *bad=(void *)1;
            assert(p0_gui_admit(pool,&bad)==P0_GUI_LIMIT && !bad);
            p0_gui_usage u=p0_gui_pool_read(pool);assert(!u.surfaces && !u.committed && !u.live);
        }
        assert(p0_gui_pool_close(pool)==P0_GUI_OK);return 0;
    }
    if(!strcmp(argv[1],"race")) {
        for(unsigned i=0;i<32;i++) {
            atomic_bool go=false;attempt attempts[2]={{.pool=pool,.go=&go},{.pool=pool,.go=&go}};pthread_t threads[2];
            for(unsigned j=0;j<2;j++)assert(!pthread_create(&threads[j],NULL,admit_thread,&attempts[j]));
            atomic_store(&go,true);
            for(unsigned j=0;j<2;j++)assert(!pthread_join(threads[j],NULL));
            assert((attempts[0].result==P0_GUI_OK)+(attempts[1].result==P0_GUI_OK)==1);
            assert((attempts[0].result==P0_GUI_LIMIT)+(attempts[1].result==P0_GUI_LIMIT)==1);
            for(unsigned j=0;j<2;j++)if(attempts[j].lease)assert(p0_gui_release(attempts[j].lease)==P0_GUI_OK);
            p0_gui_usage u=p0_gui_pool_read(pool);assert(!u.surfaces && !u.committed && !u.live);
        }
        assert(p0_gui_pool_close(pool)==P0_GUI_OK);return 0;
    }
    p0_gui_budget *a=NULL,*b=NULL;assert(p0_gui_admit(pool,&a)==P0_GUI_OK && a);
    assert(p0_gui_pool_read(pool).committed==6*page && p0_gui_healthy(a));
    if(!strcmp(argv[1],"slots")) {
        assert(p0_gui_admit(pool,&b)==P0_GUI_LIMIT && !b);
    } else if(!strcmp(argv[1],"bytes")) {
        assert(p0_gui_admit(pool,&b)==P0_GUI_OK && b);p0_gui_budget *denied=(void *)1;
        assert(p0_gui_admit(pool,&denied)==P0_GUI_LIMIT && !denied);
        p0_gui_usage full=p0_gui_pool_read(pool);assert(full.surfaces==2 && full.committed==12*page);
        assert(p0_gui_healthy(a) && p0_gui_healthy(b));
        assert(p0_gui_release(a)==P0_GUI_OK);a=NULL;
        assert(p0_gui_admit(pool,&a)==P0_GUI_OK && p0_gui_healthy(a));
    } else if(!strcmp(argv[1],"wire")) {
        assert(p0_gui_admit(pool,&b)==P0_GUI_OK && b);
        void *bytes=p0_gui_buffer_new(a,page+1);assert(bytes);memset(bytes,0x5a,page+1);
        assert(p0_gui_pool_read(pool).wire_live==2*page);
        assert(!p0_gui_buffer_new(a,1) && !p0_gui_healthy(a) && p0_gui_healthy(b));
        assert(p0_gui_release(a)==P0_GUI_BUSY);
        p0_gui_buffer_free(a,bytes,page+1);assert(!p0_gui_healthy(a));
        assert(p0_gui_release(a)==P0_GUI_OK);a=NULL;
        assert(p0_gui_admit(pool,&a)==P0_GUI_OK && p0_gui_healthy(a));
    } else if(!strcmp(argv[1],"lifetime")) {
        assert(p0_gui_pool_close(pool)==P0_GUI_BUSY);
        const GhosttyAllocator *allocator=p0_gui_general(a);assert(allocator);
        void *bytes=allocator->vtable->alloc(allocator->ctx,1,0,0);assert(bytes);*(uint8_t *)bytes=42;
        assert(!p0_gui_idle(a) && p0_gui_release(a)==P0_GUI_BUSY && *(uint8_t *)bytes==42);
        allocator->vtable->free(allocator->ctx,bytes,1,0,0);
    } else assert(!strcmp(argv[1],"valid"));
    assert(p0_gui_release(a)==P0_GUI_OK);
    if(b)assert(p0_gui_release(b)==P0_GUI_OK);
    p0_gui_usage end=p0_gui_pool_read(pool);assert(!end.surfaces && !end.live && !end.committed && !end.wire_live);
    assert(p0_gui_pool_close(pool)==P0_GUI_OK);return 0;
}
