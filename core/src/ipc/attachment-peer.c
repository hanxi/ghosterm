#define _POSIX_C_SOURCE 200809L
#include "attachment-peer.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
struct gt_attachment_peer {
  gt_allocator_t allocator;
  gt_session_attachment_t *attachment;
  uint64_t generation;
  int fd;
  gt_peer_result_t terminal;
};
static gt_peer_result_t result(gt_peer_kind_t kind,size_t length,int error) {
  return (gt_peer_result_t){kind,length,error};
}
static gt_peer_result_t finish(gt_attachment_peer_t *p,gt_peer_kind_t kind,int error) {
  /* generation check in detach protects a replacement from this old owner. */
  (void)gt__attachment_detach(p->attachment,p->generation);
  close(p->fd);p->fd=-1;p->terminal=result(kind,0,error);return p->terminal;
}
static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
gt_status_t gt__peer_adopt(gt_session_attachment_t *a,uint64_t g,int fd,gt_attachment_peer_t **out) {
  return gt__peer_adopt_with_allocator(a,g,fd,NULL,out);
}
gt_status_t gt__peer_adopt_with_allocator(gt_session_attachment_t *a,uint64_t g,int fd,const gt_allocator_t *allocator,gt_attachment_peer_t **out) {
  if(out)*out=NULL;
  if(!out || !a || fd<0 || !gt__attachment_current(a,g))return GT_ERR_INVALID_ARG;
  gt_allocator_t domain=allocator?*allocator:(gt_allocator_t){NULL,default_allocate,default_release};
  if(!domain.allocate || !domain.release)return GT_ERR_INVALID_ARG;
  int type=0;socklen_t n=sizeof(type);struct sockaddr_un address;
  if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&n) || type!=SOCK_STREAM)return GT_ERR_INVALID_ARG;
  n=sizeof(address);
  if(getpeername(fd,(struct sockaddr *)&address,&n) || address.sun_family!=AF_UNIX)return GT_ERR_INVALID_ARG;
  gt_attachment_peer_t *p=domain.allocate(domain.context,sizeof(*p));if(!p)return GT_ERR_INTERNAL;
  int flags=fcntl(fd,F_GETFL),descriptor=fcntl(fd,F_GETFD);
  if(flags<0 || descriptor<0) {domain.release(domain.context,p,sizeof(*p));return GT_ERR_IO;}
  if(fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC) || fcntl(fd,F_SETFL,flags|O_NONBLOCK)) {
    (void)fcntl(fd,F_SETFD,descriptor);domain.release(domain.context,p,sizeof(*p));return GT_ERR_IO;
  }
#ifdef SO_NOSIGPIPE
  int enabled=1;
  if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled))) {
    (void)fcntl(fd,F_SETFL,flags);(void)fcntl(fd,F_SETFD,descriptor);domain.release(domain.context,p,sizeof(*p));return GT_ERR_IO;
  }
#endif
  *p=(gt_attachment_peer_t){.allocator=domain,.attachment=a,.generation=g,.fd=fd};*out=p;return GT_OK;
}
static bool milliseconds(int64_t *out) {
  struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return false;
  *out=(int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;return true;
}
static gt_peer_result_t transfer(gt_attachment_peer_t *p,void *bytes,size_t length,int timeout,bool sending) {
  if(!p || !bytes || !length || length>(size_t)SSIZE_MAX || timeout<0 || timeout>1000)
    return result(GT_PEER_INVALID,0,0);
  if(p->fd<0)return p->terminal;
  if(!gt__attachment_current(p->attachment,p->generation))return finish(p,GT_PEER_STALE,0);
  int64_t now;if(!milliseconds(&now))return finish(p,GT_PEER_ERROR,errno);
  const int64_t deadline=now+timeout;
  for(;;) {
    struct pollfd descriptor={.fd=p->fd,.events=sending ? POLLOUT : POLLIN};
    int ready=poll(&descriptor,1,(int)(deadline>now ? deadline-now : 0));
    if(ready<0 && errno!=EINTR)return finish(p,GT_PEER_ERROR,errno);
    if(ready>0) {
      if(!gt__attachment_current(p->attachment,p->generation))return finish(p,GT_PEER_STALE,0);
      if(descriptor.revents&POLLNVAL)return finish(p,GT_PEER_ERROR,EBADF);
      if(descriptor.revents&POLLERR) {
        int error=0;socklen_t size=sizeof(error);
        if(getsockopt(p->fd,SOL_SOCKET,SO_ERROR,&error,&size))error=errno;
        return finish(p,GT_PEER_ERROR,error ? error : EIO);
      }
      if(descriptor.revents&POLLHUP)return finish(p,GT_PEER_CLOSED,0);
      int flags=0;
#ifdef MSG_NOSIGNAL
      flags=MSG_NOSIGNAL;
#endif
      ssize_t count=sending ? send(p->fd,bytes,length,flags) : recv(p->fd,bytes,length,0);
      if(count>0)return result(GT_PEER_DATA,(size_t)count,0);
      if(!count)return finish(p,GT_PEER_CLOSED,0);
      if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)return finish(p,GT_PEER_ERROR,errno);
    }
    if(!milliseconds(&now))return finish(p,GT_PEER_ERROR,errno);
    if(!ready || now>=deadline)return result(GT_PEER_WAIT,0,0);
  }
}
gt_peer_result_t gt__peer_receive(gt_attachment_peer_t *p,void *bytes,size_t length,int timeout) {
  return transfer(p,bytes,length,timeout,false);
}
gt_peer_result_t gt__peer_send(gt_attachment_peer_t *p,const void *bytes,size_t length,int timeout) {
  return transfer(p,(void *)bytes,length,timeout,true);
}
void gt__peer_close(gt_attachment_peer_t *p) {
  if(!p)return;
  if(p->fd>=0)(void)finish(p,GT_PEER_CLOSED,0);
  gt_allocator_t domain=p->allocator;domain.release(domain.context,p,sizeof(*p));
}
