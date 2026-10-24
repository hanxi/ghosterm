#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN page_provider_prior_main
#include "resource-snapshot-probe.c"
#include <sys/mman.h>
typedef struct {size_t limit,live,peak,calls,denied,fail_at,mapped,unmapped;} PageBudget;
static size_t page_round(size_t n) {size_t page=(size_t)sysconf(_SC_PAGESIZE);REQUIRE(page && n<=SIZE_MAX-page+1);return (n+page-1)/page*page;}
static void *page_alloc(void *ctx,size_t n,uint8_t alignment,uintptr_t ret) {
 (void)ret;PageBudget *b=ctx;size_t mapped=page_round(n);b->calls++;
 REQUIRE(alignment<sizeof(size_t)*8 && ((size_t)1<<alignment)<=(size_t)sysconf(_SC_PAGESIZE));
 if(b->calls==b->fail_at || mapped>b->limit-b->live) {b->denied++;return NULL;}
 void *p=mmap(NULL,mapped,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);REQUIRE(p!=MAP_FAILED);
 b->live+=mapped;b->mapped+=mapped;if(b->live>b->peak)b->peak=b->live;return p;
}
static void page_free(void *ctx,void *p,size_t n,uint8_t alignment,uintptr_t ret) {
 (void)alignment;(void)ret;PageBudget *b=ctx;size_t mapped=page_round(n);REQUIRE(b->live>=mapped && munmap(p,mapped)==0);b->live-=mapped;b->unmapped+=mapped;
}
static GhosttyAllocator page_allocator(PageBudget *b) {
 static const GhosttyAllocatorVtable table={page_alloc,png_resize,png_remap,page_free};
 return (GhosttyAllocator){.ctx=b,.vtable=&table};
}
static GhosttyTerminal page_new(GhosttyAllocator *a) {
 GhosttyTerminal t=NULL;REQUIRE(ghostty_terminal_p0_new_with_page_allocator(NULL,a,&t,80,24)==GHOSTTY_SUCCESS && t);return t;
}
static void page_create(bool denied) {
 PageBudget b={.limit=denied?0:SIZE_MAX};GhosttyAllocator a=page_allocator(&b);GhosttyTerminal t=NULL;
 GhosttyResult rc=ghostty_terminal_p0_new_with_page_allocator(NULL,&a,&t,80,24);
 if(denied)REQUIRE(rc==GHOSTTY_OUT_OF_MEMORY && !t && b.denied==1 && b.live==0);
 else {REQUIRE(rc==GHOSTTY_SUCCESS && t && b.calls>0 && b.live>0);feed(t,"page budget\r\n中文");require_no_semantic_error(t);ghostty_terminal_free(t);}
 REQUIRE(b.live==0 && b.mapped==b.unmapped);printf("PASS page provider create/%zu peak bytes\n",b.peak);
}
static void page_quota(void) {
 PageBudget measured={.limit=SIZE_MAX};GhosttyAllocator provider=page_allocator(&measured);GhosttyTerminal t=page_new(&provider);
 ghostty_terminal_free(t);REQUIRE(measured.live==0 && measured.peak>0);
 PageBudget exact={.limit=measured.peak};GhosttyAllocator a=page_allocator(&exact);t=page_new(&a);ghostty_terminal_free(t);
 REQUIRE(exact.live==0 && exact.denied==0 && exact.peak<=exact.limit);
 PageBudget shortfall={.limit=measured.peak-1};a=page_allocator(&shortfall);t=NULL;
 REQUIRE(ghostty_terminal_p0_new_with_page_allocator(NULL,&a,&t,80,24)==GHOSTTY_OUT_OF_MEMORY && !t);
 REQUIRE(shortfall.live==0 && shortfall.denied==1 && shortfall.peak<=shortfall.limit);
}
static void page_alternate(bool restored) {
 PageBudget b={.limit=SIZE_MAX};GhosttyAllocator a=page_allocator(&b);GhosttyTerminal t=NULL;
 if(restored) {
  GhosttyTerminal source=new_terminal();feed(source,"before restore");Bytes bytes=capture(source);GhosttySnapshotDecoder decoder=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(NULL,&decoder,bytes.data,bytes.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_p0_page_allocator(decoder,&a)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&t)==GHOSTTY_SUCCESS && t && b.live>0);
  REQUIRE(ghostty_snapshot_decoder_p0_page_allocator(decoder,NULL)==GHOSTTY_INVALID_VALUE);
  ghostty_snapshot_decoder_free(decoder);require_equal_state(source,t);ghostty_terminal_free(source);free(bytes.data);
 } else t=page_new(&a);
 b.limit=b.live;b.calls=0;feed(t,"\x1b[?47h");bool failed=false;
 REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed)==GHOSTTY_SUCCESS);
 REQUIRE(failed && b.denied>0 && b.live<=b.limit);ghostty_terminal_free(t);REQUIRE(b.live==0 && b.mapped==b.unmapped);
}
static void page_restore(bool oom,bool disabled) {
 GhosttyTerminal source=new_terminal();feed(source,"primary\r\n中文");
 for(unsigned i=0;i<800;i++)feed(source,"history row\r\n");
 feed(source,"\x1b[?47halternate\r\n\x1b[?47l");Bytes bytes=capture(source);size_t failures=0;bool completed=false;
 for(size_t fail=1;fail<=32;fail++) {
  PageBudget b={.limit=SIZE_MAX,.fail_at=oom?fail:0};GhosttyAllocator a=page_allocator(&b);GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal t=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(NULL,&decoder,bytes.data,bytes.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_p0_page_allocator(decoder,&a)==GHOSTTY_SUCCESS);
  if(disabled)REQUIRE(ghostty_snapshot_decoder_p0_page_allocator(decoder,NULL)==GHOSTTY_SUCCESS);
  GhosttyResult rc=ghostty_snapshot_decoder_decode(decoder,&t);
  if(rc==GHOSTTY_SUCCESS) {
   REQUIRE(t);completed=true;REQUIRE(b.denied==0);
   REQUIRE(disabled?(b.calls==0 && b.live==0):(b.calls>0 && b.live>0));require_equal_state(source,t);ghostty_terminal_free(t);
  } else {REQUIRE(oom && rc==GHOSTTY_OUT_OF_MEMORY && !t && b.denied==1);failures++;}
  ghostty_snapshot_decoder_free(decoder);REQUIRE(b.live==0 && b.mapped==b.unmapped);
  if(completed)break;
 }
 REQUIRE(completed && (!oom || failures>=4));free(bytes.data);ghostty_terminal_free(source);
 printf("PASS page provider restore/%zu failed page allocations, all mappings released\n",failures);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *mode=argv[1];
 if(strcmp(mode,"create")==0)page_create(false);else if(strcmp(mode,"create-oom")==0)page_create(true);
 else if(strcmp(mode,"quota")==0)page_quota();else if(strcmp(mode,"alternate")==0)page_alternate(false);else if(strcmp(mode,"restored-alternate")==0)page_alternate(true);
 else if(strcmp(mode,"restore")==0)page_restore(false,false);else if(strcmp(mode,"restore-oom")==0)page_restore(true,false);
 else {REQUIRE(strcmp(mode,"default")==0);page_restore(false,true);}
 printf("PASS page provider/%s\n",mode);return 0;
}
