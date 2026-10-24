#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct fixture fixture;
typedef struct {gt_io_backend_t base;gt_io_backend_t *real;fixture *f;} backend;
struct fixture {pthread_mutex_t lock;pthread_cond_t changed;bool ready,release,finished,fail;
  unsigned completed,events,writes,resizes,invalidated,closed;uint32_t dark;
  gt_status_t completion,ending;gt_state_capture_t capture;};
static void wait_for(fixture *f,bool completion,unsigned n){
  struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
  pthread_mutex_lock(&f->lock);
  while(completion?f->completed<n:!f->ready)assert(!pthread_cond_timedwait(&f->changed,&f->lock,&deadline));
  pthread_mutex_unlock(&f->lock);
}
static int read_io(gt_io_backend_t *b,uint8_t *p,size_t n,int ms){backend *w=(backend *)b;return w->real->ops->read(w->real,p,n,ms);}
static int write_io(gt_io_backend_t *b,const uint8_t *p,size_t n){backend *w=(backend *)b;w->f->writes++;return w->real->ops->write(w->real,p,n);}
static int resize_io(gt_io_backend_t *b,uint16_t c,uint16_t r,uint32_t x,uint32_t y){backend *w=(backend *)b;w->f->resizes++;return w->real->ops->resize(w->real,c,r,x,y);}
static bool exited_io(const gt_io_backend_t *b,int *c){const backend *w=(const backend *)b;return w->real->ops->exited(w->real,c);}
static void close_io(gt_io_backend_t *b){backend *w=(backend *)b;w->real->ops->close(w->real);free(w);}
static const gt_io_ops_t ops={.read=read_io,.write=write_io,.resize=resize_io,.exited=exited_io,.close=close_io};
static gt_io_backend_t *open_io(void *context){
  const char *args[]={"-c","stty -echo; printf R; read line",NULL};
  gt_pty_opts_t options={.executable="/bin/sh",.cmd_kind=GT_PTY_CMD_DIRECT,.argv=args,.cols=80,.rows=24};
  backend *w=calloc(1,sizeof(*w));assert(w);w->real=gt__pty_open(&options);assert(w->real);w->base.ops=&ops;w->f=context;return &w->base;
}
static gt_status_t open_state(void *c,gt_worker_reply_sink_t sink,void **out){(void)sink;*out=c;return GT_OK;}
static void close_state(void *c){fixture *f=c;assert(f->invalidated==(f->fail?1u:0u));f->closed++;}
static gt_status_t apply(void *c,const gt_state_event_t *e){
  fixture *f=c;
  if(e->kind==GT_STATE_OUTPUT){assert(e->length==1 && *(const char *)e->payload=='R');return GT_OK;}
  assert(e->kind==GT_STATE_COLOR_SCHEME && e->length==sizeof(gt_state_color_scheme_t));
  if(f->fail)return GT_ERR_UNSUPPORTED;
  f->dark=((const gt_state_color_scheme_t *)e->payload)->dark;return GT_OK;
}
static gt_status_t freeze(void *c,void **out){fixture *f=c;uint32_t *copy=malloc(sizeof(*copy));assert(copy);*copy=f->dark;*out=copy;return GT_OK;}
static void event(void *c,const gt_state_event_t *e,uint64_t sequence){fixture *f=c;assert(sequence==++f->events);assert(e->kind==(sequence==1?GT_STATE_OUTPUT:GT_STATE_COLOR_SCHEME));}
static void output(void *c,const uint8_t *p,size_t n,uint64_t sequence){fixture *f=c;assert(n==1 && *p=='R' && sequence==1);pthread_mutex_lock(&f->lock);f->ready=true;pthread_cond_broadcast(&f->changed);while(!f->release)pthread_cond_wait(&f->changed,&f->lock);pthread_mutex_unlock(&f->lock);}
static void complete(void *c,gt_status_t rc,gt_state_capture_t capture){fixture *f=c;pthread_mutex_lock(&f->lock);f->completed++;f->completion=rc;f->capture=capture;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);}
static void invalidated(void *c,gt_status_t rc,uint64_t sequence){fixture *f=c;assert(f->fail && rc==GT_ERR_UNSUPPORTED && sequence==1 && !f->closed);f->invalidated++;}
static void finished(void *c,gt_status_t rc){fixture *f=c;pthread_mutex_lock(&f->lock);f->ending=rc;f->finished=true;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);}
int main(int argc,char **argv){
  assert(argc==2);bool unsupported=!strcmp(argv[1],"unsupported"),invalid=!strcmp(argv[1],"invalid");
  fixture f={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.fail=!strcmp(argv[1],"failure")};
  assert(unsupported || invalid || f.fail || !strcmp(argv[1],"flow"));
  gt_worker_config_t config={.context=&f,.open_backend=open_io,.open_state=open_state,.close_state=close_state,.state_ops={apply,freeze},.event=event,.output=output,.finished=finished,.state_invalidated=invalidated,.color_scheme_supported=!unsupported};
  gt_session_worker_t *worker=gt__worker_start(&config);assert(worker);wait_for(&f,false,0);
  gt_state_color_scheme_t scheme={1};
  if(invalid){scheme.dark=2;assert(gt__worker_submit_internal(worker,GT_WORKER_COLOR_SCHEME,&scheme,sizeof(scheme),complete,&f)==GT_WORKER_INVALID);scheme.dark=1;assert(gt__worker_submit_internal(worker,GT_WORKER_COLOR_SCHEME,&scheme,sizeof(scheme)-1,complete,&f)==GT_WORKER_INVALID);assert(gt__worker_submit_internal(worker,GT_WORKER_COLOR_SCHEME,NULL,sizeof(scheme),complete,&f)==GT_WORKER_INVALID);}
  else assert(gt__worker_submit_internal(worker,GT_WORKER_COLOR_SCHEME,&scheme,sizeof(scheme),complete,&f)==(unsupported?GT_WORKER_UNSUPPORTED:GT_WORKER_ACCEPTED));
  scheme.dark=0; /* Caller may immediately reuse accepted POD. */
  if(!unsupported && !invalid && !f.fail)assert(gt__worker_submit_internal(worker,GT_WORKER_CAPTURE,NULL,0,complete,&f)==GT_WORKER_ACCEPTED);
  if(unsupported || invalid)gt__worker_stop(worker);
  pthread_mutex_lock(&f.lock);f.release=true;pthread_cond_broadcast(&f.changed);pthread_mutex_unlock(&f.lock);
  if(!unsupported && !invalid){wait_for(&f,true,f.fail?1:2);assert(f.completion==(f.fail?GT_ERR_UNSUPPORTED:GT_OK));if(!f.fail){assert(f.capture.sequence==2 && f.capture.frozen && *(uint32_t *)f.capture.frozen==1);free(f.capture.frozen);gt__worker_stop(worker);}}
  gt_worker_result_t result;struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;pthread_mutex_lock(&f.lock);while(!f.finished)assert(!pthread_cond_timedwait(&f.changed,&f.lock,&deadline));pthread_mutex_unlock(&f.lock);
  assert(gt__worker_result(worker,&result)==GT_WORKER_RESULT_READY);
  assert(result.has_sequence && result.sequence==(unsupported || invalid || f.fail?1u:2u) && f.events==result.sequence);
  assert(result.status==(f.fail?GT_ERR_UNSUPPORTED:GT_OK) && f.ending==result.status && result.stage==(f.fail?GT_WORKER_STAGE_APPLY:GT_WORKER_STAGE_NONE));
  assert(!f.writes && !f.resizes && f.dark==(unsupported || invalid || f.fail?0u:1u) && f.closed==1);
  gt__worker_join(worker);assert(!pthread_cond_destroy(&f.changed) && !pthread_mutex_destroy(&f.lock));return 0;
}
