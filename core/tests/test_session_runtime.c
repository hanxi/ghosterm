#define _POSIX_C_SOURCE 200809L
#include "session/runtime.h"
#include "ipc/server.h"
#include "ipc/socket-stream.h"
#include "ipc/user-dispatcher.h"
#include "ipc/data-router.h"
#include "base/secure-random.h"
#include <sys/socket.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef union {max_align_t align;size_t size;} allocation;
typedef struct {atomic_size_t calls,fail,hits,live,bytes;} budget;
static void *admit(void *ctx,size_t n) {
  budget *b=ctx;size_t call=atomic_fetch_add(&b->calls,1)+1;
  if(call==atomic_load(&b->fail)){atomic_fetch_add(&b->hits,1);return NULL;}
  allocation *p=malloc(sizeof(*p)+n);CHECK(p);p->size=n;atomic_fetch_add(&b->live,1);atomic_fetch_add(&b->bytes,n);return p+1;
}
static void refund(void *ctx,void *memory,size_t n) {
  budget *b=ctx;allocation *p=(allocation *)memory-1;CHECK(p->size==n);
  CHECK(atomic_fetch_sub(&b->live,1)>0 && atomic_fetch_sub(&b->bytes,n)>=n);free(p);
}
typedef struct fixture fixture;
typedef struct {fixture *f;uint64_t id,principal;pthread_t worker;char text[4096];size_t length;bool closed;unsigned resized,scheme;} model;
struct fixture {
  gt_session_runtime_t *runtime;gt_checkpoint_tasks_t *tasks;gt_runtime_config_t config;budget memory;
  pthread_mutex_t lock;pthread_cond_t changed;pthread_t control;
  unsigned opened,closed,invalidated,completed,data_calls;bool state_fail,backend_fail,apply_fail,clock_fail;
  bool hold_apply,entered_apply,release_apply;uint64_t now;model states[8];
  bool hold_close,entered_close,release_close;
  unsigned freezes,released_frozen;gt_status_t freeze_status;bool freeze_null;
  gt_status_t entropy;gt_token_scope_t last_scope;
};
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static struct timespec deadline(void){struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;return end;}
static gt_status_t open_state(void *ctx,uint64_t principal,uint64_t id,const gt_create_spec_t *spec,gt_worker_reply_sink_t sink,void **out) {
  fixture *f=ctx;CHECK(principal && id && spec && spec->nonce[0] && sink.write && !pthread_equal(f->control,pthread_self()));
  *out=NULL;if(f->state_fail)return GT_ERR_UNSUPPORTED;
  CHECK(!pthread_mutex_lock(&f->lock));CHECK(f->opened<8);model *m=&f->states[f->opened++];
  *m=(model){.f=f,.id=id,.principal=principal,.worker=pthread_self()};*out=m;
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));
  if(f->backend_fail)atomic_store(&f->memory.fail,atomic_load(&f->memory.calls)+1);
  return GT_OK;
}
static gt_status_t apply(void *ctx,const gt_state_event_t *event) {
  model *m=ctx;fixture *f=m->f;CHECK(pthread_equal(m->worker,pthread_self()) && !m->closed);
  if(f->apply_fail)return GT_ERR_PROTOCOL;
  CHECK(!pthread_mutex_lock(&f->lock));
  if(event->kind==GT_STATE_COLOR_SCHEME && f->hold_apply) {
    f->entered_apply=true;CHECK(!pthread_cond_broadcast(&f->changed));
    while(!f->release_apply)CHECK(!pthread_cond_wait(&f->changed,&f->lock));
  }
  if(event->kind==GT_STATE_OUTPUT){CHECK(event->length<sizeof(m->text)-m->length);memcpy(m->text+m->length,event->payload,event->length);m->length+=event->length;m->text[m->length]=0;}
  else if(event->kind==GT_STATE_RESIZE){CHECK(event->length==sizeof(gt_state_size_t));m->resized++;}
  else {CHECK(event->kind==GT_STATE_COLOR_SCHEME && event->length==sizeof(gt_state_color_scheme_t));m->scheme++;}
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));return GT_OK;
}
typedef struct {char text[4096];} frozen;
static gt_status_t freeze(void *ctx,void **out) {
  model *m=ctx;CHECK(pthread_equal(m->worker,pthread_self()) && !m->closed);*out=NULL;
  CHECK(!pthread_mutex_lock(&m->f->lock));gt_status_t status=m->f->freeze_status;bool null=m->f->freeze_null;CHECK(!pthread_mutex_unlock(&m->f->lock));
  if(status!=GT_OK || null)return status;
  frozen *copy=calloc(1,sizeof(*copy));if(!copy)return GT_ERR_INTERNAL;
  CHECK(!pthread_mutex_lock(&m->f->lock));memcpy(copy->text,m->text,sizeof(copy->text));m->f->freezes++;CHECK(!pthread_mutex_unlock(&m->f->lock));*out=copy;return GT_OK;
}
static void release_frozen(void *ctx,void *p){fixture *f=ctx;CHECK(p);free(p);CHECK(!pthread_mutex_lock(&f->lock));f->released_frozen++;CHECK(!pthread_mutex_unlock(&f->lock));}
static void close_state(void *ctx) {
  model *m=ctx;CHECK(pthread_equal(m->worker,pthread_self()) && !m->closed);CHECK(!pthread_mutex_lock(&m->f->lock));
  m->f->entered_close=true;CHECK(!pthread_cond_broadcast(&m->f->changed));
  while(m->f->hold_close && !m->f->release_close)CHECK(!pthread_cond_wait(&m->f->changed,&m->f->lock));
  m->closed=true;m->f->closed++;CHECK(!pthread_cond_broadcast(&m->f->changed));CHECK(!pthread_mutex_unlock(&m->f->lock));
}
static void invalidated(void *ctx,gt_status_t status,uint64_t sequence){(void)sequence;model *m=ctx;CHECK(pthread_equal(m->worker,pthread_self()) && status==GT_ERR_PROTOCOL);CHECK(!pthread_mutex_lock(&m->f->lock));m->f->invalidated++;CHECK(!pthread_mutex_unlock(&m->f->lock));}
static void init(fixture *f,size_t fail) {
  memset(f,0,sizeof(*f));atomic_init(&f->memory.calls,0);atomic_init(&f->memory.fail,fail);atomic_init(&f->memory.hits,0);atomic_init(&f->memory.live,0);atomic_init(&f->memory.bytes,0);
  f->control=pthread_self();CHECK(!pthread_mutex_init(&f->lock,NULL));CHECK(!pthread_cond_init(&f->changed,NULL));
  f->tasks=gt__tasks_open(4);CHECK(f->tasks);
  f->config=(gt_runtime_config_t){.instance={1},.sessions=2,.creation_records=4,.creation_retention_ms=100,
    .queue_bytes=8192,.queue_events=32,.tasks=f->tasks,.state={.context=f,.open=open_state,.ops={apply,freeze},.close=close_state,.invalidated=invalidated,.color_scheme_supported=true,.release_frozen=release_frozen}};
}
static gt_status_t open_runtime(fixture *f){gt_allocator_t a={&f->memory,admit,refund};return gt__runtime_open(&f->config,0,&a,&f->runtime);}
static void finish(fixture *f) {
  if(f->runtime){CHECK(gt__runtime_close(f->runtime)==GT_RUNTIME_OK);f->runtime=NULL;}
  CHECK(gt__tasks_close(f->tasks)==GT_TASK_OK && !atomic_load(&f->memory.live) && !atomic_load(&f->memory.bytes));
  CHECK(f->freezes==f->released_frozen);
  CHECK(!pthread_cond_destroy(&f->changed));CHECK(!pthread_mutex_destroy(&f->lock));
}
static gt_create_spec_t specification(const char *cwd,unsigned nonce,const char *executable) {
  gt_create_spec_t s={.nonce={(uint8_t)nonce},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,
    .executable=executable,.cwd=cwd,.term="xterm-256color",.argc=4,.envc=1,.env={"RT_TEST_ENV=literal ;$() 中文"},
    .argv={"-c","stty -echo; printf 'READY:%s:%s:%s:%s\\n' \"$$\" \"$PWD\" \"$RT_TEST_ENV\" \"$1\"; read a; printf FIRST; read a; printf SECOND; exit 7","zero","arg ;$() 中文"}};
  return s;
}
static gt_creation_result_t create(fixture *f,const char *cwd,unsigned nonce,uint64_t principal,const char *executable) {
  gt_create_spec_t s=specification(cwd,nonce,executable);uint8_t bytes[8192];size_t n;CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_OK);
  gt_creation_result_t result=gt__creation_submit(gt__runtime_creation(f->runtime),principal,bytes,n,f->now);memset(bytes,0xa5,n);return result;
}
static gt_runtime_session_t *acquire(fixture *f,uint64_t principal,uint64_t id) {
  gt_runtime_session_t *c=NULL;CHECK(gt__runtime_acquire(f->runtime,principal,id,&c)==GT_RUNTIME_OK && c);return c;
}
static model *find_model(fixture *f,uint64_t id) {for(unsigned i=0;i<f->opened;i++)if(f->states[i].id==id)return &f->states[i];return NULL;}
static void wait_text(fixture *f,uint64_t id,const char *text) {
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f->lock));model *m;
  while(!(m=find_model(f,id)) || !strstr(m->text,text))CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));
  CHECK(!pthread_mutex_unlock(&f->lock));
}
static gt_managed_info_t terminal(fixture *f,uint64_t id) {
  gt_managed_info_t info;for(unsigned i=0;i<5000;i++){CHECK(gt__manager_info(gt__runtime_manager(f->runtime),id,&info)==GT_MANAGER_OK);if(info.has_result)return info;pause_one();}CHECK(false);return info;
}
static void submit_command(fixture *f,uint64_t id,gt_worker_command_t kind,const void *p,size_t n,gt_worker_completion_t done,void *ctx) {
  gt_worker_submit_t status;CHECK(gt__manager_submit_internal(gt__runtime_manager(f->runtime),id,kind,p,n,done,ctx,&status)==GT_MANAGER_OK && status==GT_WORKER_ACCEPTED);
}
static void stop(fixture *f,uint64_t id){CHECK(gt__manager_terminate(gt__runtime_manager(f->runtime),id)==GT_MANAGER_OK);(void)terminal(f,id);CHECK(gt__runtime_reap(f->runtime,id)==GT_MANAGER_OK);}
static void lifecycle(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED && r.session);wait_text(&f,r.session,"arg ;$() 中文");
  CHECK(!pthread_mutex_lock(&f.lock));model *m=find_model(&f,r.session);CHECK(m && m->principal==73 && strstr(m->text,cwd) && strstr(m->text,"literal ;$() 中文"));char original[4096];memcpy(original,m->text,sizeof(original));CHECK(!pthread_mutex_unlock(&f.lock));
  gt_runtime_session_t *lease=(gt_runtime_session_t *)1;CHECK(gt__runtime_acquire(f.runtime,74,r.session,&lease)==GT_RUNTIME_NOT_FOUND && !lease);
  lease=acquire(&f,73,r.session);gt_session_attachment_t *attachment=gt__runtime_attachment(lease);uint64_t old;CHECK(gt__attachment_attach(attachment,&old)==GT_TASK_OK);
  CHECK(gt__attachment_detach(attachment,old)==GT_TASK_OK);gt__runtime_release(&lease);CHECK(!lease);
  f.now=100;CHECK(gt__creation_advance(gt__runtime_creation(f.runtime),f.now)==GT_OK);uint8_t nonce[16]={1};CHECK(gt__creation_query(gt__runtime_creation(f.runtime),73,nonce,f.now).status==GT_CREATION_UNKNOWN);
  lease=acquire(&f,73,r.session);CHECK(gt__runtime_attachment(lease)==attachment);uint64_t next;CHECK(gt__attachment_attach(attachment,&next)==GT_TASK_OK && next>old);
  CHECK(gt__attachment_detach(attachment,old)==GT_TASK_INVALID && gt__attachment_current(attachment,next));
  submit_command(&f,r.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,r.session,"FIRST");
  CHECK(!pthread_mutex_lock(&f.lock));CHECK(!strncmp(find_model(&f,r.session)->text,original,strlen(original)));CHECK(!pthread_mutex_unlock(&f.lock));
  CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_BUSY);submit_command(&f,r.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,r.session,"SECOND");
  gt_managed_info_t info=terminal(&f,r.session);CHECK(info.result.kind==GT_WORKER_END_EOF && !info.termination_requested);CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);
  CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_BUSY);CHECK(gt__attachment_detach(attachment,next)==GT_TASK_OK);gt__runtime_release(&lease);
  CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK && gt__runtime_acquire(f.runtime,73,r.session,&lease)==GT_RUNTIME_NOT_FOUND);
  /* New ID, same table slot; a stale session cannot find its replacement. */
  uint64_t old_id=r.session;r=create(&f,cwd,2,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED && r.session>old_id);wait_text(&f,r.session,"READY:");
  finish(&f);CHECK(f.opened==2 && f.closed==2);
}
typedef struct {
  fixture *f;gt_runtime_session_t *lease;uint64_t generation,sequence,task;
  frozen *input;bool callback,encoding,hold,go;unsigned input_freed,result_freed;
} job;
static gt_status_t encode(void *ctx,void **out) {
  job *j=ctx;fixture *f=j->f;CHECK(!pthread_mutex_lock(&f->lock));j->encoding=true;CHECK(!pthread_cond_broadcast(&f->changed));
  while(j->hold && !j->go)CHECK(!pthread_cond_wait(&f->changed,&f->lock));
  frozen *result=malloc(sizeof(*result));CHECK(result);*result=*j->input;*out=result;CHECK(!pthread_mutex_unlock(&f->lock));return GT_OK;
}
static void free_input(void *ctx){job *j=ctx;release_frozen(j->f,j->input);CHECK(!pthread_mutex_lock(&j->f->lock));j->input=NULL;j->input_freed++;CHECK(!pthread_cond_broadcast(&j->f->changed));CHECK(!pthread_mutex_unlock(&j->f->lock));}
static void free_result(void *ctx,void *p){job *j=ctx;free(p);CHECK(!pthread_mutex_lock(&j->f->lock));j->result_freed++;CHECK(!pthread_cond_broadcast(&j->f->changed));CHECK(!pthread_mutex_unlock(&j->f->lock));}
static void captured(void *ctx,gt_status_t status,gt_state_capture_t capture) {
  job *j=ctx;CHECK(status==GT_OK && capture.frozen);j->input=capture.frozen;j->sequence=capture.sequence;
  gt_session_attachment_t *a=gt__runtime_attachment(j->lease);CHECK(gt__attachment_begin(a,j->generation,capture.sequence)==GT_RESTORE_OK);
  gt_task_spec_t task={.context=j,.encode=encode,.release_input=free_input,.release_result=free_result,.sequence=capture.sequence};
  CHECK(gt__attachment_submit(a,j->generation,&task,&j->task)==GT_TASK_OK);
  CHECK(!pthread_mutex_lock(&j->f->lock));j->callback=true;CHECK(!pthread_cond_broadcast(&j->f->changed));CHECK(!pthread_mutex_unlock(&j->f->lock));
}
static void wait_job(job *j) {
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&j->f->lock));while(!j->callback || !j->encoding)CHECK(!pthread_cond_timedwait(&j->f->changed,&j->f->lock,&end));CHECK(!pthread_mutex_unlock(&j->f->lock));
}
static gt_attachment_retired_t reap_job(job *j) {
  gt_attachment_retired_t retired;gt_session_attachment_t *a=gt__runtime_attachment(j->lease);
  for(unsigned i=0;;i++){CHECK(i<5000);gt_task_rc_t rc=gt__attachment_reap(a,&retired);if(rc==GT_TASK_OK)break;CHECK(rc==GT_TASK_BUSY);pause_one();}
  CHECK(retired.context==j && retired.generation==j->generation && j->input_freed==1 && j->result_freed==1);
  gt__runtime_release(&j->lease);return retired;
}
static void command_done(void *ctx,gt_status_t status,gt_state_capture_t capture) {
  fixture *f=ctx;CHECK(status==GT_OK && !capture.frozen);CHECK(!pthread_mutex_lock(&f->lock));f->completed++;
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void stream(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);wait_text(&f,r.session,"READY:");
  gt_runtime_session_t *connection=acquire(&f,73,r.session);gt_session_attachment_t *a=gt__runtime_attachment(connection);uint64_t g;CHECK(gt__attachment_attach(a,&g)==GT_TASK_OK);
  job j={.f=&f,.lease=acquire(&f,73,r.session),.generation=g};submit_command(&f,r.session,GT_WORKER_CAPTURE,NULL,0,captured,&j);wait_job(&j);
  gt_attachment_checkpoint_t checkpoint;for(unsigned i=0;;i++){CHECK(i<5000);gt_task_rc_t rc=gt__attachment_take(a,g,&checkpoint);if(rc==GT_TASK_OK)break;CHECK(rc==GT_TASK_BUSY);pause_one();}
  CHECK(strstr(((frozen *)checkpoint.result)->text,"READY:") && checkpoint.sequence==j.sequence);
  CHECK(gt__attachment_release(a,&checkpoint)==GT_TASK_OK);
  submit_command(&f,r.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,r.session,"FIRST");
  gt_state_size_t size={.cols=90,.rows=30};submit_command(&f,r.session,GT_WORKER_RESIZE,&size,sizeof(size),NULL,NULL);
  gt_state_color_scheme_t scheme={1};submit_command(&f,r.session,GT_WORKER_COLOR_SCHEME,&scheme,sizeof(scheme),command_done,&f);
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f.lock));while(!f.completed)CHECK(!pthread_cond_timedwait(&f.changed,&f.lock,&end));CHECK(find_model(&f,r.session)->resized && find_model(&f,r.session)->scheme);CHECK(!pthread_mutex_unlock(&f.lock));
  CHECK(gt__attachment_ready_snapshot(a,g,j.sequence)==GT_RESTORE_OK);
  uint64_t previous=j.sequence;bool output=false,resize=false,color=false;
  for(unsigned i=0;;i++) {
    CHECK(i<5000);uint8_t data[4096];gt_restore_event_t event;gt_restore_rc_t rc=gt__attachment_next(a,g,&event,data,sizeof(data));
    if(rc==GT_RESTORE_WAIT){if(output && resize && color)break;pause_one();continue;}
    CHECK(rc==GT_RESTORE_OK && event.sequence==previous+1);previous=event.sequence;
    if(event.kind==GT_STATE_OUTPUT)output=true;else if(event.kind==GT_STATE_RESIZE)resize=true;else {CHECK(event.kind==GT_STATE_COLOR_SCHEME);color=true;}
  }
  uint64_t cut;CHECK(gt__attachment_live_begin(a,g,&cut)==GT_RESTORE_OK && cut==previous);
  CHECK(!gt__attachment_input_allowed(a,g,cut));CHECK(gt__attachment_ready_live(a,g,cut)==GT_RESTORE_OK && gt__attachment_input_allowed(a,g,cut));
  (void)reap_job(&j);CHECK(gt__attachment_detach(a,g)==GT_TASK_OK);gt__runtime_release(&connection);finish(&f);
}
static void pending(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);wait_text(&f,r.session,"READY:");
  gt_runtime_session_t *connection=acquire(&f,73,r.session);gt_session_attachment_t *a=gt__runtime_attachment(connection);uint64_t old;CHECK(gt__attachment_attach(a,&old)==GT_TASK_OK);
  job j={.f=&f,.lease=acquire(&f,73,r.session),.generation=old,.hold=true};submit_command(&f,r.session,GT_WORKER_CAPTURE,NULL,0,captured,&j);wait_job(&j);
  CHECK(gt__attachment_detach(a,old)==GT_TASK_OK);gt__runtime_release(&connection);stop(&f,r.session);
  CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_BUSY && gt__runtime_close(f.runtime)==GT_RUNTIME_BUSY);
  connection=acquire(&f,73,r.session);uint64_t next;CHECK(gt__runtime_attachment(connection)==a && gt__attachment_attach(a,&next)==GT_TASK_OK && next>old);
  CHECK(gt__attachment_detach(a,old)==GT_TASK_INVALID && gt__attachment_current(a,next));
  CHECK(!pthread_mutex_lock(&f.lock));j.go=true;CHECK(!pthread_cond_broadcast(&f.changed));CHECK(!pthread_mutex_unlock(&f.lock));
  gt_attachment_retired_t retired=reap_job(&j);CHECK(retired.state==GT_TASK_CANCELLED && gt__attachment_current(a,next));
  CHECK(gt__attachment_detach(a,next)==GT_TASK_OK);gt__runtime_release(&connection);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK);finish(&f);
}
static void limits(const char *cwd) {
  fixture f;init(&f,0);f.config.sessions=1;CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);wait_text(&f,r.session,"READY:");
  CHECK(create(&f,cwd,2,73,"/bin/sh").status==GT_CREATION_BUSY);gt_runtime_session_t *refs[256];for(unsigned i=0;i<256;i++)refs[i]=acquire(&f,73,r.session);
  gt_runtime_session_t *extra=(gt_runtime_session_t *)1;CHECK(gt__runtime_acquire(f.runtime,73,r.session,&extra)==GT_RUNTIME_BUSY && !extra);
  CHECK(gt__runtime_close(f.runtime)==GT_RUNTIME_BUSY);submit_command(&f,r.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,r.session,"FIRST");
  gt_session_attachment_t *a=gt__runtime_attachment(refs[0]);uint64_t g;CHECK(gt__attachment_attach(a,&g)==GT_TASK_OK);
  for(unsigned i=0;i<256;i++)gt__runtime_release(&refs[i]);
  /* Even a missing connection reference cannot let shutdown destroy an active controller. */
  CHECK(gt__runtime_close(f.runtime)==GT_RUNTIME_BUSY);extra=acquire(&f,73,r.session);a=gt__runtime_attachment(extra);CHECK(gt__attachment_detach(a,g)==GT_TASK_OK);gt__runtime_release(&extra);
  stop(&f,r.session);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK);gt_creation_result_t next=create(&f,cwd,3,73,"/bin/sh");CHECK(next.status==GT_CREATION_ACCEPTED && next.session>r.session);
  wait_text(&f,next.session,"READY:");CHECK(gt__runtime_acquire(f.runtime,73,r.session,&extra)==GT_RUNTIME_NOT_FOUND && !extra);finish(&f);
}
static void failures(const char *cwd) {
  for(unsigned kind=0;kind<3;kind++) {
    fixture f;init(&f,0);f.state_fail=kind==0;f.backend_fail=kind==1;f.apply_fail=kind==2;CHECK(open_runtime(&f)==GT_OK);
    gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);
    gt_managed_info_t info=terminal(&f,r.session);CHECK(info.result.kind==GT_WORKER_END_ERROR && !info.result.has_exit_code);
    CHECK(info.result.stage==(kind==0?GT_WORKER_STAGE_STATE_OPEN:kind==1?GT_WORKER_STAGE_BACKEND_OPEN:GT_WORKER_STAGE_APPLY));
    CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);gt_runtime_session_t *lease=acquire(&f,73,r.session);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_BUSY);
    gt__runtime_release(&lease);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK);finish(&f);CHECK(f.closed==(kind?1u:0u) && f.invalidated==(kind==2?1u:0u));
  }
}
static void allocation_test(const char *cwd) {
  size_t total=0;
  for(size_t fail=0;fail<=total;fail++) {
    fixture f;init(&f,fail);gt_status_t rc=open_runtime(&f);
    if(rc==GT_OK) {
      if(!fail)CHECK(atomic_load(&f.memory.calls)==3); /* runtime, manager, ledger */
      gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");
      if(r.status==GT_CREATION_ACCEPTED) {
        for(unsigned i=0;;i++) {
          CHECK(i<5000);gt_managed_info_t info;CHECK(gt__manager_info(gt__runtime_manager(f.runtime),r.session,&info)==GT_MANAGER_OK);
          CHECK(!pthread_mutex_lock(&f.lock));model *m=find_model(&f,r.session);bool ready=m && strstr(m->text,"READY:");CHECK(!pthread_mutex_unlock(&f.lock));
          if(ready || info.has_result)break;
          pause_one();
        }
      } else CHECK(fail && !r.session);
      if(!fail){total=atomic_load(&f.memory.calls);CHECK(total>10 && total<128);}
    } else CHECK(fail && !f.runtime);
    finish(&f);CHECK(atomic_load(&f.memory.hits)==(fail?1u:0u));
  }
  printf("PASS %zu allocation rejection sites with exact refunds\n",total);
}
typedef struct {fixture *f;uint64_t id;atomic_bool *stop;atomic_uint *passes;} reader;
static void *read_session(void *ctx) {
  reader *p=ctx;while(!atomic_load(p->stop)) {
    gt_runtime_session_t *lease;gt_runtime_rc_t rc=gt__runtime_acquire(p->f->runtime,73,p->id,&lease);
    CHECK(rc==GT_RUNTIME_OK || rc==GT_RUNTIME_NOT_FOUND);if(rc==GT_RUNTIME_OK){CHECK(gt__runtime_attachment(lease));atomic_fetch_add(p->passes,1);gt__runtime_release(&lease);}
  }return NULL;
}
static void concurrency(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t a=create(&f,cwd,1,73,"/bin/sh"),b=create(&f,cwd,2,91,"/bin/sh");
  CHECK(a.status==GT_CREATION_ACCEPTED && b.status==GT_CREATION_ACCEPTED && a.session!=b.session);wait_text(&f,a.session,"READY:");wait_text(&f,b.session,"READY:");
  atomic_bool done;atomic_uint passes;atomic_init(&done,false);atomic_init(&passes,0);reader r={&f,a.session,&done,&passes};pthread_t threads[4];
  for(unsigned i=0;i<4;i++)CHECK(!pthread_create(&threads[i],NULL,read_session,&r));
  for(unsigned i=0;atomic_load(&passes)<100;i++){CHECK(i<5000);pause_one();}
  stop(&f,a.session);gt_runtime_rc_t rc=gt__runtime_forget(f.runtime,a.session);CHECK(rc==GT_RUNTIME_OK || rc==GT_RUNTIME_BUSY);
  atomic_store(&done,true);for(unsigned i=0;i<4;i++)CHECK(!pthread_join(threads[i],NULL));
  if(rc==GT_RUNTIME_BUSY)CHECK(gt__runtime_forget(f.runtime,a.session)==GT_RUNTIME_OK);
  submit_command(&f,b.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,b.session,"FIRST");finish(&f);CHECK(f.opened==2 && f.closed==2);
}
static void invalid(const char *cwd) {
  (void)cwd;fixture f;init(&f,0);gt_session_runtime_t *out=(gt_session_runtime_t *)1;
  CHECK(gt__runtime_open(NULL,0,NULL,&out)==GT_ERR_INVALID_ARG && !out);
  f.config.sessions=0;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG && !f.runtime);f.config.sessions=65;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);
  f.config.sessions=2;f.config.state.ops.freeze=NULL;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.state.ops.freeze=freeze;
  f.config.state.release_frozen=NULL;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.state.release_frozen=release_frozen;
  f.config.queue_bytes=0;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.queue_bytes=16*1024*1024+1;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.queue_bytes=8192;
  f.config.queue_events=4097;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.queue_events=32;
  f.config.creation_records=0;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG && !atomic_load(&f.memory.live));f.config.creation_records=4;
  f.config.worker_threads.guard_size=4096;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.worker_threads.guard_size=0;
  f.config.backend_threads.guard_size=4096;CHECK(open_runtime(&f)==GT_ERR_INVALID_ARG);f.config.backend_threads.guard_size=0;
  gt_runtime_session_t *lease=(gt_runtime_session_t *)1;CHECK(gt__runtime_acquire(NULL,1,1,&lease)==GT_RUNTIME_INVALID && !lease);
  CHECK(gt__runtime_forget(NULL,1)==GT_RUNTIME_INVALID && gt__runtime_reap(NULL,1)==GT_MANAGER_INVALID && gt__runtime_close(NULL)==GT_RUNTIME_INVALID);
  CHECK(!gt__runtime_attachment(NULL) && !gt__runtime_manager(NULL) && !gt__runtime_creation(NULL));gt__runtime_release(NULL);gt__runtime_release(&lease);finish(&f);
}
typedef struct {gt_server_t *server;gt_connection_t *client;gt_socket_stream_t inbound,outbound;uint64_t id,generation;unsigned process_count;gt_session_process_t process;} peer;
static bool clock_now(void *ctx,uint64_t *now){fixture *f=ctx;if(f->clock_fail)return false;*now=f->now;return true;}
static void peer_send(peer *p,uint16_t type,uint64_t id,const void *bytes,size_t n) {
  gt_wire_header_t h={.type=type,.request_id=id,.payload_length=(uint32_t)n};CHECK(gt__connection_send(p->client,&h,bytes)==GT_SEND_ACCEPTED);
}
static gt_wire_frame_t peer_receive(peer *p,uint16_t type,uint64_t id) {
  for(unsigned i=0;i<5000;i++) {
    CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING && gt__connection_step(p->client)==GT_CONNECTION_OPEN);
    gt_wire_frame_t frame;if(gt__connection_frame(p->client,&frame)){CHECK(frame.header.type==type && frame.header.request_id==id);return frame;}
    pause_one();
  }CHECK(false);return (gt_wire_frame_t){0};
}
static void peer_connect(peer *p,gt_server_t *server,uint64_t principal,gt_protocol_role_t role) {
  *p=(peer){.server=server};int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));gt_stream_ops_t in,out;
  CHECK(gt__socket_stream_adopt(sockets[0],&p->inbound,&in)==GT_OK && gt__socket_stream_adopt(sockets[1],&p->outbound,&out)==GT_OK);
  CHECK(gt__server_admit(server,&in,principal,&p->id)==GT_SERVER_ADMITTED && gt__connection_open(&out,(gt_connection_limits_t){65560,65560,1000},NULL,&p->client)==GT_CONNECTION_CREATED);
  gt_protocol_hello_t hello={.role=role,.compatibility={2},.expected_instance={1}};uint8_t bytes[GT_HELLO_BYTES];CHECK(gt__hello_encode(&hello,bytes));
  uint64_t request=role==GT_ROLE_MANAGEMENT?7:0;
  peer_send(p,GT_MSG_HELLO,request,bytes,sizeof(bytes));gt_wire_frame_t frame=peer_receive(p,GT_MSG_HELLO_REPLY,request);gt_protocol_hello_reply_t reply;
  CHECK(gt__hello_reply_decode(frame.payload,frame.header.payload_length,&reply) && reply.status==GT_HELLO_OK);
  CHECK(gt__connection_release_frame(p->client));peer_send(p,GT_MSG_HELLO_ACK,0,reply.challenge,sizeof(reply.challenge));
  for(unsigned i=0;i<8;i++)CHECK(gt__connection_step(p->client)==GT_CONNECTION_OPEN && gt__server_step(server)==GT_SERVER_RUNNING);
}
static void peer_open(peer *p,gt_server_t *server){peer_connect(p,server,73,GT_ROLE_MANAGEMENT);}
static void peer_close(peer *p) {
  gt__connection_destroy(p->client);p->client=NULL;
  for(unsigned i=0;gt__server_connected(p->server,p->id);i++){CHECK(i<5000);CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING);pause_one();}
}
static void service(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);
  gt_server_config_t config={.manager=gt__runtime_manager(f.runtime),.creation=gt__runtime_creation(f.runtime),.connections=2,
    .limits={8192,8192,1000},.idle_timeout_ms=1000,
    .handshake={.identity={.instance={1},.compatibility={2}},.clock_context=&f,.clock=clock_now,.timeout_ms=1000}};
  gt_server_t *server;CHECK(gt__server_open(&config,NULL,&server)==GT_OK);peer p;peer_open(&p,server);
  gt_create_spec_t s=specification(cwd,1,"/bin/sh");uint8_t bytes[8192];size_t n;CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_OK);
  peer_send(&p,GT_MSG_CREATE,1,bytes,n);gt_wire_frame_t frame=peer_receive(&p,GT_MSG_CREATE_REPLY,1);uint8_t nonce[16];gt_creation_result_t result;
  CHECK(gt__control_creation_decode(frame.payload,frame.header.payload_length,nonce,&result) && !memcmp(nonce,s.nonce,16) && result.status==GT_CREATION_ACCEPTED && result.session);
  CHECK(gt__connection_release_frame(p.client));peer_close(&p);wait_text(&f,result.session,"READY:");
  gt_runtime_session_t *lease=acquire(&f,73,result.session);submit_command(&f,result.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,result.session,"FIRST");
  peer_open(&p,server);peer_send(&p,GT_MSG_QUERY_CREATE,1,s.nonce,16);frame=peer_receive(&p,GT_MSG_QUERY_CREATE_REPLY,1);gt_creation_result_t queried;
  CHECK(gt__control_creation_decode(frame.payload,frame.header.payload_length,nonce,&queried) && queried.status==GT_CREATION_ACCEPTED && queried.session==result.session);
  CHECK(gt__connection_release_frame(p.client));peer_close(&p);gt__server_close(server);
  submit_command(&f,result.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,result.session,"SECOND");CHECK(terminal(&f,result.session).result.kind==GT_WORKER_END_EOF);
  CHECK(gt__runtime_reap(f.runtime,result.session)==GT_MANAGER_OK);gt__runtime_release(&lease);CHECK(gt__runtime_forget(f.runtime,result.session)==GT_RUNTIME_OK);finish(&f);CHECK(f.opened==1 && f.closed==1);
}
typedef struct {fixture f;gt_attachment_owner_t *owner;gt_server_t *server;gt_server_config_t config;uint64_t session;void *owned[2];size_t charges[2];void *server_memory;size_t server_bytes;} attach_fixture;
static void *server_admit(void *ctx,size_t n){attach_fixture *a=ctx;void *p=admit(&a->f.memory,n);if(p && !a->server_memory){a->server_memory=p;a->server_bytes=n;}return p;}
static void server_refund(void *ctx,void *p,size_t n){attach_fixture *a=ctx;if(p==a->server_memory){a->server_memory=NULL;a->server_bytes=0;}refund(&a->f.memory,p,n);}
static bool pattern(const void *memory,size_t length,const uint8_t *bytes,size_t n) {
  for(size_t i=0;i+n<=length;i++)if(!memcmp((const uint8_t *)memory+i,bytes,n))return true;
  return false;
}
static void *owner_admit(void *ctx,size_t n) {
  attach_fixture *a=ctx;void *p=admit(&a->f.memory,n);if(!p)return NULL;
  for(unsigned i=0;i<2;i++)if(!a->owned[i]){a->owned[i]=p;a->charges[i]=n;return p;}
  CHECK(false);return NULL;
}
static void owner_refund(void *ctx,void *p,size_t n) {
  attach_fixture *a=ctx;unsigned i=0;while(i<2 && a->owned[i]!=p)i++;CHECK(i<2 && a->charges[i]==n);
  for(size_t j=0;j<n;j++){CHECK(!((uint8_t *)p)[j]);}
  a->owned[i]=NULL;a->charges[i]=0;refund(&a->f.memory,p,n);
}
static gt_status_t token_random(void *ctx,uint8_t *bytes,size_t n) {
  fixture *f=ctx;if(f->entropy!=GT_OK){memset(bytes,0xda,n);return f->entropy;}return gt__secure_random(bytes,n);
}
static gt_server_data_rc_t observe_data(void *ctx,gt_connection_t *connection,const gt_token_scope_t *scope) {
  fixture *f=ctx;(void)connection;f->data_calls++;f->last_scope=*scope;return GT_SERVER_DATA_IDLE;
}
static void attach_init(attach_fixture *a,const char *cwd,size_t capacity) {
  memset(a,0,sizeof(*a));init(&a->f,0);CHECK(open_runtime(&a->f)==GT_OK);gt_creation_result_t r=create(&a->f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);a->session=r.session;wait_text(&a->f,r.session,"READY:");
  gt_attachment_owner_config_t o={.runtime=a->f.runtime,.instance={1},.capacity=capacity,.token_ttl_ms=10,.random={&a->f,token_random}};
  gt_allocator_t domain={a,owner_admit,owner_refund};CHECK(gt__attachment_owner_open(&o,0,&domain,&a->owner)==GT_OK);
  a->config=(gt_server_config_t){.manager=gt__runtime_manager(a->f.runtime),.creation=gt__runtime_creation(a->f.runtime),.attachments=a->owner,.connections=4,
    .limits={8192,8192,1000},.idle_timeout_ms=1000,.data_context=&a->f,.data_step=observe_data,
    .handshake={.identity={.instance={1},.compatibility={2}},.clock_context=&a->f,.clock=clock_now,.timeout_ms=1000}};
  gt_allocator_t service_domain={a,server_admit,server_refund};CHECK(gt__server_open(&a->config,&service_domain,&a->server)==GT_OK);
}
static void attach_finish(attach_fixture *a) {
  gt__server_close(a->server);a->server=NULL;CHECK(gt__attachment_owner_close(a->owner)==GT_RUNTIME_OK);a->owner=NULL;finish(&a->f);
}
static void encode_id(uint8_t bytes[8],uint64_t id){for(unsigned i=0;i<8;i++)bytes[i]=(uint8_t)(id>>(56-8*i));}
static gt_attach_result_t request_attach(peer *p,uint64_t id,uint64_t request) {
  uint8_t bytes[8];encode_id(bytes,id);peer_send(p,GT_MSG_ATTACH,request,bytes,sizeof(bytes));gt_wire_frame_t frame=peer_receive(p,GT_MSG_ATTACH_REPLY,request);gt_attach_result_t r;
  CHECK(gt__control_attach_decode(frame.payload,frame.header.payload_length,&r) && r.session==id);CHECK(gt__connection_release_frame(p->client));return r;
}
static void bind_peer(peer *p,const gt_attach_result_t *r,bool success) {
  p->generation=r->generation;
  gt_protocol_bind_t binding={.session=r->session,.generation=r->generation,.token=r->token};memcpy(binding.instance,r->instance,16);
  uint8_t bytes[GT_BIND_BYTES];CHECK(gt__bind_encode(&binding,bytes));peer_send(p,GT_MSG_BIND,0,bytes,sizeof(bytes));gt_wire_frame_t frame=peer_receive(p,GT_MSG_BIND_REPLY,0);gt_protocol_bind_reply_t reply;
  CHECK(gt__bind_reply_decode(frame.payload,frame.header.payload_length,&reply) && reply.status==(success?GT_HELLO_OK:GT_HELLO_DENIED));
  CHECK(gt__connection_release_frame(p->client));peer_send(p,GT_MSG_BIND_ACK,0,reply.challenge,sizeof(reply.challenge));
  for(unsigned i=0;i<8;i++){(void)gt__connection_step(p->client);CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING);}
}
static void empty_capability(const gt_attach_result_t *r) {
  CHECK(r->status!=GT_ATTACH_OK && !r->generation && !r->ttl_ms);uint8_t zero[40]={0};CHECK(!memcmp(r->instance,zero,16) && !memcmp(r->token.bytes,zero,40));
}
static void attach_flow(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer management,data;peer_open(&management,a.server);gt_attach_result_t r=request_attach(&management,a.session,1);CHECK(r.status==GT_ATTACH_OK && r.ttl_ms==10);
  CHECK(a.server_memory && !pattern(a.server_memory,a.server_bytes,r.token.bytes+8,32));
  peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&r,true);CHECK(a.f.data_calls && a.f.last_scope.issuer==management.id && a.f.last_scope.principal==73 && a.f.last_scope.generation==r.generation);
  gt_session_attachment_t *controller=gt__attachment_owner_attachment(a.owner,&a.f.last_scope);CHECK(controller && !gt__attachment_input_allowed(controller,r.generation,0));
  for(unsigned field=0;field<4;field++) {
    gt_token_scope_t wrong=a.f.last_scope;
    if(!field)wrong.principal++;else if(field==1)wrong.issuer++;else if(field==2)wrong.session++;else wrong.generation++;
    CHECK(!gt__attachment_owner_current(a.owner,&wrong) && !gt__attachment_owner_attachment(a.owner,&wrong));gt__attachment_owner_drop(a.owner,&wrong);CHECK(gt__attachment_owner_current(a.owner,&a.f.last_scope));
  }
  peer_close(&management);CHECK(gt__attachment_owner_current(a.owner,&a.f.last_scope));submit_command(&a.f,a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&a.f,a.session,"FIRST");
  peer_open(&management,a.server);gt_attach_result_t busy=request_attach(&management,a.session,1);CHECK(busy.status==GT_ATTACH_BUSY);empty_capability(&busy);
  peer_send(&data,GT_MSG_DETACH,0,NULL,0);for(unsigned i=0;gt__server_connected(a.server,data.id);i++){CHECK(i<5000);(void)gt__connection_step(data.client);CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING);pause_one();}
  gt__connection_destroy(data.client);data.client=NULL;CHECK(!gt__attachment_owner_current(a.owner,&a.f.last_scope));
  gt_attach_result_t next=request_attach(&management,a.session,2);CHECK(next.status==GT_ATTACH_OK && next.generation>r.generation);peer_close(&management);attach_finish(&a);CHECK(a.f.opened==1 && a.f.closed==1);
}
static void attach_expiry(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p,data;peer_open(&p,a.server);gt_attach_result_t old=request_attach(&p,a.session,1);CHECK(old.status==GT_ATTACH_OK);
  gt_attach_result_t next;CHECK(gt__attachment_owner_issue(a.owner,73,p.id,a.session,9,&next)==GT_ATTACH_BUSY);
  a.f.now=10;CHECK(gt__attachment_owner_issue(a.owner,73,p.id,a.session,10,&next)==GT_ATTACH_OK && next.generation>old.generation);CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING);
  peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&old,false);peer_close(&data);
  peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&next,true);unsigned calls=a.f.data_calls;
  /* Expiry/issuer loss no longer owns a capability already consumed by DATA. */
  a.f.now=20;CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING && a.f.data_calls>calls);peer_close(&data);
  gt_attach_result_t pending=request_attach(&p,a.session,2);CHECK(pending.status==GT_ATTACH_OK);gt_token_scope_t scope={73,p.id,a.session,pending.generation};
  a.f.now=30;CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING && !gt__attachment_owner_current(a.owner,&scope));CHECK(request_attach(&p,a.session,3).status==GT_ATTACH_OK);peer_close(&p);attach_finish(&a);
}
static void attach_issuer(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p,wrong,data;peer_open(&p,a.server);gt_attach_result_t old=request_attach(&p,a.session,1);CHECK(old.status==GT_ATTACH_OK);peer_close(&p);
  peer_open(&p,a.server);gt_attach_result_t next=request_attach(&p,a.session,1);CHECK(next.status==GT_ATTACH_OK && next.generation>old.generation);
  peer_connect(&wrong,a.server,91,GT_ROLE_MANAGEMENT);gt_attach_result_t denied=request_attach(&wrong,a.session,1);CHECK(denied.status==GT_ATTACH_NOT_FOUND);empty_capability(&denied);peer_close(&wrong);
  peer_connect(&data,a.server,91,GT_ROLE_DATA);bind_peer(&data,&next,false);peer_close(&data);
  peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&next,true);peer other;peer_connect(&other,a.server,73,GT_ROLE_DATA);bind_peer(&other,&next,false);peer_close(&other);
  CHECK(a.f.last_scope.issuer==p.id && gt__attachment_owner_current(a.owner,&a.f.last_scope));peer_close(&p);peer_close(&data);attach_finish(&a);
}
static void attach_stale(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p,data;peer_open(&p,a.server);gt_attach_result_t old=request_attach(&p,a.session,1);peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&old,true);gt_token_scope_t stale=a.f.last_scope;
  gt_session_attachment_t *controller=gt__attachment_owner_attachment(a.owner,&stale);CHECK(controller && gt__attachment_detach(controller,old.generation)==GT_TASK_OK);
  gt_attach_result_t next;CHECK(gt__attachment_owner_issue(a.owner,73,p.id,a.session,a.f.now,&next)==GT_ATTACH_OK && next.generation>old.generation);
  gt__attachment_owner_drop(a.owner,&stale);gt__attachment_owner_drop(a.owner,&stale);
  CHECK(gt__attachment_current(controller,next.generation));peer_close(&data);CHECK(gt__attachment_current(controller,next.generation));
  peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&next,true);peer_close(&p);peer_close(&data);attach_finish(&a);
}
static void attach_failures(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p;peer_open(&p,a.server);
  a.f.entropy=GT_ERR_IO;gt_attach_result_t r=request_attach(&p,a.session,1);CHECK(r.status==GT_ATTACH_ENTROPY);empty_capability(&r);
  a.f.entropy=GT_ERR_UNSUPPORTED;r=request_attach(&p,a.session,2);CHECK(r.status==GT_ATTACH_UNSUPPORTED);empty_capability(&r);
  a.f.entropy=GT_OK;r=request_attach(&p,a.session,3);CHECK(r.status==GT_ATTACH_OK && r.generation>=3);
  a.f.now=5;CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING);a.f.now=4;CHECK(gt__server_step(a.server)==GT_SERVER_CLOCK_FAILED);
  gt__connection_destroy(p.client);p.client=NULL;submit_command(&a.f,a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&a.f,a.session,"FIRST");attach_finish(&a);
}
static void attach_capacity(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,1);gt_creation_result_t second=create(&a.f,cwd,2,73,"/bin/sh");CHECK(second.status==GT_CREATION_ACCEPTED);wait_text(&a.f,second.session,"READY:");
  peer p;peer_open(&p,a.server);CHECK(request_attach(&p,a.session,1).status==GT_ATTACH_OK);gt_attach_result_t r=request_attach(&p,second.session,2);CHECK(r.status==GT_ATTACH_BUSY);empty_capability(&r);
  peer_close(&p);peer_open(&p,a.server);r=request_attach(&p,second.session,1);CHECK(r.status==GT_ATTACH_OK);peer_close(&p);attach_finish(&a);CHECK(a.f.opened==2 && a.f.closed==2);
}
static void attach_malformed(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p;uint8_t id[9]={0};encode_id(id,a.session);
  for(unsigned kind=0;kind<4;kind++) {
    peer_open(&p,a.server);if(kind==2)memset(id,0,8);else encode_id(id,a.session);
    peer_send(&p,kind==3?GT_MSG_ATTACH_REPLY:GT_MSG_ATTACH,1,id,kind==0?7:kind==1?9:8);
    for(unsigned i=0;gt__server_connected(a.server,p.id);i++){CHECK(i<5000);(void)gt__connection_step(p.client);CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING);pause_one();}
    gt__connection_destroy(p.client);p.client=NULL;
  }
  peer_open(&p,a.server);gt_attach_result_t r=request_attach(&p,a.session,1);CHECK(r.status==GT_ATTACH_OK);peer_close(&p);attach_finish(&a);
}
static void attach_codec(const char *cwd) {
  (void)cwd;uint8_t bytes[89]={0};bytes[8]=1;bytes[31]=1;bytes[39]=2;bytes[43]=10;bytes[55]=1;bytes[56]=9;gt_attach_result_t r;
  CHECK(gt__control_attach_decode(bytes,88,&r) && r.status==GT_ATTACH_OK && r.session==1 && r.generation==2 && r.ttl_ms==10 && r.token.bytes[7]==1 && r.token.bytes[8]==9);
  for(unsigned i=0;i<88;i++)CHECK(!gt__control_attach_decode(bytes,i,&r) && !r.session && !r.generation);
  CHECK(!gt__control_attach_decode(bytes,89,&r));bytes[44]=1;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[44]=0;
  bytes[7]=GT_ERR_IO;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[7]=0;
  bytes[42]=0xea;bytes[43]=0x61;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[42]=0;bytes[43]=10;
  bytes[39]=0;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[39]=2;bytes[43]=0;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[43]=10;
  bytes[8]=0;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[8]=1;bytes[55]=0;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[55]=1;bytes[56]=0;CHECK(!gt__control_attach_decode(bytes,88,&r));
  memset(bytes,0,sizeof(bytes));bytes[3]=GT_ATTACH_BUSY;bytes[31]=1;CHECK(gt__control_attach_decode(bytes,88,&r) && r.status==GT_ATTACH_BUSY);
  bytes[3]=99;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[3]=GT_ATTACH_BUSY;bytes[7]=99;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[7]=0;
  bytes[56]=9;CHECK(!gt__control_attach_decode(bytes,88,&r));bytes[56]=0;bytes[31]=0;CHECK(!gt__control_attach_decode(bytes,88,&r));
}
static void attach_config(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);gt_server_t *other=(gt_server_t *)1;
  CHECK(gt__attachment_owner_close(a.owner)==GT_RUNTIME_BUSY && gt__server_open(&a.config,NULL,&other)==GT_ERR_INVALID_ARG && !other);
  gt__attachment_owner_unclaim(a.owner,&other);CHECK(gt__attachment_owner_close(a.owner)==GT_RUNTIME_BUSY);
  gt__server_close(a.server);a.server=NULL;
  gt_session_manager_t *unrelated=gt__manager_open(1,NULL);CHECK(unrelated && !gt__attachment_owner_matches(a.owner,unrelated,a.config.creation,a.config.handshake.identity.instance));CHECK(gt__manager_close(unrelated)==GT_MANAGER_OK);
  gt_server_config_t bad=a.config;bad.handshake.tokens=gt__attachment_owner_tokens(a.owner);CHECK(gt__server_open(&bad,NULL,&other)==GT_ERR_INVALID_ARG);
  bad=a.config;bad.handshake.identity.instance[0]=3;CHECK(gt__server_open(&bad,NULL,&other)==GT_ERR_INVALID_ARG);
  bad=a.config;bad.creation=NULL;CHECK(gt__server_open(&bad,NULL,&other)==GT_ERR_INVALID_ARG);
  uint8_t id[8],scratch[GT_CONTROL_REPLY_BYTES];encode_id(id,a.session);gt_wire_header_t reply;gt_wire_frame_t frame={.header={.type=GT_MSG_ATTACH,.request_id=1,.payload_length=8},.payload=id};
  gt_control_context_t control={.manager=a.config.manager,.creation=a.config.creation,.attachments=a.owner,.principal=73};
  CHECK(gt__control_process(&control,&frame,scratch,sizeof(scratch),&reply)==GT_CONTROL_MALFORMED);control.issuer=99;
  CHECK(gt__control_process(&control,&frame,scratch,4,&reply)==GT_CONTROL_NO_SPACE);
  a.f.clock_fail=true;CHECK(gt__server_open(&a.config,NULL,&other)==GT_ERR_IO && !other);a.f.clock_fail=false;
  /* Failed constructor must release its exclusive claim. No dispatcher means
   * ATTACH is explicit unsupported, with no invisible reservation. */
  a.config.data_step=NULL;CHECK(gt__server_open(&a.config,NULL,&a.server)==GT_OK);peer p;peer_open(&p,a.server);gt_attach_result_t r=request_attach(&p,a.session,1);CHECK(r.status==GT_ATTACH_UNSUPPORTED);empty_capability(&r);peer_close(&p);
  gt__server_close(a.server);a.server=NULL;CHECK(gt__attachment_owner_close(a.owner)==GT_RUNTIME_OK);a.owner=NULL;
  gt_attachment_owner_config_t config={.runtime=a.f.runtime,.instance={1},.capacity=4,.token_ttl_ms=10};gt_attachment_owner_t *out=(gt_attachment_owner_t *)1;
  for(unsigned fail=1;fail<=2;fail++) {
    size_t before=atomic_load(&a.f.memory.live);atomic_store(&a.f.memory.fail,atomic_load(&a.f.memory.calls)+fail);gt_allocator_t domain={&a.f.memory,admit,refund};
    CHECK(gt__attachment_owner_open(&config,0,&domain,&out)==GT_ERR_INTERNAL && !out && atomic_load(&a.f.memory.live)==before);atomic_store(&a.f.memory.fail,0);
  }
  config.capacity=0;CHECK(gt__attachment_owner_open(&config,0,NULL,&out)==GT_ERR_INVALID_ARG);config.capacity=257;CHECK(gt__attachment_owner_open(&config,0,NULL,&out)==GT_ERR_INVALID_ARG);config.capacity=4;
  config.token_ttl_ms=0;CHECK(gt__attachment_owner_open(&config,0,NULL,&out)==GT_ERR_INVALID_ARG && !out);config.token_ttl_ms=10;config.instance[0]=2;CHECK(gt__attachment_owner_open(&config,0,NULL,&out)==GT_ERR_INVALID_ARG);
  finish(&a.f);
}
static void attach_close(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p,data;peer_open(&p,a.server);gt_attach_result_t r=request_attach(&p,a.session,1);peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&r,true);
  gt__server_close(a.server);a.server=NULL;gt__connection_destroy(p.client);gt__connection_destroy(data.client);
  CHECK(gt__attachment_owner_close(a.owner)==GT_RUNTIME_OK);a.owner=NULL;submit_command(&a.f,a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&a.f,a.session,"FIRST");
  /* Also clean a directly consumed scope after the token store no longer owns it. */
  gt_attachment_owner_config_t config={.runtime=a.f.runtime,.instance={1},.capacity=1,.token_ttl_ms=10};CHECK(gt__attachment_owner_open(&config,0,NULL,&a.owner)==GT_OK);
  CHECK(gt__attachment_owner_issue(a.owner,73,99,a.session,0,&r)==GT_ATTACH_OK);CHECK(!gt__attachment_owner_claim(a.owner,&a));gt_token_scope_t scope;
  CHECK(gt__tokens_consume(gt__attachment_owner_tokens(a.owner),&r.token,r.instance,73,r.session,r.generation,0,&scope)==GT_TOKEN_OK && gt__attachment_owner_current(a.owner,&scope));
  CHECK(gt__attachment_owner_close(a.owner)==GT_RUNTIME_OK);a.owner=NULL;finish(&a.f);CHECK(a.f.opened==1 && a.f.closed==1);
}
static void attach_lost(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p;peer_open(&p,a.server);uint8_t bytes[8];encode_id(bytes,a.session);peer_send(&p,GT_MSG_ATTACH,1,bytes,8);
  for(unsigned i=0;i<16;i++){CHECK(gt__connection_step(p.client)==GT_CONNECTION_OPEN && gt__server_step(a.server)==GT_SERVER_RUNNING);}
  gt_runtime_session_t *lease=acquire(&a.f,73,a.session);uint64_t generation;
  CHECK(gt__attachment_attach(gt__runtime_attachment(lease),&generation)==GT_TASK_BUSY);gt__runtime_release(&lease);
  /* Never decode the old response; an ambiguous reservation dies with issuer. */
  peer_close(&p);peer_open(&p,a.server);gt_attach_result_t r=request_attach(&p,a.session,1);CHECK(r.status==GT_ATTACH_OK && r.generation>1);peer_close(&p);attach_finish(&a);
}
static void attach_lease(const char *cwd) {
  attach_fixture a;attach_init(&a,cwd,4);peer p,data;peer_open(&p,a.server);gt_attach_result_t old=request_attach(&p,a.session,1);peer_connect(&data,a.server,73,GT_ROLE_DATA);bind_peer(&data,&old,true);gt_token_scope_t scope=a.f.last_scope;
  a.f.now=1000;CHECK(gt__server_step(a.server)==GT_SERVER_RUNNING && !gt__server_connected(a.server,data.id) && !gt__attachment_owner_current(a.owner,&scope));
  gt__connection_destroy(data.client);gt__connection_destroy(p.client);submit_command(&a.f,a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&a.f,a.session,"FIRST");
  peer_open(&p,a.server);gt_attach_result_t next=request_attach(&p,a.session,1);CHECK(next.status==GT_ATTACH_OK && next.generation>old.generation);peer_close(&p);attach_finish(&a);
}
/* This is a real controller/task/worker fence with a C model snapshot consumer;
 * it is not evidence that a Ghostty GUI has imported/applied the snapshot. */
