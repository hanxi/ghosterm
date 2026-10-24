#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN pending_static_prior_main
#include "resource-snapshot-probe.c"
#include <pthread.h>
static const uint8_t pending_red[4]={255,0,0,255},pending_blue[4]={0,0,255,255};
static void pending_equal(GhosttyTerminal a,GhosttyTerminal b) {
 require_equal_state(a,b);Bytes x=resource_capture(a),y=resource_capture(b);
 REQUIRE(x.len==y.len && memcmp(x.data,y.data,x.len)==0);free(x.data);free(y.data);
}
static void pending_assert(GhosttyTerminal t,size_t expected) {
 GhosttyKittyGraphicsImage image=image7(t);REQUIRE(image);const uint8_t *data=NULL;size_t n=0;
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&n)==GHOSTTY_SUCCESS && n==expected);
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&data)==GHOSTTY_NO_VALUE && data==NULL);
}
static void pending_pixels(GhosttyTerminal t,const uint8_t *expected,size_t n) {
 GhosttyKittyGraphicsImage image=image7(t);REQUIRE(image);const uint8_t *data=NULL;size_t len=0;
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&data)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&len)==GHOSTTY_SUCCESS && len==n && memcmp(data,expected,n)==0);
}
static uint64_t pending_begin(GhosttyTerminal t,bool rgb,uint32_t number) {
 uint64_t token=0;REQUIRE(ghostty_terminal_p0_pending_begin(t,7,number,1,1,rgb?3:4,&token)==0 && token!=0);
 pending_assert(t,rgb?3:4);return token;
}
typedef struct {pthread_mutex_t mutex;pthread_cond_t cond;bool release;uint8_t decoded[4];} PendingProducer;
static void *pending_produce(void *opaque) {
 PendingProducer *p=opaque;REQUIRE(pthread_mutex_lock(&p->mutex)==0);
 while(!p->release)REQUIRE(pthread_cond_wait(&p->cond,&p->mutex)==0);
 REQUIRE(pthread_mutex_unlock(&p->mutex)==0);
 PngAlloc state={0};const GhosttyAllocatorVtable table={png_alloc,png_resize,png_remap,png_free};
 GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySysImage image={0};
 REQUIRE(ghostty_p0_decode_png(NULL,&allocator,png_pixel,sizeof png_pixel,&image));
 REQUIRE(image.width==1 && image.height==1 && image.data_len==4 && image.data);
 REQUIRE(memcmp(image.data,pending_red,4)==0);memcpy(p->decoded,image.data,4);
 png_free(&state,image.data,image.data_len,0,0);REQUIRE(state.live==0);return NULL;
}
static void pending_roundtrip(const char *mode) {
 bool rgb=strcmp(mode,"rgb")==0,number=strcmp(mode,"number")==0;
 bool stale=strcmp(mode,"stale")==0,replaced=strcmp(mode,"replaced-pending")==0,deleted=strcmp(mode,"deleted")==0;
 bool cancel=strcmp(mode,"cancel")==0,async=strcmp(mode,"async")==0,async_cancel=strcmp(mode,"async-cancel")==0;
 bool frame=strcmp(mode,"frame")==0,frame_number=strcmp(mode,"frame-number")==0,frame_early=strcmp(mode,"frame-early")==0;
 GhosttyTerminal source=new_terminal();configure_images(source,".");uint64_t token=pending_begin(source,rgb,(number||frame_number)?9:0);
 feed(source,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");
 PendingProducer producer={.mutex=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};pthread_t thread;
 if(async||async_cancel)REQUIRE(pthread_create(&thread,NULL,pending_produce,&producer)==0);
 if(frame||frame_number||frame_early)feed(source,frame_number?"\x1b_Ga=f,I=9,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\":"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\");
 GhosttyTerminal copy=clone_resources(source);pending_equal(source,copy);pending_assert(copy,rgb?3:4);bool a=false,b=false;
 if(stale) {const char *replace="\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;AAD//w==\x1b\\";feed(source,replace);feed(copy,replace);}
 if(replaced) {uint64_t newer=pending_begin(source,false,0);REQUIRE(newer!=token);/* Import the new authority token rather than inventing a replica producer. */ghostty_terminal_free(copy);copy=clone_resources(source);pending_equal(source,copy);}
 if(deleted) {const char *remove="\x1b_Ga=d,d=I,i=7,q=2\x1b\\";feed(source,remove);feed(copy,remove);}
 if(cancel||async_cancel) {REQUIRE(ghostty_terminal_p0_pending_cancel(source,7,token,&a)==0 && a);REQUIRE(ghostty_terminal_p0_pending_cancel(copy,7,token,&b)==0 && b);pending_equal(source,copy);}
 if(frame_early) {feed(source,"\x1b_Gm=0;/w==\x1b\\");feed(copy,"\x1b_Gm=0;/w==\x1b\\");pending_assert(source,4);pending_assert(copy,4);pending_equal(source,copy);}
 if(strcmp(mode,"wrong-length")==0) {
  a=true;REQUIRE(ghostty_terminal_p0_pending_ready(copy,7,token,pending_red,3,&a)==1 && !a);pending_assert(copy,4);pending_equal(source,copy);
 }
 if(strcmp(mode,"zero-token")==0) {
  a=true;REQUIRE(ghostty_terminal_p0_pending_ready(copy,7,0,pending_red,4,&a)==1 && !a);pending_equal(source,copy);
 }
 const uint8_t *data=pending_red;
 if(async||async_cancel) {
  REQUIRE(pthread_mutex_lock(&producer.mutex)==0);producer.release=true;REQUIRE(pthread_cond_signal(&producer.cond)==0);REQUIRE(pthread_mutex_unlock(&producer.mutex)==0);
  REQUIRE(pthread_join(thread,NULL)==0);data=producer.decoded;
 }
 bool expected=!(stale||replaced||deleted||cancel||async_cancel);
 REQUIRE(ghostty_terminal_p0_pending_ready(source,7,token,data,rgb?3:4,&a)==0 && a==expected);
 REQUIRE(ghostty_terminal_p0_pending_ready(copy,7,token,data,rgb?3:4,&b)==0 && b==expected);pending_equal(source,copy);
 if(expected) {
  pending_pixels(source,data,rgb?3:4);pending_pixels(copy,data,rgb?3:4);
  REQUIRE(ghostty_terminal_p0_pending_ready(copy,7,token,data,rgb?3:4,&b)==0 && !b);
  REQUIRE(ghostty_terminal_p0_pending_cancel(copy,7,token,&b)==0 && !b);pending_equal(source,copy);
 }
 if(stale) {pending_pixels(source,pending_blue,4);pending_pixels(copy,pending_blue,4);}
 if(replaced) {pending_assert(source,4);pending_assert(copy,4);}
 if(deleted||cancel||async_cancel)REQUIRE(!image7(source) && !image7(copy));
 if(frame||frame_number) {
  const char *finish="\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\";feed(source,finish);feed(copy,finish);
  pending_pixels(source,pending_blue,4);pending_pixels(copy,pending_blue,4);pending_equal(source,copy);
 }
 if(async||async_cancel) {REQUIRE(pthread_cond_destroy(&producer.cond)==0);REQUIRE(pthread_mutex_destroy(&producer.mutex)==0);}
 require_no_semantic_error(source);require_no_semantic_error(copy);ghostty_terminal_free(copy);ghostty_terminal_free(source);
}
static void pending_limits(void) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");uint64_t limit=3,token=99;
 REQUIRE(ghostty_terminal_set(source,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_terminal_p0_pending_begin(source,7,0,1,1,4,&token)==4 && token==0 && !image7(source));
 limit=1024*1024;REQUIRE(ghostty_terminal_set(source,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);token=pending_begin(source,false,0);
 size_t written=99;REQUIRE(ghostty_terminal_p0_resources_encode(source,3,NULL,0,&written)==4 && written==0);pending_assert(source,4);
 bool applied=false;REQUIRE(ghostty_terminal_p0_pending_ready(source,7,token,pending_red,4,&applied)==0 && applied);ghostty_terminal_free(source);
}
#ifndef GHOSTERM_PENDING_MAIN
#define GHOSTERM_PENDING_MAIN main
#endif
int GHOSTERM_PENDING_MAIN(int argc,char **argv) {
 REQUIRE(argc==2);if(strcmp(argv[1],"static")==0)return pending_static_prior_main(argc,argv);
 if(strcmp(argv[1],"limit")==0)pending_limits();else pending_roundtrip(argv[1]);
 printf("PASS pending-resource/%s\n",argv[1]);return 0;
}
