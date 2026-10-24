#include "session/checkpoint-task.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "allocation-test.h"

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  pthread_t caller;
  gt_checkpoint_tasks_t *pool;
  uint64_t id;
  bool entered, proceed, fail, empty;
  unsigned encodes, input_frees, result_frees;
} fixture_t;
static void init(fixture_t *f,gt_checkpoint_tasks_t *pool,bool proceed) {
  memset(f,0,sizeof(*f));
  assert(!pthread_mutex_init(&f->lock,NULL));
  assert(!pthread_cond_init(&f->changed,NULL));
  f->caller=pthread_self();f->pool=pool;f->proceed=proceed;
}
static void destroy(fixture_t *f) {
  assert(!pthread_cond_destroy(&f->changed));assert(!pthread_mutex_destroy(&f->lock));
}
static gt_status_t encode(void *context,void **out) {
  fixture_t *f=context;
  assert(!pthread_equal(f->caller,pthread_self()));
  pthread_mutex_lock(&f->lock);f->encodes++;f->entered=true;
  pthread_cond_broadcast(&f->changed);
  while(!f->proceed) pthread_cond_wait(&f->changed,&f->lock);
  pthread_mutex_unlock(&f->lock);
  if(!f->empty) {unsigned *value=malloc(sizeof(*value));assert(value);*value=42;*out=value;}
  return f->fail ? GT_ERR_IO : GT_OK;
}
static void release_input(void *context) {
  fixture_t *f=context;
  assert(!pthread_equal(f->caller,pthread_self()));
  /* Callback can reenter registry APIs: no executor lock may be held here. */
  if(f->id) {gt_task_info_t info;assert(gt__tasks_info(f->pool,f->id,&info)==GT_TASK_OK);}
  pthread_mutex_lock(&f->lock);f->input_frees++;pthread_mutex_unlock(&f->lock);
}
static void release_result(void *context,void *result) {
  fixture_t *f=context;
  assert(!pthread_equal(f->caller,pthread_self()));
  assert(*(unsigned *)result==42);free(result);
  pthread_mutex_lock(&f->lock);f->result_frees++;pthread_mutex_unlock(&f->lock);
}
static gt_task_spec_t spec(fixture_t *f) {
  return (gt_task_spec_t){f,encode,release_input,release_result,7,40};
}
static void submit(fixture_t *f) {
  gt_task_spec_t s=spec(f);uint64_t id;
  assert(gt__tasks_submit(f->pool,&s,&id)==GT_TASK_OK);
  /* id is not used by callbacks until encoder's explicit barrier is opened. */
  f->id=id;
}
static void wait_entered(fixture_t *f) {
  struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=3;
  pthread_mutex_lock(&f->lock);
  while(!f->entered) assert(!pthread_cond_timedwait(&f->changed,&f->lock,&end));
  pthread_mutex_unlock(&f->lock);
}
static void proceed(fixture_t *f) {
  pthread_mutex_lock(&f->lock);f->proceed=true;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);
}
static gt_task_info_t wait_state(fixture_t *f,gt_task_state_t state) {
  for(unsigned i=0;i<3000;i++) {
    gt_task_info_t info;assert(gt__tasks_info(f->pool,f->id,&info)==GT_TASK_OK);
    if(info.state==state)return info;
    struct timespec delay={0,1000000};nanosleep(&delay,NULL);
  }
  assert(!"task state timeout");return (gt_task_info_t){0};
}
static void *close_pool(void *context) {
  assert(gt__tasks_close(context)==GT_TASK_OK);return NULL;
}
static void task_allocation_case(void) {
  allocation_test_budget_t b={.limit=1024*1024};gt_allocator_t domain={&b,allocation_test_allocate,allocation_test_release};
  gt_allocator_t invalid={&b,NULL,allocation_test_release};
  assert(!gt__tasks_open_with_allocator(1,&invalid) && !b.calls);
  b.fail=1;assert(!gt__tasks_open_with_allocator(1,&domain) && !b.live && !b.count);
  b.fail=0;b.limit=1;assert(!gt__tasks_open_with_allocator(1,&domain) && !b.live);
  b.limit=1024*1024;gt_checkpoint_tasks_t *pool=gt__tasks_open_with_allocator(1,&domain);assert(pool);
  size_t live=b.live,calls=b.calls;assert(live && b.count==1);
  fixture_t f;init(&f,pool,false);submit(&f);wait_entered(&f);proceed(&f);wait_state(&f,GT_TASK_READY);
  void *value=NULL;uint64_t sequence=0;assert(gt__tasks_take(pool,f.id,7,&value,&sequence)==GT_TASK_OK);
  assert(gt__tasks_close(pool)==GT_TASK_BUSY && b.live==live && b.calls==calls && *(unsigned *)value==42);
  assert(gt__tasks_release(pool,f.id)==GT_TASK_OK);wait_state(&f,GT_TASK_FINISHED);
  assert(gt__tasks_forget(pool,f.id)==GT_TASK_OK && b.live==live && b.count==1 && b.calls==calls);
  assert(gt__tasks_close(pool)==GT_TASK_OK && !b.live && !b.count);destroy(&f);
}
int main(int argc,char **argv) {
  assert(argc==2);
  if(!strcmp(argv[1],"allocator")){task_allocation_case();return 0;}
  gt_checkpoint_tasks_t *pool=gt__tasks_open(2);assert(pool);
  fixture_t a,b,c;init(&a,pool,false);init(&b,pool,false);init(&c,pool,false);
  bool closed=false;
  if(!strcmp(argv[1],"invalid")) {
    gt_task_spec_t s=spec(&a);uint64_t id=99;s.generation=0;
    assert(gt__tasks_submit(pool,&s,&id)==GT_TASK_INVALID && !id);
    assert(!a.encodes && !a.input_frees && !a.result_frees);
    assert(!gt__tasks_open(0) && !gt__tasks_open(17));
  } else if(!strcmp(argv[1],"close")) {
    submit(&a);wait_entered(&a);submit(&b);
    pthread_t closer;assert(!pthread_create(&closer,NULL,close_pool,pool));
    wait_state(&a,GT_TASK_CANCELLING);
    gt_task_spec_t s=spec(&c);uint64_t id=99;
    assert(gt__tasks_submit(pool,&s,&id)==GT_TASK_STOPPED && !id);
    assert(!a.input_frees && !b.input_frees);
    proceed(&a);assert(!pthread_join(closer,NULL));closed=true;
    assert(a.input_frees==1 && a.result_frees==1 && !b.encodes && b.input_frees==1);
  } else if(!strcmp(argv[1],"success")) {
    submit(&a);wait_entered(&a);proceed(&a);
    gt_task_info_t info=wait_state(&a,GT_TASK_READY);
    assert(info.generation==7 && info.sequence==40 && info.error==GT_OK);
    void *value=(void *)1;uint64_t sequence=99;
    assert(gt__tasks_take(pool,a.id,8,&value,&sequence)==GT_TASK_INVALID);
    assert(!value && !sequence);
    assert(gt__tasks_take(pool,a.id,7,&value,&sequence)==GT_TASK_OK);
    assert(*(unsigned *)value==42 && sequence==40);
    assert(gt__tasks_cancel(pool,a.id)==GT_TASK_BUSY);
    assert(gt__tasks_close(pool)==GT_TASK_BUSY);
    assert(*(unsigned *)value==42);
    void *again=NULL;
    assert(gt__tasks_take(pool,a.id,7,&again,&sequence)==GT_TASK_BUSY && !again);
    assert(gt__tasks_forget(pool,a.id)==GT_TASK_BUSY);
    assert(gt__tasks_release(pool,a.id)==GT_TASK_OK);
    wait_state(&a,GT_TASK_FINISHED);
    assert(a.encodes==1 && a.input_frees==1 && a.result_frees==1);
    uint64_t old=a.id;assert(gt__tasks_forget(pool,old)==GT_TASK_OK);
    assert(gt__tasks_info(pool,old,&info)==GT_TASK_INVALID);
    submit(&b);assert(b.id!=old);wait_entered(&b);proceed(&b);wait_state(&b,GT_TASK_READY);
  } else if(!strcmp(argv[1],"cancel-running")) {
    submit(&a);wait_entered(&a);
    assert(gt__tasks_cancel(pool,a.id)==GT_TASK_OK);
    gt_task_info_t info;assert(gt__tasks_info(pool,a.id,&info)==GT_TASK_OK && info.state==GT_TASK_CANCELLING);
    assert(a.input_frees==0 && a.result_frees==0);
    assert(gt__tasks_forget(pool,a.id)==GT_TASK_BUSY);
    void *value=NULL;uint64_t seq;
    assert(gt__tasks_take(pool,a.id,7,&value,&seq)==GT_TASK_BUSY && !value);
    proceed(&a);assert(wait_state(&a,GT_TASK_CANCELLED).error==GT_ERR_CANCELLED);
    assert(a.input_frees==1 && a.result_frees==1);
  } else if(!strcmp(argv[1],"cancel-queued")) {
    submit(&a);wait_entered(&a);submit(&b);
    gt_task_spec_t s=spec(&c);uint64_t rejected=99;
    assert(gt__tasks_submit(pool,&s,&rejected)==GT_TASK_FULL && !rejected);
    assert(!c.input_frees && !c.encodes);
    assert(gt__tasks_cancel(pool,b.id)==GT_TASK_OK);
    proceed(&a);wait_state(&a,GT_TASK_READY);wait_state(&b,GT_TASK_CANCELLED);
    assert(!b.encodes && b.input_frees==1 && !b.result_frees);
    assert(gt__tasks_submit(pool,&s,&rejected)==GT_TASK_FULL);
    assert(gt__tasks_forget(pool,b.id)==GT_TASK_OK);
    assert(gt__tasks_submit(pool,&s,&c.id)==GT_TASK_OK);
    wait_entered(&c);proceed(&c);wait_state(&c,GT_TASK_READY);
  } else if(!strcmp(argv[1],"cancel-ready")) {
    submit(&a);wait_entered(&a);proceed(&a);wait_state(&a,GT_TASK_READY);
    assert(gt__tasks_cancel(pool,a.id)==GT_TASK_OK);
    void *value=NULL;uint64_t seq;
    assert(gt__tasks_take(pool,a.id,7,&value,&seq)==GT_TASK_BUSY && !value);
    wait_state(&a,GT_TASK_CANCELLED);
    assert(a.input_frees==1 && a.result_frees==1);
  } else if(!strcmp(argv[1],"failure") || !strcmp(argv[1],"empty")) {
    a.fail=!strcmp(argv[1],"failure");a.empty=!strcmp(argv[1],"empty");
    submit(&a);wait_entered(&a);proceed(&a);
    gt_task_info_t info=wait_state(&a,GT_TASK_FAILED);
    assert(info.error==(a.fail ? GT_ERR_IO : GT_ERR_INTERNAL));
    assert(a.input_frees==1 && a.result_frees==(a.fail ? 1u : 0u));
  } else assert(!"unknown case");
  if(!closed) assert(gt__tasks_close(pool)==GT_TASK_OK);
  if(!strcmp(argv[1],"success")) assert(b.input_frees==1 && b.result_frees==1);
  if(!strcmp(argv[1],"cancel-queued")) assert(a.result_frees==1 && c.result_frees==1);
  destroy(&a);destroy(&b);destroy(&c);
  return 0;
}
