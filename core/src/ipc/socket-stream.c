#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "socket-stream.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
static gt_stream_result_t result(gt_stream_kind_t kind,size_t n,int error){return (gt_stream_result_t){kind,n,error};}
static gt_stream_result_t receive(void *context,void *bytes,size_t length) {
  gt_socket_stream_t *s=context;struct pollfd p={.fd=s->fd,.events=POLLIN};
  int ready=poll(&p,1,0);
  if(ready<0)return errno==EINTR?result(GT_STREAM_WAIT,0,0):result(GT_STREAM_ERROR,0,errno);
  if(!ready)return result(GT_STREAM_WAIT,0,0);
  if(p.revents&POLLNVAL)return result(GT_STREAM_ERROR,0,EBADF);
  if(p.revents&POLLERR) {
    int error=0;socklen_t n=sizeof(error);
    if(getsockopt(s->fd,SOL_SOCKET,SO_ERROR,&error,&n))error=errno;
    return result(GT_STREAM_ERROR,0,error?error:EIO);
  }
  /* A dead attachment must not deliver previously unaccepted queued bytes. */
  if(p.revents&POLLHUP)return result(GT_STREAM_END,0,0);
  ssize_t n=recv(s->fd,bytes,length,0);
  if(n>0)return result(GT_STREAM_DATA,(size_t)n,0);
  if(!n)return result(GT_STREAM_END,0,0);
  return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR?result(GT_STREAM_WAIT,0,0):result(GT_STREAM_ERROR,0,errno);
}
static gt_stream_result_t transmit(void *context,const void *bytes,size_t length) {
  gt_socket_stream_t *s=context;int flags=0;
#ifdef MSG_NOSIGNAL
  flags=MSG_NOSIGNAL;
#endif
  ssize_t n=send(s->fd,bytes,length,flags);
  if(n>0)return result(GT_STREAM_DATA,(size_t)n,0);
  if(!n || errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)return result(GT_STREAM_WAIT,0,0);
  return result(GT_STREAM_ERROR,0,errno);
}
static bool clock_ms(void *context,uint64_t *out) {
  (void)context;struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0 || (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return false;
  *out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;
}
void gt__socket_stream_close(gt_socket_stream_t *s){if(s && s->fd>=0){close(s->fd);s->fd=-1;}}
static void close_stream(void *context){gt__socket_stream_close(context);}
gt_status_t gt__socket_stream_adopt(int fd,gt_socket_stream_t *out,gt_stream_ops_t *ops) {
  if(out)out->fd=-1;
  if(ops)memset(ops,0,sizeof(*ops));
  if(fd<0 || !out || !ops)return GT_ERR_INVALID_ARG;
  int type=0;socklen_t size=sizeof(type);struct sockaddr_un address;
  if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_STREAM)return GT_ERR_INVALID_ARG;
  size=sizeof(address);
  if(getpeername(fd,(struct sockaddr *)&address,&size) || address.sun_family!=AF_UNIX)return GT_ERR_INVALID_ARG;
  int flags=fcntl(fd,F_GETFL),descriptor=fcntl(fd,F_GETFD);
  if(flags<0 || descriptor<0)return GT_ERR_IO;
  bool configured=!fcntl(fd,F_SETFL,flags|O_NONBLOCK) && !fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int enabled=1;if(configured && setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)))configured=false;
#endif
  if(!configured){(void)fcntl(fd,F_SETFL,flags);(void)fcntl(fd,F_SETFD,descriptor);return GT_ERR_IO;}
  out->fd=fd;*ops=(gt_stream_ops_t){out,receive,transmit,clock_ms,close_stream};return GT_OK;
}