static uint64_t user_live(fixture *f,uint64_t id,gt_runtime_session_t *connection,uint64_t generation) {
  gt_session_attachment_t *a=gt__runtime_attachment(connection);job j={.f=f,.lease=acquire(f,73,id),.generation=generation};
  submit_command(f,id,GT_WORKER_CAPTURE,NULL,0,captured,&j);wait_job(&j);gt_attachment_checkpoint_t checkpoint;
  for(unsigned i=0;;i++){CHECK(i<5000);gt_task_rc_t rc=gt__attachment_take(a,generation,&checkpoint);if(rc==GT_TASK_OK)break;CHECK(rc==GT_TASK_BUSY);pause_one();}
  CHECK(checkpoint.result && gt__attachment_release(a,&checkpoint)==GT_TASK_OK && gt__attachment_ready_snapshot(a,generation,j.sequence)==GT_RESTORE_OK);
  uint64_t cut;
  for(unsigned i=0;;i++) {
    CHECK(i<5000);gt_restore_rc_t rc=gt__attachment_live_begin(a,generation,&cut);if(rc==GT_RESTORE_OK)break;CHECK(rc==GT_RESTORE_WAIT);
    uint8_t data[4096];gt_restore_event_t event;rc=gt__attachment_next(a,generation,&event,data,sizeof(data));CHECK(rc==GT_RESTORE_OK || rc==GT_RESTORE_WAIT);if(rc==GT_RESTORE_WAIT)pause_one();
  }
  CHECK(gt__attachment_ready_live(a,generation,cut)==GT_RESTORE_OK);(void)reap_job(&j);return cut;
}
typedef struct {fixture *f;bool done;gt_status_t status;gt_user_delivery_t delivery;gt_worker_stamp_t expected;} user_result;
static void user_completed(void *ctx,const gt_worker_user_result_t *result) {
  user_result *r=ctx;CHECK(result && !memcmp(&result->stamp,&r->expected,sizeof(result->stamp)));CHECK(!pthread_mutex_lock(&r->f->lock));CHECK(!r->done);r->done=true;r->status=result->status;r->delivery=result->delivery;
  CHECK(!pthread_cond_broadcast(&r->f->changed));CHECK(!pthread_mutex_unlock(&r->f->lock));
}
static void user_submit(gt_runtime_session_t *lease,gt_worker_command_t kind,gt_worker_stamp_t stamp,const void *p,size_t n,user_result *result) {
  result->expected=stamp;
  gt_worker_submit_t admission;CHECK(gt__runtime_submit_user(lease,kind,&stamp,p,n,user_completed,result,&admission)==GT_MANAGER_OK && admission==GT_WORKER_ACCEPTED);
}
static void user_wait(user_result *r,gt_status_t status){struct timespec end=deadline();CHECK(!pthread_mutex_lock(&r->f->lock));while(!r->done)CHECK(!pthread_cond_timedwait(&r->f->changed,&r->f->lock,&end));CHECK(r->status==status && r->delivery==(status==GT_OK?GT_USER_OPERATION_OK:GT_USER_NOT_EXECUTED));CHECK(!pthread_mutex_unlock(&r->f->lock));}
static void user_once(fixture *f,gt_runtime_session_t *lease,uint64_t generation,uint64_t command,uint64_t sequence,gt_worker_command_t kind,const void *p,size_t n,gt_status_t status) {
  user_result result={.f=f};user_submit(lease,kind,(gt_worker_stamp_t){generation,command,sequence},p,n,&result);user_wait(&result,status);
}
static gt_creation_result_t create_user(fixture *f,const char *cwd) {
  gt_create_spec_t spec=specification(cwd,1,"/bin/sh");spec.argv[1]="stty -echo; printf READY; read a; printf FIRST; read a; printf SECOND; exit 7";
  uint8_t bytes[8192];size_t n;CHECK(gt__create_spec_encode(&spec,bytes,sizeof(bytes),&n)==GT_OK);
  gt_creation_result_t r=gt__creation_submit(gt__runtime_creation(f->runtime),73,bytes,n,0);CHECK(r.status==GT_CREATION_ACCEPTED);wait_text(f,r.session,"READY");return r;
}
static void user_policy(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create_user(&f,cwd);
  gt_runtime_session_t *lease=acquire(&f,73,r.session);gt_session_attachment_t *a=gt__runtime_attachment(lease);uint64_t g;CHECK(gt__attachment_attach(a,&g)==GT_TASK_OK);
  gt_worker_submit_t bad=GT_WORKER_ACCEPTED;gt_worker_stamp_t invalid={g,0,0};user_result unused={.f=&f};
  CHECK(gt__runtime_submit_user(NULL,GT_WORKER_INPUT,&invalid,"X",1,user_completed,&unused,&bad)==GT_MANAGER_INVALID && bad==GT_WORKER_INVALID);
  CHECK(gt__runtime_submit_user(lease,GT_WORKER_INPUT,NULL,"X",1,user_completed,&unused,&bad)==GT_MANAGER_INVALID && bad==GT_WORKER_INVALID);
  CHECK(gt__runtime_submit_user(lease,GT_WORKER_INPUT,&invalid,"X",1,user_completed,&unused,&bad)==GT_MANAGER_OK && bad==GT_WORKER_INVALID);
  user_once(&f,lease,g,1,0,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_AUTH);user_once(&f,lease,g,1,0,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_PROTOCOL);
  uint64_t cut=user_live(&f,r.session,lease,g);CHECK(cut>0);user_once(&f,lease,g,1,cut,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_PROTOCOL);
  user_once(&f,lease,g,2,cut-1,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_AUTH);user_once(&f,lease,g,3,cut+1,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_AUTH);
  CHECK(!pthread_mutex_lock(&f.lock));CHECK(!strstr(find_model(&f,r.session)->text,"FIRST"));CHECK(!pthread_mutex_unlock(&f.lock));
  user_once(&f,lease,g,4,cut,GT_WORKER_INPUT,"go\n",3,GT_OK);wait_text(&f,r.session,"FIRST");gt_state_size_t size={90,30,0,0};
  user_once(&f,lease,g,5,cut,GT_WORKER_RESIZE,&size,sizeof(size),GT_OK);gt_state_color_scheme_t color={1};user_once(&f,lease,g,UINT64_MAX,cut,GT_WORKER_COLOR_SCHEME,&color,sizeof(color),GT_OK);
  user_once(&f,lease,g,6,cut,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_PROTOCOL);
  CHECK(gt__attachment_detach(a,g)==GT_TASK_OK);uint64_t next;CHECK(gt__attachment_attach(a,&next)==GT_TASK_OK && next>g);uint64_t newer=user_live(&f,r.session,lease,next);
  user_once(&f,lease,next,1,newer,GT_WORKER_INPUT,"go\n",3,GT_OK);wait_text(&f,r.session,"SECOND");CHECK(terminal(&f,r.session).result.kind==GT_WORKER_END_EOF);
  CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);gt_worker_stamp_t s={next,2,newer};gt_worker_submit_t admission;
  CHECK(gt__runtime_submit_user(lease,GT_WORKER_INPUT,&s,"go\n",3,user_completed,NULL,&admission)==GT_MANAGER_OK && admission==GT_WORKER_STOPPED);
  CHECK(gt__attachment_detach(a,next)==GT_TASK_OK);gt__runtime_release(&lease);finish(&f);
}
typedef struct {fixture *f;bool entered,release;} worker_barrier;
static void block_worker(void *ctx,gt_status_t status,gt_state_capture_t capture) {
  worker_barrier *b=ctx;CHECK(status==GT_OK && capture.frozen);release_frozen(b->f,capture.frozen);CHECK(!pthread_mutex_lock(&b->f->lock));b->entered=true;CHECK(!pthread_cond_broadcast(&b->f->changed));
  struct timespec end=deadline();while(!b->release)CHECK(!pthread_cond_timedwait(&b->f->changed,&b->f->lock,&end));CHECK(!pthread_mutex_unlock(&b->f->lock));
}
static void user_stale_queue(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create_user(&f,cwd);
  gt_runtime_session_t *lease=acquire(&f,73,r.session);gt_session_attachment_t *a=gt__runtime_attachment(lease);uint64_t old;CHECK(gt__attachment_attach(a,&old)==GT_TASK_OK);uint64_t cut=user_live(&f,r.session,lease,old);
  worker_barrier barrier={.f=&f};submit_command(&f,r.session,GT_WORKER_CAPTURE,NULL,0,block_worker,&barrier);
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f.lock));while(!barrier.entered)CHECK(!pthread_cond_timedwait(&f.changed,&f.lock,&end));CHECK(!pthread_mutex_unlock(&f.lock));
  user_result results[4]={{.f=&f},{.f=&f},{.f=&f},{.f=&f}};gt_state_size_t size={99,79,0,0};gt_state_color_scheme_t color={1};
  user_submit(lease,GT_WORKER_INPUT,(gt_worker_stamp_t){old,UINT64_MAX-2,cut},"OLD\n",4,&results[0]);
  user_submit(lease,GT_WORKER_RESIZE,(gt_worker_stamp_t){old,UINT64_MAX-1,cut},&size,sizeof(size),&results[1]);
  user_submit(lease,GT_WORKER_COLOR_SCHEME,(gt_worker_stamp_t){old,UINT64_MAX,cut},&color,sizeof(color),&results[2]);
  CHECK(gt__attachment_detach(a,old)==GT_TASK_OK);uint64_t next;CHECK(gt__attachment_attach(a,&next)==GT_TASK_OK && next>old);
  user_submit(lease,GT_WORKER_INPUT,(gt_worker_stamp_t){next,1,cut},"EARLY\n",6,&results[3]);
  CHECK(!pthread_mutex_lock(&f.lock));barrier.release=true;CHECK(!pthread_cond_broadcast(&f.changed));CHECK(!pthread_mutex_unlock(&f.lock));
  for(unsigned i=0;i<4;i++)user_wait(&results[i],GT_ERR_AUTH);
  CHECK(!pthread_mutex_lock(&f.lock));model *m=find_model(&f,r.session);CHECK(m && !m->resized && !m->scheme && !strstr(m->text,"FIRST"));CHECK(!pthread_mutex_unlock(&f.lock));
  gt_managed_info_t info;CHECK(gt__manager_info(gt__runtime_manager(f.runtime),r.session,&info)==GT_MANAGER_OK && !info.has_result);
  uint64_t newer=user_live(&f,r.session,lease,next);user_once(&f,lease,next,1,newer,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_PROTOCOL);
  user_once(&f,lease,next,2,newer,GT_WORKER_INPUT,"go\n",3,GT_OK);wait_text(&f,r.session,"FIRST");CHECK(gt__attachment_detach(a,next)==GT_TASK_OK);gt__runtime_release(&lease);finish(&f);
}
static void user_readonly(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create_user(&f,cwd);
  gt_runtime_session_t *lease=acquire(&f,73,r.session);gt_session_attachment_t *a=gt__runtime_attachment(lease);uint64_t g;CHECK(gt__attachment_attach(a,&g)==GT_TASK_OK);uint64_t cut=user_live(&f,r.session,lease,g);
  /* End is latched before its final tail is available. Stream remains LIVE,
   * so the session-wide readonly bit must independently reject new commands. */
  CHECK(gt__attachment_end(a,g,cut+1)==GT_RESTORE_WAIT);user_once(&f,lease,g,1,cut,GT_WORKER_INPUT,"WRONG\n",6,GT_ERR_AUTH);
  CHECK(!pthread_mutex_lock(&f.lock));CHECK(!strstr(find_model(&f,r.session)->text,"FIRST"));CHECK(!pthread_mutex_unlock(&f.lock));
  CHECK(gt__attachment_detach(a,g)==GT_TASK_OK);gt__runtime_release(&lease);finish(&f);
}
static void user_dimensions(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create_user(&f,cwd);gt_runtime_session_t *lease=acquire(&f,73,r.session);
  gt_session_attachment_t *a=gt__runtime_attachment(lease);uint64_t g;CHECK(gt__attachment_attach(a,&g)==GT_TASK_OK);uint64_t cut=user_live(&f,r.session,lease,g);
  gt_worker_stamp_t stamp={g,1,cut};gt_worker_submit_t status;user_result unused={.f=&f};size_t before=atomic_load(&f.memory.calls);
  gt_state_size_t size={99,41,65536,0};
  CHECK(gt__runtime_submit_user(lease,GT_WORKER_RESIZE,&stamp,&size,sizeof(size),user_completed,&unused,&status)==GT_MANAGER_OK && status==GT_WORKER_UNSUPPORTED);
  size.px_width=0;size.px_height=UINT32_MAX;
  CHECK(gt__runtime_submit_user(lease,GT_WORKER_RESIZE,&stamp,&size,sizeof(size),user_completed,&unused,&status)==GT_MANAGER_OK && status==GT_WORKER_UNSUPPORTED);
  size.px_height=0;size.cols=0;
  CHECK(gt__runtime_submit_user(lease,GT_WORKER_RESIZE,&stamp,&size,sizeof(size),user_completed,&unused,&status)==GT_MANAGER_OK && status==GT_WORKER_INVALID);
  CHECK(atomic_load(&f.memory.calls)==before && !unused.done);
  size=(gt_state_size_t){90,30,65535,65535};user_once(&f,lease,g,1,cut,GT_WORKER_RESIZE,&size,sizeof(size),GT_OK);
  user_once(&f,lease,g,2,cut,GT_WORKER_INPUT,"go\n",3,GT_OK);wait_text(&f,r.session,"FIRST");
  CHECK(gt__attachment_detach(a,g)==GT_TASK_OK);gt__runtime_release(&lease);finish(&f);
}
typedef struct {attach_fixture a;gt_user_dispatcher_t *users;unsigned accepted,dropped;void *allocation;size_t charge;} wire_fixture;
static void *wire_allocate(void *ctx,size_t n){wire_fixture *w=ctx;CHECK(!w->allocation);void *p=admit(&w->a.f.memory,n);if(p){w->allocation=p;w->charge=n;}return p;}
static void wire_release(void *ctx,void *p,size_t n){wire_fixture *w=ctx;CHECK(p==w->allocation && n==w->charge);for(size_t i=0;i<n;i++)CHECK(!((uint8_t *)p)[i]);w->allocation=NULL;refund(&w->a.f.memory,p,n);}
static gt_server_data_rc_t wire_step(void *ctx,gt_connection_t *c,const gt_token_scope_t *scope){wire_fixture *w=ctx;w->a.f.last_scope=*scope;gt_server_data_rc_t rc=gt__user_dispatcher_step(w->users,c,scope);if(rc==GT_SERVER_DATA_PROGRESS)w->accepted++;return rc;}
static void wire_drop(void *ctx,const gt_token_scope_t *scope){wire_fixture *w=ctx;CHECK(!gt__attachment_owner_current(w->a.owner,scope));w->dropped++;gt__user_dispatcher_drop(w->users,scope);}
static void wire_init(wire_fixture *w,const char *cwd,size_t peers,size_t commands) {
  memset(w,0,sizeof(*w));attach_init(&w->a,cwd,4);gt__server_close(w->a.server);w->a.server=NULL;
  gt_user_dispatcher_config_t c={.runtime=w->a.f.runtime,.attachments=w->a.owner,.instance={1},.connections=peers,.commands=commands};gt_allocator_t domain={w,wire_allocate,wire_release};
  CHECK(gt__user_dispatcher_open(&c,&domain,&w->users)==GT_OK);
  w->a.config.data_context=w;w->a.config.data_step=wire_step;w->a.config.data_drop=wire_drop;w->a.config.limits.receive_limit=GT_USER_PREFIX_BYTES+GT_USER_MAX_INPUT;
  CHECK(gt__server_open(&w->a.config,NULL,&w->a.server)==GT_OK);
}
static void wire_finish(wire_fixture *w) {
  gt__server_close(w->a.server);w->a.server=NULL;
  for(unsigned i=0;;i++){CHECK(i<5000);gt_runtime_rc_t rc=gt__user_dispatcher_close(w->users);if(rc==GT_RUNTIME_OK)break;CHECK(rc==GT_RUNTIME_BUSY);pause_one();}
  w->users=NULL;CHECK(!w->allocation && gt__attachment_owner_close(w->a.owner)==GT_RUNTIME_OK);w->a.owner=NULL;finish(&w->a.f);
}
static gt_attach_result_t wire_bind(wire_fixture *w,peer *management,peer *data,uint64_t request) {
  gt_attach_result_t r=request_attach(management,w->a.session,request);CHECK(r.status==GT_ATTACH_OK);
  peer_connect(data,w->a.server,73,GT_ROLE_DATA);bind_peer(data,&r,true);CHECK(w->a.f.last_scope.generation==r.generation);return r;
}
static void wire_send_user(peer *p,gt_user_request_t request) {
  uint8_t bytes[128];gt_wire_header_t h;CHECK(gt__user_request_encode(&request,bytes,sizeof(bytes),&h));peer_send(p,h.type,0,bytes,h.payload_length);memset(bytes,0xda,sizeof(bytes));
}
static gt_user_reply_t wire_result(peer *p,uint16_t type,uint64_t generation,uint64_t id) {
  gt_wire_frame_t f=peer_receive(p,type,0);gt_user_reply_t reply;CHECK(gt__user_reply_decode(&f,&reply) && reply.stamp.generation==generation && reply.stamp.command_id==id);CHECK(gt__connection_release_frame(p->client));return reply;
}
static gt_user_request_t wire_input(uint64_t gen,uint64_t id,uint64_t sequence,const char *text){return (gt_user_request_t){.kind=GT_WORKER_INPUT,.stamp={gen,id,sequence},.input=(const uint8_t *)text,.length=strlen(text)};}
static void wire_flow(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,2,4);peer control,data;peer_open(&control,w.a.server);gt_attach_result_t r=wire_bind(&w,&control,&data,1);
  CHECK(gt__user_dispatcher_close(w.users)==GT_RUNTIME_BUSY);wire_send_user(&data,wire_input(r.generation,1,0,"go\n"));gt_user_reply_t reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,1);CHECK(reply.status==GT_ERR_AUTH && reply.delivery==GT_USER_NOT_EXECUTED);
  gt_runtime_session_t *lease=acquire(&w.a.f,73,w.a.session);uint64_t cut=user_live(&w.a.f,w.a.session,lease,r.generation);gt__runtime_release(&lease);
  gt_user_request_t request={.kind=GT_WORKER_RESIZE,.stamp={r.generation,2,cut},.size={91,33,65536,0}};wire_send_user(&data,request);reply=wire_result(&data,GT_MSG_USER_REJECT,r.generation,2);CHECK(reply.rejected && reply.rejection==GT_USER_REJECT_UNSUPPORTED);
  request.size.px_width=65535;wire_send_user(&data,request);reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,2);CHECK(reply.kind==GT_WORKER_RESIZE && reply.status==GT_OK && reply.delivery==GT_USER_OPERATION_OK && reply.stamp.applied_sequence==cut);
  request=(gt_user_request_t){.kind=GT_WORKER_COLOR_SCHEME,.stamp={r.generation,3,cut},.color={1}};wire_send_user(&data,request);reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,3);CHECK(reply.kind==GT_WORKER_COLOR_SCHEME && reply.delivery==GT_USER_OPERATION_OK);
  wire_send_user(&data,wire_input(r.generation,4,cut,"go\n"));reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,4);CHECK(reply.status==GT_OK && reply.delivery==GT_USER_OPERATION_OK);wait_text(&w.a.f,w.a.session,"FIRST");
  wire_send_user(&data,wire_input(r.generation,4,cut,"go\n"));reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,4);CHECK(reply.status==GT_ERR_PROTOCOL && reply.delivery==GT_USER_NOT_EXECUTED);
  CHECK(!pthread_mutex_lock(&w.a.f.lock));model *m=find_model(&w.a.f,w.a.session);CHECK(m->resized==1 && m->scheme==1 && !strstr(m->text,"SECOND"));CHECK(!pthread_mutex_unlock(&w.a.f.lock));
  peer_close(&control);peer_close(&data);CHECK(w.dropped==1);wire_finish(&w);
}
static void wire_block(wire_fixture *w) {
  fixture *f=&w->a.f;CHECK(!pthread_mutex_lock(&f->lock));f->hold_apply=true;CHECK(!pthread_mutex_unlock(&f->lock));gt_state_color_scheme_t color={0};submit_command(f,w->a.session,GT_WORKER_COLOR_SCHEME,&color,sizeof(color),NULL,NULL);
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f->lock));while(!f->entered_apply)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void wire_unblock(wire_fixture *w){fixture *f=&w->a.f;CHECK(!pthread_mutex_lock(&f->lock));f->release_apply=true;CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));}
static void wire_accepted(wire_fixture *w,peer *p,unsigned n){for(unsigned i=0;w->accepted<n;i++){CHECK(i<5000);CHECK(gt__connection_step(p->client)==GT_CONNECTION_OPEN && gt__server_step(w->a.server)==GT_SERVER_RUNNING);pause_one();}}
static void wire_disconnected(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,2,2);peer control,data;peer_open(&control,w.a.server);gt_attach_result_t old=wire_bind(&w,&control,&data,1);gt_token_scope_t stale=w.a.f.last_scope;
  gt_runtime_session_t *lease=acquire(&w.a.f,73,w.a.session);uint64_t cut=user_live(&w.a.f,w.a.session,lease,old.generation);gt__runtime_release(&lease);wire_block(&w);
  wire_send_user(&data,wire_input(old.generation,1,cut,"go\n"));wire_accepted(&w,&data,1);peer_close(&data);CHECK(w.dropped==1 && gt__user_dispatcher_close(w.users)==GT_RUNTIME_BUSY);
  CHECK(gt__attachment_owner_close(w.a.owner)==GT_RUNTIME_BUSY);gt_attach_result_t next=wire_bind(&w,&control,&data,2);CHECK(next.generation>old.generation);
  gt__user_dispatcher_drop(w.users,&stale);CHECK(gt__attachment_owner_current(w.a.owner,&w.a.f.last_scope));wire_unblock(&w);
  lease=acquire(&w.a.f,73,w.a.session);cut=user_live(&w.a.f,w.a.session,lease,next.generation);gt__runtime_release(&lease);
  CHECK(!pthread_mutex_lock(&w.a.f.lock));CHECK(!strstr(find_model(&w.a.f,w.a.session)->text,"FIRST"));CHECK(!pthread_mutex_unlock(&w.a.f.lock));
  wire_send_user(&data,wire_input(next.generation,1,cut,"go\n"));gt_user_reply_t reply=wire_result(&data,GT_MSG_USER_RESULT,next.generation,1);CHECK(reply.status==GT_OK && reply.delivery==GT_USER_OPERATION_OK);wait_text(&w.a.f,w.a.session,"FIRST");
  peer_close(&data);peer_close(&control);wire_finish(&w);
}
static void wire_pressure(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,1,1);peer control,data;peer_open(&control,w.a.server);gt_attach_result_t r=wire_bind(&w,&control,&data,1);
  gt_runtime_session_t *lease=acquire(&w.a.f,73,w.a.session);uint64_t cut=user_live(&w.a.f,w.a.session,lease,r.generation);gt__runtime_release(&lease);wire_block(&w);
  wire_send_user(&data,wire_input(r.generation,1,cut,"go\n"));wire_accepted(&w,&data,1);wire_send_user(&data,wire_input(r.generation,2,cut,"go\n"));
  for(unsigned i=0;gt__server_connected(w.a.server,data.id);i++){CHECK(i<5000);(void)gt__connection_step(data.client);CHECK(gt__server_step(w.a.server)==GT_SERVER_RUNNING);pause_one();}
  gt__connection_destroy(data.client);data.client=NULL;CHECK(w.accepted==1 && w.dropped==1 && gt__user_dispatcher_close(w.users)==GT_RUNTIME_BUSY);
  wire_unblock(&w);peer_close(&control);wire_finish(&w);
}
static void wire_failure(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,2,4);peer control,data;peer_open(&control,w.a.server);gt_attach_result_t r=wire_bind(&w,&control,&data,1);gt_runtime_session_t *lease=acquire(&w.a.f,73,w.a.session);uint64_t cut=user_live(&w.a.f,w.a.session,lease,r.generation);gt__runtime_release(&lease);
  atomic_store(&w.a.f.memory.fail,atomic_load(&w.a.f.memory.calls)+1);wire_send_user(&data,wire_input(r.generation,1,cut,"go\n"));gt_user_reply_t reply=wire_result(&data,GT_MSG_USER_REJECT,r.generation,1);CHECK(reply.rejection==GT_USER_REJECT_NO_MEMORY);atomic_store(&w.a.f.memory.fail,0);
  wire_send_user(&data,wire_input(r.generation,1,cut,"go\n"));reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,1);CHECK(reply.delivery==GT_USER_OPERATION_OK);wait_text(&w.a.f,w.a.session,"FIRST");
  w.a.f.apply_fail=true;gt_user_request_t request={.kind=GT_WORKER_COLOR_SCHEME,.stamp={r.generation,2,cut},.color={1}};wire_send_user(&data,request);reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,2);CHECK(reply.status==GT_ERR_PROTOCOL && reply.delivery==GT_USER_OUTCOME_UNKNOWN);CHECK(terminal(&w.a.f,w.a.session).result.kind==GT_WORKER_END_ERROR);
  peer_close(&data);peer_close(&control);wire_finish(&w);
}
static void wire_malformed(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,2,4);peer control,data;peer_open(&control,w.a.server);
  for(unsigned variant=0;variant<4;variant++) {
    gt_attach_result_t r=wire_bind(&w,&control,&data,variant+1);gt_user_request_t request=wire_input(r.generation,1,0,"go\n");uint8_t b[32];gt_wire_header_t h;CHECK(gt__user_request_encode(&request,b,sizeof(b),&h));
    if(!variant)h.type=GT_MSG_USER_RESULT;else if(variant==1)h.request_id=1;else if(variant==2)encode_id(b,r.generation+1);else memset(b+8,0,8);
    peer_send(&data,h.type,h.request_id,b,h.payload_length);
    for(unsigned i=0;gt__server_connected(w.a.server,data.id);i++){CHECK(i<5000);(void)gt__connection_step(data.client);CHECK(gt__server_step(w.a.server)==GT_SERVER_RUNNING);pause_one();}gt__connection_destroy(data.client);data.client=NULL;
  }
  CHECK(w.dropped==4 && !w.accepted);peer_close(&control);
  for(unsigned type=GT_MSG_USER_INPUT;type<=GT_MSG_USER_RESULT;type++) {
    peer_open(&control,w.a.server);peer_send(&control,(uint16_t)type,1,NULL,0);
    for(unsigned i=0;gt__server_connected(w.a.server,control.id);i++){CHECK(i<5000);(void)gt__connection_step(control.client);CHECK(gt__server_step(w.a.server)==GT_SERVER_RUNNING);pause_one();}
    gt__connection_destroy(control.client);control.client=NULL;
  }
  CHECK(w.dropped==4 && !w.accepted);wire_finish(&w);
}
static void wire_backpressure(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,2,2);peer control,data;peer_open(&control,w.a.server);gt_attach_result_t r=wire_bind(&w,&control,&data,1);
  gt_runtime_session_t *lease=acquire(&w.a.f,73,w.a.session);uint64_t cut=user_live(&w.a.f,w.a.session,lease,r.generation);gt__runtime_release(&lease);wire_block(&w);
  gt_state_color_scheme_t color={0};for(unsigned i=0;i<15;i++)submit_command(&w.a.f,w.a.session,GT_WORKER_COLOR_SCHEME,&color,sizeof(color),NULL,NULL);
  wire_send_user(&data,wire_input(r.generation,1,cut,"go\n"));gt_user_reply_t reply=wire_result(&data,GT_MSG_USER_REJECT,r.generation,1);CHECK(reply.rejection==GT_USER_REJECT_BACKPRESSURE);wire_unblock(&w);
  struct timespec end=deadline();CHECK(!pthread_mutex_lock(&w.a.f.lock));while(find_model(&w.a.f,w.a.session)->scheme!=16)CHECK(!pthread_cond_timedwait(&w.a.f.changed,&w.a.f.lock,&end));CHECK(!pthread_mutex_unlock(&w.a.f.lock));
  wire_send_user(&data,wire_input(r.generation,1,cut,"go\n"));reply=wire_result(&data,GT_MSG_USER_RESULT,r.generation,1);CHECK(reply.delivery==GT_USER_OPERATION_OK);wait_text(&w.a.f,w.a.session,"FIRST");peer_close(&data);peer_close(&control);wire_finish(&w);
}

