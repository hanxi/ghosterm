#define GHOSTERM_PENDING_MAIN pending_failure_prior_main
#include "pending-resource-probe.c"
static GhosttyTerminal pending_decode(Bytes canonical,SnapshotAlloc *state,GhosttyAllocator *allocator) {
 static const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
 /* The allocator descriptor must outlive the terminal borrowing it. */
 *allocator=(GhosttyAllocator){.ctx=state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal t=NULL;
 REQUIRE(ghostty_snapshot_decoder_new_buf(allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_decoder_decode(decoder,&t)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);return t;
}
static GhosttyTerminal pending_failure_source(uint64_t *primary,uint64_t *alternate,bool placements) {
 GhosttyTerminal t=new_terminal();configure_images(t,".");*primary=pending_begin(t,false,0);
 if(placements) {
  feed(t,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");
  feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\");
 }
 feed(t,"\x1b[?47h");*alternate=pending_begin(t,false,9);
 if(placements)feed(t,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");return t;
}
static void pending_import_oom(void) {
 uint64_t primary=0,alternate=0;GhosttyTerminal a=pending_failure_source(&primary,&alternate,true);
 Bytes canonical=capture(a),resources=resource_capture(a);size_t failures=0;bool completed=false;
 for(size_t fail=1;fail<=32;fail++) {
  SnapshotAlloc state={0};GhosttyAllocator allocator;GhosttyTerminal b=pending_decode(canonical,&state,&allocator);Bytes before=resource_capture(b);
  state.calls=0;state.fail_at=fail;int rc=ghostty_terminal_p0_resources_decode(b,resources.data,resources.len,resource_cap);state.fail_at=0;
  if(rc==0) {REQUIRE(state.base.denied==0);completed=true;}
  else {
   REQUIRE(rc==2 && state.base.denied==1);failures++;REQUIRE(!image7(b));require_placement_count(b,0);
   Bytes after=resource_capture(b);REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);free(after.data);
   require_equal_state(a,b);require_no_semantic_error(b);state.fail_at=0;state.calls=0;
   REQUIRE(ghostty_terminal_p0_resources_decode(b,resources.data,resources.len,resource_cap)==0);
  }
  pending_equal(a,b);pending_assert(b,4);require_placement_count(b,1);
  state.fail_at=0;state.calls=0;bool applied=false;
  REQUIRE(ghostty_terminal_p0_pending_ready(b,7,primary,pending_red,4,&applied)==0 && applied);
  pending_assert(b,4);feed(b,"\x1b[?47l");pending_pixels(b,pending_red,4);
  feed(b,"\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");pending_pixels(b,pending_blue,4);
  ghostty_terminal_free(b);REQUIRE(state.base.live==0);free(before.data);if(completed)break;
 }
 REQUIRE(completed && failures>=4);free(canonical.data);free(resources.data);ghostty_terminal_free(a);
 printf("PASS pending import OOM/%zu failure positions, retry, frame continuation and release\n",failures);
}
static void pending_ready_oom(void) {
 uint64_t primary=0,alternate=0;GhosttyTerminal a=pending_failure_source(&primary,&alternate,false);
 Bytes canonical=capture(a),resources=resource_capture(a);SnapshotAlloc state={0};GhosttyAllocator allocator;GhosttyTerminal b=pending_decode(canonical,&state,&allocator);
 REQUIRE(ghostty_terminal_p0_resources_decode(b,resources.data,resources.len,resource_cap)==0);
 size_t live=state.base.live;state.calls=0;state.fail_at=1;bool applied=true;
 REQUIRE(ghostty_terminal_p0_pending_ready(b,7,primary,pending_red,4,&applied)==2 && !applied);
 REQUIRE(state.calls==1 && state.base.denied==1 && state.base.live==live);pending_equal(a,b);require_no_semantic_error(b);
 state.calls=0;state.fail_at=0;
 REQUIRE(ghostty_terminal_p0_pending_ready(b,7,primary,pending_red,4,&applied)==0 && applied);
 REQUIRE(ghostty_terminal_p0_pending_ready(a,7,primary,pending_red,4,&applied)==0 && applied);pending_equal(a,b);
 state.calls=0;state.fail_at=1;
 REQUIRE(ghostty_terminal_p0_pending_ready(b,7,primary,pending_red,4,&applied)==0 && !applied);
 REQUIRE(ghostty_terminal_p0_pending_cancel(b,7,alternate+UINT64_C(1000000),&applied)==0 && !applied);
 REQUIRE(ghostty_terminal_p0_pending_cancel(b,7,alternate,&applied)==0 && applied);REQUIRE(state.calls==0);
 REQUIRE(ghostty_terminal_p0_pending_cancel(a,7,alternate,&applied)==0 && applied);state.fail_at=0;pending_equal(a,b);
 state.fail_at=0;ghostty_terminal_free(b);REQUIRE(state.base.live==0);ghostty_terminal_free(a);free(canonical.data);free(resources.data);
}
static void pending_bad_word(uint8_t *p,uint64_t v) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static void pending_malformed(const char *mode) {
 uint64_t primary=0,alternate=0;GhosttyTerminal a=pending_failure_source(&primary,&alternate,false);
 Bytes canonical=capture(a),resources=resource_capture(a);REQUIRE(resources.len==320);
 size_t offset=136;uint64_t value=2;int expected=1;
 if(strcmp(mode,"bad-token")==0) {offset=144;value=0;}
 else if(strcmp(mode,"bad-complete-token")==0)value=0;
 else if(strcmp(mode,"bad-length")==0) {offset=152;value=3;}
 else if(strcmp(mode,"bad-long-length")==0) {offset=152;value=UINT64_MAX;}
 else if(strcmp(mode,"bad-width")==0) {offset=104;value=0;}
 else if(strcmp(mode,"bad-id")==0) {offset=88;value=0;}
 else if(strcmp(mode,"bad-format")==0) {offset=120;value=99;}
 else if(strcmp(mode,"bad-flags")==0) {offset=128;value=4;}
 else if(strcmp(mode,"bad-total")==0) {offset=64;value=3;}
 else if(strcmp(mode,"bad-limit")==0) {offset=56;value=3;expected=4;}
 else if(strcmp(mode,"bad-animation")==0) {offset=160;value=2;}
 else REQUIRE(strcmp(mode,"bad-kind")==0);
 pending_bad_word(resources.data+offset,value);little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
 SnapshotAlloc state={0};GhosttyAllocator allocator;GhosttyTerminal b=pending_decode(canonical,&state,&allocator);
 /* A malformed second import must not even allocate over an existing image. */
 feed(b,"\x1b[?47l\x1b_Ga=t,i=8,f=32,s=1,v=1,q=2;AAD//w==\x1b\\\x1b[?47h\x1b_Ga=t,i=8,f=32,s=1,v=1,q=2;AAD//w==\x1b\\");
 Bytes before=resource_capture(b),before_canonical=capture(b);size_t live=state.base.live;state.calls=0;
 REQUIRE(ghostty_terminal_p0_resources_decode(b,resources.data,resources.len,resource_cap)==expected);
 REQUIRE(state.calls==0 && state.base.live==live);Bytes after=resource_capture(b),after_canonical=capture(b);
 REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);
 REQUIRE(before_canonical.len==after_canonical.len && memcmp(before_canonical.data,after_canonical.data,before_canonical.len)==0);
 require_no_semantic_error(b);ghostty_terminal_free(b);REQUIRE(state.base.live==0);ghostty_terminal_free(a);
 free(canonical.data);free(resources.data);free(before.data);free(after.data);free(before_canonical.data);free(after_canonical.data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *mode=argv[1];
 if(strcmp(mode,"control")==0) {char *args[]={argv[0],"ready"};return pending_failure_prior_main(2,args);}
 if(strcmp(mode,"import-oom")==0)pending_import_oom();else if(strcmp(mode,"ready-oom")==0)pending_ready_oom();else pending_malformed(mode);
 printf("PASS pending failure/%s\n",mode);return 0;
}
