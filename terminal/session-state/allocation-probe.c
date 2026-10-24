#define main vt_baseline_main
#include "probe.c"
#undef main
#include <limits.h>

typedef struct {
  size_t live, peak, limit, denied;
  bool fail;
  /* The C allocator object itself must outlive the returned terminal. */
  GhosttyAllocator allocator;
} AllocationBudget;
static void *budget_alloc(void *ctx, size_t len, uint8_t alignment, uintptr_t ret) {
  (void)ret;
  AllocationBudget *b=ctx;
  if (b->fail || b->live > b->limit || len > b->limit-b->live || alignment >= sizeof(size_t)*CHAR_BIT) {
    b->denied++; return NULL;
  }
  size_t align=(size_t)1<<alignment;
  if (align<sizeof(void *)) align=sizeof(void *);
  void *ptr=NULL;
  if (posix_memalign(&ptr,align,len)!=0) return NULL;
  b->live+=len;
  if (b->live>b->peak) b->peak=b->live;
  return ptr;
}
static bool budget_resize(void *ctx, void *ptr, size_t len, uint8_t alignment, size_t new_len, uintptr_t ret) {
  (void)ctx;(void)ptr;(void)alignment;(void)ret;
  return len==new_len;
}
static void *budget_remap(void *ctx, void *ptr, size_t len, uint8_t alignment, size_t new_len, uintptr_t ret) {
  (void)ctx;(void)ptr;(void)len;(void)alignment;(void)new_len;(void)ret;
  return NULL;
}
static void budget_free(void *ctx, void *ptr, size_t len, uint8_t alignment, uintptr_t ret) {
  (void)alignment;(void)ret;
  AllocationBudget *b=ctx;
  REQUIRE(b->live>=len);b->live-=len;free(ptr);
}
static const GhosttyAllocatorVtable budget_vtable={budget_alloc,budget_resize,budget_remap,budget_free};
static bool processing_failed(GhosttyTerminal t) {
  bool failed=false;
  REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed)==GHOSTTY_SUCCESS);
  return failed;
}
static void test_failure(void) {
  AllocationBudget budget={.limit=16*1024*1024};
  GhosttyAllocator alloc={.ctx=&budget,.vtable=&budget_vtable};
  GhosttyTerminal t=NULL;
  REQUIRE(ghostty_terminal_new(&alloc,&t,80,24)==GHOSTTY_SUCCESS);
  REQUIRE(!processing_failed(t));
  budget.fail=true;
  feed(t,"\x1b]2;unavailable\x1b\\");
  REQUIRE(budget.denied>0);
  REQUIRE(processing_failed(t));
  budget.fail=false;
  ghostty_terminal_reset(t);
  REQUIRE(processing_failed(t));
  ghostty_terminal_free(t);
  REQUIRE(budget.live==0);
  puts("PASS allocation/feed semantic failure visible, sticky after reset, allocations released");
}
static GhosttyResult decode_budget(Bytes bytes,AllocationBudget *budget,GhosttyTerminal *t) {
  GhosttyAllocator *alloc=&budget->allocator;
  *alloc=(GhosttyAllocator){.ctx=budget,.vtable=&budget_vtable};
  GhosttySnapshotDecoder d=NULL;
  GhosttyResult result=ghostty_snapshot_decoder_new_buf(alloc,&d,bytes.data,bytes.len);
  if(result!=GHOSTTY_SUCCESS) return result;
  result=ghostty_snapshot_decoder_decode(d,t);
  ghostty_snapshot_decoder_free(d);
  return result;
}
static void test_budget(void) {
  GhosttyTerminal source=new_terminal();
  for(int row=0;row<1000;row++) feed(source,"history 中\r\n");
  Bytes bytes=capture(source);
  AllocationBudget normal={.limit=16*1024*1024};
  GhosttyTerminal t=NULL;
  REQUIRE(decode_budget(bytes,&normal,&t)==GHOSTTY_SUCCESS);
  REQUIRE(t!=NULL);
  size_t peak=normal.peak;
  require_equal_state(source,t);
  ghostty_terminal_free(t);
  REQUIRE(normal.live==0);
  AllocationBudget small={.limit=peak-1};
  t=NULL;
  GhosttyResult result=decode_budget(bytes,&small,&t);
  REQUIRE(result==GHOSTTY_OUT_OF_MEMORY);
  REQUIRE(t==NULL);
  REQUIRE(small.denied>0 && small.peak>0 && small.peak<=small.limit);
  REQUIRE(small.live==0);
  free(bytes.data);ghostty_terminal_free(source);
  printf("PASS allocation/decoder rejects budget exhaustion and releases allocations (normal peak=%zu, cap=%zu)\n",peak,small.limit);
}
int main(int argc,char **argv) {
  REQUIRE(argc==2);
  if(strcmp(argv[1],"failure")==0)test_failure();
  else if(strcmp(argv[1],"budget")==0)test_budget();
  else return 2;
  return 0;
}