static void wire_config(const char *cwd) {
  wire_fixture w;wire_init(&w,cwd,2,2);gt_user_dispatcher_config_t c={.runtime=w.a.f.runtime,.attachments=w.a.owner,.instance={1},.connections=2,.commands=2};gt_user_dispatcher_t *out=(gt_user_dispatcher_t *)1;
  c.connections=0;CHECK(gt__user_dispatcher_open(&c,NULL,&out)==GT_ERR_INVALID_ARG && !out);c.connections=65;CHECK(gt__user_dispatcher_open(&c,NULL,&out)==GT_ERR_INVALID_ARG);c.connections=2;c.commands=0;CHECK(gt__user_dispatcher_open(&c,NULL,&out)==GT_ERR_INVALID_ARG);c.commands=17;CHECK(gt__user_dispatcher_open(&c,NULL,&out)==GT_ERR_INVALID_ARG);c.commands=2;c.instance[0]=2;CHECK(gt__user_dispatcher_open(&c,NULL,&out)==GT_ERR_INVALID_ARG);c.instance[0]=1;
  atomic_store(&w.a.f.memory.fail,atomic_load(&w.a.f.memory.calls)+1);gt_allocator_t domain={&w.a.f.memory,admit,refund};CHECK(gt__user_dispatcher_open(&c,&domain,&out)==GT_ERR_INTERNAL && !out);atomic_store(&w.a.f.memory.fail,0);
  gt__server_close(w.a.server);w.a.server=NULL;
  gt_server_config_t bad=w.a.config;bad.data_step=NULL;gt_server_t *other;CHECK(gt__server_open(&bad,NULL,&other)==GT_ERR_INVALID_ARG && !other);
  CHECK(gt__server_open(&w.a.config,NULL,&w.a.server)==GT_OK);
  gt_token_scope_t zero={0};CHECK(gt__user_dispatcher_step(w.users,NULL,&zero)==GT_SERVER_DATA_CLOSE);gt__user_dispatcher_drop(w.users,&zero);CHECK(gt__user_dispatcher_close(NULL)==GT_RUNTIME_OK);wire_finish(&w);
}

