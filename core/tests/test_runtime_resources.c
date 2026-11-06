#define EXT_SOURCE_MAIN runtime_resource_source_fixture_main
#include "test_ext_source.c"
#include "extension/runtime-resource.h"
static uint64_t resource_clock_ms=100;
static bool resource_clock_valid=true;
static bool resource_clock(void *context,uint64_t *out){(void)context;if(resource_clock_valid)*out=resource_clock_ms;return resource_clock_valid;}
static gt_runtime_resources_t *resource_owner(gt_ext_session_t *s,gt_ext_resource_store_t *store,const gt_ext_lease_t *lease,int fd){
 gt_runtime_root_t root={"workspace",fd,262144};gt_runtime_resources_t *r=NULL;CHECK(gt__runtime_resources_open(s,store,lease,fd>=0?&root:NULL,fd>=0?1:0,NULL,resource_clock,&r)==GT_OK && r);return r;
}
static gt_runtime_publication_t publication(gt_runtime_resources_t *r,uint64_t id){gt_runtime_publication_t p;CHECK(gt__runtime_resources_publication(r,id,&p)==GT_RUNTIME_RESOURCE_OK);return p;}
static uint64_t publish(gt_runtime_resources_t *r){uint64_t id=0;CHECK(gt__runtime_resources_publish(r,"workspace","file.bin","application/octet-stream",262144,10000,&id)==GT_RUNTIME_RESOURCE_OK && id);return id;}
static void resource_cycle(pub_source_fixture_t *f,gt_runtime_resources_t *a,gt_runtime_resources_t *b){rpc_pump(&f->pair.rpc);gt__runtime_resources_step(a);gt__runtime_resources_step(b);}
static uint64_t resource_complete(pub_source_fixture_t *f,gt_runtime_resources_t *sender,gt_runtime_resources_t *receiver,uint64_t id,size_t size){
 uint64_t prior=0;for(unsigned i=0;i<10000;i++){resource_cycle(f,sender,receiver);gt_runtime_publication_t p=publication(sender,id);CHECK(p.source.read_bytes-prior<=16384);prior=p.source.read_bytes;if(p.source.phase==GT_EXT_SOURCE_SUCCEEDED)break;}
 gt_runtime_publication_t p=publication(sender,id);CHECK(p.source.phase==GT_EXT_SOURCE_SUCCEEDED && p.source.confirmed && p.source.accepted==size && p.source.read_bytes==size && p.source.transfer_state==GT_EXT_TRANSFER_SUCCEEDED && !p.expired && !p.clock_error);
 gt_ext_resource_descriptor_t d;uint64_t ref=0;CHECK(gt__runtime_resources_take(receiver,&ref,&d)==GT_RUNTIME_RESOURCE_OK && ref && d.size_known && d.size==size && strlen(d.resource_id)==32 && d.generation);
 CHECK(gt__runtime_resources_forget(sender,id)==GT_RUNTIME_RESOURCE_OK);CHECK(gt__runtime_resources_publication(sender,id,&p)==GT_RUNTIME_RESOURCE_INVALID && !p.source.has_transfer);return ref;
}
static void verify_resource(pub_source_fixture_t *f,gt_runtime_resources_t *r,uint64_t ref,size_t size,const char *mode){
 uint8_t bytes[16384];size_t read=99;CHECK(gt__runtime_resources_read(r,ref,0,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_INVALID && !read);
 CHECK(gt__runtime_resources_begin_read(r,ref,size?size-1:16777217,1000)==GT_RUNTIME_RESOURCE_LIMIT);
 CHECK(gt__runtime_resources_begin_read(r,ref,size,1000)==GT_RUNTIME_RESOURCE_OK && gt__runtime_resources_begin_read(r,ref,size,1000)==GT_RUNTIME_RESOURCE_BUSY);
 if(!strcmp(mode,"read-expired")){resource_clock_ms+=1000;CHECK(gt__runtime_resources_read(r,ref,0,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_EXPIRED && !read);resource_clock_ms=100;CHECK(gt__runtime_resources_read(r,ref,0,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_EXPIRED);return;}
 if(!strcmp(mode,"read-clock")){resource_clock_valid=false;CHECK(gt__runtime_resources_read(r,ref,0,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_IO && !read);resource_clock_valid=true;CHECK(gt__runtime_resources_read(r,ref,0,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_IO);return;}
 if(!strcmp(mode,"revoked")){gt_ext_session_revoke(f->pair.rpc.pair.e);CHECK(gt__runtime_resources_read(r,ref,0,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_CLOSED && !read);return;}
 size_t at=0;for(unsigned steps=0;steps<1000;steps++){
  gt__runtime_resources_step(r);CHECK(gt__runtime_resources_read(r,ref,at+1,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_INVALID && !read);
  CHECK(gt__runtime_resources_read(r,ref,at,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_OK);for(size_t i=0;i<read;i++)CHECK(bytes[i]==(size==3?(uint8_t)"abc"[at+i]:(uint8_t)((at+i)%251)));at+=read;
  if(!read){CHECK(at==size && gt__runtime_resources_read(r,ref,at,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_CLOSED);break;}
  if(size>=16384)CHECK(gt__runtime_resources_read(r,ref,at,bytes,sizeof(bytes),&read)==GT_RUNTIME_RESOURCE_WAIT && !read);
 }CHECK(at==size);
}
#ifndef RUNTIME_RESOURCE_MAIN
#define RUNTIME_RESOURCE_MAIN main
#endif
int RUNTIME_RESOURCE_MAIN(int argc,char **argv){
 CHECK(argc==3);res_root=argv[2];const char *mode=argv[1];size_t before=ext_fds();pub_source_fixture_t f={.root=-1};pub_pair(&f.pair,!strcmp(mode,"ungranted")?"ungranted":"roundtrip");size_t size=!strcmp(mode,"large") || !strcmp(mode,"close-sent") || !strcmp(mode,"fairness")?262144:!strcmp(mode,"empty")?0:3;pub_file(&f,size,"file.bin");
 gt_runtime_resources_t *sender=resource_owner(f.pair.rpc.pair.h,f.pair.h.s,&f.pair.rpc.h,f.root),*receiver=resource_owner(f.pair.rpc.pair.e,f.pair.e.s,&f.pair.rpc.e,-1);
 CHECK(gt__runtime_resources_roots(sender)==1 && !strcmp(gt__runtime_resources_root(sender,0),"workspace") && !gt__runtime_resources_root(sender,1));
 struct stat root_st;CHECK(!fstat(f.root,&root_st));unsigned roots=0;for(int fd=3;fd<1024;fd++){struct stat st;if(fd!=f.root && !fstat(fd,&st) && st.st_dev==root_st.st_dev && st.st_ino==root_st.st_ino){CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);roots++;}}CHECK(roots==1);
 if(!strcmp(mode,"invalid")){
  gt_runtime_resources_t *bad=(void *)1;gt_runtime_root_t roots[2]={{"workspace",f.root,1},{"workspace",f.root,1}};
  CHECK(gt__runtime_resources_open(f.pair.rpc.pair.h,NULL,&f.pair.rpc.h,NULL,0,NULL,resource_clock,&bad)==GT_ERR_UNSUPPORTED && !bad);
  CHECK(gt__runtime_resources_open(f.pair.rpc.pair.h,f.pair.h.s,&f.pair.rpc.h,roots,2,NULL,resource_clock,&bad)==GT_ERR_INVALID_ARG && !bad);
  roots[0].id="../workspace";CHECK(gt__runtime_resources_open(f.pair.rpc.pair.h,f.pair.h.s,&f.pair.rpc.h,roots,1,NULL,resource_clock,&bad)==GT_ERR_INVALID_ARG && !bad);
  roots[0]=(gt_runtime_root_t){"workspace",f.root,16777217};CHECK(gt__runtime_resources_open(f.pair.rpc.pair.h,f.pair.h.s,&f.pair.rpc.h,roots,1,NULL,resource_clock,&bad)==GT_ERR_INVALID_ARG && !bad);
  roots[0]=(gt_runtime_root_t){"workspace",f.root,0};CHECK(gt__runtime_resources_open(f.pair.rpc.pair.h,f.pair.h.s,&f.pair.rpc.h,roots,1,NULL,resource_clock,&bad)==GT_ERR_INVALID_ARG && !bad);
  uint64_t id=99;CHECK(gt__runtime_resources_publish(sender,"other","file.bin","application/octet-stream",3,1000,&id)==GT_RUNTIME_RESOURCE_DENIED && !id);CHECK(gt__runtime_resources_publish(sender,"workspace","file.bin","application/octet-stream",262145,1000,&id)==GT_RUNTIME_RESOURCE_LIMIT && !id);
 }else if(!strcmp(mode,"parent") || !strcmp(mode,"ungranted") || !strcmp(mode,"symlink")){
  if(!strcmp(mode,"symlink")){CHECK(!unlink(f.file) && !symlink("unread-target",f.file));}
  uint64_t id=99;gt_runtime_resource_status_t status=gt__runtime_resources_publish(sender,"workspace",!strcmp(mode,"parent")?"../file.bin":"file.bin","application/octet-stream",3,1000,&id);CHECK(status==(!strcmp(mode,"parent")?GT_RUNTIME_RESOURCE_DENIED:GT_RUNTIME_RESOURCE_UNSUPPORTED) && !id);
 }else if(!strcmp(mode,"capacity")){
  uint64_t ids[16];for(unsigned i=0;i<16;i++)ids[i]=publish(sender);uint64_t id=99;CHECK(gt__runtime_resources_publish(sender,"workspace","file.bin","application/octet-stream",3,1000,&id)==GT_RUNTIME_RESOURCE_LIMIT && !id);CHECK(gt__runtime_resources_forget(sender,ids[0])==GT_RUNTIME_RESOURCE_WAIT);
  CHECK(gt__runtime_resources_cancel(sender,ids[0])==GT_RUNTIME_RESOURCE_OK && gt__runtime_resources_forget(sender,ids[0])==GT_RUNTIME_RESOURCE_OK);id=publish(sender);CHECK(id>ids[15]);
 }else if(!strcmp(mode,"fairness")){
  uint64_t a=publish(sender),b=publish(sender),prior=0;for(unsigned i=0;i<100;i++){
   resource_cycle(&f,sender,receiver);gt_runtime_publication_t x=publication(sender,a),y=publication(sender,b);uint64_t total=x.source.hashed+x.source.read_bytes+y.source.hashed+y.source.read_bytes;CHECK(total>=prior && total-prior<=16384);prior=total;
  }CHECK(publication(sender,a).source.hashed==size && publication(sender,b).source.hashed==size);
 }else{
  uint64_t id=publish(sender);CHECK(!close(f.root));f.root=-1; /* owner copied the root */
  if(!strcmp(mode,"expired") || !strcmp(mode,"clock")){
   if(!strcmp(mode,"expired"))resource_clock_ms+=10000;else resource_clock_valid=false;gt__runtime_resources_step(sender);gt_runtime_publication_t p=publication(sender,id);CHECK(p.source.phase==GT_EXT_SOURCE_STOPPED && !p.source.has_transfer && !p.source.read_bytes && p.expired==!strcmp(mode,"expired") && p.clock_error==!strcmp(mode,"clock"));resource_clock_valid=true;CHECK(gt__runtime_resources_forget(sender,id)==GT_RUNTIME_RESOURCE_OK);
  }else if(!strcmp(mode,"cancel")){
   CHECK(gt__runtime_resources_cancel(sender,id)==GT_RUNTIME_RESOURCE_OK);gt__runtime_resources_step(sender);CHECK(publication(sender,id).source.phase==GT_EXT_SOURCE_STOPPED && !publication(sender,id).source.has_transfer);CHECK(gt__runtime_resources_forget(sender,id)==GT_RUNTIME_RESOURCE_OK);
  }else if(!strcmp(mode,"close-sent")){
   for(unsigned i=0;i<40;i++)resource_cycle(&f,sender,receiver);CHECK(publication(sender,id).source.has_transfer);CHECK(gt__runtime_resources_destroy(&sender)==GT_RUNTIME_RESOURCE_WAIT && sender);gt_ext_session_stop(f.pair.rpc.pair.h);gt_ext_session_step(f.pair.rpc.pair.h,0);
  }else{
   uint64_t ref=resource_complete(&f,sender,receiver,id,size);verify_resource(&f,receiver,ref,size,mode);CHECK(gt__runtime_resources_release(receiver,ref)==GT_RUNTIME_RESOURCE_OK && gt__runtime_resources_release(receiver,ref)==GT_RUNTIME_RESOURCE_INVALID);CHECK(!res_usage(&f.pair.e).resources);
  }
 }
 gt_ext_session_stop(f.pair.rpc.pair.h);gt_ext_session_stop(f.pair.rpc.pair.e);gt_ext_session_step(f.pair.rpc.pair.h,0);gt_ext_session_step(f.pair.rpc.pair.e,0);
 CHECK(gt__runtime_resources_destroy(&sender)==GT_RUNTIME_RESOURCE_OK && !sender);CHECK(gt__runtime_resources_destroy(&receiver)==GT_RUNTIME_RESOURCE_OK && !receiver);pub_fixture_drop(&f);CHECK(ext_fds()==before);printf("PASS runtime_resources_%s\n",mode);return 0;
}
