#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN reserved_budget_prior_main
#include "resource-snapshot-probe.c"
#include "checkpoint-budget.h"
static void *reserve(p0_budget_scope *s,size_t n) {
 const GhosttyAllocator *a=p0_budget_allocator(s,P0_BUDGET_GENERAL);return a->vtable->alloc(a->ctx,n,0,0);
}
static void release(p0_budget_scope *s,void *p,size_t n) {
 const GhosttyAllocator *a=p0_budget_allocator(s,P0_BUDGET_GENERAL);a->vtable->free(a->ctx,p,n,0,0);
}
static void cleanup(p0_budget *b,p0_budget_scope *s) {
 REQUIRE(p0_budget_scope_read(s).live==0 && p0_budget_scope_close(s));
 p0_budget_usage all=p0_budget_read(b);REQUIRE(all.live==0 && all.committed==0 && all.mapped==all.unmapped && p0_budget_destroy(b));
}
static void on_demand(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,2*p));
 p0_budget_scope s;p0_budget_scope_init(&s,&b,2*p);void *data=reserve(&s,1);REQUIRE(data);
 p0_budget_usage u=p0_budget_read(&b);REQUIRE(u.live==p && u.committed==p && p0_budget_scope_fault(&s)==P0_BUDGET_HEALTHY);
 release(&s,data,1);cleanup(&b,&s);
}
static void headroom(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 p0_budget_scope owner,task;REQUIRE(p0_budget_scope_admit(&owner,&b,3*p));
 void *first=reserve(&owner,1);REQUIRE(first);REQUIRE(p0_budget_scope_admit(&task,&b,p));
 void *job=reserve(&task,1);REQUIRE(job && p0_budget_read(&b).committed==4*p && p0_budget_read(&b).live==2*p);
 void *growth=reserve(&owner,2*p);REQUIRE(growth && p0_budget_scope_fault(&owner)==P0_BUDGET_HEALTHY);
 REQUIRE(p0_budget_read(&b).live==4*p && p0_budget_read(&b).committed==4*p);
 release(&owner,growth,2*p);release(&owner,first,1);REQUIRE(p0_budget_read(&b).committed==4*p);
 release(&task,job,1);REQUIRE(p0_budget_scope_close(&task));REQUIRE(p0_budget_read(&b).committed==3*p);cleanup(&b,&owner);
}
static void admission(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 p0_budget_scope owner,rejected,small;REQUIRE(p0_budget_scope_admit(&owner,&b,3*p));
 REQUIRE(!p0_budget_scope_admit(&rejected,&b,2*p));REQUIRE(p0_budget_scope_fault(&rejected)==P0_BUDGET_GLOBAL_LIMIT);
 REQUIRE(p0_budget_scope_read(&rejected).committed==0 && p0_budget_scope_read(&rejected).live==0);
 REQUIRE(p0_budget_scope_close(&rejected));REQUIRE(p0_budget_scope_admit(&small,&b,p));
 REQUIRE(p0_budget_read(&b).live==0 && p0_budget_read(&b).committed==4*p);
 REQUIRE(p0_budget_scope_close(&small));void *all=reserve(&owner,3*p);REQUIRE(all);release(&owner,all,3*p);cleanup(&b,&owner);
}
static void mixed(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 p0_budget_scope owner,temporary,next;REQUIRE(p0_budget_scope_admit(&owner,&b,3*p));p0_budget_scope_init(&temporary,&b,SIZE_MAX);
 void *first=reserve(&temporary,1);REQUIRE(first && p0_budget_read(&b).committed==4*p);
 REQUIRE(!reserve(&temporary,1) && p0_budget_scope_fault(&temporary)==P0_BUDGET_GLOBAL_LIMIT);
 void *full=reserve(&owner,3*p);REQUIRE(full && p0_budget_read(&b).live==4*p);
 release(&owner,full,3*p);release(&temporary,first,1);REQUIRE(p0_budget_scope_close(&temporary));
 REQUIRE(!p0_budget_scope_admit(&next,&b,2*p));REQUIRE(p0_budget_scope_close(&next));cleanup(&b,&owner);
}
static void resizing(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,6*p));
 p0_budget_scope a,c;REQUIRE(p0_budget_scope_admit(&a,&b,3*p));REQUIRE(p0_budget_scope_admit(&c,&b,3*p));
 REQUIRE(!p0_budget_set_limit(&b,6*p-1));REQUIRE(!p0_budget_scope_set_limit(&a,4*p));
 REQUIRE(p0_budget_scope_read(&a).committed==3*p && p0_budget_scope_fault(&a)==P0_BUDGET_HEALTHY);
 REQUIRE(p0_budget_scope_set_limit(&c,2*p) && p0_budget_scope_set_limit(&a,4*p));REQUIRE(p0_budget_read(&b).committed==6*p);
 void *data=reserve(&a,4*p);REQUIRE(data);REQUIRE(!p0_budget_scope_set_limit(&a,4*p-1));
 release(&a,data,4*p);REQUIRE(p0_budget_scope_set_limit(&a,p));REQUIRE(p0_budget_read(&b).committed==3*p);
 REQUIRE(p0_budget_set_limit(&b,3*p));REQUIRE(p0_budget_scope_close(&c));cleanup(&b,&a);
}
static void close_grant(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,3*p));
 p0_budget_scope owner,next;REQUIRE(p0_budget_scope_admit(&owner,&b,3*p));void *data=reserve(&owner,1);REQUIRE(data);
 REQUIRE(!p0_budget_scope_close(&owner) && !p0_budget_destroy(&b));release(&owner,data,1);
 REQUIRE(p0_budget_read(&b).committed==3*p && !p0_budget_scope_admit(&next,&b,p));REQUIRE(p0_budget_scope_close(&next));
 REQUIRE(p0_budget_scope_close(&owner) && p0_budget_read(&b).committed==0);
 REQUIRE(p0_budget_scope_admit(&next,&b,3*p));cleanup(&b,&next);
}
static void local_limit(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 p0_budget_scope a,other;REQUIRE(p0_budget_scope_admit(&a,&b,p));REQUIRE(p0_budget_scope_admit(&other,&b,3*p));
 void *first=reserve(&a,1);REQUIRE(first && !reserve(&a,1));REQUIRE(p0_budget_scope_fault(&a)==P0_BUDGET_SCOPE_LIMIT);
 REQUIRE(p0_budget_scope_set_limit(&other,2*p) && p0_budget_scope_set_limit(&a,2*p));REQUIRE(!reserve(&a,1));
 void *healthy=reserve(&other,2*p);REQUIRE(healthy);release(&other,healthy,2*p);release(&a,first,1);REQUIRE(p0_budget_scope_close(&other));cleanup(&b,&a);
}
static void engine_growth(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,128*1024*1024));p0_budget_scope owner,task,denied;
 REQUIRE(p0_budget_scope_admit(&owner,&b,64*1024*1024));GhosttyTerminal t=NULL;
 REQUIRE(ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(&owner,P0_BUDGET_GENERAL),p0_budget_allocator(&owner,P0_BUDGET_PAGES),&t,80,24)==GHOSTTY_SUCCESS);
 const char *text="reserved growth\r\n中文";REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)text,strlen(text))==GHOSTTY_SUCCESS);
 size_t grant=p0_budget_scope_read(&owner).live+8*1024*1024,task_grant=1024*1024;
 REQUIRE(p0_budget_scope_set_limit(&owner,grant) && p0_budget_set_limit(&b,grant+task_grant));REQUIRE(p0_budget_scope_admit(&task,&b,task_grant));
 Bytes expected=capture(t);p0_checkpoint checkpoint={0};p0_checkpoint_error error={0};
 REQUIRE(p0_checkpoint_capture(&owner,&task,t,1024*1024,&checkpoint,&error)==P0_CHECKPOINT_OK);
 REQUIRE(checkpoint.canonical_len==expected.len && memcmp(checkpoint.canonical,expected.data,expected.len)==0);
 REQUIRE(p0_budget_read(&b).committed==grant+task_grant && p0_budget_read(&b).live<grant+task_grant);
 REQUIRE(!p0_budget_scope_admit(&denied,&b,b.page_size));REQUIRE(p0_budget_scope_close(&denied));
 REQUIRE(p0_budget_resize(&owner,t,120,40)==GHOSTTY_SUCCESS);
 REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)"still running",13)==GHOSTTY_SUCCESS);
 REQUIRE(p0_budget_scope_fault(&owner)==P0_BUDGET_HEALTHY && p0_budget_scope_read(&task).live>0);
 REQUIRE(checkpoint.canonical_len==expected.len && memcmp(checkpoint.canonical,expected.data,expected.len)==0);
 p0_checkpoint_release(&checkpoint);REQUIRE(p0_budget_scope_close(&task));free(expected.data);ghostty_terminal_free(t);cleanup(&b,&owner);
}
static void roles(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,128*1024*1024));p0_budget_scope owner,task,raw;
 REQUIRE(p0_budget_scope_admit(&owner,&b,64*1024*1024));REQUIRE(p0_budget_scope_admit(&task,&b,64*1024*1024));p0_budget_scope_init(&raw,&b,SIZE_MAX);
 GhosttyTerminal t=NULL;REQUIRE(ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(&owner,P0_BUDGET_GENERAL),p0_budget_allocator(&owner,P0_BUDGET_PAGES),&t,80,24)==GHOSTTY_SUCCESS);
 p0_checkpoint output={0};p0_checkpoint_error error={0};
 REQUIRE(p0_checkpoint_capture(&owner,&raw,t,1024*1024,&output,&error)==P0_CHECKPOINT_INVALID);
 REQUIRE(p0_checkpoint_capture(&raw,&task,t,1024*1024,&output,&error)==P0_CHECKPOINT_INVALID);
 REQUIRE(!output.canonical && !output.resources && p0_budget_scope_read(&task).attempts==0 && p0_budget_scope_read(&raw).attempts==0);
 REQUIRE(p0_budget_scope_close(&raw) && p0_budget_scope_close(&task));ghostty_terminal_free(t);cleanup(&b,&owner);
}
typedef struct {pthread_mutex_t mutex;pthread_cond_t cond;unsigned arrived;bool release;} AdmissionGate;
typedef struct {p0_budget *budget;p0_budget_scope scope;AdmissionGate *gate;size_t grant;bool admitted;} AdmissionWorker;
static void *admission_worker(void *opaque) {
 AdmissionWorker *w=opaque;w->admitted=p0_budget_scope_admit(&w->scope,w->budget,w->grant);void *data=NULL;
 REQUIRE(pthread_mutex_lock(&w->gate->mutex)==0);w->gate->arrived++;REQUIRE(pthread_cond_broadcast(&w->gate->cond)==0);
 while(!w->gate->release)REQUIRE(pthread_cond_wait(&w->gate->cond,&w->gate->mutex)==0);
 REQUIRE(pthread_mutex_unlock(&w->gate->mutex)==0);
 if(w->admitted) {data=reserve(&w->scope,w->grant);REQUIRE(data);release(&w->scope,data,w->grant);}
 else REQUIRE(p0_budget_scope_fault(&w->scope)==P0_BUDGET_GLOBAL_LIMIT);
 REQUIRE(p0_budget_scope_close(&w->scope));return NULL;
}
static void concurrent(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 AdmissionGate gate={.mutex=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};AdmissionWorker workers[8];pthread_t threads[8];
 for(unsigned i=0;i<8;i++) {workers[i]=(AdmissionWorker){.budget=&b,.gate=&gate,.grant=p};REQUIRE(pthread_create(&threads[i],NULL,admission_worker,&workers[i])==0);}
 REQUIRE(pthread_mutex_lock(&gate.mutex)==0);while(gate.arrived!=8)REQUIRE(pthread_cond_wait(&gate.cond,&gate.mutex)==0);
 unsigned accepted=0;for(unsigned i=0;i<8;i++)accepted+=workers[i].admitted;
 REQUIRE(accepted==4 && p0_budget_read(&b).committed==4*p && p0_budget_read(&b).live==0);
 gate.release=true;REQUIRE(pthread_cond_broadcast(&gate.cond)==0);REQUIRE(pthread_mutex_unlock(&gate.mutex)==0);
 for(unsigned i=0;i<8;i++)REQUIRE(pthread_join(threads[i],NULL)==0);
 p0_budget_usage all=p0_budget_read(&b);REQUIRE(all.live==0 && all.committed==0 && all.peak_committed==4*p && all.mapped==all.unmapped);
 REQUIRE(p0_budget_destroy(&b));REQUIRE(pthread_cond_destroy(&gate.cond)==0 && pthread_mutex_destroy(&gate.mutex)==0);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *mode=argv[1];
 if(strcmp(mode,"on-demand")==0)on_demand();else if(strcmp(mode,"headroom")==0)headroom();else if(strcmp(mode,"admission")==0)admission();else if(strcmp(mode,"mixed")==0)mixed();
 else if(strcmp(mode,"resize-grant")==0)resizing();else if(strcmp(mode,"close")==0)close_grant();else if(strcmp(mode,"local-limit")==0)local_limit();
 else if(strcmp(mode,"engine-growth")==0)engine_growth();else if(strcmp(mode,"roles")==0)roles();else {REQUIRE(strcmp(mode,"concurrent")==0);concurrent();}
 printf("PASS reserved budget/%s\n",mode);return 0;
}