static void final_retained(const char *cwd) {
  fixture f;init(&f,0);CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);wait_text(&f,r.session,"READY:");gt_runtime_session_t *lease=acquire(&f,73,r.session);gt_runtime_final_t out;
  memset(&out,0xab,sizeof(out));CHECK(gt__runtime_final(lease,&out)==GT_RUNTIME_BUSY && !out.frozen && !out.result.has_sequence && !out.sequence);
  submit_command(&f,r.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,r.session,"FIRST");submit_command(&f,r.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&f,r.session,"SECOND");gt_managed_info_t info=terminal(&f,r.session);
  CHECK(gt__runtime_final(lease,&out)==GT_RUNTIME_OK && out.capture_status==GT_OK && out.frozen && out.result.kind==GT_WORKER_END_EOF && out.result.has_sequence && out.sequence==info.result.sequence && out.result.sequence==out.sequence && out.sequence>0);
  const frozen *saved=out.frozen;CHECK(strstr(saved->text,"FIRST") && strstr(saved->text,"SECOND"));CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);
  CHECK(!pthread_mutex_lock(&f.lock));CHECK(find_model(&f,r.session)->closed);memset(find_model(&f,r.session)->text,'!',sizeof(f.states[0].text));CHECK(f.freezes==1 && !f.released_frozen);CHECK(!pthread_mutex_unlock(&f.lock));
  gt_runtime_session_t *second=acquire(&f,73,r.session);gt_runtime_final_t another;CHECK(gt__runtime_final(second,&another)==GT_RUNTIME_OK && another.frozen==saved && another.sequence==out.sequence && strstr(((const frozen *)another.frozen)->text,"SECOND"));
  CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_BUSY);gt__runtime_release(&lease);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_BUSY && strstr(saved->text,"SECOND"));gt__runtime_release(&second);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK && f.released_frozen==1);
  CHECK(gt__runtime_final(NULL,&out)==GT_RUNTIME_INVALID && !out.frozen && !out.result.has_sequence);finish(&f);
}
static void final_unavailable(const char *cwd) {
  fixture f;init(&f,0);f.freeze_status=GT_ERR_UNSUPPORTED;CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");wait_text(&f,r.session,"READY:");gt_runtime_session_t *lease=acquire(&f,73,r.session);
  submit_command(&f,r.session,GT_WORKER_INPUT,"go\ngo\n",6,NULL,NULL);(void)terminal(&f,r.session);gt_runtime_final_t out;CHECK(gt__runtime_final(lease,&out)==GT_RUNTIME_OK && out.result.kind==GT_WORKER_END_EOF && out.result.status==GT_OK && out.result.has_sequence && out.result.sequence>0 && out.capture_status==GT_ERR_UNSUPPORTED && !out.frozen && !out.sequence);
  CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);gt__runtime_release(&lease);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK && !f.freezes && !f.released_frozen);finish(&f);
}
static void final_publication(const char *cwd) {
  fixture f;init(&f,0);f.hold_close=true;CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");wait_text(&f,r.session,"READY:");gt_runtime_session_t *lease=acquire(&f,73,r.session);
  submit_command(&f,r.session,GT_WORKER_INPUT,"go\ngo\n",6,NULL,NULL);struct timespec end=deadline();CHECK(!pthread_mutex_lock(&f.lock));while(!f.entered_close)CHECK(!pthread_cond_timedwait(&f.changed,&f.lock,&end));CHECK(f.freezes==1 && !f.closed);CHECK(!pthread_mutex_unlock(&f.lock));
  gt_runtime_final_t out;memset(&out,0xab,sizeof(out));CHECK(gt__runtime_final(lease,&out)==GT_RUNTIME_BUSY && !out.frozen && !out.sequence && !out.result.has_sequence);
  CHECK(!pthread_mutex_lock(&f.lock));f.release_close=true;CHECK(!pthread_cond_broadcast(&f.changed));CHECK(!pthread_mutex_unlock(&f.lock));(void)terminal(&f,r.session);
  CHECK(gt__runtime_final(lease,&out)==GT_RUNTIME_OK && out.frozen && strstr(((const frozen *)out.frozen)->text,"SECOND"));CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);gt__runtime_release(&lease);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK && f.released_frozen==1);finish(&f);
}
static void final_errors(const char *cwd) {
  for(unsigned kind=0;kind<2;kind++) {
    fixture f;init(&f,0);f.state_fail=!kind;f.apply_fail=kind;CHECK(open_runtime(&f)==GT_OK);gt_creation_result_t r=create(&f,cwd,1,73,"/bin/sh");CHECK(r.status==GT_CREATION_ACCEPTED);gt_managed_info_t info=terminal(&f,r.session);gt_runtime_session_t *lease=acquire(&f,73,r.session);gt_runtime_final_t out;
    CHECK(gt__runtime_final(lease,&out)==GT_RUNTIME_OK && !out.frozen && !out.sequence && out.result.kind==GT_WORKER_END_ERROR && out.result.has_sequence==(kind!=0) && out.result.sequence==0 && out.capture_status==(kind?GT_ERR_PROTOCOL:GT_ERR_UNSUPPORTED) && out.result.status==info.result.status && !f.freezes);
    CHECK(gt__runtime_reap(f.runtime,r.session)==GT_MANAGER_OK);gt__runtime_release(&lease);CHECK(gt__runtime_forget(f.runtime,r.session)==GT_RUNTIME_OK);finish(&f);
  }
}

