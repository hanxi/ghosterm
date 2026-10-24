/* Only this test's endpoint object substitutes syscall shims; production has
 * no fault-injection switch or alternate behavior. */
#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "ipc/unix-endpoint.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static bool fail_config,fail_identity;static int failed_fd=-1,accept_error;
int gt_test_fcntl(int fd,int command,...) {
  int argument=0;if(command==F_SETFL || command==F_SETFD){va_list args;va_start(args,command);argument=va_arg(args,int);va_end(args);}
  if(fail_config && command==F_SETFL){fail_config=false;failed_fd=fd;errno=EIO;return -1;}
  return fcntl(fd,command,argument);
}
int gt_test_accept(int fd,struct sockaddr *address,socklen_t *size) {
  if(accept_error){errno=accept_error;accept_error=0;return -1;}return accept(fd,address,size);
}
#ifdef __APPLE__
int gt_test_getpeereid(int fd,uid_t *uid,gid_t *gid) {
  if(fail_identity){fail_identity=false;failed_fd=fd;errno=EIO;return -1;}return getpeereid(fd,uid,gid);
}
#else
int gt_test_getsockopt(int fd,int level,int name,void *value,socklen_t *size) {
  if(fail_identity && level==SOL_SOCKET && name==SO_PEERCRED){fail_identity=false;failed_fd=fd;errno=EIO;return -1;}
  return getsockopt(fd,level,name,value,size);
}
#endif
static void healthy(gt_unix_endpoint_t *e) {
  int client=-1,peer=-1;CHECK(gt__unix_endpoint_connect(e,1000,&client)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_accept(e,&peer)==GT_ENDPOINT_OK && client>=0 && peer>=0);
  CHECK((fcntl(peer,F_GETFL)&O_NONBLOCK) && (fcntl(peer,F_GETFD)&FD_CLOEXEC));
  CHECK(write(client,"x",1)==1);char b=0;CHECK(read(peer,&b,1)==1 && b=='x');close(client);close(peer);
}
int main(int argc,char **argv) {
  CHECK(argc==3);char directory[512];CHECK(snprintf(directory,sizeof(directory),"%s/epp-XXXXXX",argv[2])>0 && mkdtemp(directory));
  gt_unix_endpoint_t *e=NULL;CHECK(gt__unix_endpoint_open(directory,true,NULL,&e)==GT_ENDPOINT_OK && gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_OK);
  int fd=-1,peer=999;
  if(!strcmp(argv[1],"capacity")) {
    const int errors[]={EMFILE,ENFILE,ENOMEM,ENOBUFS};
    for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++){accept_error=errors[i];CHECK(gt__unix_endpoint_accept(e,&peer)==GT_ENDPOINT_BUSY && peer==-1);}
  } else {
    CHECK(gt__unix_endpoint_connect(e,1000,&fd)==GT_ENDPOINT_OK);
    if(!strcmp(argv[1],"configuration"))fail_config=true;else {CHECK(!strcmp(argv[1],"identity"));fail_identity=true;}
    CHECK(gt__unix_endpoint_accept(e,&peer)==GT_ENDPOINT_REJECTED && peer==-1 && failed_fd>=0);
    CHECK(fcntl(failed_fd,F_GETFD)<0 && errno==EBADF);close(fd);
  }
  healthy(e);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  char path[600];CHECK(snprintf(path,sizeof(path),"%s/daemon.lock",directory)>0 && !unlink(path) && !rmdir(directory));
  printf("PASS endpoint peer rejection %s\n",argv[1]);return 0;
}
