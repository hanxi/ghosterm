#define _POSIX_C_SOURCE 200809L
#include "ssh-link.h"
#include "proxy-status.h"
#include "internal.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
#ifdef GT_HAVE_LIBSSH2
#include <libssh2.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define CAP 65536u
#define CHUNK 4096u
typedef struct {uint8_t bytes[CAP];size_t head,count;} queue;
typedef struct {
  struct gt_ssh_link *owner;unsigned slot;LIBSSH2_CHANNEL *channel;
  gt_ssh_link_info_t info;queue rx,tx;char command[4096];bool detached,eof;
} stream;
typedef enum {OPEN,EXEC,WRITE,CLOSE,FREE} operation;
struct gt_ssh_link {
  gt_allocator_t allocator;LIBSSH2_SESSION *session;int fd;
  stream streams[2];int active;operation operation;unsigned turn;
  uint8_t packet[CHUNK];size_t packet_size;bool failed;gt_status_t error;
};
static void *allocate(void *c,size_t n){(void)c;return malloc(n);}
static void release(void *c,void *p,size_t n){(void)c;(void)n;free(p);}
static size_t take(queue *q,void *bytes,size_t n) {
  if(n>q->count)n=q->count;
  size_t first=CAP-q->head;if(first>n)first=n;
  memcpy(bytes,q->bytes+q->head,first);memcpy((uint8_t *)bytes+first,q->bytes,n-first);return n;
}
static void retire(queue *q,size_t n) {
  size_t first=CAP-q->head;if(first>n)first=n;
  gt__secure_erase(q->bytes+q->head,first);gt__secure_erase(q->bytes,n-first);
  q->head=(q->head+n)%CAP;q->count-=n;
}
static size_t append(queue *q,const void *bytes,size_t n) {
  if(n>CAP-q->count)n=CAP-q->count;
  size_t tail=(q->head+q->count)%CAP,first=CAP-tail;if(first>n)first=n;
  memcpy(q->bytes+tail,bytes,first);memcpy(q->bytes,(const uint8_t *)bytes+first,n-first);q->count+=n;return n;
}
static bool clock_ms(void *context,uint64_t *out) {
  (void)context;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0 || (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return false;
  *out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;
}
static gt_stream_result_t read_stream(void *context,void *bytes,size_t n) {
  stream *s=context;if(s->detached || s->owner->failed)return (gt_stream_result_t){GT_STREAM_ERROR,0,0};
  if(s->rx.count){size_t got=take(&s->rx,bytes,n);retire(&s->rx,got);return (gt_stream_result_t){GT_STREAM_DATA,got,0};}
  return (gt_stream_result_t){s->info.phase==GT_SSH_LINK_ENDED?GT_STREAM_END:s->info.phase==GT_SSH_LINK_FAILED?GT_STREAM_ERROR:GT_STREAM_WAIT,0,0};
}
static gt_stream_result_t write_stream(void *context,const void *bytes,size_t n) {
  stream *s=context;if(s->detached || s->owner->failed || s->eof || s->info.phase>=GT_SSH_LINK_CLOSING)return (gt_stream_result_t){GT_STREAM_ERROR,0,0};
  size_t sent=append(&s->tx,bytes,n);return (gt_stream_result_t){sent?GT_STREAM_DATA:GT_STREAM_WAIT,sent,0};
}
static void close_stream(void *context) {
  stream *s=context;s->detached=true;gt__secure_erase(&s->rx,sizeof(s->rx));
  /* The active packet is separate/stable and may already be in flight. */
  if(s->owner->active!=(int)s->slot || s->owner->operation!=WRITE)gt__secure_erase(&s->tx,sizeof(s->tx));
}
static void fail_link(gt_ssh_link_t *l,gt_status_t error) {
  l->failed=true;l->error=error;(void)shutdown(l->fd,SHUT_RDWR);
  for(unsigned i=0;i<2;i++){stream *s=&l->streams[i];s->info.phase=GT_SSH_LINK_FAILED;s->info.error=error;gt__secure_erase(&s->rx,sizeof(s->rx));gt__secure_erase(&s->tx,sizeof(s->tx));}
}
/* POSIX single quotes; no expansion, substitutions, option or newline injection. */
bool gt__ssh_posix_path(char *out,size_t capacity,size_t *used,const char *path) {
  if(!path || path[0]!='/' || strlen(path)>1024)return false;
  if(*used+2>=capacity)return false;
  out[(*used)++]='\'';
  for(const unsigned char *p=(const unsigned char *)path;*p;p++) {
    if(*p<32 || *p==127)return false;
    const char *text=*p=='\''?"'\\''":NULL;size_t n=text?4:1;
    if(*used+n+2>=capacity)return false;
    if(text)memcpy(out+*used,text,n);else out[*used]=(char)*p;
    *used+=n;
  }
  out[(*used)++]='\'';out[*used]=0;return true;
}
static bool literal(char *out,size_t capacity,size_t *used,const char *text) {
  size_t n=strlen(text);if(*used+n>=capacity)return false;
  memcpy(out+*used,text,n+1);*used+=n;return true;
}
gt_status_t gt__ssh_link_open(gt_ssh_t **ssh,const gt_allocator_t *domain,gt_ssh_link_t **out) {
  if(out)*out=NULL;
  gt_allocator_t a=domain?*domain:(gt_allocator_t){NULL,allocate,release};
  if(!out || !ssh || !*ssh || !a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  gt_ssh_link_t *l=a.allocate(a.context,sizeof(*l));if(!l)return GT_ERR_INTERNAL;
  memset(l,0,sizeof(*l));l->allocator=a;l->active=-1;l->fd=-1;void *session=NULL;
  gt_status_t rc=(gt_status_t)gt__ssh_take(ssh,&session,&l->fd);
  if(rc!=GT_OK){a.release(a.context,l,sizeof(*l));return rc;}
  l->session=session;libssh2_session_set_blocking(l->session,0);libssh2_session_set_timeout(l->session,0);
  for(unsigned i=0;i<2;i++){l->streams[i].owner=l;l->streams[i].slot=i;l->streams[i].detached=true;}
  *out=l;return GT_OK;
}
gt_status_t gt__ssh_link_stream(gt_ssh_link_t *l,unsigned slot,const char *proxy,const char *runtime,const char *daemon,gt_stream_ops_t *ops) {
  if(ops)memset(ops,0,sizeof(*ops));
  if(!l || l->failed || slot>1 || !ops)return GT_ERR_INVALID_ARG;
  stream *s=&l->streams[slot];if(!s->detached || s->channel || l->active==(int)slot)return GT_ERR_INVALID_ARG;
  size_t used=0;char command[4096];
  if(!literal(command,sizeof(command),&used,"exec ") || !gt__ssh_posix_path(command,sizeof(command),&used,proxy) ||
    !literal(command,sizeof(command),&used," --runtime-dir ") || !gt__ssh_posix_path(command,sizeof(command),&used,runtime) ||
    !literal(command,sizeof(command),&used,slot?" --role data --daemon ":" --role management --daemon ") ||
    !gt__ssh_posix_path(command,sizeof(command),&used,daemon))return GT_ERR_INVALID_ARG;
  memset(s,0,sizeof(*s));s->owner=l;s->slot=slot;s->info.phase=GT_SSH_LINK_OPENING;memcpy(s->command,command,used+1);
  *ops=(gt_stream_ops_t){s,read_stream,write_stream,clock_ms,close_stream};gt__secure_erase(command,sizeof(command));return GT_OK;
}
static void receive_stream(gt_ssh_link_t *l,stream *s) {
  if(!s->channel || s->info.phase!=GT_SSH_LINK_READY)return;
  uint8_t bytes[CHUNK];ssize_t output=LIBSSH2_ERROR_EAGAIN,diagnostic;
  if(!s->detached && s->rx.count<CAP){size_t room=CAP-s->rx.count;if(room>CHUNK)room=CHUNK;output=libssh2_channel_read(s->channel,(char *)bytes,room);if(output>0)(void)append(&s->rx,bytes,(size_t)output);}
  diagnostic=libssh2_channel_read_stderr(s->channel,(char *)bytes,sizeof(bytes));
  if(diagnostic>0){uint64_t n=(uint64_t)diagnostic;s->info.stderr_bytes=UINT64_MAX-s->info.stderr_bytes<n?UINT64_MAX:s->info.stderr_bytes+n;}
  gt__secure_erase(bytes,sizeof(bytes));
  if((output<0 && output!=LIBSSH2_ERROR_EAGAIN) || (diagnostic<0 && diagnostic!=LIBSSH2_ERROR_EAGAIN)){fail_link(l,GT_ERR_IO);return;}
  if(output==0 && diagnostic==0 && libssh2_channel_eof(s->channel))s->eof=true;
}
gt_status_t gt__ssh_link_step(gt_ssh_link_t *l) {
  if(!l)return GT_ERR_INVALID_ARG;
  if(l->failed)return l->error;
  struct pollfd fd={.fd=l->fd,.events=POLLIN};
  if(poll(&fd,1,0)<0 && errno!=EINTR){fail_link(l,GT_ERR_IO);return GT_ERR_IO;}
  if(fd.revents&(POLLERR|POLLHUP|POLLNVAL)){fail_link(l,GT_ERR_IO);return GT_ERR_IO;}
  for(unsigned i=0;i<2;i++){receive_stream(l,&l->streams[i]);if(l->failed)return l->error;}
  if(l->active<0) {
    for(unsigned n=0;n<2;n++) {
      unsigned i=(l->turn+n)%2;stream *s=&l->streams[i];
      if(s->detached && !s->channel){s->info.phase=GT_SSH_LINK_ENDED;continue;}
      if(s->info.phase==GT_SSH_LINK_OPENING)l->operation=s->channel?EXEC:OPEN;
      else if(s->channel && (s->detached || s->eof)){l->operation=CLOSE;s->info.phase=GT_SSH_LINK_CLOSING;}
      else if(s->info.phase==GT_SSH_LINK_READY && s->tx.count){l->operation=WRITE;l->packet_size=take(&s->tx,l->packet,CHUNK);}
      else continue;
      l->active=(int)i;l->turn=(i+1)%2;break;
    }
  }
  if(l->active<0)return GT_OK;
  stream *s=&l->streams[l->active];int rc=0;
  switch(l->operation) {
    case OPEN:s->channel=libssh2_channel_open_session(l->session);rc=s->channel?0:libssh2_session_last_errno(l->session);break;
    case EXEC:rc=libssh2_channel_exec(s->channel,s->command);if(!rc){s->info.phase=GT_SSH_LINK_READY;gt__secure_erase(s->command,sizeof(s->command));}break;
    case WRITE:{ssize_t n=libssh2_channel_write(s->channel,(const char *)l->packet,l->packet_size);rc=n<0?(int)n:0;if(n>0)retire(&s->tx,(size_t)n);break;}
    case CLOSE:rc=libssh2_channel_close(s->channel);if(!rc){int code=libssh2_channel_get_exit_status(s->channel);s->info.has_exit_code=code!=0;s->info.exit_code=code;gt_status_t error;if(!s->detached && gt__proxy_exit_status(code,&error)){fail_link(l,error);return error;}l->operation=FREE;return GT_OK;}break;
    case FREE:rc=libssh2_channel_free(s->channel);if(!rc){s->channel=NULL;s->info.phase=GT_SSH_LINK_ENDED;}break;
  }
  if(rc==LIBSSH2_ERROR_EAGAIN)return GT_OK;
  if(rc<0){fail_link(l,rc==LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED?GT_ERR_UNSUPPORTED:GT_ERR_IO);return s->info.error;}
  if(l->operation==WRITE){gt__secure_erase(l->packet,sizeof(l->packet));l->packet_size=0;if(s->detached)gt__secure_erase(&s->tx,sizeof(s->tx));}
  l->active=-1;return GT_OK;
}
bool gt__ssh_link_info(const gt_ssh_link_t *l,unsigned slot,gt_ssh_link_info_t *out) {
  if(!l || slot>1 || !out)return false;
  *out=l->streams[slot].info;return true;
}
void gt__ssh_link_cancel(gt_ssh_link_t *l){if(l && !l->failed)fail_link(l,GT_ERR_CANCELLED);}
void gt__ssh_link_close(gt_ssh_link_t *l) {
  if(!l)return;
  gt__ssh_link_cancel(l);
  /* Broken socket makes libssh2 channel cleanup independent of remote replies.
   * Keep fd reserved until session free; never let a recycled fd receive IO. */
  libssh2_session_set_blocking(l->session,1);libssh2_session_set_timeout(l->session,1);
  (void)libssh2_session_free(l->session);close(l->fd);
  gt_allocator_t a=l->allocator;gt__secure_erase(l,sizeof(*l));a.release(a.context,l,sizeof(*l));
}
#else
struct gt_ssh_link {int unused;};
bool gt__ssh_posix_path(char *o,size_t n,size_t *u,const char *p){(void)o;(void)n;(void)u;(void)p;return false;}
gt_status_t gt__ssh_link_open(gt_ssh_t **s,const gt_allocator_t *a,gt_ssh_link_t **o){(void)s;(void)a;if(o)*o=NULL;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_link_stream(gt_ssh_link_t *l,unsigned n,const char *p,const char *r,const char *d,gt_stream_ops_t *o){(void)l;(void)n;(void)p;(void)r;(void)d;if(o)memset(o,0,sizeof(*o));return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_link_step(gt_ssh_link_t *l){(void)l;return GT_ERR_UNSUPPORTED;}
bool gt__ssh_link_info(const gt_ssh_link_t *l,unsigned n,gt_ssh_link_info_t *o){(void)l;(void)n;(void)o;return false;}
void gt__ssh_link_cancel(gt_ssh_link_t *l){(void)l;}
void gt__ssh_link_close(gt_ssh_link_t *l){(void)l;}
#endif
