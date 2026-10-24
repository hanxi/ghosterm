#define main vt_baseline_main
#include "probe.c"
#undef main
#include <errno.h>

static void configure_images(GhosttyTerminal t,const char *dir) {
  REQUIRE(ghostty_terminal_resize(t,80,24,8,16)==GHOSTTY_SUCCESS);
  uint64_t limit=1024*1024;
  GhosttyString directory={.ptr=(const uint8_t *)dir,.len=strlen(dir)};
  REQUIRE(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE,&directory)==GHOSTTY_SUCCESS);
}
static GhosttyKittyGraphicsImage image7(GhosttyTerminal t) {
  GhosttyKittyGraphics graphics=NULL;
  REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);
  return ghostty_kitty_graphics_image(graphics,7);
}
static void require_red_pixel(GhosttyTerminal t) {
  GhosttyKittyGraphicsImage image=image7(t);REQUIRE(image!=NULL);
  const uint8_t *data=NULL;size_t len=0;
  REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&data)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&len)==GHOSTTY_SUCCESS);
  const uint8_t red[]={255,0,0,255};REQUIRE(len==sizeof red && memcmp(data,red,sizeof red)==0);
}
static void require_placement_at(GhosttyTerminal t,int32_t col,int32_t row) {
  GhosttyKittyGraphics graphics=NULL;
  REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);
  GhosttyKittyGraphicsPlacementIterator iter=NULL;
  REQUIRE(ghostty_kitty_graphics_placement_iterator_new(NULL,&iter)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_get(graphics,GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,&iter)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_placement_next(iter));
  uint32_t id=0;
  REQUIRE(ghostty_kitty_graphics_placement_get(iter,GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID,&id)==GHOSTTY_SUCCESS && id==7);
  int32_t actual_col=-1,actual_row=-1;
  REQUIRE(ghostty_kitty_graphics_placement_viewport_pos(iter,image7(t),t,&actual_col,&actual_row)==GHOSTTY_SUCCESS);
  REQUIRE(actual_col==col && actual_row==row);
  REQUIRE(!ghostty_kitty_graphics_placement_next(iter));
  ghostty_kitty_graphics_placement_iterator_free(iter);
}
static void base64(const uint8_t *text,size_t len,char *encoded,size_t cap) {
  const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t out=0;
  REQUIRE(((len+2)/3)*4+1<=cap);
  for(size_t i=0;i<len;i+=3) {
    unsigned a=(uint8_t)text[i],b=i+1<len?(uint8_t)text[i+1]:0,c=i+2<len?(uint8_t)text[i+2]:0;
    encoded[out++]=alphabet[a>>2];encoded[out++]=alphabet[((a&3)<<4)|(b>>4)];
    encoded[out++]=i+1<len?alphabet[((b&15)<<2)|(c>>6)]:'=';
    encoded[out++]=i+2<len?alphabet[c&63]:'=';
  }
  encoded[out]=0;
}
static void require_no_semantic_error(GhosttyTerminal t) {
  bool failed=true;
  REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed)==GHOSTTY_SUCCESS);
  REQUIRE(!failed);
}
static int temporary_file_case(const char *dir,bool resolved) {
  char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/tty-graphics-protocol-XXXXXX",dir)<(int)sizeof path);
  int fd=mkstemp(path);REQUIRE(fd>=0);
  const uint8_t red[]={255,0,0,255};fd_write_all(fd,red,sizeof red);REQUIRE(close(fd)==0);
  GhosttyTerminal authority=new_terminal();configure_images(authority,dir);
  Bytes initial=capture(authority);GhosttyTerminal replica=restore(initial);configure_images(replica,dir);
  require_equal_state(authority,replica);
  char encoded[8192],command[9000];base64((const uint8_t *)path,strlen(path),encoded,sizeof encoded);
  REQUIRE(snprintf(command,sizeof command,"\x1b[3;5H\x1b_Ga=T,i=7,f=32,s=1,v=1,t=t,q=2;%s\x1b\\\x1b[4;6H",encoded)<(int)sizeof command);
  feed(authority,command);
  require_red_pixel(authority);
  require_placement_at(authority,4,2);
  REQUIRE(access(path,F_OK)<0 && errno==ENOENT);
  if(resolved) {
    GhosttyKittyGraphicsImage image=image7(authority);
    const uint8_t *pixels=NULL;size_t length=0;
    REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels)==GHOSTTY_SUCCESS);
    REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&length)==GHOSTTY_SUCCESS);
    base64(pixels,length,encoded,sizeof encoded);
    char normalized[9000];
    REQUIRE(snprintf(normalized,sizeof normalized,"\x1b[3;5H\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;%s\x1b\\\x1b[4;6H",encoded)<(int)sizeof normalized);
    feed(replica,normalized);
    require_red_pixel(replica);
    require_placement_at(replica,4,2);
  } else feed(replica,command);
  /* Both parsers reached the same cursor/state: replay really happened. */
  require_equal_state(authority,replica);
  require_no_semantic_error(authority);require_no_semantic_error(replica);
  bool missing=image7(replica)==NULL;
  ghostty_terminal_free(authority);ghostty_terminal_free(replica);free(initial.data);
  if(resolved) puts("PASS resource-io/resolved source pixels restore replica without rereading consumed file");
  if(missing) {
    puts("LIMITATION resource-io/temporary file consumed once; raw replica replay loses image without semantic error");return 3;
  }
  return 0;
}
static void inline_case(void) {
  GhosttyTerminal authority=new_terminal(),replica=new_terminal();
  const uint64_t limit=1024*1024;
  REQUIRE(ghostty_terminal_set(authority,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_set(replica,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
  const char *inline_data="\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\";
  feed(authority,inline_data);feed(replica,inline_data);
  require_red_pixel(authority);require_red_pixel(replica);
  ghostty_terminal_free(authority);ghostty_terminal_free(replica);
  puts("PASS resource-io/inline RGBA pixels match in both parsers");
}
#ifndef GHOSTERM_RESOURCE_IO_MAIN
#define GHOSTERM_RESOURCE_IO_MAIN main
#endif
int GHOSTERM_RESOURCE_IO_MAIN(int argc,char **argv) {
  REQUIRE(argc==3);
  if(strcmp(argv[1],"temporary")==0)return temporary_file_case(argv[2],false);
  if(strcmp(argv[1],"resolved")==0)return temporary_file_case(argv[2],true);
  if(strcmp(argv[1],"inline")==0){inline_case();return 0;}
  return 2;
}
