#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

typedef struct {
  pthread_mutex_t lock;pthread_cond_t changed;
  bool entered,release,finished,closed,legacy,stop,pending;
  unsigned writes,reads;gt_status_t status;pthread_t caller;bool fd_was_open[1024];
} fixture;
typedef struct {gt_io_backend_t base;fixture *f;} fake;
static void wait_flag(fixture *f,bool *flag) {
  struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  assert(!pthread_mutex_lock(&f->lock));while(!*flag)assert(!pthread_cond_timedwait(&f->changed,&f->lock,&end));assert(!pthread_mutex_unlock(&f->lock));
}
static int read_io(gt_io_backend_t *io,uint8_t *p,size_t n,int timeout) {
  (void)p;(void)n;fake *b=(fake *)io;assert(b->f->legacy && timeout==0);return GT_TRANSPORT_READ_EOF;
}
static int read_wake(gt_io_backend_t *io,uint8_t *p,size_t n,int timeout,int fd) {
  (void)p;(void)n;fake *b=(fake *)io;fixture *f=b->f;
  assert(!pthread_equal(f->caller,pthread_self()));
  assert((fcntl(fd,F_GETFD)&FD_CLOEXEC)!=0 && (fcntl(fd,F_GETFL)&O_NONBLOCK)!=0);
  f->reads++;
  if(f->writes){struct pollfd drained={.fd=fd,.events=POLLIN};assert(poll(&drained,1,0)==0);}
  if(f->pending){assert(f->writes>=1);if(f->writes==1){assert(timeout==0);return GT_TRANSPORT_READ_TIMEOUT;}assert(f->writes==2);return GT_TRANSPORT_READ_EOF;}
  if(f->writes){assert(f->writes==1);return GT_TRANSPORT_READ_EOF;}
  assert(timeout==10);
  assert(!pthread_mutex_lock(&f->lock));f->entered=true;assert(!pthread_cond_broadcast(&f->changed));
  while(!f->release)assert(!pthread_cond_wait(&f->changed,&f->lock));
  assert(!pthread_mutex_unlock(&f->lock));
  /* A real notification is required. Poll timeout cannot make this green,
   * even if the surrounding worker still performs 10 ms timed reads. */
  struct pollfd wake={.fd=fd,.events=POLLIN};int rc;do{rc=poll(&wake,1,2000);}while(rc<0 && errno==EINTR);
  assert(rc==1 && wake.revents==POLLIN);return GT_TRANSPORT_READ_TIMEOUT;
}
static int write_io(gt_io_backend_t *io,const uint8_t *p,size_t n) {
  fake *b=(fake *)io;assert(!pthread_equal(b->f->caller,pthread_self()));assert(n==1 && *p=='x');b->f->writes++;return GT_OK;
}
static void close_io(gt_io_backend_t *io) {fake *b=(fake *)io;b->f->closed=true;free(b);}
static const gt_io_ops_t wake_ops={.read=read_io,.write=write_io,.close=close_io,.read_with_wake=read_wake};
static const gt_io_ops_t legacy_ops={.read=read_io,.write=write_io,.close=close_io};
static gt_io_backend_t *open_io(void *context) {
  fixture *f=context;fake *b=calloc(1,sizeof(*b));assert(b);b->f=f;b->base.ops=f->legacy?&legacy_ops:&wake_ops;return &b->base;
}
static gt_status_t open_state(void *context,gt_worker_reply_sink_t sink,void **out) {
  (void)sink;fixture *f=context;unsigned added=0;
  for(int fd=0;fd<1024;fd++)if(!f->fd_was_open[fd] && fcntl(fd,F_GETFD)>=0) {
    assert((fcntl(fd,F_GETFD)&FD_CLOEXEC)!=0 && (fcntl(fd,F_GETFL)&O_NONBLOCK)!=0);added++;
  }
  assert(added==2);
  if(f->pending || f->legacy){assert(!pthread_mutex_lock(&f->lock));f->entered=true;assert(!pthread_cond_broadcast(&f->changed));while(!f->release)assert(!pthread_cond_wait(&f->changed,&f->lock));
  assert(!pthread_mutex_unlock(&f->lock));}
  *out=f;return GT_OK;
}
static void close_state(void *state) {(void)state;}
static gt_status_t apply(void *context,const gt_state_event_t *event) {(void)context;(void)event;assert(0);return GT_ERR_INTERNAL;}
static gt_status_t freeze(void *context,void **out) {(void)context;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void finished(void *context,gt_status_t status) {fixture *f=context;assert(!pthread_mutex_lock(&f->lock));f->status=status;f->finished=true;assert(!pthread_cond_broadcast(&f->changed));assert(!pthread_mutex_unlock(&f->lock));}
static unsigned fd_count(void) {unsigned n=0;for(int fd=0;fd<1024;fd++)if(fcntl(fd,F_GETFD)>=0)n++;return n;}
static void worker_case(const char *mode) {
  unsigned before=fd_count();fixture f={.caller=pthread_self(),.pending=!strcmp(mode,"pending"),.legacy=!strcmp(mode,"legacy"),.stop=!strcmp(mode,"stop")};
  for(int fd=0;fd<1024;fd++)f.fd_was_open[fd]=fcntl(fd,F_GETFD)>=0;
  assert(!pthread_mutex_init(&f.lock,NULL));assert(!pthread_cond_init(&f.changed,NULL));
  gt_worker_config_t config={.context=&f,.open_backend=open_io,.open_state=open_state,.close_state=close_state,.state_ops={apply,freeze},.finished=finished};
  gt_session_worker_t *w=gt__worker_start(&config);assert(w);wait_flag(&f,&f.entered);
  if(f.stop)gt__worker_stop(w);else {
    assert(gt__worker_submit_internal(w,GT_WORKER_INPUT,"x",1,NULL,NULL)==GT_WORKER_ACCEPTED);
    if(f.pending || f.legacy)assert(gt__worker_submit_internal(w,GT_WORKER_INPUT,"x",1,NULL,NULL)==GT_WORKER_ACCEPTED);
  }
  assert(!pthread_mutex_lock(&f.lock));f.release=true;assert(!pthread_cond_broadcast(&f.changed));assert(!pthread_mutex_unlock(&f.lock));
  wait_flag(&f,&f.finished);gt__worker_join(w);
  assert(f.closed && f.status==GT_OK && f.writes==(f.stop?0u:f.pending?2u:1u));
  assert(!pthread_cond_destroy(&f.changed));assert(!pthread_mutex_destroy(&f.lock));assert(fd_count()==before);
}
static void real_case(void) {
  const char *args[]={"-c","stty -echo; printf READY; read answer; printf DONE; read answer",NULL};
  gt_pty_opts_t options={.executable="/bin/sh",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
  gt_io_backend_t *b=gt__pty_open(&options);assert(b && b->ops->read_with_wake);
  char bytes[64]={0};size_t used=0;for(unsigned tries=0;!strstr(bytes,"READY");tries++){assert(tries<50);int n=b->ops->read(b,(uint8_t *)bytes+used,sizeof(bytes)-1-used,100);if(n==GT_TRANSPORT_READ_TIMEOUT)continue;assert(n>0);used+=(size_t)n;}
  int wake[2];assert(!pipe(wake));assert(fcntl(wake[0],F_SETFL,O_NONBLOCK)==0);assert(write(wake[1],"w",1)==1);
  struct timespec begin,end;assert(!clock_gettime(CLOCK_MONOTONIC,&begin));
  assert(b->ops->read_with_wake(b,(uint8_t *)bytes,sizeof(bytes),5000,wake[0])==GT_TRANSPORT_READ_TIMEOUT);
  assert(!clock_gettime(CLOCK_MONOTONIC,&end));assert((end.tv_sec-begin.tv_sec)*1000+(end.tv_nsec-begin.tv_nsec)/1000000<1000);
  char value;assert(read(wake[0],&value,1)==1 && value=='w'); /* backend did not consume caller wake */
  assert(write(wake[1],"w",1)==1);assert(b->ops->write(b,(const uint8_t *)"go\n",3)==GT_OK);
  used=0;memset(bytes,0,sizeof(bytes));
  for(unsigned tries=0;!strstr(bytes,"DONE");tries++) {
    assert(tries<500);int n=b->ops->read_with_wake(b,(uint8_t *)bytes+used,sizeof(bytes)-1-used,100,wake[0]);
    if(n==GT_TRANSPORT_READ_TIMEOUT){struct timespec delay={0,2000000};nanosleep(&delay,NULL);continue;}
    assert(n>0);used+=(size_t)n;
  }
  assert(read(wake[0],&value,1)==1 && value=='w'); /* output progressed with wake still pending */
  close(wake[0]);close(wake[1]);b->ops->close(b);
}
static void failure_case(void) {
  struct rlimit original,small;assert(!getrlimit(RLIMIT_NOFILE,&original));small=original;small.rlim_cur=32;assert(!setrlimit(RLIMIT_NOFILE,&small));
  int held[32];unsigned n=0;while(n<32){int fd=open("/dev/null",O_RDONLY);if(fd<0){assert(errno==EMFILE);break;}held[n++]=fd;}
  assert(n>0 && n<32);close(held[--n]); /* exactly one free slot: pipe needs two */
  unsigned before=fd_count();fixture f={0};gt_worker_config_t config={.context=&f,.open_backend=open_io,.open_state=open_state,.close_state=close_state,.state_ops={apply,freeze}};
  assert(!gt__worker_start(&config));assert(fd_count()==before);
  while(n)close(held[--n]);
  assert(!setrlimit(RLIMIT_NOFILE,&original));
}
int main(int argc,char **argv) {
  assert(argc==2);if(!strcmp(argv[1],"failure")){unsigned before=fd_count();failure_case();assert(fd_count()==before);return 0;}
  if(!strcmp(argv[1],"real")){unsigned before=fd_count();real_case();assert(fd_count()==before);return 0;}
  assert(!strcmp(argv[1],"input") || !strcmp(argv[1],"stop") || !strcmp(argv[1],"pending") || !strcmp(argv[1],"legacy"));
  for(unsigned i=0;i<8;i++)worker_case(argv[1]);
  return 0;
}