typedef struct {gt_snapshot_blob_t blob;gt_allocator_t domain;size_t charge;uint8_t data[];} encoded_model;
typedef struct {attach_fixture a;gt_data_router_t *router;gt_data_router_config_t config;
  bool hold,entered,release,clock_fault;unsigned blob_mode;gt_status_t encode_status;unsigned encoded,freed,dropped;} router_fixture;
static gt_status_t route_encode(void *ctx,const void *source,size_t limit,const gt_allocator_t *domain,gt_snapshot_blob_t **out) {
  router_fixture *r=ctx;const frozen *f=source;*out=NULL;CHECK(!pthread_mutex_lock(&r->a.f.lock));r->entered=true;r->encoded++;CHECK(!pthread_cond_broadcast(&r->a.f.changed));
  while(r->hold && !r->release)CHECK(!pthread_cond_wait(&r->a.f.changed,&r->a.f.lock));
  gt_status_t status=r->encode_status;CHECK(!pthread_mutex_unlock(&r->a.f.lock));if(status!=GT_OK && r->blob_mode!=3)return status;
  size_t n=strlen(f->text)+1;if(n>limit)return GT_ERR_UNSUPPORTED;encoded_model *blob=domain->allocate(domain->context,sizeof(*blob)+n);if(!blob)return GT_ERR_INTERNAL;
  blob->domain=*domain;blob->charge=sizeof(*blob)+n;memcpy(blob->data,f->text,n);blob->blob=(gt_snapshot_blob_t){blob->data,r->blob_mode==1?0:r->blob_mode==2?limit+1:n};*out=&blob->blob;return r->blob_mode==3?GT_ERR_UNSUPPORTED:status;
}
static void route_free(void *ctx,gt_snapshot_blob_t *result){router_fixture *r=ctx;encoded_model *blob=(encoded_model *)result;gt_allocator_t a=blob->domain;size_t n=blob->charge;gt__secure_erase(blob,n);a.release(a.context,blob,n);CHECK(!pthread_mutex_lock(&r->a.f.lock));r->freed++;CHECK(!pthread_mutex_unlock(&r->a.f.lock));}
static gt_server_data_rc_t route_step(void *ctx,gt_connection_t *c,const gt_token_scope_t *s){router_fixture *r=ctx;r->a.f.last_scope=*s;return gt__data_router_step(r->router,c,s);}
static gt_server_data_rc_t route_revoked(void *ctx,gt_connection_t *c,const gt_token_scope_t *s){router_fixture *r=ctx;return gt__data_router_revoked(r->router,c,s);}
static void route_drop(void *ctx,const gt_token_scope_t *s){router_fixture *r=ctx;CHECK(!gt__attachment_owner_current(r->a.owner,s));r->dropped++;gt__data_router_drop(r->router,s);}
static bool route_now(void *ctx,uint64_t *now){router_fixture *r=ctx;return !r->clock_fault && clock_now(&r->a.f,now);}
static void route_init(router_fixture *r,const char *cwd,bool hold) {
  memset(r,0,sizeof(*r));attach_init(&r->a,cwd,4);gt__server_close(r->a.server);r->a.server=NULL;r->hold=hold;
  r->config=(gt_data_router_config_t){.runtime=r->a.f.runtime,.attachments=r->a.owner,.instance={1},.connections=4,.user_commands=4,.snapshot_limit=8192,.chunk_bytes=7,.format=7,
    .restore_timeout_ms=100,.error_timeout_ms=50,.clock_context=r,.clock=route_now,.encoder={r,route_encode,route_free}};
  gt_allocator_t domain={&r->a.f.memory,admit,refund};CHECK(gt__data_router_open(&r->config,&domain,&r->router)==GT_OK);
  r->a.config.data_context=r;r->a.config.data_step=route_step;r->a.config.data_drop=route_drop;r->a.config.data_revoked=route_revoked;r->a.config.limits.receive_limit=65560;r->a.config.limits.send_limit=16408;
  CHECK(gt__server_open(&r->a.config,NULL,&r->a.server)==GT_OK);
}
static void route_release(router_fixture *r){CHECK(!pthread_mutex_lock(&r->a.f.lock));r->release=true;CHECK(!pthread_cond_broadcast(&r->a.f.changed));CHECK(!pthread_mutex_unlock(&r->a.f.lock));}
static void route_entered(router_fixture *r){struct timespec end=deadline();CHECK(!pthread_mutex_lock(&r->a.f.lock));while(!r->entered)CHECK(!pthread_cond_timedwait(&r->a.f.changed,&r->a.f.lock,&end));CHECK(!pthread_mutex_unlock(&r->a.f.lock));}
static void route_finish(router_fixture *r){gt__server_close(r->a.server);r->a.server=NULL;for(unsigned i=0;;i++){CHECK(i<5000);gt_runtime_rc_t rc=gt__data_router_close(r->router);if(rc==GT_RUNTIME_OK)break;CHECK(rc==GT_RUNTIME_BUSY);pause_one();}r->router=NULL;CHECK(gt__attachment_owner_close(r->a.owner)==GT_RUNTIME_OK);r->a.owner=NULL;finish(&r->a.f);CHECK(r->freed<=r->encoded);}
static gt_attach_result_t route_bind(router_fixture *r,peer *control,peer *data,uint64_t request){gt_attach_result_t a=request_attach(control,r->a.session,request);CHECK(a.status==GT_ATTACH_OK);peer_connect(data,r->a.server,73,GT_ROLE_DATA);bind_peer(data,&a,true);return a;}
static gt_wire_frame_t route_receive(peer *p){for(unsigned i=0;i<5000;i++){CHECK(gt__connection_step(p->client)==GT_CONNECTION_OPEN && gt__server_step(p->server)==GT_SERVER_RUNNING);gt_wire_frame_t f;if(gt__connection_frame(p->client,&f)) {
  if(f.header.type!=GT_MSG_PROCESS_META)return f;
  gt_data_message_t m;CHECK(gt__data_decode(&f,&m) && m.generation==p->generation && !m.sequence);
  p->process=m.process;p->process_count++;CHECK(gt__connection_release_frame(p->client));
 }pause_one();}CHECK(false);return (gt_wire_frame_t){0};}
