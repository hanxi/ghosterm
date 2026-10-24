#define _POSIX_C_SOURCE 200809L
/* Reuse bounded checkpoint fixtures to verify cancellation/borrowed lifetimes. */
#define main checkpoint_fixture_main
#include "test_checkpoint_task.c"
#undef main
#include "ipc/attachment-peer.h"
#include "io/backend.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
static void submit_checkpoint(gt_session_attachment_t *a,uint64_t g,fixture_t *f) {
  gt_task_spec_t task=spec(f);
  assert(gt__attachment_begin(a,g,task.sequence)==GT_RESTORE_OK);
  assert(gt__attachment_submit(a,g,&task,&f->id)==GT_TASK_OK);
}
static gt_attachment_retired_t reap(gt_session_attachment_t *a) {
  for(unsigned i=0;i<3000;i++) {
    gt_attachment_retired_t retired;gt_task_rc_t rc=gt__attachment_reap(a,&retired);
    assert(rc==GT_TASK_OK || rc==GT_TASK_BUSY);
    if(rc==GT_TASK_OK)return retired;
    struct timespec pause={0,1000000};nanosleep(&pause,NULL);
  }
  assert(!"peer cleanup timeout");return (gt_attachment_retired_t){0};
}
static volatile sig_atomic_t alarms;
static void alarm_tick(int number) {(void)number;alarms++;}
static double monotonic(void) {
  struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));
  return ts.tv_sec+ts.tv_nsec/1e9;
}
static void terminal_read(gt_io_backend_t *t,const char *expected) {
  char bytes[256]={0};size_t used=0;double deadline=monotonic()+3;
  while(!strstr(bytes,expected) && monotonic()<deadline) {
    int n=t->ops->read(t,(uint8_t *)bytes+used,sizeof(bytes)-used-1,50);
    if(n==GT_TRANSPORT_READ_TIMEOUT)continue;
    assert(n>0);used+=(size_t)n;assert(used<sizeof(bytes)-1);
  }
  assert(strstr(bytes,expected));
}
static void peer_allocation_case(void) {
  int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
  int flags=fcntl(pair[0],F_GETFL),descriptor=fcntl(pair[0],F_GETFD);assert(flags>=0 && descriptor>=0);
  gt_checkpoint_tasks_t *pool=gt__tasks_open(1);assert(pool);
  gt_session_attachment_t *a=gt__attachment_open(pool,8,3);assert(a);uint64_t g=0;
  assert(gt__attachment_attach(a,&g)==GT_TASK_OK);
  allocation_test_budget_t b={.limit=1024*1024};gt_allocator_t domain={&b,allocation_test_allocate,allocation_test_release};
  gt_allocator_t invalid={&b,NULL,allocation_test_release};gt_attachment_peer_t *p=(void *)1;
  assert(gt__peer_adopt_with_allocator(a,g,pair[0],&invalid,&p)==GT_ERR_INVALID_ARG && !p && !b.calls);
  for(unsigned i=0;i<2;i++) {
    b.fail=i?0:b.calls+1;b.limit=i?1:1024*1024;p=(void *)1;
    assert(gt__peer_adopt_with_allocator(a,g,pair[0],&domain,&p)==GT_ERR_INTERNAL && !p && !b.live && !b.count);
    assert(fcntl(pair[0],F_GETFL)==flags && fcntl(pair[0],F_GETFD)==descriptor && gt__attachment_current(a,g));
  }
  b.limit=1024*1024;assert(gt__peer_adopt_with_allocator(a,g,pair[0],&domain,&p)==GT_OK && p);
  size_t live=b.live,calls=b.calls;assert(live && b.count==1);
  assert(fcntl(pair[0],F_GETFL)&O_NONBLOCK);assert(fcntl(pair[0],F_GETFD)&FD_CLOEXEC);
  close(pair[1]);char byte;
  assert(gt__peer_receive(p,&byte,1,100).kind==GT_PEER_CLOSED);
  assert(!gt__attachment_current(a,g) && fcntl(pair[0],F_GETFD)==-1 && errno==EBADF);
  assert(b.live==live && b.count==1 && b.calls==calls);
  uint64_t next=0;assert(gt__attachment_attach(a,&next)==GT_TASK_OK && next>g);
  gt__peer_close(p);assert(!b.live && !b.count && gt__attachment_current(a,next));
  assert(gt__attachment_detach(a,next)==GT_TASK_OK);
  assert(gt__attachment_close(a)==GT_TASK_OK && gt__tasks_close(pool)==GT_TASK_OK);
}
int main(int argc,char **argv) {
  assert(argc>=2);
  if(!strcmp(argv[1],"allocator")){peer_allocation_case();return 0;}
  if(!strcmp(argv[1],"--fd-check")) {
    assert(argc==3);errno=0;
    assert(fcntl(atoi(argv[2]),F_GETFD)==-1 && errno==EBADF);return 0;
  }
  const char *mode=argv[1];int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
  pid_t child=-1;
  if(!strcmp(mode,"kill")) {
    /* Fork before executor/PTY threads; child is a separate idle client. */
    child=fork();assert(child>=0);
    if(!child) {
      close(pair[0]);if(write(pair[1],"R",1)!=1)_exit(2);
      for(;;) {
        char c;ssize_t n=read(pair[1],&c,1);
        if(!n)_exit(0);
        if(n<0 && errno!=EINTR)_exit(2);
      }
    }
    close(pair[1]);pair[1]=-1;
  }
  sigset_t alarm_set;assert(!sigemptyset(&alarm_set) && !sigaddset(&alarm_set,SIGALRM));
  if(!strcmp(mode,"signals"))assert(!pthread_sigmask(SIG_BLOCK,&alarm_set,NULL));
  gt_checkpoint_tasks_t *pool=gt__tasks_open(2);assert(pool);
  gt_session_attachment_t *a=gt__attachment_open(pool,128,8);assert(a);
  uint64_t g=0,next=0;assert(gt__attachment_attach(a,&g)==GT_TASK_OK);
  gt_attachment_peer_t *peer=NULL;
  assert(gt__peer_adopt(a,g,pair[0],&peer)==GT_OK && peer);
  uint8_t bytes[8192];
  if(!strcmp(mode,"flow")) {
    assert(fcntl(pair[0],F_GETFD)&FD_CLOEXEC);
    assert(fcntl(pair[0],F_GETFL)&O_NONBLOCK);
    gt_peer_result_t r=gt__peer_receive(peer,bytes,sizeof(bytes),20);
    assert(r.kind==GT_PEER_WAIT && !r.length && !r.system_error && gt__attachment_current(a,g));
    const uint8_t binary[]={0,255,'X'};
    assert(write(pair[1],binary,sizeof(binary))==(ssize_t)sizeof(binary));
    r=gt__peer_receive(peer,bytes,sizeof(bytes),100);
    assert(r.kind==GT_PEER_DATA && r.length==sizeof(binary) && !memcmp(bytes,binary,sizeof(binary)));
    r=gt__peer_send(peer,binary,sizeof(binary),100);
    assert(r.kind==GT_PEER_DATA && r.length==sizeof(binary));
    assert(read(pair[1],bytes,sizeof(bytes))==(ssize_t)sizeof(binary) && !memcmp(bytes,binary,sizeof(binary)));
  } else if(!strcmp(mode,"kill")) {
    gt_peer_result_t r=gt__peer_receive(peer,bytes,sizeof(bytes),1000);
    assert(r.kind==GT_PEER_DATA && r.length==1 && bytes[0]=='R');
    const char *args[]={"-c","stty -echo; printf READY; read x; printf SURVIVED; read x",NULL};
    gt_pty_opts_t opts={.executable="/bin/sh",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
    gt_io_backend_t *terminal=gt__pty_open(&opts);assert(terminal);terminal_read(terminal,"READY");
    assert(gt__attachment_attach(a,&next)==GT_TASK_BUSY && !next);
    assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    double start=monotonic();r=gt__peer_receive(peer,bytes,sizeof(bytes),1000);
    assert(r.kind==GT_PEER_CLOSED && !r.length && monotonic()-start<1.5);
    assert(!gt__attachment_current(a,g));
    assert(gt__attachment_attach(a,&next)==GT_TASK_OK && next>g);
    assert(terminal->ops->write(terminal,(const uint8_t *)"go\n",3)==GT_OK);
    terminal_read(terminal,"SURVIVED");terminal->ops->close(terminal);
    /* Late destruction of the killed client's owner must not revoke next. */
    gt__peer_close(peer);peer=NULL;assert(gt__attachment_current(a,next));
  } else if(!strcmp(mode,"stale")) {
    assert(gt__attachment_detach(a,g)==GT_TASK_OK);
    assert(gt__attachment_attach(a,&next)==GT_TASK_OK && next>g);
    gt_peer_result_t r=gt__peer_send(peer,"X",1,0);
    assert(r.kind==GT_PEER_STALE && !r.length);
    assert(gt__attachment_current(a,next));
    gt__peer_close(peer);peer=NULL;assert(gt__attachment_current(a,next));
  } else if(!strcmp(mode,"pressure")) {
    int size=4096;assert(!setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)));
    memset(bytes,0xa5,sizeof(bytes));size_t sent=0;bool full=false;double start=monotonic();
    for(unsigned i=0;i<4096;i++) {
      gt_peer_result_t r=gt__peer_send(peer,bytes,sizeof(bytes),0);
      if(r.kind==GT_PEER_WAIT) {assert(!r.length);full=true;break;}
      assert(r.kind==GT_PEER_DATA && r.length && r.length<=sizeof(bytes));sent+=r.length;
    }
    assert(full && sent && monotonic()-start<1);
    start=monotonic();gt_peer_result_t r=gt__peer_send(peer,bytes,sizeof(bytes),20);
    assert(r.kind==GT_PEER_WAIT && !r.length && monotonic()-start<0.5 && gt__attachment_current(a,g));
    size_t received=0;while(received<sent) {
      ssize_t n=read(pair[1],bytes,sizeof(bytes));assert(n>0);
      for(ssize_t i=0;i<n;i++)assert(bytes[i]==0xa5);
      received+=(size_t)n;
    }
    assert(received==sent);
    r=gt__peer_send(peer,"Z",1,100);assert(r.kind==GT_PEER_DATA && r.length==1);
    assert(read(pair[1],bytes,1)==1 && bytes[0]=='Z');
  } else if(!strcmp(mode,"signals")) {
    struct sigaction action={0},previous;action.sa_handler=alarm_tick;
    assert(!sigemptyset(&action.sa_mask) && !sigaction(SIGALRM,&action,&previous));
    struct itimerval timer={.it_interval={0,1000},.it_value={0,1000}},off={0};
    assert(!pthread_sigmask(SIG_UNBLOCK,&alarm_set,NULL));
    assert(!setitimer(ITIMER_REAL,&timer,NULL));
    double start=monotonic();gt_peer_result_t r=gt__peer_receive(peer,bytes,sizeof(bytes),30);
    double elapsed=monotonic()-start;
    assert(!pthread_sigmask(SIG_BLOCK,&alarm_set,NULL));
    assert(!setitimer(ITIMER_REAL,&off,NULL) && !sigaction(SIGALRM,&previous,NULL));
    assert(alarms>2 && elapsed>=0.015 && elapsed<0.5);
    assert(r.kind==GT_PEER_WAIT && gt__attachment_current(a,g));
  } else if(!strcmp(mode,"invalidated-fd")) {
    /* Fault injection: ownership forbids external closes in real callers. */
    close(pair[0]);
    gt_peer_result_t r=gt__peer_receive(peer,bytes,sizeof(bytes),0);
    assert(r.kind==GT_PEER_ERROR && !r.length && r.system_error==EBADF);
    assert(!gt__attachment_current(a,g));
    assert(gt__peer_send(peer,"X",1,0).system_error==EBADF);
  } else if(!strcmp(mode,"running") || !strcmp(mode,"borrowed")) {
    fixture_t f;init(&f,pool,false);submit_checkpoint(a,g,&f);wait_entered(&f);
    gt_attachment_checkpoint_t lease={0};
    if(!strcmp(mode,"borrowed")) {
      proceed(&f);wait_state(&f,GT_TASK_READY);
      assert(gt__attachment_take(a,g,&lease)==GT_TASK_OK && *(unsigned *)lease.result==42);
    }
    close(pair[1]);pair[1]=-1;
    gt_peer_result_t r=gt__peer_receive(peer,bytes,sizeof(bytes),1000);
    assert(r.kind==GT_PEER_CLOSED && !gt__attachment_current(a,g));
    assert(gt__attachment_attach(a,&next)==GT_TASK_OK && next>g);
    if(lease.result) {
      assert(*(unsigned *)lease.result==42 && gt__tasks_close(pool)==GT_TASK_BUSY);
      assert(gt__attachment_release(a,&lease)==GT_TASK_OK);wait_state(&f,GT_TASK_FINISHED);
      assert(reap(a).state==GT_TASK_FINISHED);
    } else {
      proceed(&f);wait_state(&f,GT_TASK_CANCELLED);assert(reap(a).state==GT_TASK_CANCELLED);
    }
    assert(f.input_frees==1 && f.result_frees==1 && gt__attachment_current(a,next));destroy(&f);
  } else if(!strcmp(mode,"exec")) {
    char fd[32];snprintf(fd,sizeof(fd),"%d",pair[0]);
    child=fork();assert(child>=0);
    if(!child) {
      close(pair[1]);execl(argv[0],argv[0],"--fd-check",fd,(char *)NULL);_exit(3);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(gt__attachment_current(a,g));
  } else if(!strcmp(mode,"invalid")) {
    gt_attachment_peer_t *bad=(void *)1;
    assert(gt__peer_adopt(a,g+1,pair[1],&bad)==GT_ERR_INVALID_ARG && !bad);
    assert(fcntl(pair[1],F_GETFD)>=0);
    assert(gt__peer_receive(peer,bytes,0,0).kind==GT_PEER_INVALID);
    assert(gt__peer_receive(peer,bytes,sizeof(bytes),-1).kind==GT_PEER_INVALID);
    assert(gt__peer_send(peer,bytes,sizeof(bytes),1001).kind==GT_PEER_INVALID);
    int datagrams[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM,0,datagrams));
    assert(gt__peer_adopt(a,g,datagrams[0],&bad)==GT_ERR_INVALID_ARG && !bad);
    assert(fcntl(datagrams[0],F_GETFD)>=0);close(datagrams[0]);close(datagrams[1]);
    assert(gt__attachment_current(a,g));
  } else {
    assert(!strcmp(mode,"eof") || !strcmp(mode,"send-close") || !strcmp(mode,"pending"));
    if(!strcmp(mode,"pending"))assert(write(pair[1],"UNACCEPTED",10)==10);
    assert(!shutdown(pair[1],SHUT_RDWR));close(pair[1]);pair[1]=-1;
    gt_peer_result_t r=!strcmp(mode,"send-close") ? gt__peer_send(peer,"X",1,100) : gt__peer_receive(peer,bytes,sizeof(bytes),100);
    assert(r.kind==GT_PEER_CLOSED && !r.length && !r.system_error);
    assert(!gt__attachment_current(a,g) && fcntl(pair[0],F_GETFD)==-1 && errno==EBADF);
    assert(gt__peer_receive(peer,bytes,sizeof(bytes),0).kind==GT_PEER_CLOSED);
    assert(gt__attachment_attach(a,&next)==GT_TASK_OK && next>g);
  }
  gt__peer_close(peer);
  if(next)assert(gt__attachment_detach(a,next)==GT_TASK_OK);
  else assert(!gt__attachment_current(a,g));
  if(pair[1]>=0)close(pair[1]);
  assert(gt__attachment_close(a)==GT_TASK_OK);assert(gt__tasks_close(pool)==GT_TASK_OK);return 0;
}
