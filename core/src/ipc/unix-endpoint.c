#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "unix-endpoint.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const char socket_name[]="session.sock";
static const char startup_name[]="startup.lock";
static const char daemon_name[]="daemon.lock";
struct gt_unix_endpoint {
  gt_allocator_t allocator;
  uid_t uid;
  int directory,startup,daemon,listener;
  struct stat directory_identity,socket_identity;
  struct sockaddr_un address;
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};
static void *allocate(void *context,size_t n){(void)context;return malloc(n);}
static void release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
static bool same(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev && a->st_ino==b->st_ino;}
static gt_endpoint_rc_t io_error(void){return errno==ENOENT?GT_ENDPOINT_NOT_FOUND:GT_ENDPOINT_IO;}
static bool ancestor(const struct stat *s,uid_t uid) {
  return S_ISDIR(s->st_mode) && (s->st_uid==0 || s->st_uid==uid) && !(s->st_mode&0022);
}
static gt_endpoint_rc_t walk_directory(gt_unix_endpoint_t *e,bool create) {
  int dir=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(dir<0)return GT_ENDPOINT_IO;
  struct stat st;
  if(fstat(dir,&st) || !ancestor(&st,e->uid)){close(dir);return GT_ENDPOINT_AUTH;}
  const char *p=e->path+1;
  while(*p) {
    const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);
    char component[sizeof(e->path)];memcpy(component,p,n);component[n]=0;
    bool leaf=!slash;
    if(fstatat(dir,component,&st,AT_SYMLINK_NOFOLLOW)) {
      if(errno!=ENOENT || !create){gt_endpoint_rc_t rc=io_error();close(dir);return rc;}
      if(mkdirat(dir,component,0700) && errno!=EEXIST){close(dir);return GT_ENDPOINT_IO;}
      if(fstatat(dir,component,&st,AT_SYMLINK_NOFOLLOW)){close(dir);return GT_ENDPOINT_IO;}
    }
    if(!ancestor(&st,e->uid) || (leaf && (st.st_uid!=e->uid || (st.st_mode&07777)!=0700))) {
      close(dir);return GT_ENDPOINT_AUTH;
    }
    int next=openat(dir,component,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat opened;
    if(next<0){close(dir);return GT_ENDPOINT_IO;}
    if(fstat(next,&opened) || !same(&st,&opened) || !ancestor(&opened,e->uid) ||
       (leaf && (opened.st_uid!=e->uid || (opened.st_mode&07777)!=0700))) {
      close(next);close(dir);return GT_ENDPOINT_AUTH;
    }
    close(dir);dir=next;if(leaf)break;p=slash+1;
  }
  e->directory=dir;
  if(fstat(dir,&e->directory_identity)){close(dir);e->directory=-1;return GT_ENDPOINT_IO;}
  return GT_ENDPOINT_OK;
}
static gt_endpoint_rc_t current_directory(gt_unix_endpoint_t *e) {
  struct stat st;
  if(lstat(e->path,&st))return io_error();
  return S_ISDIR(st.st_mode) && st.st_uid==e->uid && (st.st_mode&07777)==0700 &&
    same(&st,&e->directory_identity)?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;
}
gt_endpoint_rc_t gt__unix_endpoint_open(const char *path,bool create,const gt_allocator_t *allocator,gt_unix_endpoint_t **out) {
  if(out)*out=NULL;
  if(!out || !path || path[0]!='/' || !path[1])return GT_ENDPOINT_INVALID;
  size_t n=strlen(path);
  if(n>sizeof(((struct sockaddr_un *)0)->sun_path)-1-sizeof(socket_name))return GT_ENDPOINT_INVALID;
  for(const char *p=path+1;*p;) {
    const char *slash=strchr(p,'/');size_t length=slash?(size_t)(slash-p):strlen(p);
    if(!length || (length==1 && p[0]=='.') || (length==2 && p[0]=='.' && p[1]=='.') || (slash && !slash[1]))return GT_ENDPOINT_INVALID;
    if(!slash)break;
    p=slash+1;
  }
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_ENDPOINT_INVALID;
  gt_unix_endpoint_t *e=a.allocate(a.context,sizeof(*e));if(!e)return GT_ENDPOINT_NO_MEMORY;
  memset(e,0,sizeof(*e));e->allocator=a;e->uid=geteuid();
  e->directory=e->startup=e->daemon=e->listener=-1;memcpy(e->path,path,n+1);
  memcpy(e->address.sun_path,path,n);e->address.sun_path[n]='/';
  memcpy(e->address.sun_path+n+1,socket_name,sizeof(socket_name));e->address.sun_family=AF_UNIX;
#ifdef __APPLE__
  e->address.sun_len=(uint8_t)(offsetof(struct sockaddr_un,sun_path)+n+1+sizeof(socket_name));
#endif
  gt_endpoint_rc_t rc=walk_directory(e,create);
  if(rc!=GT_ENDPOINT_OK){a.release(a.context,e,sizeof(*e));return rc;}
  *out=e;return GT_ENDPOINT_OK;
}
static gt_endpoint_rc_t lock_identity(gt_unix_endpoint_t *e,int fd,const char *name) {
  struct stat opened,node;
  if(fstat(fd,&opened) || fstatat(e->directory,name,&node,AT_SYMLINK_NOFOLLOW))return GT_ENDPOINT_AUTH;
  return S_ISREG(node.st_mode) && node.st_uid==e->uid && (node.st_mode&07777)==0600 &&
    node.st_nlink==1 && same(&opened,&node)?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;
}
static gt_endpoint_rc_t acquire(gt_unix_endpoint_t *e,int *fd,const char *name) {
  gt_endpoint_rc_t rc=current_directory(e);if(rc!=GT_ENDPOINT_OK)return rc;
  if(*fd>=0)return lock_identity(e,*fd,name);
  struct stat node;
  if(!fstatat(e->directory,name,&node,AT_SYMLINK_NOFOLLOW)) {
    if(!S_ISREG(node.st_mode) || node.st_uid!=e->uid || (node.st_mode&07777)!=0600 || node.st_nlink!=1)return GT_ENDPOINT_AUTH;
  } else if(errno!=ENOENT)return GT_ENDPOINT_IO;
  int lock=openat(e->directory,name,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
  if(lock<0)return GT_ENDPOINT_IO;
  rc=lock_identity(e,lock,name);
  if(rc==GT_ENDPOINT_OK && flock(lock,LOCK_EX|LOCK_NB))rc=(errno==EWOULDBLOCK || errno==EAGAIN)?GT_ENDPOINT_BUSY:GT_ENDPOINT_IO;
  if(rc!=GT_ENDPOINT_OK){close(lock);return rc;}
  *fd=lock;return GT_ENDPOINT_OK;
}
gt_endpoint_rc_t gt__unix_endpoint_startup_lock(gt_unix_endpoint_t *e) {
  return e?acquire(e,&e->startup,startup_name):GT_ENDPOINT_INVALID;
}
gt_endpoint_rc_t gt__unix_endpoint_startup_unlock(gt_unix_endpoint_t *e) {
  if(!e)return GT_ENDPOINT_INVALID;
  if(e->startup<0)return GT_ENDPOINT_OK;
  gt_endpoint_rc_t rc=lock_identity(e,e->startup,startup_name);
  close(e->startup);e->startup=-1;return rc;
}
static int stream_socket(void) {
  int type=SOCK_STREAM;
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  type|=SOCK_NONBLOCK|SOCK_CLOEXEC;
#endif
  return socket(AF_UNIX,type,0);
}
static bool configure(int fd) {
  int flags=fcntl(fd,F_GETFL),descriptor=fcntl(fd,F_GETFD);
  if(flags<0 || descriptor<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK) || fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC))return false;
