#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct {gt_io_backend_t backend;gt_session_worker_t *worker;pthread_mutex_t lock;pthread_cond_t changed;pthread_t owner;const char *mode;
  bool entered,go,sealed,release,finished;unsigned reads,backend_open,backend_close,state_close,freezes,invalidated,finals;gt_status_t captured;gt_state_capture_t final;} fixture;
static bool is(fixture *f,const char *s){return !strcmp(f->mode,s);}
static struct timespec deadline(void){struct timespec t;CHECK(!clock_gettime(CLOCK_REALTIME,&t));t.tv_sec+=5;return t;}
static void wait_flag(fixture *f,bool *flag){struct timespec t=deadline();CHECK(!pthread_mutex_lock(&f->lock));while(!*flag)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&t));CHECK(!pthread_mutex_unlock(&f->lock));}
static void set_flag(fixture *f,bool *flag){CHECK(!pthread_mutex_lock(&f->lock));*flag=true;CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t sink,void **out){fixture *f=ctx;CHECK(sink.write);f->owner=pthread_self();set_flag(f,&f->entered);wait_flag(f,&f->go);if(is(f,"startup"))return GT_ERR_NOT_FOUND;*out=f;return GT_OK;}
static int read_backend(gt_io_backend_t *b,uint8_t *p,size_t n,int timeout){(void)timeout;fixture *f=(fixture *)b;CHECK(pthread_equal(f->owner,pthread_self()) && n);if(!f->reads++ && !is(f,"zero")){*p='X';return 1;}return GT_TRANSPORT_READ_EOF;}
static int write_backend(gt_io_backend_t *b,const uint8_t *p,size_t n){(void)b;(void)p;(void)n;CHECK(false);return GT_ERR_IO;}
static int resize_backend(gt_io_backend_t *b,uint16_t c,uint16_t r,uint32_t x,uint32_t y){(void)b;(void)c;(void)r;(void)x;(void)y;CHECK(false);return GT_ERR_IO;}
static void close_backend(gt_io_backend_t *b){fixture *f=(fixture *)b;CHECK(!f->backend_close && !f->state_close);f->backend_close++;}
static const gt_io_ops_t ops={.read=read_backend,.write=write_backend,.resize=resize_backend,.close=close_backend};
static gt_io_backend_t *open_backend(void *ctx){fixture *f=ctx;if(is(f,"backend"))return NULL;f->backend_open++;f->backend.ops=&ops;return &f->backend;}
static gt_status_t apply(void *ctx,const gt_state_event_t *e){fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && !f->state_close && e->kind==GT_STATE_OUTPUT && e->length==1 && *(const char *)e->payload=='X');return is(f,"invalid-state")?GT_ERR_PROTOCOL:GT_OK;}
static gt_status_t freeze(void *ctx,void **out){fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && !f->state_close && f->backend_close==f->backend_open);f->freezes++;*out=NULL;if(is(f,"failure"))return GT_ERR_UNSUPPORTED;if(is(f,"allocation"))return GT_ERR_INTERNAL;if(is(f,"null"))return GT_OK;char *p=malloc(1);CHECK(p);*p='X';*out=p;return GT_OK;}
static void invalidated(void *ctx,gt_status_t status,uint64_t sequence){fixture *f=ctx;CHECK(!f->invalidated && !f->state_close && f->backend_close==f->backend_open);CHECK((is(f,"null") && status==GT_ERR_INTERNAL && sequence==1) || (is(f,"invalid-state") && status==GT_ERR_PROTOCOL && sequence==0));f->invalidated++;}
static void close_state(void *ctx){fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && !f->state_close);f->state_close++;}
static void final(void *ctx,gt_status_t status,gt_state_capture_t capture){fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && !f->finals && !f->state_close && f->backend_close==f->backend_open);gt_worker_result_t result;CHECK(gt__worker_result(f->worker,&result)==GT_WORKER_RESULT_PENDING);f->captured=status;f->final=capture;f->finals++;set_flag(f,&f->sealed);if(is(f,"flow"))wait_flag(f,&f->release);}
static void finished(void *ctx,gt_status_t status){fixture *f=ctx;(void)status;CHECK(f->state_close==(is(f,"startup")?0u:1u));set_flag(f,&f->finished);}
int main(int argc,char **argv) {
  CHECK(argc==2);fixture f={.mode=argv[1]};CHECK(!pthread_mutex_init(&f.lock,NULL) && !pthread_cond_init(&f.changed,NULL));
  CHECK(is(&f,"flow") || is(&f,"zero") || is(&f,"failure") || is(&f,"allocation") || is(&f,"null") || is(&f,"startup") || is(&f,"backend") || is(&f,"invalid-state") || is(&f,"stop") || is(&f,"disabled"));
  gt_worker_config_t c={.context=&f,.open_state=open_state,.open_backend=open_backend,.state_ops={apply,freeze},.close_state=close_state,.state_invalidated=invalidated,.final_capture=is(&f,"disabled")?NULL:final,.finished=finished};
  f.worker=gt__worker_start(&c);CHECK(f.worker);wait_flag(&f,&f.entered);if(is(&f,"stop"))gt__worker_stop(f.worker);set_flag(&f,&f.go);
  if(is(&f,"flow")){wait_flag(&f,&f.sealed);gt_worker_result_t r;CHECK(gt__worker_result(f.worker,&r)==GT_WORKER_RESULT_PENDING && !f.state_close);set_flag(&f,&f.release);}
  wait_flag(&f,&f.finished);gt_worker_result_t r;CHECK(gt__worker_result(f.worker,&r)==GT_WORKER_RESULT_READY);gt__worker_join(f.worker);
  bool startup=is(&f,"startup"),backend=is(&f,"backend"),bad=is(&f,"invalid-state"),null=is(&f,"null"),disabled=is(&f,"disabled");
  CHECK(f.finals==(disabled?0u:1u) && f.freezes==((startup || backend || bad || disabled)?0u:1u));
  CHECK(r.has_sequence==!startup && r.sequence==((startup || backend || bad || is(&f,"zero") || is(&f,"stop"))?0u:1u));
  if(startup || backend || bad || null){gt_status_t status=startup?GT_ERR_NOT_FOUND:backend?GT_ERR_IO:bad?GT_ERR_PROTOCOL:GT_ERR_INTERNAL;CHECK(r.kind==GT_WORKER_END_ERROR && r.status==status && !f.final.frozen && f.captured==status);CHECK(r.stage==(startup?GT_WORKER_STAGE_STATE_OPEN:backend?GT_WORKER_STAGE_BACKEND_OPEN:bad?GT_WORKER_STAGE_APPLY:GT_WORKER_STAGE_CAPTURE));}
  else {CHECK(r.status==GT_OK && r.kind==(is(&f,"stop")?GT_WORKER_END_STOPPED:GT_WORKER_END_EOF));if(is(&f,"failure") || is(&f,"allocation"))CHECK(f.captured==(is(&f,"allocation")?GT_ERR_INTERNAL:GT_ERR_UNSUPPORTED) && !f.final.frozen);else if(!disabled)CHECK(f.captured==GT_OK && f.final.frozen && *(char *)f.final.frozen=='X' && f.final.sequence==r.sequence);}
  CHECK(f.invalidated==((bad || null)?1u:0u));free(f.final.frozen);CHECK(!pthread_cond_destroy(&f.changed) && !pthread_mutex_destroy(&f.lock));printf("PASS terminal capture %s\n",f.mode);return 0;
}
