#include "extension/memory.h"
#include "extension/frame.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL memory:%d %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct {size_t calls;bool fail,wiped,charged;} observer_t;
static void *allocate(void *opaque,size_t n){observer_t *o=opaque;o->calls++;return o->fail?NULL:malloc(n);}
static void release(void *opaque,void *p,size_t n){
 observer_t *o=opaque;o->wiped=true;for(size_t i=0;i<n;i++)if(((unsigned char *)p)[i])o->wiped=false;
 o->charged=gt__plugin_memory_info().used>=n;free(p);
}
static void *race(void *opaque){
 bool *ok=opaque;*ok=true;for(unsigned i=0;i<2000;i++){
  void *p=gt__plugin_memory_alloc(NULL,256);gt_plugin_memory_info_t info=gt__plugin_memory_info();
  if(info.used>info.limit || info.peak>info.limit)*ok=false;
  if(p){memset(p,0xa5,256);gt__plugin_memory_free(NULL,p,256);}
 }return NULL;
}
int main(int argc,char **argv){
 const char *mode=argc>1?argv[1]:"budget";CHECK(!gt__plugin_memory_info().used && !gt__plugin_memory_limit(0));
 if(!strcmp(mode,"race")){
  CHECK(gt__plugin_memory_limit(1024));pthread_t threads[8];bool ok[8];
  for(unsigned i=0;i<8;i++)CHECK(!pthread_create(&threads[i],NULL,race,&ok[i]));
  for(unsigned i=0;i<8;i++)CHECK(!pthread_join(threads[i],NULL) && ok[i]);
 }else if(!strcmp(mode,"queue")){
  /* A real frame buffer and its owner share the same quota as other owners.
   * A failed second constructor must return its partial reservation. */
  CHECK(gt__plugin_memory_limit(4096));gt_bridge_decoder_t *decoder=NULL;CHECK(gt__bridge_decoder_new(2048,NULL,&decoder)==GT_OK);
  size_t used=gt__plugin_memory_info().used;CHECK(used>=2048);gt_bridge_decoder_t *other=(void *)1;
  CHECK(gt__bridge_decoder_new(2048,NULL,&other)==GT_ERR_IO && !other && gt__plugin_memory_info().used==used);
  gt__bridge_decoder_free(decoder);
 }else{
  CHECK(!strcmp(mode,"budget") && gt__plugin_memory_limit(1024));observer_t o={0};gt_allocator_t a={&o,allocate,release};
  void *p=gt__plugin_memory_alloc(&a,600);CHECK(p && o.calls==1 && gt__plugin_memory_info().used==600);memset(p,0xa5,600);
  CHECK(!gt__plugin_memory_limit(2048) && gt__plugin_memory_info().limit==1024);
  CHECK(!gt__plugin_memory_alloc(&a,425) && o.calls==1 && gt__plugin_memory_info().used==600);
  CHECK(!gt__plugin_memory_alloc(&a,SIZE_MAX) && !gt__plugin_memory_calloc(SIZE_MAX,2));
  o.fail=true;CHECK(!gt__plugin_memory_alloc(&a,424) && gt__plugin_memory_info().used==600);
  gt__plugin_memory_free(&a,p,600);CHECK(o.wiped && o.charged && !gt__plugin_memory_info().used);
  p=gt__plugin_memory_calloc(2,512);CHECK(p);for(size_t i=0;i<1024;i++)CHECK(!((unsigned char *)p)[i]);
  CHECK(!gt__plugin_memory_alloc(NULL,1));gt__plugin_memory_free(NULL,p,1024);
 }
 gt_plugin_memory_info_t info=gt__plugin_memory_info();CHECK(!info.used && info.peak && info.peak<=info.limit);
 CHECK(gt__plugin_memory_limit(GT_PLUGIN_MEMORY_DEFAULT));printf("PASS plugin memory %s\n",mode);return 0;
}