#ifdef SO_NOSIGPIPE
  int enabled=1;if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)))return false;
#endif
  return true;
}
gt_endpoint_rc_t gt__unix_peer_check(int fd,uid_t expected) {
  int type=0;socklen_t size=sizeof(type);struct sockaddr_un address;
  if(fd<0 || getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_STREAM)return GT_ENDPOINT_INVALID;
  size=sizeof(address);
  if(getpeername(fd,(struct sockaddr *)&address,&size) || address.sun_family!=AF_UNIX)return GT_ENDPOINT_INVALID;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  uid_t uid;gid_t gid;if(getpeereid(fd,&uid,&gid))return GT_ENDPOINT_AUTH;
#elif defined(__linux__)
  struct ucred credentials;size=sizeof(credentials);
  if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&size) || size!=sizeof(credentials))return GT_ENDPOINT_AUTH;
  uid_t uid=credentials.uid;
#else
  (void)expected;return GT_ENDPOINT_UNSUPPORTED;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__linux__)
  return uid==expected?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;
#endif
}
static gt_endpoint_rc_t socket_node(gt_unix_endpoint_t *e,struct stat *st) {
  if(fstatat(e->directory,socket_name,st,AT_SYMLINK_NOFOLLOW))return io_error();
  return S_ISSOCK(st->st_mode) && st->st_uid==e->uid && (st->st_mode&07777)==0600 && st->st_nlink==1?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;
}
static bool milliseconds(int64_t *out) {
  struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;
  *out=(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;return true;
}
gt_endpoint_rc_t gt__unix_endpoint_connect(gt_unix_endpoint_t *e,int timeout,int *out) {
  if(out)*out=-1;
  if(!e || !out || timeout<0 || timeout>1000)return GT_ENDPOINT_INVALID;
  gt_endpoint_rc_t rc=current_directory(e);if(rc!=GT_ENDPOINT_OK)return rc;
  struct stat st;rc=socket_node(e,&st);if(rc!=GT_ENDPOINT_OK)return rc;
  int fd=stream_socket();if(fd<0)return GT_ENDPOINT_IO;
  if(!configure(fd)){close(fd);return GT_ENDPOINT_IO;}
  int64_t now;if(!milliseconds(&now)){close(fd);return GT_ENDPOINT_IO;}
  const int64_t end=now+timeout;
  if(connect(fd,(struct sockaddr *)&e->address,sizeof(e->address))) {
    int error=errno;
    if(error==ENOENT || error==ECONNREFUSED){close(fd);return GT_ENDPOINT_NOT_FOUND;}
    if(error!=EINPROGRESS && error!=EINTR && error!=EAGAIN && error!=EWOULDBLOCK){close(fd);return GT_ENDPOINT_IO;}
    /* EAGAIN on Unix connect can mean full backlog, not an in-progress socket. */
    if(error==EAGAIN || error==EWOULDBLOCK){close(fd);return GT_ENDPOINT_WAIT;}
    for(;;) {
      struct pollfd p={.fd=fd,.events=POLLOUT};
      int ready=poll(&p,1,(int)(end>now?end-now:0));
      if(ready>0) {
        if(p.revents&POLLNVAL){close(fd);return GT_ENDPOINT_IO;}
        socklen_t n=sizeof(error);
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)){close(fd);return GT_ENDPOINT_IO;}
        if(error){close(fd);return error==ENOENT || error==ECONNREFUSED?GT_ENDPOINT_NOT_FOUND:GT_ENDPOINT_IO;}
        break;
      }
      if(ready<0 && errno!=EINTR){close(fd);return GT_ENDPOINT_IO;}
      if(!milliseconds(&now)){close(fd);return GT_ENDPOINT_IO;}
      if(!ready || now>=end){close(fd);return GT_ENDPOINT_WAIT;}
    }
  }
  rc=gt__unix_peer_check(fd,e->uid);
  struct stat after;
  if(rc==GT_ENDPOINT_OK && (socket_node(e,&after)!=GT_ENDPOINT_OK || !same(&st,&after)))rc=GT_ENDPOINT_AUTH;
  if(rc!=GT_ENDPOINT_OK){close(fd);return rc;}
  *out=fd;return GT_ENDPOINT_OK;
}
gt_endpoint_rc_t gt__unix_endpoint_listen(gt_unix_endpoint_t *e,int backlog) {
  if(!e || backlog<1 || backlog>128)return GT_ENDPOINT_INVALID;
  if(e->listener>=0)return GT_ENDPOINT_BUSY;
  gt_endpoint_rc_t rc=acquire(e,&e->daemon,daemon_name);if(rc!=GT_ENDPOINT_OK)return rc;
  struct stat st;rc=socket_node(e,&st);
  if(rc==GT_ENDPOINT_OK) {
    int probe=-1;rc=gt__unix_endpoint_connect(e,0,&probe);
    if(probe>=0)close(probe);
    if(rc==GT_ENDPOINT_OK || rc==GT_ENDPOINT_WAIT)rc=GT_ENDPOINT_BUSY;
    else if(rc==GT_ENDPOINT_NOT_FOUND) {
      struct stat after;
      if(lock_identity(e,e->daemon,daemon_name)!=GT_ENDPOINT_OK || socket_node(e,&after)!=GT_ENDPOINT_OK || !same(&st,&after))rc=GT_ENDPOINT_AUTH;
      else rc=unlinkat(e->directory,socket_name,0)?GT_ENDPOINT_IO:GT_ENDPOINT_NOT_FOUND;
    }
  }
  if(rc!=GT_ENDPOINT_NOT_FOUND){close(e->daemon);e->daemon=-1;return rc;}
  int fd=stream_socket();if(fd<0){close(e->daemon);e->daemon=-1;return GT_ENDPOINT_IO;}
  if(!configure(fd) || bind(fd,(struct sockaddr *)&e->address,sizeof(e->address))) {
    close(fd);close(e->daemon);e->daemon=-1;return GT_ENDPOINT_IO;
  }
  /* The 0700 directory protects the node before chmod; never change umask. */
  struct stat bound;
  bool owned=!fstatat(e->directory,socket_name,&bound,AT_SYMLINK_NOFOLLOW) && S_ISSOCK(bound.st_mode) && bound.st_uid==e->uid;
  if(!owned || fchmodat(e->directory,socket_name,0600,0) || socket_node(e,&st)!=GT_ENDPOINT_OK || !same(&bound,&st) || listen(fd,backlog)) {
    struct stat after;if(owned && !fstatat(e->directory,socket_name,&after,AT_SYMLINK_NOFOLLOW) && same(&bound,&after)) (void)unlinkat(e->directory,socket_name,0);
    close(fd);close(e->daemon);e->daemon=-1;return GT_ENDPOINT_IO;
  }
  e->socket_identity=st;e->listener=fd;return GT_ENDPOINT_OK;
}
int gt__unix_endpoint_listener(const gt_unix_endpoint_t *e){return e?e->listener:-1;}
gt_endpoint_rc_t gt__unix_endpoint_accept(gt_unix_endpoint_t *e,int *out) {
  if(out)*out=-1;
  if(!e || !out || e->listener<0)return GT_ENDPOINT_INVALID;
  if(current_directory(e)!=GT_ENDPOINT_OK || lock_identity(e,e->daemon,daemon_name)!=GT_ENDPOINT_OK)return GT_ENDPOINT_AUTH;
  int fd=accept(e->listener,NULL,NULL);
  if(fd<0) {
    if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)return GT_ENDPOINT_WAIT;
    return errno==EMFILE || errno==ENFILE || errno==ENOMEM || errno==ENOBUFS?GT_ENDPOINT_BUSY:GT_ENDPOINT_IO;
  }
  if(!configure(fd)){close(fd);return GT_ENDPOINT_REJECTED;}
  gt_endpoint_rc_t rc=gt__unix_peer_check(fd,e->uid);
  if(rc!=GT_ENDPOINT_OK){close(fd);return rc==GT_ENDPOINT_UNSUPPORTED?rc:GT_ENDPOINT_REJECTED;}
  *out=fd;return GT_ENDPOINT_OK;
}
gt_endpoint_rc_t gt__unix_endpoint_close(gt_unix_endpoint_t *e) {
  if(!e)return GT_ENDPOINT_INVALID;
  gt_endpoint_rc_t rc=GT_ENDPOINT_OK;
  if(e->listener>=0) {
    if(lock_identity(e,e->daemon,daemon_name)!=GT_ENDPOINT_OK)rc=GT_ENDPOINT_AUTH;
    else {
      struct stat st;
      if(!fstatat(e->directory,socket_name,&st,AT_SYMLINK_NOFOLLOW)) {
        if(same(&st,&e->socket_identity) && unlinkat(e->directory,socket_name,0))rc=GT_ENDPOINT_IO;
      } else if(errno!=ENOENT)rc=GT_ENDPOINT_IO;
    }
    close(e->listener);
  }
  if(e->startup>=0)close(e->startup);
  if(e->daemon>=0)close(e->daemon);
  close(e->directory);gt_allocator_t a=e->allocator;a.release(a.context,e,sizeof(*e));return rc;
}
