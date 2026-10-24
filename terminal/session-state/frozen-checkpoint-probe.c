#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN frozen_checkpoint_prior_main
#include "resource-snapshot-probe.c"
#include "checkpoint-budget.h"
static GhosttyTerminal checkpoint_source(p0_budget_scope *owner,const char *kind) {
 GhosttyTerminal t=NULL;REQUIRE(ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(owner,P0_BUDGET_GENERAL),p0_budget_allocator(owner,P0_BUDGET_PAGES),&t,80,24)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES,&continuation_limit)==GHOSTTY_SUCCESS);configure_images(t,".");
 feed(t,"checkpoint source\r\n中文");
 if(strstr(kind,"compressed")) {
  for(unsigned i=0;i<4000;i++)feed(t,"repeated and compressible terminal history\r\n");
  GhosttyTerminalCompressionResult result=GHOSTTY_TERMINAL_COMPRESSION_RESULT_UNSUPPORTED;
  REQUIRE(ghostty_terminal_compress(t,GHOSTTY_TERMINAL_COMPRESSION_MODE_FULL,&result)==GHOSTTY_SUCCESS);
  REQUIRE(result!=GHOSTTY_TERMINAL_COMPRESSION_RESULT_UNSUPPORTED);
  GhosttyTerminalMemoryUsage usage={.size=sizeof usage};REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_MEMORY_USAGE,&usage)==GHOSTTY_SUCCESS);
  REQUIRE(usage.primary_compressed_pages>0);
 }
 if(strstr(kind,"resources") || strcmp(kind,"pair-oom")==0) {
  if(strcmp(kind,"pair-oom")==0) {
   uint64_t token=0;REQUIRE(ghostty_terminal_p0_pending_begin(t,7,0,256,256,4,&token)==0);
   size_t n=256*256*4;uint8_t *pixels=malloc(n);REQUIRE(pixels);memset(pixels,127,n);bool applied=false;
   REQUIRE(ghostty_terminal_p0_pending_ready(t,7,token,pixels,n,&applied)==0 && applied);free(pixels);
  } else feed(t,red_command);
 }
 if(strstr(kind,"loading"))feed(t,"\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
 if(strstr(kind,"continuation") || strcmp(kind,"pair-oom")==0)feed(t,"\x1b[?2004");
 if(strcmp(kind,"utf8")==0)feed(t,"\xe4\xb8");
 require_no_semantic_error(t);return t;
}
#include <time.h>
static Bytes frozen_resources(GhosttyTerminal t) {
 Bytes b={0};REQUIRE(ghostty_terminal_p0_resources_encode(t,32*1024*1024,NULL,0,&b.len)==5);b.data=malloc(b.len);REQUIRE(b.data);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,32*1024*1024,b.data,b.len,&b.len)==0);return b;
}
static double monotonic_ms(void) {struct timespec t;REQUIRE(clock_gettime(CLOCK_MONOTONIC,&t)==0);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
typedef struct {
 pthread_mutex_t mutex;pthread_cond_t cond;bool entered,release;
 const GhosttyAllocator *base;GhosttyAllocator allocator;GhosttyP0FrozenCheckpoint frozen;
 uint8_t *canonical,*resources;size_t canonical_len,resources_len;GhosttyResult result;
} FrozenWorker;
static void *encoding_alloc(void *ctx,size_t n,uint8_t alignment,uintptr_t ret) {
 FrozenWorker *w=ctx;REQUIRE(pthread_mutex_lock(&w->mutex)==0);
 if(!w->entered) {w->entered=true;REQUIRE(pthread_cond_signal(&w->cond)==0);}
 while(!w->release)REQUIRE(pthread_cond_wait(&w->cond,&w->mutex)==0);
 REQUIRE(pthread_mutex_unlock(&w->mutex)==0);return w->base->vtable->alloc(w->base->ctx,n,alignment,ret);
}
static bool encoding_resize(void *ctx,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {(void)ctx;(void)p;(void)n;(void)a;(void)m;(void)r;return false;}
static void *encoding_remap(void *ctx,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {(void)ctx;(void)p;(void)n;(void)a;(void)m;(void)r;return NULL;}
static void encoding_free(void *ctx,void *p,size_t n,uint8_t alignment,uintptr_t ret) {FrozenWorker *w=ctx;w->base->vtable->free(w->base->ctx,p,n,alignment,ret);}
static void *encode_worker(void *ctx) {
 FrozenWorker *w=ctx;w->result=ghostty_snapshot_p0_frozen_encode(w->frozen,&w->allocator,32*1024*1024,&w->canonical,&w->canonical_len,&w->resources,&w->resources_len);return NULL;
}
static void equal_encoded(FrozenWorker *w,Bytes canonical,Bytes resources) {
 REQUIRE(w->result==GHOSTTY_SUCCESS && w->canonical_len==canonical.len && w->resources_len==resources.len);
 REQUIRE(memcmp(w->canonical,canonical.data,canonical.len)==0 && memcmp(w->resources,resources.data,resources.len)==0);
}
static void frozen_case(const char *kind) {
 p0_budget budget;REQUIRE(p0_budget_init(&budget,256*1024*1024));p0_budget_scope owner,capture_scope,encode_scope;
 REQUIRE(p0_budget_scope_admit(&owner,&budget,64*1024*1024) && p0_budget_scope_admit(&capture_scope,&budget,64*1024*1024) && p0_budget_scope_admit(&encode_scope,&budget,64*1024*1024));
 GhosttyTerminal t=checkpoint_source(&owner,kind);
 if(strcmp(kind,"dual")==0)feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\\x1b[?47h\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;AAD//w==\x1b\\");
 if(strcmp(kind,"rich")==0)feed(t,"\x1b]8;;https://ghosterm.invalid/frozen\x07\x1b[31me\xcc\x81 👩‍💻\x1b" "7\x1b]8;;\x07\x1b[0m\x1b]2;冻结标题\x07\x1b]7;file:///frozen/cwd\x07");
 if(strcmp(kind,"bulk")==0) {
  uint64_t limit=32*1024*1024,token=0;REQUIRE(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_p0_pending_begin(t,7,0,2048,2048,4,&token)==0);size_t n=2048*2048*4;uint8_t *pixels=malloc(n);REQUIRE(pixels);memset(pixels,173,n);bool applied=false;
  REQUIRE(ghostty_terminal_p0_pending_ready(t,7,token,pixels,n,&applied)==0 && applied);free(pixels);
 }
 Bytes canonical=capture(t),resources=frozen_resources(t);
 p0_budget_usage before=p0_budget_scope_read(&owner);REQUIRE(p0_budget_scope_set_limit(&owner,before.live));
 GhosttyP0FrozenCheckpoint frozen=NULL;double start=monotonic_ms();
 REQUIRE(ghostty_snapshot_p0_freeze(t,p0_budget_allocator(&capture_scope,P0_BUDGET_SCRATCH),32*1024*1024,&frozen)==GHOSTTY_SUCCESS && frozen);
 double elapsed=monotonic_ms()-start;p0_budget_usage after=p0_budget_scope_read(&owner);
 REQUIRE(before.live==after.live && before.attempts==after.attempts && before.mapped==after.mapped && before.unmapped==after.unmapped && p0_budget_scope_fault(&owner)==P0_BUDGET_HEALTHY);
 if(strcmp(kind,"cancel")==0) {
  ghostty_snapshot_p0_frozen_free(frozen);REQUIRE(p0_budget_scope_read(&capture_scope).live==0);
  REQUIRE(p0_budget_scope_set_limit(&owner,64*1024*1024));REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)"W",1)==GHOSTTY_SUCCESS);
  ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner) && p0_budget_scope_close(&capture_scope) && p0_budget_scope_close(&encode_scope));
  p0_budget_usage all=p0_budget_read(&budget);REQUIRE(all.live==0 && all.committed==0 && all.mapped==all.unmapped && p0_budget_destroy(&budget));free(canonical.data);free(resources.data);return;
 }
 FrozenWorker worker={.mutex=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER,.base=p0_budget_allocator(&encode_scope,P0_BUDGET_SCRATCH),.frozen=frozen};
 static const GhosttyAllocatorVtable callbacks={encoding_alloc,encoding_resize,encoding_remap,encoding_free};worker.allocator=(GhosttyAllocator){.ctx=&worker,.vtable=&callbacks};pthread_t thread;
 REQUIRE(pthread_create(&thread,NULL,encode_worker,&worker)==0);
 REQUIRE(pthread_mutex_lock(&worker.mutex)==0);while(!worker.entered)REQUIRE(pthread_cond_wait(&worker.cond,&worker.mutex)==0);REQUIRE(pthread_mutex_unlock(&worker.mutex)==0);
 /* Encoding has entered its real allocator callback. It owns no source lock. */
 REQUIRE(p0_budget_scope_set_limit(&owner,64*1024*1024));const char *tail=strstr(kind,"continuation")?"h":strcmp(kind,"utf8")==0?"\xad":strstr(kind,"loading")?"\x1b_Gm=0;/w==\x1b\\":"";
 REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)tail,strlen(tail))==GHOSTTY_SUCCESS);
 const char *changed="\x1b[?47hchanged after S\x1b[?47l\x1b" "c";REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)changed,strlen(changed))==GHOSTTY_SUCCESS);
 ghostty_terminal_free(t);REQUIRE(p0_budget_scope_read(&owner).live==0 && p0_budget_scope_close(&owner));
 REQUIRE(pthread_mutex_lock(&worker.mutex)==0);worker.release=true;REQUIRE(pthread_cond_broadcast(&worker.cond)==0);REQUIRE(pthread_mutex_unlock(&worker.mutex)==0);
 REQUIRE(pthread_join(thread,NULL)==0);equal_encoded(&worker,canonical,resources);
 ghostty_free(&worker.allocator,worker.canonical,worker.canonical_len);ghostty_free(&worker.allocator,worker.resources,worker.resources_len);
 REQUIRE(pthread_cond_destroy(&worker.cond)==0 && pthread_mutex_destroy(&worker.mutex)==0);
 ghostty_snapshot_p0_frozen_free(frozen);REQUIRE(p0_budget_scope_read(&capture_scope).live==0 && p0_budget_scope_read(&encode_scope).live==0);
 size_t capture_peak=p0_budget_scope_read(&capture_scope).peak,encode_peak=p0_budget_scope_read(&encode_scope).peak;REQUIRE(p0_budget_scope_close(&capture_scope) && p0_budget_scope_close(&encode_scope));p0_budget_usage all=p0_budget_read(&budget);REQUIRE(all.live==0 && all.committed==0 && all.mapped==all.unmapped && p0_budget_destroy(&budget));
 free(canonical.data);free(resources.data);printf("MEASURE frozen capture/%s owner_ms=%.3f capture_peak=%zu encode_peak=%zu\n",kind,elapsed,capture_peak,encode_peak);
}
static void capture_limit(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,128*1024*1024));p0_budget_scope owner,task;REQUIRE(p0_budget_scope_admit(&owner,&b,64*1024*1024) && p0_budget_scope_admit(&task,&b,64*1024*1024));
 GhosttyTerminal t=checkpoint_source(&owner,"ground");GhosttyP0FrozenCheckpoint f=NULL;
 REQUIRE(ghostty_snapshot_p0_freeze(t,p0_budget_allocator(&task,P0_BUDGET_SCRATCH),0,&f)==GHOSTTY_LIMIT_EXCEEDED && !f);
 REQUIRE(p0_budget_scope_read(&task).live==0 && p0_budget_result(&owner,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS);
 ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner) && p0_budget_scope_close(&task) && p0_budget_destroy(&b));
}
static void oom_case(bool capture_oom,bool compressed) {
 p0_budget b;REQUIRE(p0_budget_init(&b,256*1024*1024));p0_budget_scope owner,captured;
 REQUIRE(p0_budget_scope_admit(&owner,&b,64*1024*1024));GhosttyTerminal t=checkpoint_source(&owner,compressed?"compressed":"ground");
 Bytes canonical=capture(t),resources=frozen_resources(t);p0_budget_usage before=p0_budget_scope_read(&owner);REQUIRE(p0_budget_scope_set_limit(&owner,before.live));
 GhosttyP0FrozenCheckpoint frozen=NULL;
 if(!capture_oom) {
  REQUIRE(p0_budget_scope_admit(&captured,&b,64*1024*1024));REQUIRE(ghostty_snapshot_p0_freeze(t,p0_budget_allocator(&captured,P0_BUDGET_SCRATCH),32*1024*1024,&frozen)==GHOSTTY_SUCCESS);
  ghostty_terminal_free(t);t=NULL;REQUIRE(p0_budget_scope_close(&owner));
 }
 bool completed=false;size_t failures=0;
 for(size_t limit=0;limit<=8*1024*1024;limit+=b.page_size) {
  p0_budget_scope task;REQUIRE(p0_budget_scope_admit(&task,&b,limit));const GhosttyAllocator *allocator=p0_budget_allocator(&task,P0_BUDGET_SCRATCH);
  GhosttyResult rc;uint8_t *c=NULL,*r=NULL;size_t cn=0,rn=0;
  if(capture_oom)rc=ghostty_snapshot_p0_freeze(t,allocator,32*1024*1024,&frozen);
  else rc=ghostty_snapshot_p0_frozen_encode(frozen,allocator,32*1024*1024,&c,&cn,&r,&rn);
  if(rc==GHOSTTY_SUCCESS) {
   completed=true;if(capture_oom)ghostty_snapshot_p0_frozen_free(frozen);
   else {REQUIRE(cn==canonical.len && rn==resources.len && memcmp(c,canonical.data,cn)==0 && memcmp(r,resources.data,rn)==0);ghostty_free(allocator,c,cn);ghostty_free(allocator,r,rn);}
  } else {
   REQUIRE(rc==GHOSTTY_OUT_OF_MEMORY && p0_budget_result(&task,rc)==GHOSTTY_LIMIT_EXCEEDED);failures++;
   if(capture_oom)REQUIRE(!frozen);else REQUIRE(!c && !r && cn==0 && rn==0);
  }
  if(capture_oom) {p0_budget_usage after=p0_budget_scope_read(&owner);REQUIRE(after.live==before.live && after.attempts==before.attempts && p0_budget_scope_fault(&owner)==P0_BUDGET_HEALTHY);}
  p0_budget_usage u=p0_budget_scope_read(&task);REQUIRE(u.live==0 && u.mapped==u.unmapped && u.peak<=limit && p0_budget_scope_close(&task));if(completed)break;
 }
 REQUIRE(completed && failures>0);
 if(capture_oom) {ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner));}
 else {ghostty_snapshot_p0_frozen_free(frozen);REQUIRE(p0_budget_scope_close(&captured));}
 p0_budget_usage all=p0_budget_read(&b);REQUIRE(all.live==0 && all.committed==0 && all.mapped==all.unmapped && p0_budget_destroy(&b));free(canonical.data);free(resources.data);
 printf("PASS frozen quota sweep/%zu limits\n",failures);
}
static void wire_limit(void) {
 p0_budget b;REQUIRE(p0_budget_init(&b,192*1024*1024));p0_budget_scope owner,cap,enc;
 REQUIRE(p0_budget_scope_admit(&owner,&b,64*1024*1024) && p0_budget_scope_admit(&cap,&b,64*1024*1024) && p0_budget_scope_admit(&enc,&b,64*1024*1024));
 GhosttyTerminal t=checkpoint_source(&owner,"resources");Bytes canonical=capture(t),resources=frozen_resources(t);GhosttyP0FrozenCheckpoint f=NULL;
 REQUIRE(ghostty_snapshot_p0_freeze(t,p0_budget_allocator(&cap,P0_BUDGET_SCRATCH),32*1024*1024,&f)==GHOSTTY_SUCCESS);
 ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner));const GhosttyAllocator *allocator=p0_budget_allocator(&enc,P0_BUDGET_SCRATCH);
 uint8_t *c=NULL,*r=NULL;size_t cn=0,rn=0;
 REQUIRE(ghostty_snapshot_p0_frozen_encode(f,allocator,canonical.len+resources.len-1,&c,&cn,&r,&rn)==GHOSTTY_LIMIT_EXCEEDED);
 REQUIRE(!c && !r && cn==0 && rn==0 && p0_budget_scope_read(&enc).live==0 && p0_budget_result(&enc,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_p0_frozen_encode(f,allocator,canonical.len+resources.len,&c,&cn,&r,&rn)==GHOSTTY_SUCCESS);
 REQUIRE(cn==canonical.len && rn==resources.len && memcmp(c,canonical.data,cn)==0 && memcmp(r,resources.data,rn)==0);
 ghostty_free(allocator,c,cn);ghostty_free(allocator,r,rn);ghostty_snapshot_p0_frozen_free(f);
 REQUIRE(p0_budget_scope_close(&cap) && p0_budget_scope_close(&enc));p0_budget_usage all=p0_budget_read(&b);REQUIRE(all.live==0 && all.committed==0 && all.mapped==all.unmapped && p0_budget_destroy(&b));free(canonical.data);free(resources.data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);if(strcmp(argv[1],"wire-limit")==0)wire_limit();else if(strcmp(argv[1],"capture-limit")==0)capture_limit();else if(strstr(argv[1],"-oom") && strcmp(argv[1],"pair-oom")!=0)oom_case(strstr(argv[1],"freeze")!=NULL,strstr(argv[1],"compressed")!=NULL);else frozen_case(argv[1]);
 printf("PASS frozen checkpoint/%s\n",argv[1]);return 0;
}
