#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include "extension/stream.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
static gt_ext_channel_t *boundary_owner;
static bool boundary_fail_allocation,boundary_stop_write,boundary_stop_read;
static int boundary_close;
static unsigned boundary_allocations;
static const uint8_t *boundary_borrow;
static size_t boundary_borrow_size;
static void *boundary_calloc(size_t n,size_t size){boundary_allocations++;return boundary_fail_allocation?NULL:calloc(n,size);}
static gt_bridge_io_t boundary_write(gt_bridge_stream_t *s,const void *p,size_t n,int ms){gt_bridge_io_t r=gt__bridge_stream_write(s,p,n,ms);if(r.state==GT_BRIDGE_WAIT){boundary_borrow=p;boundary_borrow_size=n;}if(boundary_stop_write)gt_ext_channel_stop(boundary_owner);return r;}
static gt_bridge_io_t boundary_read(gt_bridge_stream_t *s,void *p,size_t n,int ms){gt_bridge_io_t r=gt__bridge_stream_read(s,p,n,ms);if(boundary_stop_read)gt_ext_channel_stop(boundary_owner);return r;}
static gt_bridge_close_t boundary_close_step(gt_bridge_stream_t *s,int ms){if(boundary_close)return boundary_close==1?GT_BRIDGE_DRAINING:GT_BRIDGE_CLOSE_ERROR;return gt__bridge_stream_close(s,ms);}
#define calloc boundary_calloc
#define gt__bridge_stream_write boundary_write
#define gt__bridge_stream_read boundary_read
#define gt__bridge_stream_close boundary_close_step
#include "../src/extension/channel.c"
#undef calloc
#undef gt__bridge_stream_write
#undef gt__bridge_stream_read
#undef gt__bridge_stream_close
static gt_ext_channel_info_t boundary_info(void){gt_ext_channel_info_t i;CHECK(gt_ext_channel_info(boundary_owner,&i));return i;}
int main(int argc,char **argv){
  CHECK(argc==2);const char *mode=argv[1];int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
  if(!strcmp(mode,"allocation")){boundary_fail_allocation=true;int flags=fcntl(fd[0],F_GETFL);CHECK(gt_ext_channel_open_fd(fd[0],fd[0],&boundary_owner)==GT_ERR_IO && !boundary_owner && flags==fcntl(fd[0],F_GETFL) && !atomic_load(&channel_count));boundary_fail_allocation=false;}
  if(!strcmp(mode,"constructor-failure")){int file=open("/dev/null",O_RDONLY);CHECK(file>=0 && gt_ext_channel_open_fd(file,file,&boundary_owner)==GT_ERR_INVALID_ARG && !boundary_owner && !atomic_load(&channel_count) && fcntl(file,F_GETFD)>=0 && !close(file));}
  if(!strcmp(mode,"wait-stop") || !strcmp(mode,"draining") || !strcmp(mode,"close-error")){
    int small=1024;CHECK(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)) && !fcntl(fd[0],F_SETFL,O_NONBLOCK));uint8_t fill[4096]={0};while(write(fd[0],fill,sizeof(fill))>0){}CHECK(errno==EAGAIN);
  }
  CHECK(gt_ext_channel_open_fd(fd[0],fd[0],&boundary_owner)==GT_OK && boundary_owner);
  if(!strcmp(mode,"wait-stop") || !strcmp(mode,"draining") || !strcmp(mode,"close-error")){
    CHECK(gt_ext_channel_write(boundary_owner,"retained",8).count==8 && gt_ext_channel_step(boundary_owner,0)==GT_EXT_READY && boundary_borrow && boundary_borrow_size==8 && !memcmp(boundary_borrow,"retained",8));
    if(!strcmp(mode,"wait-stop")){gt_ext_channel_stop(boundary_owner);CHECK(gt_ext_channel_step(boundary_owner,0)==GT_EXT_STOPPED);}
    else{boundary_close=!strcmp(mode,"draining")?1:2;CHECK(gt_ext_channel_close(boundary_owner,0)==(boundary_close==1?GT_EXT_DRAINING:GT_EXT_CLOSE_ERROR));if(boundary_close==2)CHECK(boundary_info().cleanup_error==GT_ERR_IO);CHECK(gt_ext_channel_destroy(&boundary_owner)==GT_ERR_INVALID_ARG && boundary_owner && atomic_load(&channel_count)==1);}
    CHECK(!memcmp(boundary_borrow,"retained",8) && boundary_info().buffered==8 && !boundary_info().sent);
  }else if(!strcmp(mode,"post-write-stop")){boundary_stop_write=true;CHECK(gt_ext_channel_write(boundary_owner,"abc",3).count==3 && gt_ext_channel_step(boundary_owner,0)==GT_EXT_STOPPED);uint8_t p[3];CHECK(read(fd[1],p,3)==3 && !memcmp(p,"abc",3) && boundary_info().sent==3);}
  else if(!strcmp(mode,"post-read-stop")){CHECK(write(fd[1],"abc",3)==3);boundary_stop_read=true;uint8_t p[3];gt_ext_io_t io=gt_ext_channel_read(boundary_owner,p,sizeof(p),0);CHECK(io.state==GT_EXT_IO_CLOSED && !io.count && boundary_info().received==3);}
  else if(!strcmp(mode,"counter")){boundary_owner->info.accepted=UINT64_MAX-1;CHECK(gt_ext_channel_write(boundary_owner,"abc",3).error==GT_ERR_INTERNAL && boundary_info().accepted==UINT64_MAX-1);}
  else CHECK(!strcmp(mode,"allocation") || !strcmp(mode,"constructor-failure"));
  boundary_close=0;CHECK(gt_ext_channel_close(boundary_owner,0)==GT_EXT_RELEASED && !boundary_info().buffered && boundary_info().cleanup_error==GT_OK && gt_ext_channel_destroy(&boundary_owner)==GT_OK && !boundary_owner && !atomic_load(&channel_count));CHECK(!close(fd[1]));
  if(!strcmp(mode,"allocation")){
    gt_ext_channel_t *all[GT_EXT_CHANNEL_LIMIT]={0},*extra=NULL;int peers[GT_EXT_CHANNEL_LIMIT];unsigned before=boundary_allocations;
    for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++){CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));CHECK(gt_ext_channel_open_fd(fd[0],fd[0],&all[i])==GT_OK);peers[i]=fd[1];}
    CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));CHECK(gt_ext_channel_open_fd(fd[0],fd[0],&extra)==GT_ERR_IO && !extra && boundary_allocations==before+GT_EXT_CHANNEL_LIMIT && atomic_load(&channel_count)==GT_EXT_CHANNEL_LIMIT);
    CHECK(!close(fd[0]) && !close(fd[1]));for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++){CHECK(gt_ext_channel_close(all[i],0)==GT_EXT_RELEASED && gt_ext_channel_destroy(&all[i])==GT_OK && !close(peers[i]));}CHECK(!atomic_load(&channel_count));
  }
  printf("PASS ext_channel_boundary_%s\n",mode);return 0;
}
