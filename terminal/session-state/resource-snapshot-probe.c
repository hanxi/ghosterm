#define GHOSTERM_RESOURCE_HOOK_MAIN resource_hooks_prior_main
#include "resource-hooks-probe.c"
static const size_t resource_cap=1024*1024;
static const char red_command[]="\x1b[3;5H\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\\x1b[4;6H";
static Bytes resource_capture(GhosttyTerminal t) {
  Bytes b={0};REQUIRE(ghostty_terminal_p0_resources_encode(t,resource_cap,NULL,0,&b.len)==5 && b.len>16);
  b.data=malloc(b.len);REQUIRE(b.data);REQUIRE(ghostty_terminal_p0_resources_encode(t,resource_cap,b.data,b.len,&b.len)==0);return b;
}
static GhosttyTerminal clone_resources(GhosttyTerminal t) {
  Bytes canonical=capture(t),resources=resource_capture(t);GhosttyTerminal copy=restore(canonical);
  REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==0);
  free(canonical.data);free(resources.data);return copy;
}
static GhosttyKittyGraphicsImage image_at(GhosttyTerminal t,uint32_t id) {
  GhosttyKittyGraphics graphics=NULL;REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);
  return ghostty_kitty_graphics_image(graphics,id);
}
static void require_placement_count(GhosttyTerminal t,unsigned count) {
  GhosttyKittyGraphics graphics=NULL;REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);
  GhosttyKittyGraphicsPlacementIterator iter=NULL;REQUIRE(ghostty_kitty_graphics_placement_iterator_new(NULL,&iter)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_get(graphics,GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,&iter)==GHOSTTY_SUCCESS);
  unsigned actual=0;while(ghostty_kitty_graphics_placement_next(iter))actual++;
  ghostty_kitty_graphics_placement_iterator_free(iter);REQUIRE(actual==count);
}
static void roundtrip(const char *mode) {
  GhosttyTerminal source=new_terminal();configure_images(source,".");feed(source,red_command);
  bool screens=strcmp(mode,"screens")==0;
  if(screens) {feed(source,"\x1b[?1049h");feed(source,"\x1b[8;9H\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");}
  GhosttyTerminal copy=clone_resources(source);require_red_pixel(copy);require_placement_at(copy,screens?8:4,screens?7:2);require_equal_state(source,copy);
  if(screens) {feed(source,"\x1b[?1049l");feed(copy,"\x1b[?1049l");require_red_pixel(copy);require_placement_at(copy,4,2);}
  if(strcmp(mode,"placement-counter")==0) {
    const char *place="\x1b[8;9H\x1b_Ga=p,i=7,q=2\x1b\\";feed(source,place);feed(copy,place);require_placement_count(source,2);require_placement_count(copy,2);
  }
  const char *remove="\x1b_Ga=d,d=I,i=7,q=2\x1b\\";feed(source,remove);feed(copy,remove);
  REQUIRE(image7(source)==NULL && image7(copy)==NULL);require_equal_state(source,copy);
  if(screens) {feed(source,"\x1b[?47h");feed(copy,"\x1b[?47h");require_red_pixel(copy);require_placement_at(copy,8,7);require_equal_state(source,copy);}
  ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void format_and_virtual(const char *mode) {
  bool rgb=strcmp(mode,"rgb")==0;
  GhosttyTerminal source=new_terminal();configure_images(source,".");
  feed(source,rgb?"\x1b_Ga=T,i=7,f=24,s=1,v=1,q=2;/wAA\x1b\\":"\x1b_Ga=T,i=7,p=9,U=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  GhosttyTerminal copy=clone_resources(source);
  for(unsigned i=0;i<2;i++) {
    GhosttyTerminal t=i?copy:source;GhosttyKittyGraphicsImage img=image7(t);REQUIRE(img);
    const uint8_t *data=NULL;size_t len=0;const uint8_t red[]={255,0,0,255};
    REQUIRE(ghostty_kitty_graphics_image_get(img,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&data)==GHOSTTY_SUCCESS);
    REQUIRE(ghostty_kitty_graphics_image_get(img,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&len)==GHOSTTY_SUCCESS);
    REQUIRE(len==(rgb?3u:4u) && memcmp(data,red,len)==0);
    if(!rgb) {
      GhosttyKittyGraphics g=NULL;GhosttyKittyGraphicsPlacementIterator it=NULL;bool virt=false;
      REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&g)==GHOSTTY_SUCCESS);
      REQUIRE(ghostty_kitty_graphics_placement_iterator_new(NULL,&it)==GHOSTTY_SUCCESS);
      REQUIRE(ghostty_kitty_graphics_get(g,GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,&it)==GHOSTTY_SUCCESS && ghostty_kitty_graphics_placement_next(it));
      REQUIRE(ghostty_kitty_graphics_placement_get(it,GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IS_VIRTUAL,&virt)==GHOSTTY_SUCCESS && virt);
      ghostty_kitty_graphics_placement_iterator_free(it);
    }
  }
  require_equal_state(source,copy);ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void future_ids(void) {
  GhosttyTerminal source=new_terminal();configure_images(source,".");
  feed(source,"\x1b_Ga=t,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");REQUIRE(image_at(source,2147483647u));
  feed(source,"\x1b_Ga=d,d=I,i=2147483647,q=2\x1b\\");REQUIRE(!image_at(source,2147483647u));feed(source,red_command);
  GhosttyTerminal copy=clone_resources(source);
  const char *next="\x1b_Ga=t,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\";feed(source,next);feed(copy,next);
  REQUIRE(image_at(source,2147483648u) && image_at(copy,2147483648u));REQUIRE(!image_at(copy,2147483647u));
  require_equal_state(source,copy);ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void eviction_age(bool transient) {
  GhosttyTerminal source=new_terminal();configure_images(source,".");uint64_t limit=8;
  REQUIRE(ghostty_terminal_set(source,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
  feed(source,"\x1b_Ga=t,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  feed(source,transient?"\x1b_Ga=t,i=8,N=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\":"\x1b_Ga=t,i=8,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  REQUIRE(image_at(source,7) && image_at(source,8));GhosttyTerminal copy=clone_resources(source);
  const char *next="\x1b_Ga=t,i=9,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\";feed(source,next);feed(copy,next);
  uint32_t evicted=transient?8:7,kept=transient?7:8;
  REQUIRE(!image_at(source,evicted) && !image_at(copy,evicted));REQUIRE(image_at(source,kept) && image_at(copy,kept) && image_at(source,9) && image_at(copy,9));
  ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void unsupported(const char *mode) {
  GhosttyTerminal t=new_terminal();configure_images(t,".");
  if(strcmp(mode,"loading")==0)feed(t,"\x1b_Ga=T,i=7,f=32,s=1,v=1,m=1,q=2;/wAA\x1b\\");
  else if(strcmp(mode,"relative")==0)feed(t,"\x1b_Ga=T,i=7,p=9,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\\x1b_Ga=p,i=7,p=10,P=7,Q=9,q=2\x1b\\");
  else {feed(t,red_command);feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,q=2;AP8A/w==\x1b\\");}
  size_t n=99;REQUIRE(ghostty_terminal_p0_resources_encode(t,resource_cap,NULL,0,&n)==3 && n==0);
  require_no_semantic_error(t);ghostty_terminal_free(t);
}
static uint32_t crc32c(const uint8_t *p,size_t n) {
  uint32_t c=~0u;for(size_t i=0;i<n;i++) {c^=p[i];for(unsigned bit=0;bit<8;bit++)c=(c>>1)^((0u-(c&1u))&0x82f63b78u);}return ~c;
}
static void little32_write(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void invalid(const char *mode) {
  GhosttyTerminal t=new_terminal();configure_images(t,".");feed(t,red_command);Bytes canonical=capture(t),resources=resource_capture(t);
  PngAlloc state={0};const GhosttyAllocatorVtable table={png_alloc,png_resize,png_remap,png_free};GhosttyAllocator allocator={.ctx=&state,.vtable=&table};
  GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);size_t before=state.live;
  if(strcmp(mode,"oom")==0)state.deny=true;
  if(strcmp(mode,"crc")==0)resources.data[resources.len-8]^=1;
  if(strcmp(mode,"late-error")==0) {
    resources.data=realloc(resources.data,resources.len+1);REQUIRE(resources.data);resources.data[resources.len++]=0;
    little32_write(resources.data+8,(uint32_t)(resources.len-16));little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
  }
  int result=ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,strcmp(mode,"limit")==0?resources.len-1:resource_cap);
  REQUIRE(result==(strcmp(mode,"oom")==0?2:strcmp(mode,"limit")==0?4:1));REQUIRE(image7(copy)==NULL);REQUIRE(state.live==before);
  if(strcmp(mode,"oom")==0)REQUIRE(state.denied>0);
  state.deny=false;require_equal_state(t,copy);require_no_semantic_error(copy);ghostty_terminal_free(copy);REQUIRE(state.live==0);
  free(canonical.data);free(resources.data);ghostty_terminal_free(t);
}
typedef struct {PngAlloc base;size_t calls,fail_at;} SnapshotAlloc;
static void *snapshot_alloc(void *ctx,size_t len,uint8_t alignment,uintptr_t ret) {
 SnapshotAlloc *state=ctx;
 if(++state->calls==state->fail_at) {state->base.denied++;return NULL;}
 return png_alloc(&state->base,len,alignment,ret);
}
static void allocation_sweep(void) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");feed(source,red_command);
 Bytes canonical=capture(source),resources=resource_capture(source);bool completed=false;size_t failures=0;
 for(size_t fail_at=1;fail_at<=32;fail_at++) {
  SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
  GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
  REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
  state.calls=0;state.fail_at=fail_at;
  int result=ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap);
  if(result==0) {completed=true;REQUIRE(state.base.denied==0);require_red_pixel(copy);require_placement_at(copy,4,2);}
  else {
   REQUIRE(result==2 && state.base.denied==1);failures++;
   REQUIRE(image7(copy)==NULL);require_placement_count(copy,0);require_equal_state(source,copy);require_no_semantic_error(copy);
   state.fail_at=0;state.calls=0;
   REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==0);
   require_red_pixel(copy);require_placement_at(copy,4,2);
  }
  ghostty_terminal_free(copy);REQUIRE(state.base.live==0);
  if(completed)break;
 }
 REQUIRE(completed && failures>=3);free(canonical.data);free(resources.data);ghostty_terminal_free(source);
 printf("PASS allocation sweep/%zu failure positions, retry and release\n",failures);
}
#ifndef GHOSTERM_RESOURCE_SNAPSHOT_MAIN
#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN main
#endif
int GHOSTERM_RESOURCE_SNAPSHOT_MAIN(int argc,char **argv) {
  REQUIRE(argc==2);const char *mode=argv[1];
  if(strcmp(mode,"oom-sweep")==0)allocation_sweep();
  else if(strcmp(mode,"static")==0 || strcmp(mode,"screens")==0 || strcmp(mode,"placement-counter")==0)roundtrip(mode);
  else if(strcmp(mode,"rgb")==0 || strcmp(mode,"virtual")==0)format_and_virtual(mode);
  else if(strcmp(mode,"ids")==0)future_ids();else if(strcmp(mode,"age")==0 || strcmp(mode,"transient")==0)eviction_age(strcmp(mode,"transient")==0);
  else if(strcmp(mode,"loading")==0 || strcmp(mode,"animation")==0 || strcmp(mode,"relative")==0)unsupported(mode);
  else if(strcmp(mode,"crc")==0 || strcmp(mode,"late-error")==0 || strcmp(mode,"oom")==0 || strcmp(mode,"limit")==0)invalid(mode);
  else return 2;
  printf("PASS resource-snapshot/%s\n",mode);return 0;
}
