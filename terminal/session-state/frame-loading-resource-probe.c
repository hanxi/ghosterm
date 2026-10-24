#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN frame_loading_prior_main
#include "resource-snapshot-probe.c"
static uint32_t frame_target_id=7;
static void frame_equal(GhosttyTerminal a,GhosttyTerminal b) {
 require_equal_state(a,b);Bytes x=resource_capture(a),y=resource_capture(b);
 REQUIRE(x.len==y.len && memcmp(x.data,y.data,x.len)==0);free(x.data);free(y.data);
}
static void frame_pixels(GhosttyTerminal t,const uint8_t *expected,size_t n) {
 GhosttyKittyGraphicsImage image=image_at(t,frame_target_id);REQUIRE(image);const uint8_t *pixels=NULL;size_t actual=0;
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&actual)==GHOSTTY_SUCCESS);
 REQUIRE(actual==n && memcmp(pixels,expected,n)==0);
}
static void frame_roundtrip(const char *mode) {
 bool by_number=strcmp(mode,"number")==0,edit=strcmp(mode,"edit")==0;
 bool stale=strcmp(mode,"stale")==0,tick_after=strcmp(mode,"tick-after")==0,missing=strcmp(mode,"missing")==0;
 bool blend=strcmp(mode,"blend")==0,overwrite=strcmp(mode,"overwrite")==0,background=strcmp(mode,"background")==0;
 bool png=strcmp(mode,"png")==0,zlib=strcmp(mode,"zlib")==0,quiet=strcmp(mode,"quiet")==0;
 bool continuation=strcmp(mode,"continuation")==0,negative=strcmp(mode,"negative-gap")==0;
 frame_target_id=by_number?1:7;
 GhosttyTerminal source=new_terminal();configure_images(source,".");
 if(background)feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=2,v=1,q=2;/wAA//8AAP8=\x1b\\");
 else if(png||zlib)feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;AP8A/w==\x1b\\");
 else if(by_number)feed(source,"\x1b_Ga=T,I=9,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
 else feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
 REQUIRE(image_at(source,frame_target_id));
 if(edit||stale||tick_after)feed(source,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=40,q=2;AP8A/w==\x1b\\");
 uint64_t delay=0;
 if(stale||tick_after) {feed(source,"\x1b_Ga=a,i=7,r=1,z=20,c=1,s=3,q=2\x1b\\");REQUIRE(ghostty_terminal_p0_animation_tick(source,0,&delay)==GHOSTTY_SUCCESS);}
 Replies owner={0},replica={0};enable_replies(source,&owner);char command[512],tail[512]="\x1b_Gm=0;/w==\x1b\\";
 if(png) {
  REQUIRE(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void*)ghostty_p0_decode_png)==GHOSTTY_SUCCESS);
  char first[256],last[256];base64(png_pixel,3,first,sizeof first);base64(png_pixel+3,sizeof png_pixel-3,last,sizeof last);
  REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=f,i=7,f=100,z=77,q=0,m=1;%s\x1b\\",first)<(int)sizeof command);
  REQUIRE(snprintf(tail,sizeof tail,"\x1b_Gm=0;%s\x1b\\",last)<(int)sizeof tail);
 } else if(zlib) {strcpy(command,"\x1b_Ga=f,i=7,f=32,s=1,v=1,o=z,z=77,q=0,m=1;eJz7\x1b\\");strcpy(tail,"\x1b_Gm=0;z8DwHwAE/wH/\x1b\\");}
 else {
  REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=f,%s,f=32,s=1,v=1,z=%d,q=%u,m=1%s%s%s;AAD/\x1b\\",by_number?"I=9":"i=7",negative?-7:77,quiet?2u:0u,edit?",r=2":"",(blend||overwrite)?",c=1":"",background?",x=1,X=1,Y=16711935":overwrite?",X=1":"")<(int)sizeof command);
  if(blend||overwrite)strcpy(tail,"\x1b_Gm=0;gA==\x1b\\");
 }
 feed(source,command);REQUIRE(owner.calls==0);
 if(stale)REQUIRE(ghostty_terminal_p0_animation_tick(source,20,&delay)==GHOSTTY_SUCCESS);
 if(missing) {uint64_t limit=1;REQUIRE(ghostty_terminal_set(source,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);REQUIRE(!image7(source));}
 if(continuation) {feed(source,"\x1b_Gm=");strcpy(tail,"0;/w==\x1b\\");}
 GhosttyTerminal copy=clone_resources(source);frame_equal(source,copy);enable_replies(copy,&replica);
 if(tick_after) {REQUIRE(ghostty_terminal_p0_animation_tick(source,20,&delay)==GHOSTTY_SUCCESS);REQUIRE(ghostty_terminal_p0_animation_tick(copy,20,&delay)==GHOSTTY_SUCCESS);frame_equal(source,copy);}
 feed(source,tail);feed(copy,tail);frame_equal(source,copy);
 REQUIRE(owner.calls==(quiet?0u:1u) && owner.calls==replica.calls && owner.len==replica.len && memcmp(owner.data,replica.data,owner.len)==0);
 if(stale||missing||tick_after) {
  REQUIRE(owner.len==strlen("\x1b_Gi=7;ENOENT: image not found\x1b\\") && memcmp(owner.data,"\x1b_Gi=7;ENOENT: image not found\x1b\\",owner.len)==0);
  REQUIRE((image7(source)==NULL)==missing && (image7(copy)==NULL)==missing);
 } else {
  const char *reply=by_number?"\x1b_Gi=1,I=9,r=2;OK\x1b\\":"\x1b_Gi=7,r=2;OK\x1b\\";
  if(!quiet)REQUIRE(owner.len==strlen(reply) && memcmp(owner.data,reply,owner.len)==0);
  const char *select=by_number?"\x1b_Ga=a,i=1,c=2,s=1,q=2\x1b\\":"\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\";feed(source,select);feed(copy,select);
  const uint8_t blue[]={0,0,255,255},red[]={255,0,0,255},purple[]={127,0,128,255},alpha[]={0,0,255,128},pair[]={0,255,0,255,0,0,255,255};
  const uint8_t *expected=background?pair:(png||zlib)?red:blend?purple:overwrite?alpha:blue;size_t n=background?8:4;
  frame_pixels(source,expected,n);frame_pixels(copy,expected,n);frame_equal(source,copy);
  if(!negative) {
   const char *run=by_number?"\x1b_Ga=a,i=1,r=1,z=20,s=3,q=2\x1b\\":"\x1b_Ga=a,i=7,r=1,z=20,s=3,q=2\x1b\\";feed(source,run);feed(copy,run);
   REQUIRE(ghostty_terminal_p0_animation_tick(source,0,&delay)==GHOSTTY_SUCCESS && delay==77);
   REQUIRE(ghostty_terminal_p0_animation_tick(copy,0,&delay)==GHOSTTY_SUCCESS && delay==77);frame_equal(source,copy);
  }
 }
 require_no_semantic_error(source);require_no_semantic_error(copy);ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static GhosttyTerminal frame_pending(bool edited) {
 GhosttyTerminal t=new_terminal();configure_images(t,".");feed(t,red_command);
 if(edited)feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=40,q=2;AP8A/w==\x1b\\");
 feed(t,edited?"\x1b_Ga=f,i=7,f=32,s=1,v=1,r=2,z=77,q=2,m=1;AAD/\x1b\\":"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\");return t;
}
static void frame_oom(void) {
 GhosttyTerminal source=frame_pending(true);Bytes canonical=capture(source),resources=resource_capture(source);bool complete=false;size_t failures=0;
 for(size_t position=1;position<=64;position++) {
  SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
  GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
  Bytes before=resource_capture(copy);state.calls=0;state.fail_at=position;
  int result=ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap);
  if(result==0)complete=state.base.denied==0;
  else {REQUIRE(result==2 && state.base.denied>0);failures++;state.fail_at=0;
   Bytes after=resource_capture(copy);REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);free(after.data);
   REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==0);
  }
  state.fail_at=0;frame_equal(source,copy);feed(copy,"\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");
  const uint8_t blue[]={0,0,255,255};frame_pixels(copy,blue,sizeof blue);free(before.data);ghostty_terminal_free(copy);REQUIRE(state.base.live==0);if(complete)break;
 }
 REQUIRE(complete && failures>=8);printf("PASS frame loading allocations/%zu failure positions\n",failures);
 ghostty_terminal_free(source);free(canonical.data);free(resources.data);
}
static uint64_t frame_word(const uint8_t *p) {uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void frame_invalid(const char *mode) {
 GhosttyTerminal source=frame_pending(false);Bytes canonical=capture(source),resources=resource_capture(source);
 REQUIRE(resources.len==567 && memcmp(resources.data,"GTIR0005",8)==0 && frame_word(resources.data+380)==1 && frame_word(resources.data+388)==1);
 size_t offset=388;uint64_t value=2;
 if(strcmp(mode,"bad-frame-presence")==0)offset=380;
 else if(strcmp(mode,"bad-reference")==0) {offset=284;value=99;}
 else if(strcmp(mode,"bad-format")==0) {offset=396;value=99;}
 else if(strcmp(mode,"bad-medium")==0) {offset=404;value=99;}
 else if(strcmp(mode,"bad-both")==0) {offset=452;value=9;}
 else if(strcmp(mode,"bad-none")==0) {offset=444;value=0;}
 else if(strcmp(mode,"bad-composition")==0)offset=532;
 else if(strcmp(mode,"bad-wide")==0) {offset=500;value=UINT64_C(0x100000000);}
 else if(strcmp(mode,"bad-more")==0)offset=476;
 else if(strcmp(mode,"bad-usage")==0)offset=484;
 else if(strcmp(mode,"bad-number")==0) {offset=284;value=0;}
 else REQUIRE(strcmp(mode,"bad-match")==0);
 for(unsigned i=0;i<8;i++)resources.data[offset+i]=(uint8_t)(value>>(8*i));little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
 SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
 GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
 REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
 Bytes before=resource_capture(copy);size_t live=state.base.live;state.calls=0;
 REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==1);
 REQUIRE(state.calls==0 && state.base.live==live);Bytes after=resource_capture(copy);
 REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);free(before.data);free(after.data);
 ghostty_terminal_free(copy);REQUIRE(state.base.live==0);ghostty_terminal_free(source);free(canonical.data);free(resources.data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);if(strcmp(argv[1],"static")==0)return frame_loading_prior_main(argc,argv);
 if(strcmp(argv[1],"oom")==0)frame_oom();
 else if(strncmp(argv[1],"bad-",4)==0)frame_invalid(argv[1]);
 else frame_roundtrip(argv[1]);printf("PASS frame-loading/%s\n",argv[1]);return 0;
}
