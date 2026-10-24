#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include "activation.h"
#include "ipc/wire.h"
#include "ipc/socket-stream.h"
#include "base/secure-random.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
_Static_assert(ATOMIC_BOOL_LOCK_FREE==2,"Proxy signals require lock-free state");
static atomic_bool stopping;
static void stop(int signal){(void)signal;atomic_store_explicit(&stopping,true,memory_order_relaxed);}
static bool cancelled(void *ctx){
  (void)ctx;if(atomic_load_explicit(&stopping,memory_order_relaxed))return true;
  /* Darwin needs an actual read/write filter to report EOF under full queues. */
  struct pollfd fds[2]={{.fd=0,.events=POLLIN},{.fd=1,.events=POLLOUT}};(void)poll(fds,2,0);
  return ((fds[0].revents|fds[1].revents)&(POLLHUP|POLLERR|POLLNVAL))!=0;
}
static bool clock_ms(uint64_t *out){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0 || (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static bool setup_signals(void) {
  struct sigaction a={0};sigemptyset(&a.sa_mask);a.sa_handler=stop;
  if(sigaction(SIGTERM,&a,NULL) || sigaction(SIGINT,&a,NULL) || sigaction(SIGHUP,&a,NULL))return false;
  a.sa_handler=SIG_IGN;if(sigaction(SIGPIPE,&a,NULL))return false;
  a.sa_handler=SIG_DFL;return !sigaction(SIGCHLD,&a,NULL);
}
static gt_activation_rc_t read_hello(gt_protocol_role_t role,uint8_t bytes[84],gt_protocol_hello_t *hello) {
  uint64_t start,previous;if(!clock_ms(&start))return GT_ACTIVATION_IO;previous=start;size_t used=0;
  while(used<84) {
    uint64_t now;if(cancelled(NULL))return GT_ACTIVATION_CANCELLED;
    if(!clock_ms(&now) || now<previous)return GT_ACTIVATION_IO;
    previous=now;
    if(now-start>=5000)return GT_ACTIVATION_TIMEOUT;
    struct pollfd p={.fd=0,.events=POLLIN};int rc=poll(&p,1,10);
    if(rc<0 && errno!=EINTR)return GT_ACTIVATION_IO;
    if(p.revents&(POLLERR|POLLHUP|POLLNVAL))return GT_ACTIVATION_CANCELLED;
    if(!(p.revents&POLLIN))continue;
    size_t need=used<24?24-used:84-used;ssize_t n=read(0,bytes+used,need);
    if(n>0)used+=(size_t)n;else if(!n)return GT_ACTIVATION_CANCELLED;else if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)return GT_ACTIVATION_IO;
    if(used==24){gt_wire_header_t h;if(!gt__wire_header_decode(bytes,60,&h) || h.type!=GT_MSG_HELLO || h.payload_length!=60 || (role==GT_ROLE_MANAGEMENT?!h.request_id:h.request_id!=0))return GT_ACTIVATION_INVALID;}
  }
  return gt__hello_decode(bytes+24,60,hello) && hello->role==role?GT_ACTIVATION_OK:GT_ACTIVATION_INVALID;
}
typedef struct {uint8_t bytes[65536];size_t begin,end;} buffer;
static size_t room(buffer *b) {
  if(b->end==sizeof(b->bytes) && b->begin) {
    size_t n=b->end-b->begin;memmove(b->bytes,b->bytes+b->begin,n);gt__secure_erase(b->bytes+n,sizeof(b->bytes)-n);b->begin=0;b->end=n;
  }
  size_t n=sizeof(b->bytes)-b->end;return n>4096?4096:n;
}
static size_t pending(const buffer *b){size_t n=b->end-b->begin;return n>4096?4096:n;}
static void consume(buffer *b,size_t n){gt__secure_erase(b->bytes+b->begin,n);b->begin+=n;if(b->begin==b->end)b->begin=b->end=0;}
/* One read/write attempt in each direction per round. HUP/error takes priority
 * over unaccepted input, including when either output queue is full. */
static gt_activation_rc_t relay(gt_activation_result_t *link,const uint8_t hello[84]) {
  gt_socket_stream_t stream;gt_stream_ops_t ops;
  if(gt__socket_stream_adopt(link->fd,&stream,&ops)!=GT_OK){close(link->fd);link->fd=-1;return GT_ACTIVATION_IO;}
  link->fd=-1;buffer input={0},output={0};memcpy(input.bytes,hello,84);input.end=84;gt_activation_rc_t result=GT_ACTIVATION_OK;
  while(!cancelled(NULL)) {
    gt__activation_reap(link);
    size_t in_room=room(&input),out_room=room(&output);
    struct pollfd fds[3]={{.fd=0,.events=in_room?POLLIN:0},{.fd=1,.events=pending(&output)?POLLOUT:0},
      {.fd=stream.fd,.events=(short)((out_room?POLLIN:0)|(pending(&input)?POLLOUT:0))}};
    int rc=poll(fds,3,100);if(rc<0){if(errno==EINTR)continue;result=GT_ACTIVATION_IO;break;}
    if((fds[0].revents|fds[1].revents|fds[2].revents)&POLLNVAL){result=GT_ACTIVATION_IO;break;}
    if((fds[0].revents|fds[1].revents)&(POLLHUP|POLLERR))break;
    if(fds[2].revents&POLLERR){result=GT_ACTIVATION_IO;break;}
    if(fds[2].revents&POLLHUP)break;
    if(fds[0].revents&POLLIN) {
      ssize_t n=read(0,input.bytes+input.end,in_room);if(n>0)input.end+=(size_t)n;else if(!n)break;else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){result=GT_ACTIVATION_IO;break;}
    }
    if(fds[2].revents&POLLIN) {
      gt_stream_result_t r=ops.read(ops.context,output.bytes+output.end,out_room);
      if(r.kind==GT_STREAM_DATA)output.end+=r.length;else if(r.kind!=GT_STREAM_WAIT){if(r.kind==GT_STREAM_ERROR)result=GT_ACTIVATION_IO;break;}
    }
    if((fds[2].revents&POLLOUT) && pending(&input)) {
      gt_stream_result_t r=ops.write(ops.context,input.bytes+input.begin,pending(&input));
      if(r.kind==GT_STREAM_DATA)consume(&input,r.length);else if(r.kind!=GT_STREAM_WAIT){result=GT_ACTIVATION_IO;break;}
    }
    if((fds[1].revents&POLLOUT) && pending(&output)) {
      ssize_t n=write(1,output.bytes+output.begin,pending(&output));if(n>0)consume(&output,(size_t)n);else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){result=GT_ACTIVATION_IO;break;}
    }
  }
  ops.close(ops.context);gt__secure_erase(&input,sizeof(input));gt__secure_erase(&output,sizeof(output));return result;
}
static const char *reason(gt_activation_rc_t rc) {
  switch(rc){case GT_ACTIVATION_INVALID:return "invalid request";case GT_ACTIVATION_NOT_FOUND:return "session daemon not found";
    case GT_ACTIVATION_AUTH:return "untrusted endpoint or helper";case GT_ACTIVATION_UNSUPPORTED:return "required capability unsupported";
    case GT_ACTIVATION_TIMEOUT:return "startup deadline expired";case GT_ACTIVATION_CANCELLED:return "connection cancelled";
    case GT_ACTIVATION_INCOMPATIBLE:return "incompatible daemon contract";case GT_ACTIVATION_FEATURES:return "required features unsupported";
    case GT_ACTIVATION_INSTANCE:return "wrong daemon instance";default:return "connection failed";}
}
static void diagnostic(gt_activation_rc_t rc) {
  /* A full SSH stderr channel must not pin process/connection teardown. */
  int flags=fcntl(2,F_GETFL);if(flags<0 || fcntl(2,F_SETFL,flags|O_NONBLOCK))return;
  char bytes[128];int n=snprintf(bytes,sizeof(bytes),"ghosterm-session-proxy: %s\n",reason(rc));
  if(n>0 && (size_t)n<sizeof(bytes)){ssize_t written=write(2,bytes,(size_t)n);(void)written;}
  (void)fcntl(2,F_SETFL,flags);
}
/* Atomic publication runs on the Linux host, independent of the client's
 * SFTP extension version. Never copy across filesystems or replace a release. */
