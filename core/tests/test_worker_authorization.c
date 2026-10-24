#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef union {max_align_t align;size_t size;} allocation;
typedef struct {atomic_size_t calls,fail,live;} budget;
static void *admit(void *ctx,size_t n) {
  budget *b=ctx;size_t call=atomic_fetch_add(&b->calls,1)+1;if(call==atomic_load(&b->fail))return NULL;
  allocation *p=malloc(sizeof(*p)+n);CHECK(p);p->size=n;atomic_fetch_add(&b->live,1);return p+1;
}
static void refund(void *ctx,void *memory,size_t n) {
  budget *b=ctx;allocation *p=(allocation *)memory-1;CHECK(p->size==n && atomic_load(&b->live));
  for(size_t i=0;i<n;i++){CHECK(!((uint8_t *)memory)[i]);}
  atomic_fetch_sub(&b->live,1);free(p);
}
typedef struct {
  gt_io_backend_t backend;gt_session_worker_t *worker;budget memory;
  pthread_mutex_t lock;pthread_cond_t changed;pthread_t control,owner;
  bool entered,release,closed;unsigned callbacks,authorized,writes,resizes,applied,backend_opened;
  gt_status_t policy,backend_error,statuses[32];gt_worker_stamp_t seen[3];char bytes[64];size_t written;
  gt_user_delivery_t deliveries[32];gt_worker_stamp_t returned[32];
  uint16_t cols,rows;uint32_t dark;
} fixture;
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static struct timespec deadline(void){struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;return end;}
static int read_backend(gt_io_backend_t *backend,uint8_t *p,size_t n,int timeout){(void)backend;(void)p;(void)n;(void)timeout;pause_one();return GT_TRANSPORT_READ_TIMEOUT;}
static int write_backend(gt_io_backend_t *backend,const uint8_t *p,size_t n) {
  fixture *f=(fixture *)backend;CHECK(pthread_equal(f->owner,pthread_self()));CHECK(!pthread_mutex_lock(&f->lock));f->writes++;f->written+=n;
  if(n<=sizeof(f->bytes))memcpy(f->bytes,p,n);
  gt_status_t rc=f->backend_error;CHECK(!pthread_mutex_unlock(&f->lock));return rc;
}
static int resize_backend(gt_io_backend_t *backend,uint16_t cols,uint16_t rows,uint32_t x,uint32_t y) {
  (void)x;(void)y;fixture *f=(fixture *)backend;CHECK(pthread_equal(f->owner,pthread_self()));CHECK(!pthread_mutex_lock(&f->lock));f->resizes++;f->cols=cols;f->rows=rows;
  gt_status_t rc=f->backend_error;CHECK(!pthread_mutex_unlock(&f->lock));return rc;
}
static void close_backend(gt_io_backend_t *backend){fixture *f=(fixture *)backend;CHECK(pthread_equal(f->owner,pthread_self()));}
static const gt_io_ops_t ops={.read=read_backend,.write=write_backend,.resize=resize_backend,.close=close_backend};
static gt_io_backend_t *open_backend(void *ctx){fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()));f->backend_opened++;return &f->backend;}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t sink,void **out) {
  (void)sink;fixture *f=ctx;CHECK(!pthread_mutex_lock(&f->lock));f->owner=pthread_self();CHECK(!pthread_equal(f->control,f->owner));f->entered=true;CHECK(!pthread_cond_broadcast(&f->changed));
  struct timespec end=deadline();while(!f->release)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));
  CHECK(!pthread_mutex_unlock(&f->lock));*out=f;return GT_OK;
}
static gt_status_t apply(void *ctx,const gt_state_event_t *event) {
  fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()));CHECK(!pthread_mutex_lock(&f->lock));f->applied++;
  if(event->kind==GT_STATE_COLOR_SCHEME){CHECK(event->length==sizeof(gt_state_color_scheme_t));f->dark=((const gt_state_color_scheme_t *)event->payload)->dark;}
  else CHECK(event->kind==GT_STATE_RESIZE);
  CHECK(!pthread_mutex_unlock(&f->lock));return GT_OK;
}
static gt_status_t freeze(void *ctx,void **out){(void)ctx;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void close_state(void *ctx){fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()));CHECK(!pthread_mutex_lock(&f->lock));f->closed=true;CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));}
static gt_status_t policy(void *ctx,gt_worker_command_t kind,const gt_worker_stamp_t *stamp) {
  fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && stamp && stamp->generation && stamp->command_id);
  CHECK(kind==GT_WORKER_INPUT || kind==GT_WORKER_RESIZE || kind==GT_WORKER_COLOR_SCHEME);
  CHECK(!pthread_mutex_lock(&f->lock));
  unsigned at=kind==GT_WORKER_INPUT?0:kind==GT_WORKER_RESIZE?1:2;f->seen[at]=*stamp;f->authorized++;
  gt_status_t result=f->policy;CHECK(!pthread_mutex_unlock(&f->lock));return result;
}
static void complete(void *ctx,gt_status_t status,gt_state_capture_t capture) {
  fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && !capture.frozen);CHECK(!pthread_mutex_lock(&f->lock));CHECK(f->callbacks<32);f->statuses[f->callbacks++]=status;
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void complete_user(void *ctx,const gt_worker_user_result_t *result) {
  fixture *f=ctx;CHECK(pthread_equal(f->owner,pthread_self()) && result && result->stamp.generation && result->stamp.command_id);
  CHECK(!pthread_mutex_lock(&f->lock));CHECK(f->callbacks<32);unsigned i=f->callbacks++;f->statuses[i]=result->status;f->deliveries[i]=result->delivery;f->returned[i]=result->stamp;
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void init(fixture *f,bool capability) {
  memset(f,0,sizeof(*f));f->backend.ops=&ops;f->control=pthread_self();atomic_init(&f->memory.calls,0);atomic_init(&f->memory.fail,0);atomic_init(&f->memory.live,0);
  CHECK(!pthread_mutex_init(&f->lock,NULL));CHECK(!pthread_cond_init(&f->changed,NULL));
  gt_worker_config_t config={.context=f,.open_state=open_state,.open_backend=open_backend,.state_ops={apply,freeze},.close_state=close_state,.color_scheme_supported=true,.authorize=capability?policy:NULL};
  gt_allocator_t a={&f->memory,admit,refund};f->worker=gt__worker_start_with_allocator(&config,&a);CHECK(f->worker);
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f->lock));while(!f->entered)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void release_start(fixture *f){CHECK(!pthread_mutex_lock(&f->lock));f->release=true;CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));}
static void wait_callbacks(fixture *f,unsigned n){struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f->lock));while(f->callbacks<n)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));CHECK(!pthread_mutex_unlock(&f->lock));}
static void finish(fixture *f){gt__worker_stop(f->worker);gt__worker_join(f->worker);CHECK(f->closed && !atomic_load(&f->memory.live));CHECK(!pthread_cond_destroy(&f->changed));CHECK(!pthread_mutex_destroy(&f->lock));}
static gt_worker_stamp_t stamp(uint64_t id){return (gt_worker_stamp_t){3,id,11};}
static void submit(fixture *f,gt_worker_command_t kind,gt_worker_stamp_t s,const void *p,size_t n){CHECK(gt__worker_submit_user(f->worker,kind,&s,p,n,complete_user,f)==GT_WORKER_ACCEPTED);}
static void metadata(void) {
  fixture f;init(&f,true);gt_worker_stamp_t s=stamp(7);char bytes[]="GOOD";
  submit(&f,GT_WORKER_INPUT,s,bytes,4);memset(bytes,'X',4);s=(gt_worker_stamp_t){0};
  gt_state_size_t size={90,30,900,300};submit(&f,GT_WORKER_RESIZE,stamp(8),&size,sizeof(size));size.cols=1;
  gt_state_color_scheme_t scheme={1};submit(&f,GT_WORKER_COLOR_SCHEME,stamp(9),&scheme,sizeof(scheme));scheme.dark=0;
  release_start(&f);wait_callbacks(&f,3);
  CHECK(f.authorized==3 && f.writes==1 && f.resizes==1 && f.applied==2 && f.cols==90 && f.rows==30 && f.dark==1 && !memcmp(f.bytes,"GOOD",4));
  for(unsigned i=0;i<3;i++)CHECK(f.statuses[i]==GT_OK && f.deliveries[i]==GT_USER_OPERATION_OK && f.seen[i].generation==3 && f.seen[i].command_id==7+i && f.seen[i].applied_sequence==11 && !memcmp(&f.seen[i],&f.returned[i],sizeof(gt_worker_stamp_t)));
  finish(&f);
}
static void denial(void) {
  fixture f;init(&f,true);release_start(&f);unsigned count=0;gt_status_t values[]={GT_ERR_AUTH,GT_ERR_PROTOCOL,GT_ERR_INTERNAL,(gt_status_t)99};
  for(unsigned v=0;v<4;v++) {
    CHECK(!pthread_mutex_lock(&f.lock));f.policy=values[v];CHECK(!pthread_mutex_unlock(&f.lock));
    char byte='X';gt_state_size_t size={90,30,0,0};gt_state_color_scheme_t scheme={1};
    submit(&f,GT_WORKER_INPUT,stamp(count+1),&byte,1);submit(&f,GT_WORKER_RESIZE,stamp(count+2),&size,sizeof(size));submit(&f,GT_WORKER_COLOR_SCHEME,stamp(count+3),&scheme,sizeof(scheme));
    wait_callbacks(&f,count+3);for(unsigned i=count;i<count+3;i++)CHECK(f.statuses[i]==(v==3?GT_ERR_INTERNAL:values[v]) && f.deliveries[i]==GT_USER_NOT_EXECUTED);count+=3;
  }
  CHECK(!f.writes && !f.resizes && !f.applied && f.authorized==12);gt_worker_result_t result;CHECK(gt__worker_result(f.worker,&result)==GT_WORKER_RESULT_PENDING);
  CHECK(!pthread_mutex_lock(&f.lock));f.policy=GT_OK;CHECK(!pthread_mutex_unlock(&f.lock));submit(&f,GT_WORKER_INPUT,stamp(13),"Y",1);wait_callbacks(&f,13);CHECK(f.statuses[12]==GT_OK && f.writes==1);finish(&f);
}
static void capability(void) {
  fixture f;init(&f,false);gt_worker_stamp_t s=stamp(1);CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,&s,"X",1,complete_user,&f)==GT_WORKER_UNSUPPORTED);
  s.generation=0;CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,&s,"X",1,complete_user,&f)==GT_WORKER_INVALID);s=stamp(0);CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,&s,"X",1,complete_user,&f)==GT_WORKER_INVALID);s=stamp(1);
  CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,NULL,"X",1,complete_user,&f)==GT_WORKER_INVALID);
  CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,&s,"X",1,NULL,&f)==GT_WORKER_INVALID);
  CHECK(gt__worker_submit_user(f.worker,GT_WORKER_CAPTURE,&s,NULL,0,complete_user,&f)==GT_WORKER_INVALID);
  CHECK(!f.callbacks && atomic_load(&f.memory.calls)==1);CHECK(gt__worker_submit_internal(f.worker,GT_WORKER_INPUT,"HOST",4,complete,&f)==GT_WORKER_ACCEPTED);
  release_start(&f);wait_callbacks(&f,1);CHECK(f.writes==1 && !f.authorized && f.statuses[0]==GT_OK);finish(&f);
}
static void backend_failure(void) {
  for(unsigned resize=0;resize<2;resize++) {
    fixture f;init(&f,true);f.backend_error=GT_ERR_AUTH;gt_state_size_t size={90,30,0,0};
    if(resize)submit(&f,GT_WORKER_RESIZE,stamp(1),&size,sizeof(size));else submit(&f,GT_WORKER_INPUT,stamp(1),"X",1);
    release_start(&f);wait_callbacks(&f,1);gt_worker_result_t result;
    for(unsigned i=0;;i++){CHECK(i<5000);if(gt__worker_result(f.worker,&result)==GT_WORKER_RESULT_READY)break;pause_one();}
    CHECK(result.kind==GT_WORKER_END_ERROR && result.status==GT_ERR_AUTH && result.stage==(resize?GT_WORKER_STAGE_RESIZE:GT_WORKER_STAGE_INPUT));
    CHECK(f.authorized==1 && f.statuses[0]==GT_ERR_AUTH && f.deliveries[0]==GT_USER_OUTCOME_UNKNOWN && f.writes+f.resizes==1);finish(&f);
  }
}
static void cancellation(void) {
  fixture f;init(&f,true);gt_state_size_t size={90,30,0,0};submit(&f,GT_WORKER_INPUT,stamp(1),"SECRET",6);submit(&f,GT_WORKER_RESIZE,stamp(2),&size,sizeof(size));
  gt__worker_stop(f.worker);release_start(&f);wait_callbacks(&f,2);CHECK(f.statuses[0]==GT_ERR_CANCELLED && f.statuses[1]==GT_ERR_CANCELLED && f.deliveries[0]==GT_USER_NOT_EXECUTED && f.deliveries[1]==GT_USER_NOT_EXECUTED && !f.authorized && !f.backend_opened);finish(&f);
}
static void allocation_test(void) {
  fixture f;init(&f,true);atomic_store(&f.memory.fail,atomic_load(&f.memory.calls)+1);gt_worker_stamp_t s=stamp(1);
  CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,&s,"X",1,complete_user,&f)==GT_WORKER_NO_MEMORY && !f.callbacks && atomic_load(&f.memory.live)==1);
  char *bytes=malloc(65536);CHECK(bytes);memset(bytes,'X',65536);submit(&f,GT_WORKER_INPUT,s,bytes,65536);free(bytes);
  CHECK(gt__worker_submit_user(f.worker,GT_WORKER_INPUT,&s,"X",1,complete_user,&f)==GT_WORKER_BACKPRESSURE);
  release_start(&f);wait_callbacks(&f,1);CHECK(f.written==65536 && f.authorized==1 && f.statuses[0]==GT_OK);finish(&f);
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"metadata"))metadata();else if(!strcmp(argv[1],"denial"))denial();else if(!strcmp(argv[1],"capability"))capability();
  else if(!strcmp(argv[1],"backend"))backend_failure();else if(!strcmp(argv[1],"cancel"))cancellation();else if(!strcmp(argv[1],"allocation"))allocation_test();else CHECK(false);
  printf("PASS worker authorization %s\n",argv[1]);return 0;
}
