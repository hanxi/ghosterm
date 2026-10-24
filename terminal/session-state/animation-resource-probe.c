#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN animation_static_prior_main
#include "resource-snapshot-probe.c"
static void animation_equal(GhosttyTerminal a,GhosttyTerminal b) {
 require_equal_state(a,b);Bytes x=resource_capture(a),y=resource_capture(b);
 REQUIRE(x.len==y.len && memcmp(x.data,y.data,x.len)==0);free(x.data);free(y.data);
}
static void animation_pixel(GhosttyTerminal t,unsigned color) {
 GhosttyKittyGraphicsImage img=image7(t);REQUIRE(img);const uint8_t *data=NULL;size_t n=0;
 REQUIRE(ghostty_kitty_graphics_image_get(img,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&data)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_kitty_graphics_image_get(img,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&n)==GHOSTTY_SUCCESS);
 const uint8_t pixels[3][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255}};REQUIRE(n==4 && color<3 && memcmp(data,pixels[color],4)==0);
}
static GhosttyTerminal animation_source(const char *mode) {
 GhosttyTerminal t=new_terminal();configure_images(t,".");
 bool unplaced=strcmp(mode,"unplaced")==0,root=strcmp(mode,"root-only")==0;
 feed(t,unplaced?"\x1b_Ga=t,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\":"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
 if(!root) {
  feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=40,q=2;AP8A/w==\x1b\\");
  feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=60,q=2;AAD//w==\x1b\\");
 }
 feed(t,"\x1b_Ga=a,i=7,r=1,z=20,c=1,s=3,q=2\x1b\\");
 if(strcmp(mode,"stopped")==0)feed(t,"\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");
 if(strcmp(mode,"loading")==0)feed(t,"\x1b_Ga=a,i=7,s=2,q=2\x1b\\");
 if(strcmp(mode,"finite")==0)feed(t,"\x1b_Ga=a,i=7,v=2,q=2\x1b\\");
 if(strcmp(mode,"gapless")==0)feed(t,"\x1b_Ga=a,i=7,r=1,z=-1,q=2\x1b\\\x1b_Ga=a,i=7,r=2,z=-1,q=2\x1b\\");
 require_no_semantic_error(t);return t;
}
static void animation_roundtrip(const char *mode) {
 GhosttyTerminal source=animation_source(mode);uint64_t delay=0;
 bool continuation=strcmp(mode,"continuation")==0;if(continuation)feed(source,"\x1b[1;");
 bool stopped=strcmp(mode,"stopped")==0,root=strcmp(mode,"root-only")==0,unplaced=strcmp(mode,"unplaced")==0,gapless=strcmp(mode,"gapless")==0;
 REQUIRE(ghostty_terminal_p0_animation_tick(source,0,&delay)==GHOSTTY_SUCCESS);
 REQUIRE(delay==((stopped||root||unplaced)?UINT64_MAX:gapless?60u:20u));animation_pixel(source,stopped?1:gapless?2:0);
 GhosttyTerminal copy=clone_resources(source);animation_equal(source,copy);
 const uint64_t times[]={20,60,120};
 for(unsigned i=0;i<3;i++) {
  uint64_t a=0,b=0;REQUIRE(ghostty_terminal_p0_animation_tick(source,times[i],&a)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_p0_animation_tick(copy,times[i],&b)==GHOSTTY_SUCCESS && a==b);
  unsigned color=(stopped?1:(root||unplaced)?0:gapless?2:i==0?1:i==1?2:((strcmp(mode,"finite")==0 || strcmp(mode,"loading")==0)?2:0));
  animation_pixel(source,color);animation_pixel(copy,color);animation_equal(source,copy);
  if(continuation && i==0) {feed(source,"32mZ");feed(copy,"32mZ");animation_equal(source,copy);}
 }
 Bytes before=resource_capture(copy);delay=0;REQUIRE(ghostty_terminal_p0_animation_tick(copy,119,&delay)!=GHOSTTY_SUCCESS && delay==UINT64_MAX);
 Bytes after=resource_capture(copy);REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);free(before.data);free(after.data);
 if(unplaced) {
  const char *place="\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\";feed(source,place);feed(copy,place);
  REQUIRE(ghostty_terminal_p0_animation_tick(source,140,&delay)==GHOSTTY_SUCCESS && delay==20);
  REQUIRE(ghostty_terminal_p0_animation_tick(copy,140,&delay)==GHOSTTY_SUCCESS && delay==20);
  REQUIRE(ghostty_terminal_p0_animation_tick(source,160,&delay)==GHOSTTY_SUCCESS);REQUIRE(ghostty_terminal_p0_animation_tick(copy,160,&delay)==GHOSTTY_SUCCESS);
  animation_pixel(source,1);animation_pixel(copy,1);animation_equal(source,copy);
 }
 const char *remove="\x1b_Ga=d,d=I,i=7,q=2\x1b\\";feed(source,remove);feed(copy,remove);REQUIRE(!image7(source) && !image7(copy));animation_equal(source,copy);
 ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void animation_allocation_sweep(void) {
 GhosttyTerminal source=animation_source("running");uint64_t delay=0;REQUIRE(ghostty_terminal_p0_animation_tick(source,20,&delay)==GHOSTTY_SUCCESS);
 Bytes canonical=capture(source),resources=resource_capture(source);bool complete=false;size_t failures=0;
 for(size_t position=1;position<=64;position++) {
  SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
  GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
  Bytes before=resource_capture(copy);state.calls=0;state.fail_at=position;
  int result=ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap);
  if(result==0)complete=state.base.denied==0;
  else {
   REQUIRE(result==2 && state.base.denied>0);failures++;state.fail_at=0;
   Bytes after=resource_capture(copy);REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);free(after.data);
   REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==0);
  }
  state.fail_at=0;animation_equal(source,copy);free(before.data);
  ghostty_terminal_free(copy);REQUIRE(state.base.live==0);if(complete)break;
 }
 REQUIRE(complete && failures>=5);printf("PASS animation allocations/%zu failure positions\n",failures);
 free(canonical.data);free(resources.data);ghostty_terminal_free(source);
}
static uint64_t animation_word(const uint8_t *p) {uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static size_t animation_loop_offset(Bytes b) {
 REQUIRE(b.len>220 && memcmp(b.data,"GTIR0004",8)==0);
 size_t start=16+8+(animation_word(b.data+16)?8:0)+8+7*8;
 REQUIRE(animation_word(b.data+start)==7);size_t len=(size_t)animation_word(b.data+start+6*8);
 return start+7*8+len+6*8;
}
static void animation_loop_saturation(void) {
 GhosttyTerminal source=animation_source("running");Bytes canonical=capture(source),resources=resource_capture(source);
 size_t offset=animation_loop_offset(resources);little32_write(resources.data+offset,UINT32_MAX);
 little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));GhosttyTerminal copy=restore(canonical);
 REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==0);
 uint64_t delay=0;const uint64_t times[]={0,20,60,120};
 for(unsigned i=0;i<4;i++)REQUIRE(ghostty_terminal_p0_animation_tick(copy,times[i],&delay)==GHOSTTY_SUCCESS);
 animation_pixel(copy,0);Bytes after=resource_capture(copy);
 REQUIRE(animation_word(after.data+animation_loop_offset(after))==UINT32_MAX);
 ghostty_terminal_free(copy);ghostty_terminal_free(source);free(canonical.data);free(resources.data);free(after.data);
}
static void animation_rejection(const char *mode) {
 GhosttyTerminal source=animation_source("running");Bytes canonical=capture(source),resources=resource_capture(source);
 size_t loop=animation_loop_offset(resources),offset=loop;uint64_t value=UINT64_MAX;int expected=1;
 if(strcmp(mode,"invalid-count")==0) {offset=loop-5*8;expected=4;}
 else if(strcmp(mode,"invalid-state")==0) {offset=loop-2*8;value=99;}
 else if(strcmp(mode,"invalid-index")==0) {offset=loop-3*8;value=99;}
 else if(strcmp(mode,"invalid-frame-size")==0) {REQUIRE(animation_word(resources.data+loop+8)==0);offset=loop+24;value=5;
  size_t insert=loop+36;resources.data=realloc(resources.data,resources.len+1);REQUIRE(resources.data);
  memmove(resources.data+insert+1,resources.data+insert,resources.len-insert);resources.data[insert]=0;resources.len++;
  little32_write(resources.data+8,(uint32_t)(resources.len-16));little32_write(resources.data+loop-132,13);
 }
 else if(strcmp(mode,"invalid-clock")==0) {offset=16;value=2;}
 else REQUIRE(strcmp(mode,"invalid-loop-wide")==0);
 for(unsigned i=0;i<8;i++)resources.data[offset+i]=(uint8_t)(value>>(8*i));
 little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
 SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
 GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
 REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
 Bytes before=resource_capture(copy);size_t live=state.base.live;state.calls=0;
 REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==expected);
 REQUIRE(state.calls==0 && state.base.live==live);Bytes after=resource_capture(copy);
 REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);
 ghostty_terminal_free(copy);REQUIRE(state.base.live==0);ghostty_terminal_free(source);
 free(canonical.data);free(resources.data);free(before.data);free(after.data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);if(strcmp(argv[1],"oom")==0)animation_allocation_sweep();
 else if(strncmp(argv[1],"invalid-",8)==0)animation_rejection(argv[1]);
 else if(strcmp(argv[1],"loop-max")==0)animation_loop_saturation();
 else if(strcmp(argv[1],"static")==0)return animation_static_prior_main(argc,argv);
 else animation_roundtrip(argv[1]);printf("PASS animation-resource/%s\n",argv[1]);return 0;
}
