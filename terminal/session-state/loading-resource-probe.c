#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN loading_resource_prior_main
#include "resource-snapshot-probe.c"
static const size_t loading_cap=1024*1024;
static Bytes loading_capture(GhosttyTerminal t) {
 Bytes b={0};REQUIRE(ghostty_terminal_p0_resources_encode(t,loading_cap,NULL,0,&b.len)==5 && b.len>16);
 b.data=malloc(b.len);REQUIRE(b.data);REQUIRE(ghostty_terminal_p0_resources_encode(t,loading_cap,b.data,b.len,&b.len)==0);return b;
}
static void loading_equal(GhosttyTerminal a,GhosttyTerminal b) {
 require_equal_state(a,b);Bytes x=loading_capture(a),y=loading_capture(b);
 REQUIRE(x.len==y.len && memcmp(x.data,y.data,x.len)==0);free(x.data);free(y.data);
}
static GhosttyTerminal loading_clone(GhosttyTerminal source) {
 Bytes canonical=capture(source),resources=loading_capture(source);GhosttyTerminal copy=restore(canonical);
 REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,loading_cap)==0);
 free(canonical.data);free(resources.data);loading_equal(source,copy);return copy;
}
static void loading_pixel(GhosttyTerminal t,bool green) {
 GhosttyKittyGraphicsImage image=image7(t);REQUIRE(image);const uint8_t *data=NULL;size_t n=0;
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&data)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&n)==GHOSTTY_SUCCESS);
 const uint8_t red[]={255,0,0,255},g[]={0,255,0,255};REQUIRE(n==4 && memcmp(data,green?g:red,n)==0);
}
static void loading_roundtrip(const char *mode) {
 bool plain=strcmp(mode,"no-display")==0,quiet=strcmp(mode,"quiet")==0,zlib=strcmp(mode,"zlib")==0;
 bool png=strcmp(mode,"png")==0,continuation=strcmp(mode,"continuation")==0;
 GhosttyTerminal source=new_terminal();configure_images(source,".");Replies source_reply={0},copy_reply={0};enable_replies(source,&source_reply);
 const char *tail="\x1b_Gm=0;/w==\x1b\\";
 if(png) {
  REQUIRE(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void*)ghostty_p0_decode_png)==GHOSTTY_SUCCESS);
  char first[256],last[256],command[512];base64(png_pixel,3,first,sizeof first);base64(png_pixel+3,sizeof png_pixel-3,last,sizeof last);
  REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=T,i=7,p=9,C=1,f=100,q=2,m=1;%s\x1b\\",first)<(int)sizeof command);feed(source,command);
  GhosttyTerminal copy=loading_clone(source);REQUIRE(!image7(source) && !image7(copy));
  REQUIRE(snprintf(command,sizeof command,"\x1b_Gm=0;%s\x1b\\",last)<(int)sizeof command);feed(source,command);feed(copy,command);
  loading_pixel(source,false);loading_pixel(copy,false);loading_equal(source,copy);ghostty_terminal_free(copy);ghostty_terminal_free(source);return;
 }
 if(zlib) {feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,o=z,q=2,m=1;eJz7\x1b\\");tail="\x1b_Gm=0;z8DwHwAE/wH/\x1b\\";}
 else if(plain)feed(source,"\x1b_Ga=t,i=7,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
 else if(quiet)feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
 else feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=0,m=1;/wAA\x1b\\");
 REQUIRE(!image7(source) && source_reply.calls==0);
 if(continuation) {feed(source,"\x1b_Gm=");tail="0;/w==\x1b\\";}
 GhosttyTerminal copy=loading_clone(source);enable_replies(copy,&copy_reply);REQUIRE(!image7(copy));
 feed(source,tail);feed(copy,tail);loading_pixel(source,false);loading_pixel(copy,false);loading_equal(source,copy);
 unsigned expected=(plain||quiet||zlib)?0u:1u;REQUIRE(source_reply.calls==expected && copy_reply.calls==expected);
 REQUIRE(source_reply.len==copy_reply.len && memcmp(source_reply.data,copy_reply.data,source_reply.len)==0);
 if(expected)REQUIRE(source_reply.len==strlen("\x1b_Gi=7,p=9;OK\x1b\\") && memcmp(source_reply.data,"\x1b_Gi=7,p=9;OK\x1b\\",source_reply.len)==0);
 if(!plain) {require_placement_at(source,0,0);require_placement_at(copy,0,0);}
 else {const char *place="\x1b_Ga=p,i=7,q=2\x1b\\";feed(source,place);feed(copy,place);loading_equal(source,copy);}
 ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void loading_screens(void) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");
 feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\\x1b[?1049h\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;AP8A\x1b\\");
 GhosttyTerminal copy=loading_clone(source);const char *tail="\x1b_Gm=0;/w==\x1b\\";feed(source,tail);feed(copy,tail);
 loading_pixel(source,true);loading_pixel(copy,true);loading_equal(source,copy);
 feed(source,"\x1b[?1049l");feed(copy,"\x1b[?1049l");feed(source,tail);feed(copy,tail);
 loading_pixel(source,false);loading_pixel(copy,false);loading_equal(source,copy);ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void loading_rejection(const char *mode) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");
 feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
 Bytes canonical=capture(source),resources=loading_capture(source);GhosttyTerminal copy=restore(canonical);
 Bytes empty=loading_capture(copy);size_t n=99;
 REQUIRE(resources.len==339 && memcmp(resources.data,"GTIR0003",8)==0);
 if(strcmp(mode,"limit")==0) {
  REQUIRE(ghostty_terminal_p0_resources_encode(source,resources.len-1,NULL,0,&n)==4 && n==0);
  REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resources.len-1)==4);
 } else {
  /* Invalid quiet enum, with a valid checksum, must fail in preflight. */
  resources.data[144]=99;little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
  REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,loading_cap)==1);
 }
 Bytes after=loading_capture(copy);REQUIRE(empty.len==after.len && memcmp(empty.data,after.data,empty.len)==0);
 require_no_semantic_error(copy);free(empty.data);free(after.data);free(resources.data);free(canonical.data);
 ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void loading_oom(void) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");
 feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
 Bytes canonical=capture(source),resources=loading_capture(source);size_t failures=0;bool completed=false;
 for(size_t position=1;position<=16;position++) {
  SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
  GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
  size_t before=state.base.live;state.calls=0;state.fail_at=position;
  int result=ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,loading_cap);
  if(result==0) {completed=true;REQUIRE(state.base.denied==0);}
  else {
   REQUIRE(result==2 && state.base.denied==1 && state.base.live==before);failures++;
   state.fail_at=0;state.calls=0;REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,loading_cap)==0);
  }
  state.fail_at=0;loading_equal(source,copy);
  const char *tail="\x1b_Gm=0;/w==\x1b\\";feed(copy,tail);loading_pixel(copy,false);
  ghostty_terminal_free(copy);REQUIRE(state.base.live==0);if(completed)break;
 }
 REQUIRE(completed && failures>=2);printf("PASS loading allocation sweep/%zu failure positions\n",failures);
 free(canonical.data);free(resources.data);ghostty_terminal_free(source);
}
static void loading_implicit(void) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");Replies owner={0},replica={0};enable_replies(source,&owner);
 feed(source,"\x1b_Ga=T,C=1,f=32,s=1,v=1,q=0,m=1;/wAA\x1b\\");
 GhosttyTerminal copy=loading_clone(source);enable_replies(copy,&replica);
 const char *tail="\x1b_Gm=0;/w==\x1b\\";feed(source,tail);feed(copy,tail);
 REQUIRE(image_at(source,2147483647u) && image_at(copy,2147483647u));REQUIRE(owner.calls==0 && replica.calls==0);loading_equal(source,copy);
 const char *next="\x1b_Ga=t,f=32,s=1,v=1,q=0;/wAA/w==\x1b\\";feed(source,next);feed(copy,next);
 REQUIRE(image_at(source,2147483648u) && image_at(copy,2147483648u));REQUIRE(owner.calls==0 && replica.calls==0);loading_equal(source,copy);
 ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void loading_frame_rejected(void) {
 GhosttyTerminal t=new_terminal();configure_images(t,".");feed(t,red_command);
 feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,q=2,m=1;AP8A\x1b\\");
 size_t n=99;REQUIRE(ghostty_terminal_p0_resources_encode(t,loading_cap,NULL,0,&n)==3 && n==0);
 require_no_semantic_error(t);ghostty_terminal_free(t);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);
 if(strcmp(argv[1],"screens")==0)loading_screens();
 else if(strcmp(argv[1],"limit")==0 || strcmp(argv[1],"invalid")==0)loading_rejection(argv[1]);
 else if(strcmp(argv[1],"oom")==0)loading_oom();
 else if(strcmp(argv[1],"implicit")==0)loading_implicit();
 else if(strcmp(argv[1],"frame-loading")==0)loading_frame_rejected();
 else if(strcmp(argv[1],"static")==0 || strcmp(argv[1],"animation")==0)return loading_resource_prior_main(argc,argv);
 else loading_roundtrip(argv[1]);
 printf("PASS loading-resource/%s\n",argv[1]);return 0;
}
