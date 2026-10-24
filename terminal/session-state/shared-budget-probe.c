#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN shared_budget_prior_main
#include "resource-snapshot-probe.c"
#include "budget-posix.h"
static void *reserve(p0_budget_scope *s,p0_budget_kind kind,size_t n,uint8_t alignment) {
 const GhosttyAllocator *a=p0_budget_allocator(s,kind);REQUIRE(a);return a->vtable->alloc(a->ctx,n,alignment,0);
}
static void release(p0_budget_scope *s,p0_budget_kind kind,void *p,size_t n,uint8_t alignment) {
 const GhosttyAllocator *a=p0_budget_allocator(s,kind);a->vtable->free(a->ctx,p,n,alignment,0);
}
static void finish(p0_budget *b,p0_budget_scope *s) {
 p0_budget_usage u=p0_budget_scope_read(s);REQUIRE(u.live==0 && u.mapped==u.unmapped);
 REQUIRE(p0_budget_scope_close(s));u=p0_budget_read(b);REQUIRE(u.live==0 && u.mapped==u.unmapped);REQUIRE(p0_budget_destroy(b));
}
static void categories(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,2*p));
 p0_budget_scope s;p0_budget_scope_init(&s,&b,3*p);void *general=reserve(&s,P0_BUDGET_GENERAL,1,0),*page=reserve(&s,P0_BUDGET_PAGES,1,0);
 REQUIRE(general && page && *(uint8_t*)general==0 && *(uint8_t*)page==0);
 p0_budget_usage u=p0_budget_scope_read(&s);REQUIRE(u.live==2*p && u.by_kind[P0_BUDGET_GENERAL]==p && u.by_kind[P0_BUDGET_PAGES]==p);
 REQUIRE(!p0_budget_set_limit(&b,2*p-1) && !p0_budget_scope_set_limit(&s,2*p-1));
 REQUIRE(!reserve(&s,P0_BUDGET_SCRATCH,1,0));REQUIRE(p0_budget_scope_fault(&s)==P0_BUDGET_GLOBAL_LIMIT);
 REQUIRE(p0_budget_result(&s,GHOSTTY_SUCCESS)==GHOSTTY_LIMIT_EXCEEDED);
 REQUIRE(p0_budget_set_limit(&b,4*p) && !reserve(&s,P0_BUDGET_GENERAL,1,0));
 REQUIRE(!p0_budget_scope_close(&s) && !p0_budget_destroy(&b));
 release(&s,P0_BUDGET_GENERAL,general,1,0);release(&s,P0_BUDGET_PAGES,page,1,0);
 REQUIRE(p0_budget_read(&b).peak==2*p);finish(&b,&s);
}
static void isolation(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 p0_budget_scope a,other,scratch;p0_budget_scope_init(&a,&b,p);p0_budget_scope_init(&other,&b,4*p);p0_budget_scope_init(&scratch,&b,p);
 void *first=reserve(&a,P0_BUDGET_PAGES,1,0);REQUIRE(first && !reserve(&a,P0_BUDGET_GENERAL,1,0));
 REQUIRE(p0_budget_scope_fault(&a)==P0_BUDGET_SCOPE_LIMIT);void *second=reserve(&other,P0_BUDGET_GENERAL,1,0);REQUIRE(second);
 REQUIRE(!reserve(&scratch,P0_BUDGET_SCRATCH,p+1,0));REQUIRE(p0_budget_result(&other,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS);
 void *third=reserve(&other,P0_BUDGET_PAGES,2*p,0);REQUIRE(third && p0_budget_read(&b).live==4*p);
 release(&other,P0_BUDGET_GENERAL,second,1,0);release(&other,P0_BUDGET_PAGES,third,2*p,0);release(&a,P0_BUDGET_PAGES,first,1,0);
 REQUIRE(p0_budget_scope_close(&a) && p0_budget_scope_close(&scratch));finish(&b,&other);
}
static void alignment(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;p0_budget_scope s;p0_budget_scope_init(&s,&b,SIZE_MAX);
 size_t align=4*p;uint8_t exponent=0;while(((size_t)1<<exponent)<align)exponent++;
 void *ptr=reserve(&s,P0_BUDGET_GENERAL,1,exponent);REQUIRE(ptr && (uintptr_t)ptr%align==0 && *(uint8_t*)ptr==0);
 p0_budget_usage u=p0_budget_scope_read(&s);REQUIRE(u.live==p && u.peak==4*p && u.mapped==4*p && u.unmapped==3*p);
 const GhosttyAllocator *a=p0_budget_allocator(&s,P0_BUDGET_GENERAL);
 REQUIRE(!a->vtable->resize(a->ctx,ptr,1,exponent,2,0));REQUIRE(!a->vtable->remap(a->ctx,ptr,1,exponent,2,0));
 REQUIRE(p0_budget_scope_fault(&s)==P0_BUDGET_HEALTHY);release(&s,P0_BUDGET_GENERAL,ptr,1,exponent);finish(&b,&s);
}
static void overflow(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));p0_budget_scope s;p0_budget_scope_init(&s,&b,SIZE_MAX);
 REQUIRE(!reserve(&s,P0_BUDGET_GENERAL,SIZE_MAX,0));REQUIRE(p0_budget_scope_fault(&s)==P0_BUDGET_OVERFLOW && p0_budget_read(&b).live==0);
 REQUIRE(!reserve(&s,P0_BUDGET_GENERAL,1,UINT8_MAX));finish(&b,&s);
}
static GhosttyTerminal session_new(p0_budget_scope *s) {
 GhosttyTerminal t=NULL;GhosttyResult rc=ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(s,P0_BUDGET_GENERAL),p0_budget_allocator(s,P0_BUDGET_PAGES),&t,80,24);
 REQUIRE(p0_budget_result(s,rc)==GHOSTTY_SUCCESS && t);return t;
}
static void engine(bool restore_state) {
 p0_budget b;REQUIRE(p0_budget_init(&b,128*1024*1024));p0_budget_scope s,copy_scope;
 p0_budget_scope_init(&s,&b,64*1024*1024);GhosttyTerminal t=session_new(&s);configure_images(t,".");
 const char *output="primary\r\n中文\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\\x1b[?47halternate\x1b[?47l";
 REQUIRE(p0_budget_feed(&s,t,(const uint8_t*)output,strlen(output))==GHOSTTY_SUCCESS);require_red_pixel(t);
 p0_budget_usage u=p0_budget_scope_read(&s);REQUIRE(u.by_kind[P0_BUDGET_GENERAL]>0 && u.by_kind[P0_BUDGET_PAGES]>0);
 if(restore_state) {
  Bytes canonical=capture(t),resources=resource_capture(t);p0_budget_scope_init(&copy_scope,&b,64*1024*1024);
  GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(p0_budget_allocator(&copy_scope,P0_BUDGET_GENERAL),&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_p0_page_allocator(decoder,p0_budget_allocator(&copy_scope,P0_BUDGET_PAGES))==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS && copy);ghostty_snapshot_decoder_free(decoder);
  REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,1024*1024)==0);require_red_pixel(copy);require_equal_state(t,copy);
  p0_budget_usage c=p0_budget_scope_read(&copy_scope);REQUIRE(c.by_kind[P0_BUDGET_GENERAL]>0 && c.by_kind[P0_BUDGET_PAGES]>0);
  REQUIRE(p0_budget_read(&b).live==p0_budget_scope_read(&s).live+c.live);ghostty_terminal_free(copy);REQUIRE(p0_budget_scope_close(&copy_scope));free(canonical.data);free(resources.data);
 }
 ghostty_terminal_free(t);finish(&b,&s);
}
static void engine_failure(const char *mode) {
 p0_budget b;REQUIRE(p0_budget_init(&b,64*1024*1024));p0_budget_scope s;p0_budget_scope_init(&s,&b,64*1024*1024);
 GhosttyTerminal t=session_new(&s);configure_images(t,".");bool pixel=strcmp(mode,"pixel-failure")==0,resize=strcmp(mode,"resize-failure")==0;
 uint64_t token=0;if(pixel)REQUIRE(ghostty_terminal_p0_pending_begin(t,7,0,1,1,4,&token)==0);
 p0_budget_usage before=p0_budget_scope_read(&s);REQUIRE(p0_budget_scope_set_limit(&s,before.live));
 if(pixel) {
  const uint8_t red[4]={255,0,0,255};bool applied=true;
  REQUIRE(ghostty_terminal_p0_pending_ready(t,7,token,red,4,&applied)==2 && !applied);
  REQUIRE(p0_budget_result(&s,GHOSTTY_SUCCESS)==GHOSTTY_LIMIT_EXCEEDED);
 } else if(resize) REQUIRE(p0_budget_resize(&s,t,30000,30000)==GHOSTTY_LIMIT_EXCEEDED);
 else REQUIRE(p0_budget_feed(&s,t,(const uint8_t*)"\x1b[?47h",6)==GHOSTTY_LIMIT_EXCEEDED);
 REQUIRE(p0_budget_scope_fault(&s)==P0_BUDGET_SCOPE_LIMIT && p0_budget_scope_read(&s).live<=before.live);
 size_t attempts=p0_budget_scope_read(&s).attempts;uint16_t x=0,y=0;
 REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_CURSOR_X,&x)==GHOSTTY_SUCCESS);
 REQUIRE(p0_budget_feed(&s,t,(const uint8_t*)"W",1)==GHOSTTY_LIMIT_EXCEEDED);
 REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_CURSOR_X,&y)==GHOSTTY_SUCCESS && x==y);
 REQUIRE(p0_budget_resize(&s,t,90,30)==GHOSTTY_LIMIT_EXCEEDED && p0_budget_scope_read(&s).attempts==attempts);
 ghostty_terminal_free(t);finish(&b,&s);
}
static void admission(bool scratch) {
 p0_budget b;REQUIRE(p0_budget_init(&b,64*1024*1024));p0_budget_scope owner,request;
 p0_budget_scope_init(&owner,&b,64*1024*1024);GhosttyTerminal t=session_new(&owner);
 p0_budget_scope_init(&request,&b,scratch?0:64*1024*1024);
 if(scratch)REQUIRE(!reserve(&request,P0_BUDGET_SCRATCH,1,0));
 else {
  REQUIRE(p0_budget_set_limit(&b,p0_budget_read(&b).live));GhosttyTerminal rejected=NULL;
  GhosttyResult rc=ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(&request,P0_BUDGET_GENERAL),p0_budget_allocator(&request,P0_BUDGET_PAGES),&rejected,80,24);
  REQUIRE(p0_budget_result(&request,rc)==GHOSTTY_LIMIT_EXCEEDED && !rejected);
 }
 REQUIRE(p0_budget_scope_read(&request).live==0 && p0_budget_result(&request,GHOSTTY_SUCCESS)==GHOSTTY_LIMIT_EXCEEDED);
 REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)"W",1)==GHOSTTY_SUCCESS);REQUIRE(p0_budget_scope_close(&request));
 ghostty_terminal_free(t);finish(&b,&owner);
}
typedef struct {pthread_mutex_t mutex;pthread_cond_t cond;unsigned arrived;bool release;} BudgetGate;
typedef struct {p0_budget_scope *scope;BudgetGate *gate;bool accepted;} BudgetWorker;
static void *budget_worker(void *opaque) {
 BudgetWorker *w=opaque;void *p=reserve(w->scope,P0_BUDGET_GENERAL,1,0);w->accepted=p!=NULL;
 REQUIRE(pthread_mutex_lock(&w->gate->mutex)==0);w->gate->arrived++;REQUIRE(pthread_cond_broadcast(&w->gate->cond)==0);
 while(!w->gate->release)REQUIRE(pthread_cond_wait(&w->gate->cond,&w->gate->mutex)==0);
 REQUIRE(pthread_mutex_unlock(&w->gate->mutex)==0);
 if(p)release(w->scope,P0_BUDGET_GENERAL,p,1,0);
 else {REQUIRE(!reserve(w->scope,P0_BUDGET_GENERAL,1,0));REQUIRE(p0_budget_result(w->scope,GHOSTTY_SUCCESS)==GHOSTTY_LIMIT_EXCEEDED);}
 return NULL;
}
static void concurrent(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,SIZE_MAX));size_t p=b.page_size;REQUIRE(p0_budget_set_limit(&b,4*p));
 BudgetGate gate={.mutex=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};p0_budget_scope scopes[8];BudgetWorker workers[8];pthread_t threads[8];
 for(unsigned i=0;i<8;i++) {p0_budget_scope_init(&scopes[i],&b,SIZE_MAX);workers[i]=(BudgetWorker){.scope=&scopes[i],.gate=&gate};REQUIRE(pthread_create(&threads[i],NULL,budget_worker,&workers[i])==0);}
 REQUIRE(pthread_mutex_lock(&gate.mutex)==0);while(gate.arrived!=8)REQUIRE(pthread_cond_wait(&gate.cond,&gate.mutex)==0);
 p0_budget_usage u=p0_budget_read(&b);REQUIRE(u.live==4*p && u.peak==4*p && u.denied==4);REQUIRE(p0_budget_set_limit(&b,8*p));
 gate.release=true;REQUIRE(pthread_cond_broadcast(&gate.cond)==0);REQUIRE(pthread_mutex_unlock(&gate.mutex)==0);
 unsigned accepted=0;for(unsigned i=0;i<8;i++) {REQUIRE(pthread_join(threads[i],NULL)==0);accepted+=workers[i].accepted;REQUIRE(p0_budget_scope_close(&scopes[i]));}
 REQUIRE(accepted==4);u=p0_budget_read(&b);REQUIRE(u.live==0 && u.mapped==u.unmapped && u.peak==4*p);REQUIRE(p0_budget_destroy(&b));
 REQUIRE(pthread_cond_destroy(&gate.cond)==0 && pthread_mutex_destroy(&gate.mutex)==0);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *mode=argv[1];
 if(strcmp(mode,"categories")==0)categories();else if(strcmp(mode,"isolation")==0)isolation();else if(strcmp(mode,"alignment")==0)alignment();
 else if(strcmp(mode,"overflow")==0)overflow();else if(strcmp(mode,"engine")==0)engine(false);else if(strcmp(mode,"restore")==0)engine(true);
 else if(strcmp(mode,"concurrent")==0)concurrent();else if(strcmp(mode,"admission")==0)admission(false);else if(strcmp(mode,"scratch-failure")==0)admission(true);else {REQUIRE(strcmp(mode,"feed-failure")==0||strcmp(mode,"pixel-failure")==0||strcmp(mode,"resize-failure")==0);engine_failure(mode);}
 printf("PASS shared budget/%s\n",mode);return 0;
}