static gt_data_message_t route_message(peer *p){gt_wire_frame_t f=route_receive(p);gt_data_message_t m;CHECK(gt__data_decode(&f,&m));return m;}
static void route_send(peer *p,gt_data_message_t m){uint8_t b[128];gt_wire_header_t h;CHECK(gt__data_encode(&m,b,sizeof(b),&h));peer_send(p,h.type,0,b,h.payload_length);}
typedef struct {uint64_t generation,sequence,snapshot,cut,total;char text[4096];size_t used;unsigned resizes,colors;gt_state_size_t size;bool terminal;gt_data_message_t ended;} replica;
static void route_event(peer *p,replica *c,gt_data_message_t m){CHECK(m.type==GT_MSG_STATE_EVENT && m.generation==c->generation && m.sequence==c->sequence+1);c->sequence=m.sequence;if(m.kind==GT_STATE_OUTPUT){CHECK(m.length<sizeof(c->text)-c->used);memcpy(c->text+c->used,m.bytes,m.length);c->used+=m.length;c->text[c->used]=0;}else if(m.kind==GT_STATE_RESIZE){c->resizes++;c->size=m.size;}else {CHECK(m.kind==GT_STATE_COLOR_SCHEME);c->colors++;}CHECK(gt__connection_release_frame(p->client));}
static void route_snapshot(peer *p,replica *c,uint64_t generation,bool readonly) {
  memset(c,0,sizeof(*c));c->generation=generation;gt_data_message_t m=route_message(p);CHECK(m.type==GT_MSG_SNAPSHOT_BEGIN && m.generation==generation && m.format==7 && m.total<sizeof(c->text) && m.flags==(readonly?GT_DATA_READONLY:0));c->snapshot=c->sequence=m.sequence;c->total=m.total;CHECK(gt__connection_release_frame(p->client));size_t offset=0;
  for(;;){m=route_message(p);CHECK(m.generation==generation);if(m.type==GT_MSG_SNAPSHOT_END){CHECK(offset==c->total && m.total==c->total && m.sequence==c->snapshot);CHECK(gt__connection_release_frame(p->client));break;}CHECK(m.type==GT_MSG_SNAPSHOT_CHUNK && m.offset==offset && m.length<=7 && m.length<=c->total-offset);memcpy(c->text+offset,m.bytes,m.length);offset+=m.length;CHECK(gt__connection_release_frame(p->client));}
  CHECK(offset && c->text[offset-1]==0);c->used=offset-1;
}
static void route_cut(peer *p,replica *c){route_send(p,(gt_data_message_t){.type=GT_MSG_READY_S,.generation=c->generation,.sequence=c->snapshot});for(;;){gt_data_message_t m=route_message(p);if(m.type==GT_MSG_STATE_EVENT){route_event(p,c,m);continue;}CHECK(m.type==GT_MSG_LIVE_BEGIN && m.generation==c->generation && m.sequence==c->sequence);c->cut=m.sequence;CHECK(gt__connection_release_frame(p->client));break;}}
static void route_ready(peer *p,replica *c){route_send(p,(gt_data_message_t){.type=GT_MSG_READY_L,.generation=c->generation,.sequence=c->cut});for(unsigned i=0;i<8;i++){CHECK(gt__connection_step(p->client)==GT_CONNECTION_OPEN && gt__server_step(p->server)==GT_SERVER_RUNNING);}}
static gt_user_reply_t route_user(peer *p,replica *c,gt_user_request_t request,uint16_t type){wire_send_user(p,request);for(;;){gt_wire_frame_t f=route_receive(p);if(f.header.type==GT_MSG_STATE_EVENT){gt_data_message_t m;CHECK(gt__data_decode(&f,&m));route_event(p,c,m);continue;}gt_user_reply_t reply;CHECK(f.header.type==type && gt__user_reply_decode(&f,&reply) && reply.stamp.generation==request.stamp.generation && reply.stamp.command_id==request.stamp.command_id);CHECK(gt__connection_release_frame(p->client));return reply;}}
static void route_wait_text(peer *p,replica *c,const char *text){while(!strstr(c->text,text))route_event(p,c,route_message(p));}
static void route_flow(const char *cwd) {
  router_fixture r;route_init(&r,cwd,true);peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);route_entered(&r);replica c={.generation=a.generation};
  gt_user_reply_t reply=route_user(&data,&c,wire_input(a.generation,1,0,"go\n"),GT_MSG_USER_RESULT);CHECK(reply.status==GT_ERR_AUTH && reply.delivery==GT_USER_NOT_EXECUTED);route_release(&r);route_snapshot(&data,&c,a.generation,false);CHECK(strstr(c.text,"READY:"));route_cut(&data,&c);
  reply=route_user(&data,&c,wire_input(a.generation,2,c.sequence,"go\n"),GT_MSG_USER_RESULT);CHECK(reply.status==GT_ERR_AUTH && reply.delivery==GT_USER_NOT_EXECUTED);route_ready(&data,&c);
  reply=route_user(&data,&c,wire_input(a.generation,3,c.sequence,"go\n"),GT_MSG_USER_RESULT);CHECK(reply.delivery==GT_USER_OPERATION_OK);route_wait_text(&data,&c,"FIRST");
  /* The first OS observation may precede the child's chdir/exec. Keep the
   * Source parked on its next read and consume a later metadata observation;
   * metadata does not fabricate a terminal event to wake this consumer. */
  for(unsigned i=0;!data.process_count || !data.process.has_cwd || strcmp(data.process.cwd,cwd);i++) {
    CHECK(i<5000 && gt__connection_step(data.client)==GT_CONNECTION_OPEN && gt__server_step(data.server)==GT_SERVER_RUNNING);
    gt_wire_frame_t frame;if(gt__connection_frame(data.client,&frame)) {
      gt_data_message_t m;CHECK(gt__data_decode(&frame,&m) && m.generation==c.generation);
      if(m.type==GT_MSG_PROCESS_META){data.process=m.process;data.process_count++;CHECK(!m.sequence && gt__connection_release_frame(data.client));}
      else route_event(&data,&c,m);
    }
    pause_one();
  }
  CHECK(data.process_count && data.process.has_cwd && !strcmp(data.process.cwd,cwd));
  gt_user_request_t resize={.kind=GT_WORKER_RESIZE,.stamp={a.generation,4,c.sequence},.size={91,33,320,200}};reply=route_user(&data,&c,resize,GT_MSG_USER_RESULT);CHECK(reply.delivery==GT_USER_OPERATION_OK);while(!c.resizes)route_event(&data,&c,route_message(&data));CHECK(c.size.cols==91 && c.size.rows==33 && c.size.px_width==320 && c.size.px_height==200);
  gt_user_request_t color={.kind=GT_WORKER_COLOR_SCHEME,.stamp={a.generation,5,c.sequence},.color={1}};reply=route_user(&data,&c,color,GT_MSG_USER_RESULT);CHECK(reply.delivery==GT_USER_OPERATION_OK);while(!c.colors)route_event(&data,&c,route_message(&data));
  r.a.f.now=900;route_send(&data,(gt_data_message_t){.type=GT_MSG_DATA_PING,.generation=a.generation,.nonce=123});gt_data_message_t pong=route_message(&data);CHECK(pong.type==GT_MSG_DATA_PONG && pong.nonce==123);CHECK(gt__connection_release_frame(data.client));r.a.f.now=1500;CHECK(gt__server_step(r.a.server)==GT_SERVER_RUNNING && gt__server_connected(r.a.server,data.id));
  peer_close(&data);gt__connection_destroy(control.client);control.client=NULL;route_finish(&r);
}
static void route_final(const char *cwd) {
  router_fixture r;route_init(&r,cwd,false);peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);replica c;route_snapshot(&data,&c,a.generation,false);route_cut(&data,&c);route_ready(&data,&c);
  CHECK(route_user(&data,&c,wire_input(a.generation,1,c.sequence,"go\n"),GT_MSG_USER_RESULT).delivery==GT_USER_OPERATION_OK);route_wait_text(&data,&c,"FIRST");submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&r.a.f,r.a.session,"SECOND");(void)terminal(&r.a.f,r.a.session);
  for(;;){gt_data_message_t m=route_message(&data);if(m.type==GT_MSG_STATE_EVENT){route_event(&data,&c,m);continue;}CHECK(m.type==GT_MSG_SESSION_END && !m.flags && m.terminal.result.kind==GT_WORKER_END_EOF && m.terminal.result.has_sequence && m.terminal.result.sequence==c.sequence && strstr(c.text,"SECOND"));CHECK(gt__connection_release_frame(data.client));break;}
  peer_close(&data);CHECK(gt__runtime_reap(r.a.f.runtime,r.a.session)==GT_MANAGER_OK);a=route_bind(&r,&control,&data,2);route_snapshot(&data,&c,a.generation,true);CHECK(strstr(c.text,"FIRST") && strstr(c.text,"SECOND"));route_cut(&data,&c);CHECK(c.cut==c.snapshot);route_ready(&data,&c);gt_data_message_t ended=route_message(&data);CHECK(ended.type==GT_MSG_SESSION_END && !ended.flags && ended.terminal.result.sequence==c.sequence);CHECK(gt__connection_release_frame(data.client));
  gt_user_reply_t reply=route_user(&data,&c,wire_input(a.generation,1,c.sequence,"go\n"),GT_MSG_USER_REJECT);CHECK(reply.rejection==GT_USER_REJECT_STOPPED);peer_close(&data);peer_close(&control);route_finish(&r);CHECK(r.encoded==2 && r.freed==2);
}
static void route_cancel(const char *cwd) {
  router_fixture r;route_init(&r,cwd,true);peer control,data;peer_open(&control,r.a.server);gt_attach_result_t old=route_bind(&r,&control,&data,1);gt_token_scope_t stale=r.a.f.last_scope;route_entered(&r);peer_close(&data);CHECK(gt__data_router_close(r.router)==GT_RUNTIME_BUSY);
  submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&r.a.f,r.a.session,"FIRST");gt_attach_result_t next=route_bind(&r,&control,&data,2);CHECK(next.generation>old.generation);gt__data_router_drop(r.router,&stale);route_release(&r);replica c;route_snapshot(&data,&c,next.generation,false);CHECK(strstr(c.text,"FIRST"));route_cut(&data,&c);route_ready(&data,&c);peer_close(&data);peer_close(&control);route_finish(&r);CHECK(r.freed==r.encoded);
}
static void route_ack_close(peer *p,gt_data_message_t m){m.type=GT_MSG_RESTORE_ERROR_ACK;route_send(p,m);for(unsigned i=0;gt__server_connected(p->server,p->id);i++){CHECK(i<5000);(void)gt__connection_step(p->client);CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING);pause_one();}gt__connection_destroy(p->client);p->client=NULL;}

