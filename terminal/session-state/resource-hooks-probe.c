#define GHOSTERM_RESOURCE_IO_MAIN resource_io_baseline_main
#include "resource-io-probe.c"
#include <fcntl.h>
#ifdef GHOSTERM_P0_BUILTIN_PNG
static const uint8_t png_pixel[]={0x89,0x50,0x4e,0x47,0xd,0xa,0x1a,0xa,0x0,0x0,0x0,0xd,0x49,0x48,0x44,0x52,0x0,0x0,0x0,0x1,0x0,0x0,0x0,0x1,0x8,0x6,0x0,0x0,0x0,0x1f,0x15,0xc4,0x89,0x0,0x0,0x0,0xd,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0xf0,0x1f,0x0,0x5,0x0,0x1,0xff,0x89,0x99,0x3d,0x1d,0x0,0x0,0x0,0x0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
#endif
typedef struct {
  bool present,reject;
  unsigned observed,resolved;
  uint32_t medium,status;
  uint8_t path[4096],data[4096];
  size_t path_len,data_len;
} ResourceEvent;
static bool observe_resource(void *ctx,uint32_t medium,const uint8_t *path,size_t path_len,
                             uint32_t status,const uint8_t *data,size_t len) {
  ResourceEvent *event=ctx;event->observed++;
  if(event->reject || event->present || path_len>sizeof event->path || len>sizeof event->data)return false;
  memcpy(event->path,path,path_len);if(len)memcpy(event->data,data,len);
  event->path_len=path_len;event->data_len=len;event->medium=medium;event->status=status;event->present=true;
  return true;
}
static bool resolve_resource(void *ctx,uint32_t medium,const uint8_t *path,size_t path_len,
                             uint32_t *status,const uint8_t **data,size_t *len) {
  ResourceEvent *event=ctx;event->resolved++;
  if(!event->present || medium!=event->medium || path_len!=event->path_len || memcmp(path,event->path,path_len)!=0)return false;
  *status=event->status;*data=event->data;*len=event->data_len;event->present=false;
  return true;
}
static bool semantic_error(GhosttyTerminal t) {
  bool failed=false;REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed)==GHOSTTY_SUCCESS);return failed;
}
static void test_hooks(const char *directory,const char *mode) {
  char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/tty-graphics-protocol-XXXXXX",directory)<(int)sizeof path);
  int fd=mkstemp(path);REQUIRE(fd>=0);
  const uint8_t red[]={255,0,0,255};
  bool file_case=strcmp(mode,"file")==0;
#ifdef GHOSTERM_P0_BUILTIN_PNG
  bool png_case=strcmp(mode,"png")==0;
  fd_write_all(fd,png_case?png_pixel:red,png_case?sizeof png_pixel:sizeof red);
#else
  const bool png_case=false;
  fd_write_all(fd,red,sizeof red);
#endif
  REQUIRE(close(fd)==0);
  bool source_failure=strcmp(mode,"source-failure")==0;
  bool reject=strcmp(mode,"observe-reject")==0;
  bool mismatch=strcmp(mode,"resolve-mismatch")==0;
  bool duplicate=strcmp(mode,"duplicate")==0;
  if(source_failure)REQUIRE(unlink(path)==0);
  GhosttyTerminal authority=new_terminal();configure_images(authority,directory);
  if(file_case) {const bool allowed=true;REQUIRE(ghostty_terminal_set(authority,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE,&allowed)==GHOSTTY_SUCCESS);}
  Bytes initial=capture(authority);GhosttyTerminal replica=restore(initial);
  const uint64_t limit=1024*1024;
  REQUIRE(ghostty_terminal_set(replica,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_set(replica,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE,NULL)==GHOSTTY_SUCCESS);
  ResourceEvent event={.reject=reject};
  REQUIRE(ghostty_terminal_p0_resource_hooks(authority,&event,observe_resource,NULL)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_p0_resource_hooks(replica,&event,NULL,resolve_resource)==GHOSTTY_SUCCESS);
  char encoded[8192],command[9000];base64((const uint8_t*)path,strlen(path),encoded,sizeof encoded);
  REQUIRE(snprintf(command,sizeof command,"\x1b[3;5H\x1b_Ga=T,i=7,f=%u,s=1,v=1,t=%c,q=2;%s\x1b\\\x1b[4;6H",png_case?100u:32u,file_case?'f':'t',encoded)<(int)sizeof command);
  feed(authority,command);
  REQUIRE(event.observed==1);
  if(file_case) {
    REQUIRE(access(path,F_OK)==0);
    int overwrite=open(path,O_WRONLY|O_TRUNC);REQUIRE(overwrite>=0);
    const uint8_t green[]={0,255,0,255};fd_write_all(overwrite,green,sizeof green);REQUIRE(close(overwrite)==0);
  } else REQUIRE(access(path,F_OK)<0 && errno==ENOENT);
  if(reject) {
    REQUIRE(semantic_error(authority));REQUIRE(image7(authority)==NULL);
  } else {
    REQUIRE(!semantic_error(authority));
    REQUIRE(event.present && event.status==(source_failure?1u:0u));
    if(!source_failure) {require_red_pixel(authority);require_placement_at(authority,4,2);}
    if(mismatch)event.path[0]^=1;
    feed(replica,command);
    REQUIRE(event.resolved==1);
    if(mismatch) {REQUIRE(semantic_error(replica));REQUIRE(image7(replica)==NULL);}
    else {
      REQUIRE(!event.present);REQUIRE(!semantic_error(replica));
      require_equal_state(authority,replica);
      if(source_failure)REQUIRE(image7(authority)==NULL && image7(replica)==NULL);
      else {require_red_pixel(replica);require_placement_at(replica,4,2);}
      if(duplicate) {
        feed(replica,command);
        REQUIRE(event.resolved==2);REQUIRE(semantic_error(replica));
      }
    }
  }
  if(file_case) REQUIRE(unlink(path)==0);
  free(initial.data);ghostty_terminal_free(replica);ghostty_terminal_free(authority);
  printf("PASS resource-hooks/%s\n",mode);
}
#ifdef GHOSTERM_P0_BUILTIN_PNG
typedef struct {bool deny;size_t live,denied;} PngAlloc;
static void *png_alloc(void *ctx,size_t len,uint8_t alignment,uintptr_t ret) {
  (void)ret;PngAlloc *a=ctx;
  if(a->deny) {a->denied++;return NULL;}
  REQUIRE(alignment<sizeof(size_t)*8);
  size_t align=(size_t)1<<alignment;if(align<sizeof(void*))align=sizeof(void*);
  void *p=NULL;if(posix_memalign(&p,align,len)!=0)return NULL;a->live+=len;return p;
}
static bool png_resize(void *ctx,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {
  (void)ctx;(void)p;(void)n;(void)a;(void)m;(void)r;return false;
}
static void *png_remap(void *ctx,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {
  (void)ctx;(void)p;(void)n;(void)a;(void)m;(void)r;return NULL;
}
static void png_free(void *ctx,void *p,size_t n,uint8_t alignment,uintptr_t ret) {
  (void)alignment;(void)ret;PngAlloc *a=ctx;REQUIRE(a->live>=n);a->live-=n;free(p);
}
static void test_png_failure(bool denied) {
  PngAlloc state={.deny=denied};
  const GhosttyAllocatorVtable table={png_alloc,png_resize,png_remap,png_free};
  GhosttyAllocator allocator={.ctx=&state,.vtable=&table};
  GhosttySysImage image={.width=99,.height=99,.data=NULL,.data_len=99};
  const uint8_t bad[]="not a PNG";
  REQUIRE(!ghostty_p0_decode_png(NULL,&allocator,denied?png_pixel:bad,
                               denied?sizeof png_pixel:sizeof bad-1,&image));
  REQUIRE(image.width==0 && image.height==0 && image.data==NULL && image.data_len==0);
  REQUIRE(state.live==0);
  if(denied)REQUIRE(state.denied>0);
  printf("PASS resource-hooks/png-%s\n",denied?"oom":"invalid");
}
#endif

#ifndef GHOSTERM_RESOURCE_HOOK_MAIN
#define GHOSTERM_RESOURCE_HOOK_MAIN main
#endif
int GHOSTERM_RESOURCE_HOOK_MAIN(int argc,char **argv) {
  REQUIRE(argc==3);
  if(strcmp(argv[1],"inline")==0 || strcmp(argv[1],"resolved")==0 || strcmp(argv[1],"temporary")==0)
    return resource_io_baseline_main(argc,argv);
#ifdef GHOSTERM_P0_BUILTIN_PNG
  if(strcmp(argv[1],"png-invalid")==0) {test_png_failure(false);return 0;}
  if(strcmp(argv[1],"png-oom")==0) {test_png_failure(true);return 0;}
  if(strcmp(argv[1],"png")==0) REQUIRE(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void*)ghostty_p0_decode_png)==GHOSTTY_SUCCESS);
#endif
  test_hooks(argv[2],argv[1]);
  return 0;
}
