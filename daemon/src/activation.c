#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "activation.h"
#include "startup-wire.h"
#include "ipc/unix-endpoint.h"
#include "ipc/socket-stream.h"
#include "base/secure-random.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#if defined(__APPLE__) && defined(POSIX_SPAWN_SETSID)
#define GT_DETACHED_SPAWN_AVAILABLE 1
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2,34) && defined(POSIX_SPAWN_SETSID)
#define GT_DETACHED_SPAWN_AVAILABLE 1
#endif
#endif
static bool clock_ms(uint64_t *out){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0 || (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static gt_activation_rc_t map(gt_endpoint_rc_t rc) {
  return rc==GT_ENDPOINT_NOT_FOUND?GT_ACTIVATION_NOT_FOUND:rc==GT_ENDPOINT_AUTH?GT_ACTIVATION_AUTH:
    rc==GT_ENDPOINT_UNSUPPORTED?GT_ACTIVATION_UNSUPPORTED:rc==GT_ENDPOINT_INVALID?GT_ACTIVATION_INVALID:GT_ACTIVATION_IO;
}
/* Installed absolute path; all components are root/user owned, not writable by
 * other identities, no symlinks, no set-id executable. Same-UID code is trusted
 * as with the private endpoint; this is not a same-UID sandbox. */
static bool trusted_helper(const char *path) {
  if(!path || path[0]!='/' || !path[1] || strlen(path)>4095)return false;
  int dir=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return false;
  bool okay=false;const char *p=path+1;uid_t uid=geteuid();
  while(*p) {
    const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);char name[4096];
    if(!n || n>=sizeof(name) || (n==1 && p[0]=='.') || (n==2 && p[0]=='.' && p[1]=='.') || (slash && !slash[1]))break;
    memcpy(name,p,n);name[n]=0;int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(slash?O_DIRECTORY:0));if(fd<0)break;
    struct stat st;bool valid=!fstat(fd,&st) && (st.st_uid==0 || st.st_uid==uid) && !(st.st_mode&0022);
    if(slash)valid=valid && S_ISDIR(st.st_mode);
    else valid=valid && S_ISREG(st.st_mode) && !(st.st_mode&06000) && (st.st_mode&0111);
    close(dir);dir=fd;if(!valid)break;if(!slash){okay=true;break;}p=slash+1;
  }
  close(dir);return okay;
}
static gt_activation_rc_t spawn_helper(const char *helper,const char *directory,int *control,pid_t *child) {
  *control=-1;*child=-1;
  if(getuid()!=geteuid() || getgid()!=getegid() || !trusted_helper(helper))return GT_ACTIVATION_AUTH;
#ifdef GT_DETACHED_SPAWN_AVAILABLE
  int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return GT_ACTIVATION_IO;
  for(unsigned i=0;i<2;i++)if(fcntl(pair[i],F_SETFD,FD_CLOEXEC)){close(pair[0]);close(pair[1]);return GT_ACTIVATION_IO;}
  posix_spawnattr_t attr;posix_spawn_file_actions_t actions;int rc=posix_spawnattr_init(&attr);
  if(rc){close(pair[0]);close(pair[1]);return GT_ACTIVATION_IO;}
  rc=posix_spawn_file_actions_init(&actions);if(rc){posix_spawnattr_destroy(&attr);close(pair[0]);close(pair[1]);return GT_ACTIVATION_IO;}
  sigset_t mask,defaults;sigemptyset(&mask);sigemptyset(&defaults);
  const int signals[]={SIGHUP,SIGINT,SIGTERM,SIGPIPE,SIGCHLD};for(size_t i=0;i<sizeof(signals)/sizeof(signals[0]);i++)sigaddset(&defaults,signals[i]);
  short flags=POSIX_SPAWN_SETSID|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
  flags|=POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  rc=posix_spawnattr_setflags(&attr,flags);if(!rc)rc=posix_spawnattr_setsigmask(&attr,&mask);if(!rc)rc=posix_spawnattr_setsigdefault(&attr,&defaults);
  for(int i=0;i<3 && !rc;i++)rc=posix_spawn_file_actions_addopen(&actions,i,"/dev/null",i?O_WRONLY:O_RDONLY,0);
  if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,pair[1],3);
#ifdef __linux__
  if(!rc)rc=posix_spawn_file_actions_addclosefrom_np(&actions,4);
#endif
  char *argv[]={(char *)helper,"serve-detached","--runtime-dir",(char *)directory,NULL};
  if(!rc)rc=posix_spawn(child,helper,&actions,&attr,argv,environ);
  posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attr);close(pair[1]);
  if(rc){close(pair[0]);*child=-1;return rc==ENOSYS?GT_ACTIVATION_UNSUPPORTED:GT_ACTIVATION_IO;}
  *control=pair[0];return GT_ACTIVATION_OK;
#else
  (void)directory;return GT_ACTIVATION_UNSUPPORTED;
