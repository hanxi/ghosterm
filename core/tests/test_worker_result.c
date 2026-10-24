#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
typedef struct fixture fixture_t;
typedef struct {gt_io_backend_t base;gt_io_backend_t *real;fixture_t *f;gt_io_ops_t own_ops;} wrapped_t;
struct fixture {
  pthread_mutex_t lock;pthread_cond_t changed;
  const char *mode;gt_session_worker_t *worker;gt_worker_reply_sink_t sink;
  bool opened,start,ready,release,finished;
  unsigned applies,events,writes,outputs,state_closed,backend_closed,completed,invalidations;
  gt_status_t command_status,callback_status;gt_worker_result_t result;
};
static bool mode(fixture_t *f,const char *s){return !strcmp(f->mode,s);}
static bool is_eof(fixture_t *f){return !strncmp(f->mode,"eof-",4);}
static void wait_flag(fixture_t *f,const bool *flag) {
  struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  pthread_mutex_lock(&f->lock);
  while(!*flag)assert(!pthread_cond_timedwait(&f->changed,&f->lock,&end));
  pthread_mutex_unlock(&f->lock);
}
static void release(fixture_t *f,bool initial) {
  pthread_mutex_lock(&f->lock);if(initial)f->start=true;else f->release=true;
  pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);
}
static gt_status_t open_state(void *context,gt_worker_reply_sink_t sink,void **out) {
  fixture_t *f=context;f->sink=sink;pthread_mutex_lock(&f->lock);
  f->opened=true;pthread_cond_broadcast(&f->changed);
  while(!f->start)pthread_cond_wait(&f->changed,&f->lock);
  pthread_mutex_unlock(&f->lock);
  if(mode(f,"state-open"))return GT_ERR_NOT_FOUND;
  *out=f;return GT_OK;
}
static bool corrupt(fixture_t *f){return mode(f,"apply") || mode(f,"capture-null");}
static void state_invalidated(void *context,gt_status_t error,uint64_t sequence) {
  fixture_t *f=context;
  assert(corrupt(f) && !f->invalidations && !f->state_closed && f->backend_closed==1);
  assert(error==(mode(f,"apply")?GT_ERR_PROTOCOL:GT_ERR_INTERNAL));
  assert(sequence==(mode(f,"apply")?0u:1u));
  gt_worker_result_t result;assert(gt__worker_result(f->worker,&result)==GT_WORKER_RESULT_PENDING);
  f->invalidations++;
}
static void close_state(void *context){fixture_t *f=context;assert(f->invalidations==(corrupt(f)?1u:0u));f->state_closed++;}
static int read_backend(gt_io_backend_t *base,uint8_t *bytes,size_t n,int timeout) {
  wrapped_t *b=(wrapped_t *)base;fixture_t *f=b->f;
  if(mode(f,"read") && f->applies)return GT_TRANSPORT_READ_ERROR;
  int rc=b->real->ops->read(b->real,bytes,n,timeout);
  if(rc==GT_TRANSPORT_READ_EOF && is_eof(f)) {
    int code=-1;unsigned attempts=0;
    while(!b->real->ops->exited(b->real,&code)) {
      assert(attempts++<1000);struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    assert(code==(mode(f,"eof-signal")?128+SIGTERM:mode(f,"eof-zero")?0:7));
  }
  return rc;
}
static int write_backend(gt_io_backend_t *base,const uint8_t *bytes,size_t n) {
  wrapped_t *b=(wrapped_t *)base;b->f->writes++;
  if(mode(b->f,"input"))return GT_ERR_AUTH; /* Verify opaque error preservation. */
  if(mode(b->f,"reply"))return GT_ERR_IO;
  return b->real->ops->write(b->real,bytes,n);
}
static int resize_backend(gt_io_backend_t *base,uint16_t c,uint16_t r,uint32_t x,uint32_t y) {
  wrapped_t *b=(wrapped_t *)base;if(mode(b->f,"resize"))return GT_ERR_UNSUPPORTED;
  return b->real->ops->resize(b->real,c,r,x,y);
}
static bool exited_backend(const gt_io_backend_t *base,int *code) {
  const wrapped_t *b=(const wrapped_t *)base;
  if(mode(b->f,"eof-unknown"))return false; /* Simulate unavailable observation. */
  return b->real->ops->exited(b->real,code);
}
static gt_status_t observe_backend(const gt_io_backend_t *base,gt_io_exit_t *out) {
  const wrapped_t *b=(const wrapped_t *)base;
  if(mode(b->f,"eof-unknown"))return GT_ERR_NOT_FOUND;
  if(mode(b->f,"eof-invalid")){*out=(gt_io_exit_t){GT_IO_EXIT_UNKNOWN,7};return GT_OK;}
  assert(b->real->ops->observe_exit);return b->real->ops->observe_exit(b->real,out);
}
static void close_backend(gt_io_backend_t *base) {
  wrapped_t *b=(wrapped_t *)base;b->real->ops->close(b->real);b->f->backend_closed++;free(b);
}
static const gt_io_ops_t ops={.read=read_backend,.write=write_backend,.resize=resize_backend,.exited=exited_backend,.close=close_backend,.observe_exit=observe_backend};
static gt_io_backend_t *open_backend(void *context) {
  fixture_t *f=context;if(mode(f,"backend-open"))return NULL;
  const char *script=is_eof(f)?(mode(f,"eof-signal")?"printf R; kill -TERM $$":mode(f,"eof-zero")?"printf R; exit 0":"printf R; exit 7"):"stty -echo; printf R; read x";
  const char *args[]={"-c",script,NULL};gt_pty_opts_t o={.executable="/bin/sh",.cmd_kind=GT_PTY_CMD_DIRECT,.argv=args,.cols=80,.rows=24};
  wrapped_t *b=calloc(1,sizeof(*b));assert(b);b->real=gt__pty_open(&o);assert(b->real);
  b->own_ops=ops;if(mode(f,"eof-unavailable"))b->own_ops.observe_exit=NULL;
  b->base.ops=&b->own_ops;b->f=f;return &b->base;
}
static gt_status_t apply(void *context,const gt_state_event_t *event) {
  fixture_t *f=context;assert(event->kind==GT_STATE_OUTPUT);f->applies++;
  if(mode(f,"apply"))return GT_ERR_PROTOCOL;
  if(mode(f,"reply"))assert(f->sink.write(f->sink.context,(const uint8_t *)"x",1)==GT_OK);
  return GT_OK;
}
static gt_status_t freeze(void *context,void **out) {
  fixture_t *f=context;*out=NULL;assert(mode(f,"capture") || mode(f,"capture-null"));
  return mode(f,"capture")?GT_ERR_IO:GT_OK;
}
static void event(void *context,const gt_state_event_t *e,uint64_t sequence) {
  fixture_t *f=context;assert(e->kind==GT_STATE_OUTPUT && e->length==1 && *(const char *)e->payload=='R');
  assert(sequence==++f->events && !f->outputs && !f->state_closed);
  if(mode(f,"reply"))assert(f->writes==1);
}
static void output(void *context,const uint8_t *bytes,size_t n,uint64_t sequence) {
  fixture_t *f=context;assert(n==1 && bytes[0]=='R' && sequence==1);f->outputs++;
  pthread_mutex_lock(&f->lock);f->ready=true;pthread_cond_broadcast(&f->changed);
  while(!f->release && !mode(f,"reply"))pthread_cond_wait(&f->changed,&f->lock);
  pthread_mutex_unlock(&f->lock);
}
static void completed(void *context,gt_status_t status,gt_state_capture_t capture) {
  fixture_t *f=context;assert(!capture.frozen);pthread_mutex_lock(&f->lock);
  f->completed++;f->command_status=status;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);
}
static void finished(void *context,gt_status_t status) {
  fixture_t *f=context;gt_worker_result_t r;
  /* The initial factory gate publishes the handle before this callback. */
  assert(gt__worker_result(f->worker,&r)==GT_WORKER_RESULT_READY && r.status==status);
  assert(f->state_closed==(mode(f,"state-open")?0u:1u));
  assert(f->backend_closed==((mode(f,"state-open") || mode(f,"backend-open") || mode(f,"stop"))?0u:1u));
  pthread_mutex_lock(&f->lock);f->result=r;f->callback_status=status;f->finished=true;
  pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);
}
static void check_same(const gt_worker_result_t *a,const gt_worker_result_t *b) {
  assert(a->kind==b->kind && a->stage==b->stage && a->status==b->status && a->has_sequence==b->has_sequence && a->sequence==b->sequence && a->has_exit_code==b->has_exit_code && a->exit_code==b->exit_code && a->termination_status==b->termination_status && a->termination.kind==b->termination.kind && a->termination.code==b->termination.code);
}
int main(int argc,char **argv) {
  assert(argc==2);gt_worker_result_t r;
  if(!strcmp(argv[1],"invalid")) {
    memset(&r,0xa5,sizeof(r));assert(gt__worker_result(NULL,&r)==GT_WORKER_RESULT_INVALID);
    assert(r.kind==GT_WORKER_END_UNKNOWN && !r.has_sequence && !r.has_exit_code && !r.sequence);
    assert(gt__worker_result(NULL,NULL)==GT_WORKER_RESULT_INVALID);return 0;
  }
  fixture_t f={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.mode=argv[1]};
  gt_worker_config_t c={.context=&f,.open_state=open_state,.close_state=close_state,.open_backend=open_backend,
    .state_ops={apply,freeze},.output=output,.event=event,.state_invalidated=state_invalidated,.finished=finished};
  f.worker=gt__worker_start(&c);assert(f.worker);wait_flag(&f,&f.opened);
  assert(gt__worker_result(f.worker,NULL)==GT_WORKER_RESULT_INVALID);
  memset(&r,0xa5,sizeof(r));assert(gt__worker_result(f.worker,&r)==GT_WORKER_RESULT_PENDING);
  assert(r.kind==GT_WORKER_END_UNKNOWN && !r.has_sequence && !r.has_exit_code && !r.sequence);
  if(mode(&f,"stop"))gt__worker_stop(f.worker);
  release(&f,true);
  if(!mode(&f,"state-open") && !mode(&f,"backend-open") && !mode(&f,"stop") && !mode(&f,"apply") && !mode(&f,"reply")) {
    wait_flag(&f,&f.ready);
    if(mode(&f,"input"))assert(gt__worker_submit_internal(f.worker,GT_WORKER_INPUT,"x",1,completed,&f)==GT_WORKER_ACCEPTED);
    if(mode(&f,"resize")){gt_state_size_t size={91,31,910,620};assert(gt__worker_submit_internal(f.worker,GT_WORKER_RESIZE,&size,sizeof(size),completed,&f)==GT_WORKER_ACCEPTED);}
    if(mode(&f,"capture") || mode(&f,"capture-null"))assert(gt__worker_submit_internal(f.worker,GT_WORKER_CAPTURE,NULL,0,completed,&f)==GT_WORKER_ACCEPTED);
    release(&f,false);
    if(mode(&f,"capture")) {
      struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
      pthread_mutex_lock(&f.lock);while(!f.completed)assert(!pthread_cond_timedwait(&f.changed,&f.lock,&end));pthread_mutex_unlock(&f.lock);
      assert(f.command_status==GT_ERR_IO && gt__worker_result(f.worker,&r)==GT_WORKER_RESULT_PENDING);
      gt__worker_stop(f.worker);
    }
  }
  wait_flag(&f,&f.finished);assert(gt__worker_result(f.worker,&r)==GT_WORKER_RESULT_READY);check_same(&r,&f.result);
  bool stopped=mode(&f,"stop") || mode(&f,"capture"),eof=is_eof(&f);
  assert(r.kind==(stopped?GT_WORKER_END_STOPPED:eof?GT_WORKER_END_EOF:GT_WORKER_END_ERROR));
  assert(f.events==r.sequence && f.outputs==f.events);
  assert(r.has_sequence==!mode(&f,"state-open"));
  assert(r.sequence==((mode(&f,"state-open") || mode(&f,"backend-open") || mode(&f,"stop") || mode(&f,"apply"))?0u:1u));
  assert(r.has_exit_code==(mode(&f,"eof-known") || mode(&f,"eof-zero")));if(r.has_exit_code)assert(r.exit_code==(mode(&f,"eof-zero")?0:7));else assert(!r.exit_code);
  gt_status_t observed=mode(&f,"eof-known") || mode(&f,"eof-zero") || mode(&f,"eof-signal")?GT_OK:mode(&f,"eof-unavailable")?GT_ERR_UNSUPPORTED:mode(&f,"eof-invalid")?GT_ERR_PROTOCOL:GT_ERR_NOT_FOUND;
  assert(r.termination_status==observed);
  if(mode(&f,"eof-known") || mode(&f,"eof-zero"))assert(r.termination.kind==GT_IO_EXIT_CODE && r.termination.code==(mode(&f,"eof-zero")?0:7));
  else if(mode(&f,"eof-signal"))assert(r.termination.kind==GT_IO_EXIT_SIGNAL && r.termination.code==SIGTERM);
  else assert(r.termination.kind==GT_IO_EXIT_UNKNOWN && !r.termination.code);
  gt_status_t expected=stopped || eof?GT_OK:mode(&f,"state-open")?GT_ERR_NOT_FOUND:mode(&f,"input")?GT_ERR_AUTH:mode(&f,"resize")?GT_ERR_UNSUPPORTED:mode(&f,"apply")?GT_ERR_PROTOCOL:mode(&f,"capture-null")?GT_ERR_INTERNAL:GT_ERR_IO;
  gt_worker_stage_t stage=stopped || eof?GT_WORKER_STAGE_NONE:mode(&f,"state-open")?GT_WORKER_STAGE_STATE_OPEN:mode(&f,"backend-open")?GT_WORKER_STAGE_BACKEND_OPEN:mode(&f,"input")?GT_WORKER_STAGE_INPUT:mode(&f,"resize")?GT_WORKER_STAGE_RESIZE:mode(&f,"apply")?GT_WORKER_STAGE_APPLY:mode(&f,"reply")?GT_WORKER_STAGE_REPLY:mode(&f,"capture-null")?GT_WORKER_STAGE_CAPTURE:GT_WORKER_STAGE_READ;
  assert(r.status==expected && r.stage==stage && f.callback_status==expected);
  if(mode(&f,"input") || mode(&f,"resize") || mode(&f,"capture-null"))assert(f.completed==1 && f.command_status==expected);
  gt__worker_stop(f.worker);gt_worker_result_t again;assert(gt__worker_result(f.worker,&again)==GT_WORKER_RESULT_READY);check_same(&r,&again);
  assert(gt__worker_submit_internal(f.worker,GT_WORKER_INPUT,"x",1,NULL,NULL)==GT_WORKER_STOPPED);
  gt__worker_join(f.worker);assert(!pthread_cond_destroy(&f.changed) && !pthread_mutex_destroy(&f.lock));return 0;
}