static void route_error(const char *cwd) {
  router_fixture r;route_init(&r,cwd,false);r.encode_status=GT_ERR_UNSUPPORTED;peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);gt_data_message_t error=route_message(&data);CHECK(error.type==GT_MSG_RESTORE_ERROR && error.error==GT_ERR_UNSUPPORTED && error.stage==GT_DATA_ERROR_ENCODE && error.generation==a.generation);CHECK(gt__connection_release_frame(data.client));route_ack_close(&data,error);
  submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&r.a.f,r.a.session,"FIRST");r.encode_status=GT_OK;a=route_bind(&r,&control,&data,2);replica c;route_snapshot(&data,&c,a.generation,false);CHECK(strstr(c.text,"FIRST"));route_cut(&data,&c);route_ready(&data,&c);peer_close(&data);peer_close(&control);route_finish(&r);
}
static void route_timeout(const char *cwd) {
  router_fixture r;route_init(&r,cwd,true);peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);route_entered(&r);r.a.f.now=50;route_send(&data,(gt_data_message_t){.type=GT_MSG_DATA_PING,.generation=a.generation,.nonce=99});gt_data_message_t m=route_message(&data);CHECK(m.type==GT_MSG_DATA_PONG && m.nonce==99);CHECK(gt__connection_release_frame(data.client));r.a.f.now=100;m=route_message(&data);CHECK(m.type==GT_MSG_RESTORE_ERROR && m.stage==GT_DATA_ERROR_DEADLINE && m.error==GT_ERR_CANCELLED);CHECK(gt__connection_release_frame(data.client));route_ack_close(&data,m);CHECK(gt__data_router_close(r.router)==GT_RUNTIME_BUSY);route_release(&r);peer_close(&control);route_finish(&r);
}
static void route_unavailable(const char *cwd) {
  for(unsigned kind=0;kind<3;kind++) {
    router_fixture r;route_init(&r,cwd,false);
    if(kind<2){r.a.f.state_fail=!kind;r.a.f.backend_fail=kind;gt_creation_result_t failed=create(&r.a.f,cwd,2,73,"/bin/sh");CHECK(failed.status==GT_CREATION_ACCEPTED);CHECK(terminal(&r.a.f,failed.session).result.kind==GT_WORKER_END_ERROR);r.a.session=failed.session;atomic_store(&r.a.f.memory.fail,0);}
    else {r.a.f.freeze_status=GT_ERR_UNSUPPORTED;submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"go\ngo\n",6,NULL,NULL);CHECK(terminal(&r.a.f,r.a.session).result.kind==GT_WORKER_END_EOF);}
    peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);gt_data_message_t m=route_message(&data);CHECK(m.type==GT_MSG_SESSION_END && m.flags==GT_DATA_NO_SNAPSHOT && m.error==(kind==1?GT_ERR_IO:GT_ERR_UNSUPPORTED) && m.generation==a.generation && m.terminal.result.has_sequence==(kind!=0) && (kind==2?m.terminal.result.sequence>0:m.terminal.result.sequence==0));CHECK(gt__connection_release_frame(data.client));peer_close(&data);peer_close(&control);route_finish(&r);CHECK(!r.encoded);
  }
}