#endif
}
void gt__activation_reap(gt_activation_result_t *out) {
  if(!out || out->child<=0)return;
  pid_t rc=waitpid(out->child,NULL,WNOHANG);if(rc==out->child || (rc<0 && errno==ECHILD))out->child=-1;
}
static void stop_uncommitted(gt_activation_result_t *out) {
  gt__activation_reap(out);if(out->child<=0 || out->committed)return;
  (void)kill(out->child,SIGTERM);uint64_t start=0,now=0;(void)clock_ms(&start);
  for(unsigned i=0;i<100;i++) {
    gt__activation_reap(out);if(out->child<=0)return;
    if(!clock_ms(&now) || now<start || now-start>=1000)break;
    (void)poll(NULL,0,10);
  }
  /* Not reaped yet: still our own child PID, no application IO before ACK. */
  (void)kill(out->child,SIGKILL);pid_t rc;do {rc=waitpid(out->child,NULL,0);}while(rc<0 && errno==EINTR);out->child=-1;
}
gt_activation_rc_t gt__activation_connect(const gt_activation_config_t *c,gt_activation_result_t *out) {
  if(out)*out=(gt_activation_result_t){.fd=-1,.child=-1};
  uint8_t check[GT_HELLO_BYTES];
  if(!out || !c || !c->directory || !c->timeout_ms || c->timeout_ms>60000 || !gt__hello_encode(&c->hello,check))return GT_ACTIVATION_INVALID;
  uint64_t start,previous;if(!clock_ms(&start))return GT_ACTIVATION_IO;previous=start;
  bool may_start=c->hello.role==GT_ROLE_MANAGEMENT && !nonzero(c->hello.expected_instance,16);
  gt_unix_endpoint_t *endpoint=NULL;gt_endpoint_rc_t er=gt__unix_endpoint_open(c->directory,may_start,NULL,&endpoint);
  if(er!=GT_ENDPOINT_OK)return map(er);
  gt_activation_rc_t result=GT_ACTIVATION_IO;bool locked=false,spawned=false;int ready_fd=-1;gt_socket_stream_t stream={.fd=-1};gt_stream_ops_t ops={0};
  uint8_t ready[72]={0},ack[24]={0},commit[24]={0};size_t received=0,sent=0,confirmed=0;bool validated=false;
  while(true) {
    uint64_t now;
    if(c->cancelled && c->cancelled(c->context)){result=GT_ACTIVATION_CANCELLED;break;}
    if(!clock_ms(&now) || now<previous){result=GT_ACTIVATION_IO;break;}previous=now;
    if(now-start>=c->timeout_ms){result=GT_ACTIVATION_TIMEOUT;break;}
    if(spawned && !validated) {
      /* Raw readiness receive allows a final failure frame followed by EOF. */
      ssize_t n=recv(ready_fd,ready+received,72-received,0);
      if(n>0)received+=(size_t)n;else if(!n || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))break;
      if(received==72) {
        gt_status_t status;gt_protocol_identity_t identity;
        if(!gt__startup_ready_decode(ready,&status,&identity))break;
        if(status!=GT_OK){result=status==GT_ERR_UNSUPPORTED?GT_ACTIVATION_UNSUPPORTED:status==GT_ERR_AUTH?GT_ACTIVATION_AUTH:GT_ACTIVATION_IO;break;}
        if(memcmp(identity.compatibility,c->hello.compatibility,32)){result=GT_ACTIVATION_INCOMPATIBLE;break;}
        if(c->hello.required_features&~identity.features){result=GT_ACTIVATION_FEATURES;break;}
        gt__startup_ack_encode(identity.instance,ack);validated=true;
      }
    } else if(spawned && sent<24) {
      gt_stream_result_t w=ops.write(ops.context,ack+sent,24-sent);
      if(w.kind==GT_STREAM_DATA){sent+=w.length;out->committed=true;}else if(w.kind!=GT_STREAM_WAIT)break;
    } else if(spawned && confirmed<24) {
      ssize_t n=recv(ready_fd,commit+confirmed,24-confirmed,0);
      if(n>0)confirmed+=(size_t)n;else if(!n || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))break;
      if(confirmed==24 && !gt__startup_commit_valid(commit,ready+48))break;
    } else {
      er=gt__unix_endpoint_connect(endpoint,0,&out->fd);
      if(er==GT_ENDPOINT_OK){result=GT_ACTIVATION_OK;break;}
      if(er!=GT_ENDPOINT_NOT_FOUND && er!=GT_ENDPOINT_WAIT){result=map(er);break;}
      if(er==GT_ENDPOINT_NOT_FOUND && !may_start){result=GT_ACTIVATION_NOT_FOUND;break;}
      if(er==GT_ENDPOINT_NOT_FOUND && !spawned) {
        if(!locked) {
          er=gt__unix_endpoint_startup_lock(endpoint);
          if(er==GT_ENDPOINT_OK){locked=true;continue;}
          if(er!=GT_ENDPOINT_BUSY){result=map(er);break;}
        } else {
          result=spawn_helper(c->helper,c->directory,&ready_fd,&out->child);if(result!=GT_ACTIVATION_OK)break;result=GT_ACTIVATION_IO;
          if(gt__socket_stream_adopt(ready_fd,&stream,&ops)!=GT_OK)break;
          spawned=true;
        }
      }
    }
    struct pollfd p={.fd=ready_fd,.events=(short)(validated && sent<24?POLLOUT:POLLIN)};
    /* Once ACK flushed, endpoint retry has its own bounded timer. */
    if(!spawned || confirmed==24)p.fd=-1;
    (void)poll(&p,1,10);
  }
  if(ready_fd>=0){if(stream.fd>=0)ops.close(ops.context);else close(ready_fd);}
  if(result!=GT_ACTIVATION_OK)stop_uncommitted(out);
  gt_endpoint_rc_t closed=gt__unix_endpoint_close(endpoint);
  if(closed!=GT_ENDPOINT_OK && result==GT_ACTIVATION_OK){close(out->fd);out->fd=-1;result=map(closed);}
  gt__secure_erase(ready,sizeof(ready));gt__secure_erase(ack,sizeof(ack));gt__secure_erase(commit,sizeof(commit));return result;
}