static int publication_parent(const char *path,char leaf[96],char parent[1024]) {
  if(!path || path[0]!='/' || strlen(path)>=1024)return -1;
  strcpy(parent,path);char *last=strrchr(parent,'/');if(!last || !last[1] || strlen(last+1)>=96)return -1;
  strcpy(leaf,last+1);*last=0;if(!parent[0])return -1;
  int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
  const char *p=parent+1;bool valid=true;
  while(*p && valid) {
    const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);char component[1024];
    if(!n || (n==1 && p[0]=='.') || (n==2 && !memcmp(p,"..",2))){valid=false;break;}
    memcpy(component,p,n);component[n]=0;int next=openat(fd,component,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(next<0){valid=false;break;}close(fd);fd=next;
    if(fstat(fd,&st) || (st.st_uid!=0 && st.st_uid!=geteuid()) || (st.st_mode&0022)){valid=false;break;}
    if(!slash)break;
    p=slash+1;
  }
  if(!valid){close(fd);return -1;}return fd;
}
static int publish_release(const char *source,const char *destination) {
  char from[96],to[96],source_parent[1024],destination_parent[1024],expected[1024];
  int a=publication_parent(source,from,source_parent);if(a<0)return 1;
  int b=publication_parent(destination,to,destination_parent);if(b<0){close(a);return 1;}
  bool okay=!strncmp(from,".stage-",7) && strlen(from)==31;
  for(unsigned i=7;i<31 && okay;i++)okay=(from[i]>='0' && from[i]<='9') || (from[i]>='a' && from[i]<='f');
  okay=okay && to[0] && strcmp(to,".") && strcmp(to,"..");
  for(size_t i=0;to[i] && okay;i++)okay=(to[i]>='a' && to[i]<='z') || (to[i]>='A' && to[i]<='Z') || (to[i]>='0' && to[i]<='9') || to[i]=='-' || to[i]=='_' || to[i]=='.';
  int n=snprintf(expected,sizeof(expected),"%s/releases",source_parent);okay=okay && n>0 && (size_t)n<sizeof(expected) && !strcmp(expected,destination_parent);
  struct stat st;if(fstatat(a,from,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077))okay=false;
  if(!fstatat(b,to,&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT)okay=false;
  int result=okay?renameat(a,from,b,to):-1;close(a);close(b);
  return result==0?0:1;
}
int main(int argc,char **argv) {
  if(argc==2 && !strcmp(argv[1],"--version")){puts("ghosterm-session-proxy GTSD/1 DEPLOY/1");return 0;}
  if(argc==6 && !strcmp(argv[1],"publish") && !strcmp(argv[2],"--from") && !strcmp(argv[4],"--to")) {
    if(publish_release(argv[3],argv[5]))return 1;
    return puts("GT_PUBLISH_OK")==EOF?1:0;
  }
  if(argc==2 && !strcmp(argv[1],"--help")){puts("Usage: ghosterm-session-proxy --runtime-dir ABS --role management|data [--daemon ABS]");return 0;}
  if((argc!=5 && argc!=7) || strcmp(argv[1],"--runtime-dir") || strcmp(argv[3],"--role") ||
    (strcmp(argv[4],"management") && strcmp(argv[4],"data")) || (argc==7 && strcmp(argv[5],"--daemon")))return 2;
  gt_protocol_role_t role=!strcmp(argv[4],"management")?GT_ROLE_MANAGEMENT:GT_ROLE_DATA;
  if(!setup_signals())return 1;
  int flags[2];bool configured=false;gt_activation_rc_t result=GT_ACTIVATION_IO;uint8_t hello[84]={0};gt_protocol_hello_t request={0};gt_activation_result_t link={.fd=-1,.child=-1};
  for(int i=0;i<2;i++){struct stat st;flags[i]=fcntl(i,F_GETFL);if(flags[i]<0 || fstat(i,&st))goto done;if(!S_ISFIFO(st.st_mode) && !S_ISSOCK(st.st_mode)){result=GT_ACTIVATION_UNSUPPORTED;goto done;}}
  configured=true;
  for(int i=0;i<2;i++)if(fcntl(i,F_SETFL,flags[i]|O_NONBLOCK))goto done;
  result=read_hello(role,hello,&request);
  if(result==GT_ACTIVATION_OK) {
    gt_activation_config_t config={.directory=argv[2],.helper=argc==7?argv[6]:NULL,.hello=request,.timeout_ms=5000,.cancelled=cancelled};
    result=gt__activation_connect(&config,&link);
    if(result==GT_ACTIVATION_OK)result=relay(&link,hello);
  }
done:
  if(link.fd>=0)close(link.fd);
  gt__activation_reap(&link);gt__secure_erase(hello,sizeof(hello));gt__secure_erase(&request,sizeof(request));
  if(configured)for(int i=0;i<2;i++)(void)fcntl(i,F_SETFL,flags[i]);
  if(result!=GT_ACTIVATION_OK && result!=GT_ACTIVATION_CANCELLED)diagnostic(result);
  return result==GT_ACTIVATION_OK || result==GT_ACTIVATION_CANCELLED?0:64+(int)result;
}