static void route_crossing(const char *cwd) {
  router_fixture r;route_init(&r,cwd,true);peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);route_entered(&r);
  submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&r.a.f,r.a.session,"FIRST");route_release(&r);replica c;route_snapshot(&data,&c,a.generation,false);CHECK(!strstr(c.text,"FIRST"));route_cut(&data,&c);CHECK(c.cut>c.snapshot && strstr(c.text,"FIRST"));
  submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"go\n",3,NULL,NULL);wait_text(&r.a.f,r.a.session,"SECOND");route_wait_text(&data,&c,"SECOND");CHECK(c.sequence>c.cut);route_ready(&data,&c);
  gt_data_message_t m=route_message(&data);CHECK(m.type==GT_MSG_SESSION_END && !m.flags && m.terminal.result.has_sequence && m.terminal.result.sequence==c.sequence);CHECK(gt__connection_release_frame(data.client));peer_close(&data);peer_close(&control);route_finish(&r);
}
static void *route_deny(void *ctx,size_t n){(void)n;(*(unsigned *)ctx)++;return NULL;}
static void route_forbidden_release(void *ctx,void *p,size_t n){(void)ctx;(void)p;(void)n;CHECK(false);}
static void route_config(const char *cwd) {
  router_fixture r;route_init(&r,cwd,false);gt_data_router_config_t c=r.config;gt_data_router_t *out;
  unsigned calls=0;gt_allocator_t denied={&calls,route_deny,route_forbidden_release};CHECK(gt__data_router_open(&r.config,&denied,&out)==GT_ERR_INTERNAL && !out && calls==1);
  for(unsigned variant=0;variant<17;variant++) {
    c=r.config;if(!variant)c.connections=0;else if(variant==1)c.connections=65;else if(variant==2)c.user_commands=0;else if(variant==3)c.user_commands=17;else if(variant==4)c.snapshot_limit=0;else if(variant==5)c.snapshot_limit=GT_DATA_MAX_SNAPSHOT+1;else if(variant==6)c.chunk_bytes=0;else if(variant==7)c.chunk_bytes=65537;else if(variant==8)c.format=0;else if(variant==9)c.restore_timeout_ms=0;else if(variant==10)c.restore_timeout_ms=600001;else if(variant==11)c.error_timeout_ms=0;else if(variant==12)c.error_timeout_ms=60001;else if(variant==13)c.clock=NULL;else if(variant==14)c.encoder.encode=NULL;else if(variant==15)c.encoder.release=NULL;else c.instance[0]=2;
    unsigned invalid_calls=0;gt_allocator_t invalid_domain={&invalid_calls,route_deny,route_forbidden_release};
    out=(gt_data_router_t *)1;CHECK(gt__data_router_open(&c,&invalid_domain,&out)==GT_ERR_INVALID_ARG && !out && !invalid_calls);
  }
  for(unsigned i=1;i<=3;i++){size_t before=atomic_load(&r.a.f.memory.live);atomic_store(&r.a.f.memory.fail,atomic_load(&r.a.f.memory.calls)+i);gt_allocator_t domain={&r.a.f.memory,admit,refund};CHECK(gt__data_router_open(&r.config,&domain,&out)==GT_ERR_INTERNAL && !out && atomic_load(&r.a.f.memory.live)==before);atomic_store(&r.a.f.memory.fail,0);}
  gt__server_close(r.a.server);r.a.server=NULL;gt_server_config_t bad=r.a.config;bad.data_drop=NULL;gt_server_t *other;CHECK(gt__server_open(&bad,NULL,&other)==GT_ERR_INVALID_ARG && !other);CHECK(gt__server_open(&r.a.config,NULL,&r.a.server)==GT_OK);
  r.clock_fault=true;size_t before=atomic_load(&r.a.f.memory.live);gt_allocator_t domain={&r.a.f.memory,admit,refund};CHECK(gt__data_router_open(&r.config,&domain,&out)==GT_ERR_IO && !out && atomic_load(&r.a.f.memory.live)==before);r.clock_fault=false;
  peer control;for(uint16_t type=GT_MSG_SNAPSHOT_BEGIN;type<=GT_MSG_RESTORE_ERROR_ACK;type++){peer_open(&control,r.a.server);peer_send(&control,type,1,NULL,0);for(unsigned i=0;gt__server_connected(r.a.server,control.id);i++){CHECK(i<5000);(void)gt__connection_step(control.client);CHECK(gt__server_step(r.a.server)==GT_SERVER_RUNNING);pause_one();}gt__connection_destroy(control.client);}
  route_finish(&r);
}

static void route_malformed(const char *cwd) {
  for(unsigned variant=0;variant<4;variant++) {
    router_fixture r;route_init(&r,cwd,true);peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);route_entered(&r);
    gt_data_message_t m={.generation=a.generation,.sequence=0,.type=variant==0?GT_MSG_READY_S:variant==1?GT_MSG_READY_L:variant==2?GT_MSG_DATA_PONG:GT_MSG_DATA_PING,.nonce=1};if(variant==3)m.generation++;
    route_send(&data,m);for(unsigned i=0;gt__server_connected(r.a.server,data.id);i++){CHECK(i<5000);(void)gt__connection_step(data.client);CHECK(gt__server_step(r.a.server)==GT_SERVER_RUNNING);pause_one();}gt__connection_destroy(data.client);data.client=NULL;CHECK(gt__data_router_close(r.router)==GT_RUNTIME_BUSY);route_release(&r);peer_close(&control);route_finish(&r);
  }
}
static void route_clock(const char *cwd) {
  router_fixture r;route_init(&r,cwd,true);peer control,data;peer_open(&control,r.a.server);(void)route_bind(&r,&control,&data,1);route_entered(&r);r.clock_fault=true;CHECK(gt__server_step(r.a.server)==GT_SERVER_RUNNING && !gt__server_connected(r.a.server,data.id) && gt__server_connected(r.a.server,control.id));gt__connection_destroy(data.client);gt__connection_destroy(control.client);CHECK(gt__data_router_close(r.router)==GT_RUNTIME_BUSY);route_release(&r);route_finish(&r);
}
static void route_error_deadline(const char *cwd) {
  router_fixture r;route_init(&r,cwd,false);r.encode_status=GT_ERR_UNSUPPORTED;peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);gt_data_message_t m=route_message(&data);CHECK(m.type==GT_MSG_RESTORE_ERROR && m.error==GT_ERR_UNSUPPORTED);CHECK(gt__connection_release_frame(data.client));
  r.a.f.now=49;route_send(&data,(gt_data_message_t){.type=GT_MSG_DATA_PING,.generation=a.generation,.nonce=9});for(unsigned i=0;i<8;i++){(void)gt__connection_step(data.client);CHECK(gt__server_step(r.a.server)==GT_SERVER_RUNNING);}CHECK(gt__server_connected(r.a.server,data.id));
  r.a.f.now=50;CHECK(gt__server_step(r.a.server)==GT_SERVER_RUNNING && !gt__server_connected(r.a.server,data.id));gt__connection_destroy(data.client);peer_close(&control);route_finish(&r);
}

static void route_invalid_blob(const char *cwd) {
  for(unsigned mode=1;mode<=3;mode++){router_fixture r;route_init(&r,cwd,false);r.blob_mode=mode;peer control,data;peer_open(&control,r.a.server);(void)route_bind(&r,&control,&data,1);gt_data_message_t m=route_message(&data);CHECK(m.type==GT_MSG_RESTORE_ERROR && m.error==(mode==3?GT_ERR_UNSUPPORTED:GT_ERR_INTERNAL) && m.stage==GT_DATA_ERROR_ENCODE);CHECK(gt__connection_release_frame(data.client));route_ack_close(&data,m);peer_close(&control);route_finish(&r);CHECK(r.encoded==1 && r.freed==1);}
}
static void route_capture_pending(const char *cwd) {
  router_fixture r;route_init(&r,cwd,false);worker_barrier b={.f=&r.a.f};submit_command(&r.a.f,r.a.session,GT_WORKER_CAPTURE,NULL,0,block_worker,&b);struct timespec end=deadline();CHECK(!pthread_mutex_lock(&r.a.f.lock));while(!b.entered)CHECK(!pthread_cond_timedwait(&r.a.f.changed,&r.a.f.lock,&end));CHECK(!pthread_mutex_unlock(&r.a.f.lock));
  peer control,data;peer_open(&control,r.a.server);(void)route_bind(&r,&control,&data,1);peer_close(&data);CHECK(gt__data_router_close(r.router)==GT_RUNTIME_BUSY);CHECK(!pthread_mutex_lock(&r.a.f.lock));b.release=true;CHECK(!pthread_cond_broadcast(&r.a.f.changed));CHECK(!pthread_mutex_unlock(&r.a.f.lock));peer_close(&control);route_finish(&r);CHECK(!r.encoded);
}

static void route_cancelled_barrier(void *ctx,gt_status_t status,gt_state_capture_t capture){worker_barrier *b=ctx;CHECK(status==GT_ERR_CANCELLED && !capture.frozen);CHECK(!pthread_mutex_lock(&b->f->lock));b->entered=true;CHECK(!pthread_cond_broadcast(&b->f->changed));struct timespec end=deadline();while(!b->release)CHECK(!pthread_cond_timedwait(&b->f->changed,&b->f->lock,&end));CHECK(!pthread_mutex_unlock(&b->f->lock));}
static void route_capture_ending(const char *cwd) {
  router_fixture r;route_init(&r,cwd,false);worker_barrier first={.f=&r.a.f},last={.f=&r.a.f};submit_command(&r.a.f,r.a.session,GT_WORKER_CAPTURE,NULL,0,block_worker,&first);struct timespec end=deadline();CHECK(!pthread_mutex_lock(&r.a.f.lock));while(!first.entered)CHECK(!pthread_cond_timedwait(&r.a.f.changed,&r.a.f.lock,&end));CHECK(!pthread_mutex_unlock(&r.a.f.lock));
  peer control,data;peer_open(&control,r.a.server);gt_attach_result_t a=route_bind(&r,&control,&data,1);submit_command(&r.a.f,r.a.session,GT_WORKER_INPUT,"NEVER\n",6,route_cancelled_barrier,&last);CHECK(gt__manager_terminate(gt__runtime_manager(r.a.f.runtime),r.a.session)==GT_MANAGER_OK);
  CHECK(!pthread_mutex_lock(&r.a.f.lock));first.release=true;CHECK(!pthread_cond_broadcast(&r.a.f.changed));while(!last.entered)CHECK(!pthread_cond_timedwait(&r.a.f.changed,&r.a.f.lock,&end));CHECK(!pthread_mutex_unlock(&r.a.f.lock));
  route_send(&data,(gt_data_message_t){.type=GT_MSG_DATA_PING,.generation=a.generation,.nonce=12});gt_data_message_t pong=route_message(&data);CHECK(pong.type==GT_MSG_DATA_PONG && pong.nonce==12);CHECK(gt__connection_release_frame(data.client));CHECK(!r.encoded);
  CHECK(!pthread_mutex_lock(&r.a.f.lock));last.release=true;CHECK(!pthread_cond_broadcast(&r.a.f.changed));CHECK(!pthread_mutex_unlock(&r.a.f.lock));replica c;route_snapshot(&data,&c,a.generation,true);CHECK(!strstr(c.text,"FIRST"));route_cut(&data,&c);route_ready(&data,&c);gt_data_message_t m=route_message(&data);CHECK(m.type==GT_MSG_SESSION_END && m.terminal.result.kind==GT_WORKER_END_STOPPED && !m.flags && m.terminal.result.sequence==c.sequence);CHECK(gt__connection_release_frame(data.client));peer_close(&data);peer_close(&control);route_finish(&r);CHECK(r.encoded==1 && r.freed==1);
}

int main(int argc,char **argv) {
  CHECK(argc==3);
  if(!strcmp(argv[1],"lifecycle"))lifecycle(argv[2]);else if(!strcmp(argv[1],"stream"))stream(argv[2]);else if(!strcmp(argv[1],"pending"))pending(argv[2]);
  else if(!strcmp(argv[1],"limits"))limits(argv[2]);else if(!strcmp(argv[1],"failures"))failures(argv[2]);else if(!strcmp(argv[1],"allocation"))allocation_test(argv[2]);
  else if(!strcmp(argv[1],"concurrency"))concurrency(argv[2]);else if(!strcmp(argv[1],"invalid"))invalid(argv[2]);else if(!strcmp(argv[1],"service"))service(argv[2]);
  else if(!strcmp(argv[1],"attach-flow"))attach_flow(argv[2]);
  else if(!strcmp(argv[1],"attach-expiry"))attach_expiry(argv[2]);
  else if(!strcmp(argv[1],"attach-issuer"))attach_issuer(argv[2]);
  else if(!strcmp(argv[1],"attach-stale"))attach_stale(argv[2]);
  else if(!strcmp(argv[1],"attach-failures"))attach_failures(argv[2]);
  else if(!strcmp(argv[1],"attach-capacity"))attach_capacity(argv[2]);
  else if(!strcmp(argv[1],"attach-malformed"))attach_malformed(argv[2]);
  else if(!strcmp(argv[1],"attach-codec"))attach_codec(argv[2]);
  else if(!strcmp(argv[1],"attach-config"))attach_config(argv[2]);
  else if(!strcmp(argv[1],"attach-close"))attach_close(argv[2]);else if(!strcmp(argv[1],"attach-lost"))attach_lost(argv[2]);else if(!strcmp(argv[1],"attach-lease"))attach_lease(argv[2]);
  else if(!strcmp(argv[1],"user-policy"))user_policy(argv[2]);else if(!strcmp(argv[1],"user-stale-queue"))user_stale_queue(argv[2]);else if(!strcmp(argv[1],"user-readonly"))user_readonly(argv[2]);else if(!strcmp(argv[1],"user-dimensions"))user_dimensions(argv[2]);else if(!strcmp(argv[1],"wire-flow"))wire_flow(argv[2]);else if(!strcmp(argv[1],"wire-disconnected"))wire_disconnected(argv[2]);else if(!strcmp(argv[1],"wire-pressure"))wire_pressure(argv[2]);else if(!strcmp(argv[1],"wire-failure"))wire_failure(argv[2]);else if(!strcmp(argv[1],"wire-malformed"))wire_malformed(argv[2]);else if(!strcmp(argv[1],"wire-backpressure"))wire_backpressure(argv[2]);else if(!strcmp(argv[1],"wire-config"))wire_config(argv[2]);else if(!strcmp(argv[1],"final-retained"))final_retained(argv[2]);else if(!strcmp(argv[1],"final-unavailable"))final_unavailable(argv[2]);else if(!strcmp(argv[1],"final-publication"))final_publication(argv[2]);else if(!strcmp(argv[1],"final-errors"))final_errors(argv[2]);else if(!strcmp(argv[1],"route-flow"))route_flow(argv[2]);else if(!strcmp(argv[1],"route-final"))route_final(argv[2]);else if(!strcmp(argv[1],"route-cancel"))route_cancel(argv[2]);else if(!strcmp(argv[1],"route-error"))route_error(argv[2]);else if(!strcmp(argv[1],"route-timeout"))route_timeout(argv[2]);else if(!strcmp(argv[1],"route-unavailable"))route_unavailable(argv[2]);else if(!strcmp(argv[1],"route-crossing"))route_crossing(argv[2]);else if(!strcmp(argv[1],"route-config"))route_config(argv[2]);else if(!strcmp(argv[1],"route-malformed"))route_malformed(argv[2]);else if(!strcmp(argv[1],"route-clock"))route_clock(argv[2]);else if(!strcmp(argv[1],"route-error-deadline"))route_error_deadline(argv[2]);else if(!strcmp(argv[1],"route-invalid-blob"))route_invalid_blob(argv[2]);else if(!strcmp(argv[1],"route-capture-pending"))route_capture_pending(argv[2]);else if(!strcmp(argv[1],"route-capture-ending"))route_capture_ending(argv[2]);else CHECK(false);
  printf("PASS shared runtime %s\n",argv[1]);return 0;
}
