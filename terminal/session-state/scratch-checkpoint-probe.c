#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN scratch_checkpoint_prior_main
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
static void owner_unchanged(p0_budget_scope *owner,p0_budget_usage before) {
 p0_budget_usage after=p0_budget_scope_read(owner);REQUIRE(p0_budget_scope_fault(owner)==P0_BUDGET_HEALTHY);
 REQUIRE(after.live==before.live && after.attempts==before.attempts && after.mapped==before.mapped && after.unmapped==before.unmapped);
}
static void snapshot_equal(p0_checkpoint *c,Bytes canonical,Bytes resources) {
 REQUIRE(c->canonical && c->canonical_len==canonical.len && memcmp(c->canonical,canonical.data,canonical.len)==0);
 REQUIRE(c->resources && c->resources_len==resources.len && memcmp(c->resources,resources.data,resources.len)==0);
}
static void successful(const char *kind) {
 p0_budget budget;REQUIRE(p0_budget_init(&budget,128*1024*1024));p0_budget_scope owner,scratch;
 REQUIRE(p0_budget_scope_admit(&owner,&budget,64*1024*1024));REQUIRE(p0_budget_scope_admit(&scratch,&budget,64*1024*1024));
 GhosttyTerminal t=checkpoint_source(&owner,kind);Bytes canonical=capture(t),resources=resource_capture(t);
 p0_budget_usage before=p0_budget_scope_read(&owner);REQUIRE(p0_budget_scope_set_limit(&owner,before.live));
 GhosttyTerminalMemoryUsage m1={.size=sizeof m1},m2={.size=sizeof m2};REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_MEMORY_USAGE,&m1)==GHOSTTY_SUCCESS);
 p0_checkpoint result={0};p0_checkpoint_error error={0};
 REQUIRE(p0_checkpoint_capture(&owner,&scratch,t,1024*1024,&result,&error)==P0_CHECKPOINT_OK && error.stage==P0_CHECKPOINT_COMPLETE);
 snapshot_equal(&result,canonical,resources);owner_unchanged(&owner,before);
 REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_MEMORY_USAGE,&m2)==GHOSTTY_SUCCESS);
 REQUIRE(m1.primary_compressed_pages==m2.primary_compressed_pages && m1.primary_resident_bytes==m2.primary_resident_bytes && m1.primary_compressed_bytes==m2.primary_compressed_bytes);
 REQUIRE(p0_budget_scope_read(&scratch).live>0 && !p0_budget_scope_close(&scratch));p0_checkpoint_release(&result);
 REQUIRE(!result.canonical && !result.resources && result.canonical_len==0 && result.resources_len==0);
 REQUIRE(p0_budget_scope_read(&scratch).live==0 && p0_budget_scope_close(&scratch));
 REQUIRE(p0_budget_scope_set_limit(&owner,64*1024*1024));const char *tail=strstr(kind,"continuation")?"hW":strcmp(kind,"utf8")==0?"\xad" "W":strstr(kind,"loading")?"\x1b_Gm=0;/w==\x1b\\W":"W";
 REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)tail,strlen(tail))==GHOSTTY_SUCCESS);
 ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner));p0_budget_usage all=p0_budget_read(&budget);REQUIRE(all.live==0 && all.mapped==all.unmapped && p0_budget_destroy(&budget));
 free(canonical.data);free(resources.data);
}
static void failures(const char *kind) {
 p0_budget budget;REQUIRE(p0_budget_init(&budget,128*1024*1024));p0_budget_scope owner;
 REQUIRE(p0_budget_scope_admit(&owner,&budget,64*1024*1024));GhosttyTerminal t=checkpoint_source(&owner,kind);
 Bytes canonical=capture(t),resources=resource_capture(t);p0_budget_usage before=p0_budget_scope_read(&owner);REQUIRE(p0_budget_scope_set_limit(&owner,before.live));
 bool complete=false,canonical_failed=false,resource_failed=false;size_t failures=0;
 for(size_t limit=0;limit<=8*1024*1024;limit+=budget.page_size) {
  p0_budget_scope scratch;REQUIRE(p0_budget_scope_admit(&scratch,&budget,limit));p0_checkpoint result={0};p0_checkpoint_error error={0};
  p0_checkpoint_status status=p0_checkpoint_capture(&owner,&scratch,t,1024*1024,&result,&error);
  owner_unchanged(&owner,before);
  if(status==P0_CHECKPOINT_OK) {complete=true;snapshot_equal(&result,canonical,resources);p0_checkpoint_release(&result);}
  else {
   REQUIRE(status==P0_CHECKPOINT_LIMIT);REQUIRE(!result.canonical && !result.resources && result.canonical_len==0 && result.resources_len==0);
   REQUIRE(p0_budget_scope_fault(&scratch)==P0_BUDGET_SCOPE_LIMIT);failures++;
   canonical_failed|=error.stage==P0_CHECKPOINT_CANONICAL;resource_failed|=error.stage==P0_CHECKPOINT_RESOURCES;
  }
  p0_budget_usage u=p0_budget_scope_read(&scratch);REQUIRE(u.live==0 && u.mapped==u.unmapped && u.peak<=limit);
  REQUIRE(p0_budget_scope_close(&scratch));if(complete)break;
 }
 REQUIRE(complete && failures>0 && canonical_failed);if(strcmp(kind,"pair-oom")==0)REQUIRE(resource_failed);
 REQUIRE(p0_budget_scope_set_limit(&owner,64*1024*1024));const char *tail=strstr(kind,"continuation") || strcmp(kind,"pair-oom")==0?"hW":"W";
 REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)tail,strlen(tail))==GHOSTTY_SUCCESS);
 ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner));p0_budget_usage all=p0_budget_read(&budget);REQUIRE(all.live==0 && all.mapped==all.unmapped && p0_budget_destroy(&budget));
 free(canonical.data);free(resources.data);printf("PASS scratch allocation quota sweep/%zu rejection limits\n",failures);
}
static void wire_limit(void) {
 p0_budget budget;REQUIRE(p0_budget_init(&budget,128*1024*1024));p0_budget_scope owner,scratch;REQUIRE(p0_budget_scope_admit(&owner,&budget,64*1024*1024));REQUIRE(p0_budget_scope_admit(&scratch,&budget,64*1024*1024));
 GhosttyTerminal t=checkpoint_source(&owner,"resources");Bytes canonical=capture(t),resources=resource_capture(t);p0_budget_usage before=p0_budget_scope_read(&owner);REQUIRE(p0_budget_scope_set_limit(&owner,before.live));
 p0_checkpoint result={0};p0_checkpoint_error error={0};
 REQUIRE(p0_checkpoint_capture(&owner,&scratch,t,canonical.len-1,&result,&error)==P0_CHECKPOINT_LIMIT);
 REQUIRE(!result.canonical && !result.resources && p0_budget_scope_read(&scratch).live==0);
 REQUIRE(p0_checkpoint_capture(&owner,&scratch,t,canonical.len+resources.len-1,&result,&error)==P0_CHECKPOINT_LIMIT);
 REQUIRE(!result.canonical && !result.resources && p0_budget_scope_read(&scratch).live==0);
 REQUIRE(p0_budget_scope_fault(&scratch)==P0_BUDGET_HEALTHY);owner_unchanged(&owner,before);
 REQUIRE(p0_checkpoint_capture(&owner,&scratch,t,canonical.len+resources.len,&result,&error)==P0_CHECKPOINT_OK);snapshot_equal(&result,canonical,resources);p0_checkpoint_release(&result);
 REQUIRE(p0_budget_scope_close(&scratch));ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner));REQUIRE(p0_budget_read(&budget).live==0 && p0_budget_destroy(&budget));free(canonical.data);free(resources.data);
}
static void invalid_source(bool failed) {
 p0_budget budget,other;REQUIRE(p0_budget_init(&budget,128*1024*1024));REQUIRE(p0_budget_init(&other,128*1024*1024));
 p0_budget_scope owner,scratch,foreign;REQUIRE(p0_budget_scope_admit(&owner,&budget,64*1024*1024));REQUIRE(p0_budget_scope_admit(&scratch,&budget,64*1024*1024));REQUIRE(p0_budget_scope_admit(&foreign,&other,64*1024*1024));
 GhosttyTerminal t=checkpoint_source(&owner,"ground");p0_checkpoint result={0};p0_checkpoint_error error={0};
 if(failed) {
  REQUIRE(p0_budget_scope_set_limit(&owner,p0_budget_scope_read(&owner).live));
  REQUIRE(p0_budget_feed(&owner,t,(const uint8_t*)"\x1b[?47h",6)==GHOSTTY_LIMIT_EXCEEDED);
  REQUIRE(p0_checkpoint_capture(&owner,&scratch,t,1024*1024,&result,&error)==P0_CHECKPOINT_SOURCE_FAILED);
 } else {
  REQUIRE(p0_checkpoint_capture(&owner,&owner,t,1024*1024,&result,&error)==P0_CHECKPOINT_INVALID);
  REQUIRE(p0_checkpoint_capture(&owner,&foreign,t,1024*1024,&result,&error)==P0_CHECKPOINT_INVALID);
 }
 REQUIRE(!result.canonical && !result.resources && p0_budget_scope_read(&scratch).attempts==0 && p0_budget_scope_read(&foreign).attempts==0);
 REQUIRE(p0_budget_scope_close(&scratch) && p0_budget_scope_close(&foreign) && p0_budget_destroy(&other));
 ghostty_terminal_free(t);REQUIRE(p0_budget_scope_close(&owner));REQUIRE(p0_budget_read(&budget).live==0 && p0_budget_destroy(&budget));
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *kind=argv[1];
 if(strcmp(kind,"invalid-roles")==0)invalid_source(false);else if(strcmp(kind,"source-failed")==0)invalid_source(true);else if(strcmp(kind,"wire-limit")==0)wire_limit();else if(strstr(kind,"oom"))failures(kind);else successful(kind);
 printf("PASS scratch checkpoint/%s\n",kind);return 0;
}
