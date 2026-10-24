#define GHOSTERM_RESOURCE_HOOK_MAIN hooks_prior_main
#include "resource-hooks-probe.c"
static void require_relative_position(GhosttyTerminal t,uint32_t placement,int32_t x,int32_t y) {
 GhosttyKittyGraphics g=NULL;GhosttyKittyGraphicsPlacementIterator it=NULL;
 REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&g)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_kitty_graphics_placement_iterator_new(NULL,&it)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_kitty_graphics_get(g,GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,&it)==GHOSTTY_SUCCESS);
 bool found=false;while(ghostty_kitty_graphics_placement_next(it)) {
  uint32_t id=0;REQUIRE(ghostty_kitty_graphics_placement_get(it,GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_PLACEMENT_ID,&id)==GHOSTTY_SUCCESS);
  if(id!=placement)continue;int32_t actual_x=-1,actual_y=-1;
  REQUIRE(ghostty_kitty_graphics_placement_viewport_pos(it,image7(t),t,&actual_x,&actual_y)==GHOSTTY_SUCCESS);
  REQUIRE(actual_x==x && actual_y==y);found=true;
 }
 REQUIRE(found);ghostty_kitty_graphics_placement_iterator_free(it);
}
static uint64_t word(const uint8_t *p) {uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static uint32_t crc(const uint8_t *p,size_t n) {uint32_t c=~0u;for(size_t i=0;i<n;i++) {c^=p[i];for(unsigned j=0;j<8;j++)c=(c>>1)^((0u-(c&1u))&0x82f63b78u);}return ~c;}
static void malformed(GhosttyTerminal source,Bytes canonical,const uint8_t *valid,size_t n,bool cycle) {
 REQUIRE(n>=80 && memcmp(valid,"GTIR0002",8)==0 && word(valid+16)==1);
 uint64_t images=word(valid+64),placements=word(valid+72);REQUIRE(images==1 && placements==2);
 size_t offset=80;for(uint64_t i=0;i<images;i++) {REQUIRE(offset+56<=n);uint64_t len=word(valid+offset+48);REQUIRE(len<=n-offset-56);offset+=56+(size_t)len;}
 size_t parent=0;
 for(uint64_t i=0;i<placements;i++) {
  REQUIRE(offset+120<=n);uint64_t location=word(valid+offset+24);
  if(location==2 && word(valid+offset+16)==10)parent=offset+40;
  offset+=120+(location==2?24:0);REQUIRE(offset<=n);
 }
 REQUIRE(parent && offset==n);uint8_t *bad=malloc(n);REQUIRE(bad);memcpy(bad,valid,n);
 uint64_t value=cycle?10:99;for(unsigned i=0;i<8;i++)bad[parent+i]=(uint8_t)(value>>(8*i));
 uint32_t checksum=crc(bad+16,n-16);for(unsigned i=0;i<4;i++)bad[12+i]=(uint8_t)(checksum>>(8*i));
 PngAlloc state={0};const GhosttyAllocatorVtable table={png_alloc,png_resize,png_remap,png_free};GhosttyAllocator allocator={.ctx=&state,.vtable=&table};
 GhosttySnapshotDecoder d=NULL;GhosttyTerminal t=NULL;REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&d,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_decoder_decode(d,&t)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(d);size_t before=state.live;state.deny=cycle;
 REQUIRE(ghostty_terminal_p0_resources_decode(t,bad,n,1024*1024)==1);REQUIRE(state.denied==0 && state.live==before && !image7(t));
 state.deny=false;require_equal_state(source,t);REQUIRE(ghostty_terminal_p0_resources_decode(t,valid,n,1024*1024)==0);
 require_relative_position(t,10,6,5);ghostty_terminal_free(t);REQUIRE(state.live==0);free(bad);
}
#ifndef GHOSTERM_RELATIVE_RESOURCE_MAIN
#define GHOSTERM_RELATIVE_RESOURCE_MAIN main
#endif
int GHOSTERM_RELATIVE_RESOURCE_MAIN(int argc,char **argv) {
 REQUIRE(argc==2);
 if(strcmp(argv[1],"static") && strcmp(argv[1],"relative") && strcmp(argv[1],"chain") && strcmp(argv[1],"forward") && strcmp(argv[1],"internal") && strcmp(argv[1],"missing-parent") && strcmp(argv[1],"cycle"))return 2;
 bool plain=strcmp(argv[1],"static")==0;bool chain=strcmp(argv[1],"chain")==0 || strcmp(argv[1],"forward")==0;unsigned root=strcmp(argv[1],"forward")==0?90:strcmp(argv[1],"internal")==0?0:9;char command[1024];
 GhosttyTerminal source=new_terminal();configure_images(source,".");
 REQUIRE(snprintf(command,sizeof command,"\x1b[3;5H\x1b_Ga=T,i=7,p=%u,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\",root)<(int)sizeof command);feed(source,command);
 if(!plain) {REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=p,i=7,p=10,P=7,Q=%u,H=2,V=3,q=2\x1b\\",root)<(int)sizeof command);feed(source,command);}
 if(chain)feed(source,"\x1b_Ga=p,i=7,p=5,P=7,Q=10,H=1,V=-1,q=2\x1b\\");
 if(!plain)require_relative_position(source,10,6,5);if(chain)require_relative_position(source,5,7,4);
 Bytes canonical=capture(source);GhosttyTerminal copy=restore(canonical);size_t n=0;
 REQUIRE(ghostty_terminal_p0_resources_encode(source,1024*1024,NULL,0,&n)==5);uint8_t *resources=malloc(n);REQUIRE(resources);
 REQUIRE(ghostty_terminal_p0_resources_encode(source,1024*1024,resources,n,&n)==0);
 if(strcmp(argv[1],"missing-parent")==0 || strcmp(argv[1],"cycle")==0) {
  malformed(source,canonical,resources,n,strcmp(argv[1],"cycle")==0);
  free(resources);free(canonical.data);ghostty_terminal_free(copy);ghostty_terminal_free(source);printf("PASS relative snapshot/%s\n",argv[1]);return 0;
 }
 REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources,n,1024*1024)==0);require_red_pixel(copy);
 if(!plain)require_relative_position(copy,10,6,5);if(chain)require_relative_position(copy,5,7,4);require_equal_state(source,copy);
 REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=d,d=i,i=7,p=%u,q=2\x1b\\",root)<(int)sizeof command);feed(source,command);feed(copy,command);
 for(unsigned i=0;i<2;i++) {
  GhosttyTerminal t=i?copy:source;GhosttyKittyGraphics g=NULL;GhosttyKittyGraphicsPlacementIterator it=NULL;
  REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&g)==GHOSTTY_SUCCESS);REQUIRE(ghostty_kitty_graphics_placement_iterator_new(NULL,&it)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_get(g,GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,&it)==GHOSTTY_SUCCESS);REQUIRE(!ghostty_kitty_graphics_placement_next(it));ghostty_kitty_graphics_placement_iterator_free(it);
 }
 free(resources);free(canonical.data);ghostty_terminal_free(copy);ghostty_terminal_free(source);printf("PASS relative snapshot/%s\n",argv[1]);return 0;
}
