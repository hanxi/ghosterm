#define GHOSTERM_PENDING_MAIN pending_screen_prior_main
#include "pending-resource-probe.c"
static void screen_assert(GhosttyTerminal t,bool alternate) {
 GhosttyTerminalScreen actual=GHOSTTY_TERMINAL_SCREEN_MAX_VALUE;
 REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,&actual)==GHOSTTY_SUCCESS);
 REQUIRE(actual==(alternate?GHOSTTY_TERMINAL_SCREEN_ALTERNATE:GHOSTTY_TERMINAL_SCREEN_PRIMARY));
}
static void screen_switch(GhosttyTerminal a,GhosttyTerminal b,bool alternate) {
 feed(a,alternate?"\x1b[?47h":"\x1b[?47l");if(b)feed(b,alternate?"\x1b[?47h":"\x1b[?47l");
 screen_assert(a,alternate);if(b)screen_assert(b,alternate);
}
static void screen_roundtrip(const char *mode) {
 bool alt=strncmp(mode,"alternate",9)==0,cancel=strstr(mode,"cancel")!=NULL;
 bool frame=strcmp(mode,"frame")==0,both=strcmp(mode,"both")==0,wrong_id=strcmp(mode,"wrong-id")==0;
 GhosttyTerminal a=new_terminal();configure_images(a,".");uint64_t primary=pending_begin(a,false,0);
 feed(a,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");
 if(frame)feed(a,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\");
 screen_switch(a,NULL,true);uint64_t alternate=pending_begin(a,false,9);REQUIRE(primary!=alternate);
 feed(a,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");if(alt)screen_switch(a,NULL,false);
 GhosttyTerminal b=clone_resources(a);pending_equal(a,b);uint64_t token=alt?alternate:primary;bool x=false,y=false;
 if(wrong_id) {
  REQUIRE(ghostty_terminal_p0_pending_ready(a,8,token,pending_red,4,&x)==0 && !x);
  REQUIRE(ghostty_terminal_p0_pending_cancel(b,8,token,&y)==0 && !y);pending_equal(a,b);
 }
 if(cancel) {
  REQUIRE(ghostty_terminal_p0_pending_cancel(a,7,token,&x)==0 && x);
  REQUIRE(ghostty_terminal_p0_pending_cancel(b,7,token,&y)==0 && y);
 } else {
  REQUIRE(ghostty_terminal_p0_pending_ready(a,7,token,pending_red,4,&x)==0 && x);
  REQUIRE(ghostty_terminal_p0_pending_ready(b,7,token,pending_red,4,&y)==0 && y);
 }
 screen_assert(a,!alt);screen_assert(b,!alt);pending_assert(a,4);pending_assert(b,4);require_placement_count(a,1);require_placement_count(b,1);pending_equal(a,b);
 if(both) {
  REQUIRE(ghostty_terminal_p0_pending_ready(a,7,alternate,pending_blue,4,&x)==0 && x);
  REQUIRE(ghostty_terminal_p0_pending_ready(b,7,alternate,pending_blue,4,&y)==0 && y);
  pending_pixels(a,pending_blue,4);pending_pixels(b,pending_blue,4);pending_equal(a,b);
 }
 screen_switch(a,b,alt);if(cancel) {REQUIRE(!image7(a) && !image7(b));require_placement_count(a,0);require_placement_count(b,0);}
 else {pending_pixels(a,pending_red,4);pending_pixels(b,pending_red,4);}
 REQUIRE(ghostty_terminal_p0_pending_ready(a,7,token,pending_red,4,&x)==0 && !x);
 REQUIRE(ghostty_terminal_p0_pending_cancel(b,7,token,&y)==0 && !y);pending_equal(a,b);
 if(frame) {
  feed(a,"\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");
  feed(b,"\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");
  pending_pixels(a,pending_blue,4);pending_pixels(b,pending_blue,4);pending_equal(a,b);
 }
 require_no_semantic_error(a);require_no_semantic_error(b);ghostty_terminal_free(a);ghostty_terminal_free(b);
}
static void screen_duplicate(void) {
 GhosttyTerminal a=new_terminal();configure_images(a,".");uint64_t primary=pending_begin(a,false,0);
 screen_switch(a,NULL,true);uint64_t alternate=pending_begin(a,false,0);REQUIRE(primary!=alternate);
 Bytes canonical=capture(a),resources=resource_capture(a);REQUIRE(resources.len==320);
 for(unsigned i=0;i<8;i++)resources.data[288+i]=(uint8_t)(primary>>(8*i));
 little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
 SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
 GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal b=NULL;
 REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_decoder_decode(decoder,&b)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
 Bytes before=resource_capture(b);size_t live=state.base.live;state.calls=0;
 REQUIRE(ghostty_terminal_p0_resources_decode(b,resources.data,resources.len,resource_cap)==1);
 REQUIRE(state.calls==0 && state.base.live==live);Bytes after=resource_capture(b);
 REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);require_equal_state(a,b);
 ghostty_terminal_free(b);REQUIRE(state.base.live==0);ghostty_terminal_free(a);
 free(before.data);free(after.data);free(canonical.data);free(resources.data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);if(strcmp(argv[1],"control")==0) {char *args[]={argv[0],"ready"};return pending_screen_prior_main(2,args);}
 if(strcmp(argv[1],"duplicate-origin")==0)screen_duplicate();else screen_roundtrip(argv[1]);
 printf("PASS pending-screen/%s\n",argv[1]);return 0;
}
