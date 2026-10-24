#define _POSIX_C_SOURCE 200809L
#include "session-process-wire.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static uint32_t get32(const uint8_t *p) {uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
void pw_put64(uint8_t *p,uint64_t v) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
uint64_t pw_get64(const uint8_t *p) {uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static double now(void) {
  struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return -1;
  return ts.tv_sec+ts.tv_nsec/1e9;
}
static bool exact(pw_io *io,void *bytes,size_t n,bool sending,double end) {
  uint8_t *p=bytes;
  while(n) {
    double current=now();if(current<0 || current>=end)return false;
    ssize_t count=io->transfer(io->context,p,n,sending);
    if(count<0 || (size_t)count>n)return false;
    p+=(size_t)count;n-=(size_t)count;
  }
  return true;
}
static bool send_until(pw_io *io,uint16_t kind,uint64_t sequence,const void *bytes,size_t length,double end) {
  if(!io || !io->transfer || !kind || kind>PW_HISTORY || length>PW_CHUNK || (!bytes && length))return false;
  uint8_t header[24]={0x47,0x54,0x50,0x30,0,1,0,(uint8_t)kind};
  put32(header+12,(uint32_t)length);pw_put64(header+16,sequence);
  return exact(io,header,sizeof(header),true,end) && exact(io,(void *)bytes,length,true,end);
}
static bool receive_until(pw_io *io,pw_frame *out,double end) {
  if(!io || !io->transfer || !out)return false;
  memset(out,0,sizeof(*out));uint8_t h[24];
  if(!exact(io,h,sizeof(h),false,end))return false;
  if(memcmp(h,"GTP0",4) || h[4] || h[5]!=1 || h[6] || !h[7] || h[7]>PW_HISTORY || get32(h+8))return false;
  size_t n=get32(h+12);if(n>PW_CHUNK)return false;
  if(!exact(io,out->bytes,n,false,end))return false;
  out->kind=h[7];out->length=n;out->sequence=pw_get64(h+16);return true;
}
bool pw_send(pw_io *io,uint16_t kind,uint64_t sequence,const void *bytes,size_t length) {
  double start=now();if(start<0)return false;
  return send_until(io,kind,sequence,bytes,length,start+5);
}
bool pw_receive(pw_io *io,pw_frame *out) {
  double start=now();if(start<0)return false;
  return receive_until(io,out,start+5);
}
bool pw_send_blob(pw_io *io,uint16_t kind,const void *bytes,size_t n) {
  if(n>PW_LIMIT || (!bytes && n))return false;
  const uint8_t *p=bytes;double start=now();if(start<0)return false;
  const double end=start+5;
  while(n) {size_t count=n>PW_CHUNK ? PW_CHUNK : n;if(!send_until(io,kind,0,p,count,end))return false;p+=count;n-=count;}
  return true;
}
bool pw_receive_blob(pw_io *io,uint16_t kind,void *bytes,size_t n) {
  if(n>PW_LIMIT || (!bytes && n))return false;
  uint8_t *p=bytes;pw_frame frame;double start=now();if(start<0)return false;
  const double end=start+5;
  while(n) {
    if(!receive_until(io,&frame,end) || frame.kind!=kind || frame.sequence || !frame.length || frame.length>n)return false;
    memcpy(p,frame.bytes,frame.length);p+=frame.length;n-=frame.length;
  }
  return true;
}
ssize_t pw_socket_transfer(void *context,void *bytes,size_t n,bool sending) {
  int fd=*(int *)context;struct pollfd p={.fd=fd,.events=sending ? POLLOUT : POLLIN};
  int ready=poll(&p,1,50);
  if(ready<0)return errno==EINTR ? 0 : -1;
  if(!ready)return 0;
  if(p.revents&(POLLHUP|POLLERR|POLLNVAL))return -1;
  int flags=0;
#ifdef MSG_NOSIGNAL
  flags=MSG_NOSIGNAL;
#endif
  ssize_t count=sending ? send(fd,bytes,n,flags) : recv(fd,bytes,n,0);
  if(count<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))return 0;
  return count>0 ? count : -1;
}
