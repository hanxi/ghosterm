#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "ipc/unix-endpoint.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static char root[256],directory[256];
static void path(char out[512],const char *name){CHECK(snprintf(out,512,"%s/%s",directory,name)>0);}
static gt_unix_endpoint_t *open_endpoint(void) {
  gt_unix_endpoint_t *e=NULL;CHECK(gt__unix_endpoint_open(directory,true,NULL,&e)==GT_ENDPOINT_OK && e);return e;
}
static void close_endpoint(gt_unix_endpoint_t *e){CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);}
static void flags(int fd) {
  int f=fcntl(fd,F_GETFL),d=fcntl(fd,F_GETFD);
  CHECK(fd>=0 && f>=0 && d>=0 && (f&O_NONBLOCK) && (d&FD_CLOEXEC));
}
static int raw_listener(bool listening) {
  char name[512];path(name,"session.sock");struct sockaddr_un a={.sun_family=AF_UNIX};
  CHECK(strlen(name)<sizeof(a.sun_path));memcpy(a.sun_path,name,strlen(name)+1);
  int fd=socket(AF_UNIX,SOCK_STREAM,0);CHECK(fd>=0);
  CHECK(!bind(fd,(struct sockaddr *)&a,sizeof(a)) && !chmod(name,0600));
  if(listening)CHECK(!listen(fd,8));
  return fd;
}
static void flow(void) {
  gt_unix_endpoint_t *server=open_endpoint(),*client=open_endpoint();
  mode_t saved=umask(0);gt_endpoint_rc_t started=gt__unix_endpoint_listen(server,8);
  CHECK(umask(saved)==0 && started==GT_ENDPOINT_OK);flags(gt__unix_endpoint_listener(server));
  char lock_name[512];path(lock_name,"daemon.lock");int lock=open(lock_name,O_RDWR);CHECK(lock>=0);
  CHECK(flock(lock,LOCK_EX|LOCK_NB)<0 && (errno==EWOULDBLOCK || errno==EAGAIN));close(lock);
  int a=99,b=99;CHECK(gt__unix_endpoint_accept(server,&b)==GT_ENDPOINT_WAIT && b==-1);
  CHECK(gt__unix_endpoint_connect(client,100,&a)==GT_ENDPOINT_OK);flags(a);
  CHECK(gt__unix_endpoint_accept(server,&b)==GT_ENDPOINT_OK);flags(b);
  CHECK(gt__unix_peer_check(a,geteuid())==GT_ENDPOINT_OK && gt__unix_peer_check(b,geteuid())==GT_ENDPOINT_OK);
  CHECK(send(a,"X",1,0)==1);char byte=0;CHECK(recv(b,&byte,1,0)==1 && byte=='X');
  CHECK(send(b,"Y",1,0)==1);CHECK(recv(a,&byte,1,0)==1 && byte=='Y');
  close(a);close(b);close_endpoint(client);close_endpoint(server);
  char name[512];struct stat st;path(name,"session.sock");CHECK(lstat(name,&st)<0 && errno==ENOENT);
  path(name,"daemon.lock");CHECK(!lstat(name,&st) && (st.st_mode&07777)==0600);
}
static void contended_startup(void) {
  int pipefd[2];CHECK(!pipe(pipefd));pid_t child=fork();CHECK(child>=0);
  if(!child) {
    close(pipefd[0]);gt_unix_endpoint_t *e=open_endpoint();
    unsigned char result=(unsigned char)gt__unix_endpoint_startup_lock(e);
    CHECK(write(pipefd[1],&result,1)==1);close(pipefd[1]);close_endpoint(e);_exit(0);
  }
  close(pipefd[1]);struct pollfd p={.fd=pipefd[0],.events=POLLIN};int ready=poll(&p,1,500);
  if(ready<=0)CHECK(!kill(child,SIGKILL));
  unsigned char result=255;ssize_t n=ready>0?read(pipefd[0],&result,1):-1;close(pipefd[0]);
  int status;CHECK(waitpid(child,&status,0)==child);
  CHECK(ready>0 && n==1 && result==GT_ENDPOINT_BUSY && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void locks(void) {
  gt_unix_endpoint_t *a=open_endpoint(),*b=open_endpoint();
  CHECK(gt__unix_endpoint_startup_lock(a)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_startup_lock(a)==GT_ENDPOINT_OK);
  contended_startup();CHECK(gt__unix_endpoint_startup_lock(b)==GT_ENDPOINT_BUSY);
  /* Separate lifetime lock lets the daemon start while client holds startup. */
  CHECK(gt__unix_endpoint_listen(b,8)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_listen(a,8)==GT_ENDPOINT_BUSY);
  CHECK(gt__unix_endpoint_startup_unlock(a)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_startup_unlock(a)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_startup_lock(b)==GT_ENDPOINT_OK);
  close_endpoint(a);int fd=-1,peer=-1;
  CHECK(gt__unix_endpoint_connect(b,0,&fd)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_accept(b,&peer)==GT_ENDPOINT_OK);close(fd);close(peer);close_endpoint(b);
}
static void stale(void) {
  gt_unix_endpoint_t *e=open_endpoint();int fd=raw_listener(false);close(fd);
  int client=99;CHECK(gt__unix_endpoint_connect(e,0,&client)==GT_ENDPOINT_NOT_FOUND && client==-1);
  CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_connect(e,100,&client)==GT_ENDPOINT_OK);
  int peer=-1;CHECK(gt__unix_endpoint_accept(e,&peer)==GT_ENDPOINT_OK);close(client);close(peer);close_endpoint(e);
}
static void live(void) {
  gt_unix_endpoint_t *e=open_endpoint();int fd=raw_listener(true);
  char name[512];struct stat before,after;path(name,"session.sock");CHECK(!lstat(name,&before));
  CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_BUSY);CHECK(!lstat(name,&after) && before.st_ino==after.st_ino);
  int probe=accept(fd,NULL,NULL);CHECK(probe>=0);close(probe);
  int client=-1;CHECK(gt__unix_endpoint_connect(e,100,&client)==GT_ENDPOINT_OK);
  int peer=accept(fd,NULL,NULL);CHECK(peer>=0);close(client);close(peer);close_endpoint(e);
  CHECK(!lstat(name,&after) && before.st_ino==after.st_ino);close(fd);CHECK(!unlink(name));
}
static void replacement(void) {
  gt_unix_endpoint_t *e=open_endpoint();CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_OK);
  char name[512];path(name,"session.sock");CHECK(!unlink(name));int fd=raw_listener(true);
  struct stat before,after;CHECK(!lstat(name,&before));close_endpoint(e);
  CHECK(!lstat(name,&after) && before.st_ino==after.st_ino);close(fd);CHECK(!unlink(name));
}
static void nested_creation(void) {
  char one[512],two[512],nested[512],lock[600];
  CHECK(snprintf(one,sizeof(one),"%s/one",root)>0 && snprintf(two,sizeof(two),"%s/one/two",root)>0 && snprintf(nested,sizeof(nested),"%s/one/two/run",root)>0);
  gt_unix_endpoint_t *e=NULL;struct stat st;
  CHECK(gt__unix_endpoint_open(nested,false,NULL,&e)==GT_ENDPOINT_NOT_FOUND && !e);
  CHECK(lstat(one,&st)<0 && errno==ENOENT);
  mode_t previous=umask(0);
  CHECK(gt__unix_endpoint_open(nested,true,NULL,&e)==GT_ENDPOINT_OK && e);
  umask(previous);
  const char *paths[]={one,two,nested};
  for(unsigned i=0;i<3;i++)CHECK(!lstat(paths[i],&st) && S_ISDIR(st.st_mode) && st.st_uid==geteuid() && (st.st_mode&07777)==0700);
  CHECK(gt__unix_endpoint_listen(e,4)==GT_ENDPOINT_OK);close_endpoint(e);
  CHECK(snprintf(lock,sizeof(lock),"%s/daemon.lock",nested)>0 && !unlink(lock));
  CHECK(!rmdir(nested) && !rmdir(two) && !rmdir(one));
}
static void permissions(void) {
  gt_unix_endpoint_t *e=open_endpoint();close_endpoint(e);CHECK(!chmod(directory,0755));
  e=(gt_unix_endpoint_t *)1;CHECK(gt__unix_endpoint_open(directory,true,NULL,&e)==GT_ENDPOINT_AUTH && !e);
  struct stat st;CHECK(!stat(directory,&st) && (st.st_mode&07777)==0755);CHECK(!chmod(directory,0700));
  char unsafe[512],nested[512];CHECK(snprintf(unsafe,sizeof(unsafe),"%s/unsafe",root)>0);
  CHECK(!mkdir(unsafe,0700) && !chmod(unsafe,0777));CHECK(snprintf(nested,sizeof(nested),"%s/unsafe/runtime",root)>0);
  CHECK(gt__unix_endpoint_open(nested,true,NULL,&e)==GT_ENDPOINT_AUTH && !e);CHECK(!rmdir(unsafe));
}
static void symlinks(void) {
  gt_unix_endpoint_t *e=open_endpoint();close_endpoint(e);
  char link[512],nested[512];CHECK(snprintf(link,sizeof(link),"%s/link",root)>0);
  CHECK(!symlink(directory,link));CHECK(gt__unix_endpoint_open(link,true,NULL,&e)==GT_ENDPOINT_AUTH && !e);
  CHECK(!unlink(link));CHECK(!symlink(root,link));CHECK(snprintf(nested,sizeof(nested),"%s/link/runtime",root)>0);
  CHECK(gt__unix_endpoint_open(nested,true,NULL,&e)==GT_ENDPOINT_AUTH && !e);CHECK(!unlink(link));
}
static void nodes(void) {
  gt_unix_endpoint_t *e=open_endpoint();char name[512],target[512];path(name,"session.sock");path(target,"target");
  int fd=open(name,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0 && write(fd,"keep",4)==4);close(fd);
  CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_AUTH);
  fd=open(name,O_RDONLY);char bytes[4];CHECK(fd>=0 && read(fd,bytes,4)==4 && !memcmp(bytes,"keep",4));close(fd);CHECK(!unlink(name));
  fd=open(target,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);close(fd);CHECK(!symlink(target,name));
  CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_AUTH);struct stat st;CHECK(!lstat(name,&st) && S_ISLNK(st.st_mode));
  CHECK(!unlink(name) && !unlink(target));close_endpoint(e);
}
static void lock_nodes(void) {
  gt_unix_endpoint_t *e=open_endpoint();char name[512],target[512];path(name,"startup.lock");path(target,"target");
  int fd=open(target,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);close(fd);CHECK(!symlink(target,name));
  CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_AUTH);CHECK(!unlink(name));CHECK(!link(target,name));
  CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_AUTH);CHECK(!unlink(name));
  CHECK(!mkfifo(name,0600));CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_AUTH);CHECK(!unlink(name));
  CHECK(!unlink(target));CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_OK);
  CHECK(!chmod(name,0644));CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_AUTH);
  CHECK(gt__unix_endpoint_startup_unlock(e)==GT_ENDPOINT_AUTH);CHECK(!chmod(name,0600));close_endpoint(e);
}
static void peer(void) {
  int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));
  CHECK(gt__unix_peer_check(fds[0],geteuid())==GT_ENDPOINT_OK);
  CHECK(gt__unix_peer_check(fds[0],geteuid()==0?1:0)==GT_ENDPOINT_AUTH);
  close(fds[0]);close(fds[1]);CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,fds));
  CHECK(gt__unix_peer_check(fds[0],geteuid())==GT_ENDPOINT_INVALID);close(fds[0]);close(fds[1]);
  CHECK(gt__unix_peer_check(-1,geteuid())==GT_ENDPOINT_INVALID);
}
typedef struct {size_t live,size;bool reject;} allocation;
static void *admit(void *context,size_t n) {
  allocation *a=context;if(a->reject)return NULL;CHECK(!a->live);a->live=n;a->size=n;return malloc(n);
}
static void refund(void *context,void *p,size_t n) {
  allocation *a=context;CHECK(p && a->live==n && n==a->size);a->live=0;free(p);
}
static int descriptors(void){int n=0;for(int fd=0;fd<256;fd++)if(fcntl(fd,F_GETFD)>=0)n++;return n;}
static void allocation_test(void) {
  allocation a={.reject=true};gt_allocator_t domain={&a,admit,refund};gt_unix_endpoint_t *e=(gt_unix_endpoint_t *)1;
  int before=descriptors();CHECK(gt__unix_endpoint_open(directory,true,&domain,&e)==GT_ENDPOINT_NO_MEMORY && !e && !a.live);
  a.reject=false;
  for(unsigned i=0;i<20;i++) {
    CHECK(gt__unix_endpoint_open(directory,true,&domain,&e)==GT_ENDPOINT_OK && a.live);
    CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_OK);CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_OK);
    close_endpoint(e);CHECK(!a.live && descriptors()==before);
  }
  CHECK(!chmod(directory,0755));CHECK(gt__unix_endpoint_open(directory,false,&domain,&e)==GT_ENDPOINT_AUTH && !a.live && !e);
  CHECK(!chmod(directory,0700) && descriptors()==before);
}
static void directory_replaced(void) {
  gt_unix_endpoint_t *e=open_endpoint();char moved[512];CHECK(snprintf(moved,sizeof(moved),"%s/moved",root)>0);
  CHECK(!rename(directory,moved) && !mkdir(directory,0700));
  CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_AUTH);CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_AUTH);
  int fd=99;CHECK(gt__unix_endpoint_connect(e,0,&fd)==GT_ENDPOINT_AUTH && fd==-1);close_endpoint(e);
  CHECK(!rmdir(moved));
}
static void invalid(void) {
  gt_unix_endpoint_t *e=(gt_unix_endpoint_t *)1;
  const char *bad[]={NULL,"","/","relative","/./run","/a/../b","/a//b","/a/"};
  for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++)CHECK(gt__unix_endpoint_open(bad[i],false,NULL,&e)==GT_ENDPOINT_INVALID && !e);
  char large[256];memset(large,'x',sizeof(large));large[0]='/';large[sizeof(large)-1]=0;
  CHECK(gt__unix_endpoint_open(large,true,NULL,&e)==GT_ENDPOINT_INVALID && !e);
  CHECK(gt__unix_endpoint_open(directory,false,NULL,&e)==GT_ENDPOINT_NOT_FOUND && !e);
  gt_allocator_t domain={0};CHECK(gt__unix_endpoint_open(directory,true,&domain,&e)==GT_ENDPOINT_INVALID);
  CHECK(gt__unix_endpoint_open(directory,true,NULL,NULL)==GT_ENDPOINT_INVALID);
  int fd=99;CHECK(gt__unix_endpoint_connect(NULL,0,&fd)==GT_ENDPOINT_INVALID && fd==-1);
  CHECK(gt__unix_endpoint_accept(NULL,&fd)==GT_ENDPOINT_INVALID && fd==-1);
  CHECK(gt__unix_endpoint_listener(NULL)==-1);CHECK(gt__unix_endpoint_close(NULL)==GT_ENDPOINT_INVALID);
  CHECK(gt__unix_endpoint_startup_lock(NULL)==GT_ENDPOINT_INVALID && gt__unix_endpoint_startup_unlock(NULL)==GT_ENDPOINT_INVALID);
  e=open_endpoint();CHECK(gt__unix_endpoint_listen(e,0)==GT_ENDPOINT_INVALID && gt__unix_endpoint_listen(e,129)==GT_ENDPOINT_INVALID);
  CHECK(gt__unix_endpoint_connect(e,-1,&fd)==GT_ENDPOINT_INVALID && fd==-1);
  CHECK(gt__unix_endpoint_connect(e,1001,&fd)==GT_ENDPOINT_INVALID);
  CHECK(gt__unix_endpoint_connect(e,0,&fd)==GT_ENDPOINT_NOT_FOUND && fd==-1);close_endpoint(e);
}
static void exec_child(int argc,char **argv) {
  CHECK(argc==7);int listener=atoi(argv[3]);CHECK(fcntl(listener,F_GETFD)<0 && errno==EBADF);
  struct stat targets[3];
  for(unsigned i=0;i<3;i++)CHECK(!stat(argv[i+4],&targets[i]));
  for(int fd=3;fd<256;fd++) {
    struct stat st;if(fstat(fd,&st))continue;
    for(unsigned i=0;i<3;i++)CHECK(st.st_dev!=targets[i].st_dev || st.st_ino!=targets[i].st_ino);
  }
}
static void inheritance(const char *program) {
  gt_unix_endpoint_t *e=open_endpoint();CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_OK && gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_OK);
  char startup[512],daemon[512],listener[32];path(startup,"startup.lock");path(daemon,"daemon.lock");
  CHECK(snprintf(listener,sizeof(listener),"%d",gt__unix_endpoint_listener(e))>0);
  pid_t child=fork();CHECK(child>=0);
  if(!child){execl(program,program,"exec-child",root,listener,directory,startup,daemon,(char *)NULL);_exit(127);}
  int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);close_endpoint(e);
}
static void cleanup(void) {
  const char *names[]={"session.sock","startup.lock","daemon.lock","target"};char name[512];
  for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++){path(name,names[i]);if(unlink(name))CHECK(errno==ENOENT);}
  if(rmdir(directory))CHECK(errno==ENOENT);
  CHECK(!rmdir(root));
}
int main(int argc,char **argv) {
  CHECK(argc>=3);
  if(!strcmp(argv[1],"exec-child")){exec_child(argc,argv);return 0;}
  CHECK(argc==3 && snprintf(root,sizeof(root),"%s/ep-XXXXXX",argv[2])>0 && mkdtemp(root));
  CHECK(snprintf(directory,sizeof(directory),"%s/runtime",root)>0);
  if(!strcmp(argv[1],"nested"))nested_creation();else if(!strcmp(argv[1],"flow"))flow();else if(!strcmp(argv[1],"locks"))locks();
  else if(!strcmp(argv[1],"stale"))stale();else if(!strcmp(argv[1],"live"))live();
  else if(!strcmp(argv[1],"replacement"))replacement();else if(!strcmp(argv[1],"permissions"))permissions();
  else if(!strcmp(argv[1],"symlinks"))symlinks();else if(!strcmp(argv[1],"nodes"))nodes();
  else if(!strcmp(argv[1],"lock-nodes"))lock_nodes();else if(!strcmp(argv[1],"peer"))peer();
  else if(!strcmp(argv[1],"allocation"))allocation_test();else if(!strcmp(argv[1],"directory-replaced"))directory_replaced();
  else if(!strcmp(argv[1],"invalid"))invalid();else if(!strcmp(argv[1],"inheritance"))inheritance(argv[0]);else CHECK(false);
  cleanup();printf("PASS private Unix endpoint %s\n",argv[1]);return 0;
}
