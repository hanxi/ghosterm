#define _POSIX_C_SOURCE 200809L
#include "session/manager.h"
#include "allocation-test.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
typedef struct {
  pthread_mutex_t lock;pthread_cond_t changed;
  gt_session_manager_t *manager;uint64_t id;
  char bytes[4096];size_t length;
  bool hold_finish,finish_entered,finish_release;
  unsigned disposed,opened,finished;
} tracker;
typedef struct {tracker *t;bool closed,finished,fail_state,fail_backend;} context;
static void init(tracker *t,gt_session_manager_t *m) {memset(t,0,sizeof(*t));t->manager=m;assert(!pthread_mutex_init(&t->lock,NULL));assert(!pthread_cond_init(&t->changed,NULL));}
static void destroy(tracker *t) {assert(!pthread_cond_destroy(&t->changed));assert(!pthread_mutex_destroy(&t->lock));}
static void pause_one(void) {struct timespec delay={0,1000000};nanosleep(&delay,NULL);}
static void wait_text(tracker *t,const char *text) {
  struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  assert(!pthread_mutex_lock(&t->lock));while(!strstr(t->bytes,text))assert(!pthread_cond_timedwait(&t->changed,&t->lock,&end));assert(!pthread_mutex_unlock(&t->lock));
}
static context *make(tracker *t) {context *c=calloc(1,sizeof(*c));assert(c);c->t=t;return c;}
static gt_status_t open_state(void *arg,gt_worker_reply_sink_t sink,void **out) {
  (void)sink;context *c=arg;gt_managed_info_t rows[8];size_t count=0;
  gt_manager_rc_t rc=gt__manager_list(c->t->manager,rows,8,&count);assert(rc==GT_MANAGER_OK || rc==GT_MANAGER_CLOSED);
  if(c->fail_state)return GT_ERR_UNSUPPORTED;
  *out=c;return GT_OK;
}
static gt_io_backend_t *open_backend(void *arg) {
  context *c=arg;assert(!pthread_mutex_lock(&c->t->lock));c->t->opened++;assert(!pthread_mutex_unlock(&c->t->lock));
  if(c->fail_backend)return NULL;
  const char *args[]={"-c","stty -echo; printf READY; read answer; printf DETACHED; read answer; printf END; exit 7",NULL};
  gt_pty_opts_t options={.executable="/bin/sh",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
  return gt__pty_open(&options);
}
static gt_status_t apply(void *arg,const gt_state_event_t *event) {
  context *c=arg;assert(event->kind==GT_STATE_OUTPUT);tracker *t=c->t;
  assert(!pthread_mutex_lock(&t->lock));assert(event->length<sizeof(t->bytes)-t->length);memcpy(t->bytes+t->length,event->payload,event->length);t->length+=event->length;t->bytes[t->length]=0;
  assert(!pthread_cond_broadcast(&t->changed));assert(!pthread_mutex_unlock(&t->lock));return GT_OK;
}
static gt_status_t freeze(void *arg,void **out) {(void)arg;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void close_state(void *arg) {context *c=arg;assert(!c->closed);c->closed=true;}
static void finished(void *arg,gt_status_t status) {
  context *c=arg;assert(status==(c->fail_state?GT_ERR_UNSUPPORTED:c->fail_backend?GT_ERR_IO:GT_OK));tracker *t=c->t;
  assert(!pthread_mutex_lock(&t->lock));t->finish_entered=true;assert(!pthread_cond_broadcast(&t->changed));
  while(t->hold_finish && !t->finish_release)assert(!pthread_cond_wait(&t->changed,&t->lock));
  c->finished=true;t->finished++;assert(!pthread_cond_broadcast(&t->changed));assert(!pthread_mutex_unlock(&t->lock));
}
static void dispose(void *arg) {
  context *c=arg;assert(c->finished && (c->closed || c->fail_state));tracker *t=c->t;
  if(t->id){gt_managed_info_t info;gt_manager_rc_t rc=gt__manager_info(t->manager,t->id,&info);assert(rc==GT_MANAGER_CLOSED || (rc==GT_MANAGER_OK && info.phase==GT_MANAGED_REAPING));}
  assert(!pthread_mutex_lock(&t->lock));t->disposed++;assert(!pthread_mutex_unlock(&t->lock));free(c);
}
static gt_worker_config_t config(context *c) {return (gt_worker_config_t){.context=c,.open_state=open_state,.open_backend=open_backend,.state_ops={apply,freeze},.close_state=close_state,.finished=finished};}
static gt_managed_info_t wait_result(gt_session_manager_t *m,uint64_t id) {
  gt_managed_info_t info;for(unsigned i=0;i<5000;i++){assert(gt__manager_info(m,id,&info)==GT_MANAGER_OK);if(info.has_result)return info;pause_one();}assert(0);return info;
}
static void send(gt_session_manager_t *m,uint64_t id) {gt_worker_submit_t rc=GT_WORKER_INVALID;assert(gt__manager_submit_internal(m,id,GT_WORKER_INPUT,"go\n",3,NULL,NULL,&rc)==GT_MANAGER_OK && rc==GT_WORKER_ACCEPTED);}
static void lifecycle(void) {
  gt_session_manager_t *m=gt__manager_open(1,NULL);assert(m);tracker t;init(&t,m);context *c=make(&t);gt_worker_config_t spec=config(c);uint64_t id=0;
  assert(gt__manager_create(m,&spec,NULL,NULL,dispose,NULL,&id)==GT_MANAGER_OK && id);t.id=id;wait_text(&t,"READY");
  gt_managed_info_t info;assert(gt__manager_info(m,id,&info)==GT_MANAGER_OK && !info.has_result && info.phase==GT_MANAGED_WORKER);
  assert(gt__manager_forget(m,id)==GT_MANAGER_BUSY && gt__manager_reap(m,id)==GT_MANAGER_BUSY);
  uint64_t rejected=99;context *other=make(&t);gt_worker_config_t second=config(other);assert(gt__manager_create(m,&second,NULL,NULL,dispose,NULL,&rejected)==GT_MANAGER_FULL && !rejected);free(other);
  send(m,id);wait_text(&t,"DETACHED");assert(gt__manager_info(m,id,&info)==GT_MANAGER_OK && !info.has_result);send(m,id);wait_text(&t,"END");info=wait_result(m,id);
  assert(info.result.kind==GT_WORKER_END_EOF && info.result.status==GT_OK && info.result.has_sequence && info.result.sequence>0);
  if(info.result.has_exit_code)assert(info.result.exit_code==7);else assert(info.result.termination_status==GT_ERR_NOT_FOUND);
  assert(gt__manager_reap(m,id)==GT_MANAGER_OK);gt_managed_info_t retained;assert(gt__manager_info(m,id,&retained)==GT_MANAGER_OK && retained.phase==GT_MANAGED_RETAINED && retained.has_result);
  assert(!memcmp(&retained.result,&info.result,sizeof(info.result)) && t.disposed==1);assert(gt__manager_reap(m,id)==GT_MANAGER_OK && t.disposed==1);
  other=make(&t);second=config(other);assert(gt__manager_create(m,&second,NULL,NULL,dispose,NULL,&rejected)==GT_MANAGER_FULL && !rejected);free(other);
  assert(gt__manager_forget(m,id)==GT_MANAGER_OK);assert(gt__manager_info(m,id,&retained)==GT_MANAGER_NOT_FOUND && !retained.id && !retained.has_result);
  memset(t.bytes,0,sizeof(t.bytes));t.length=0;other=make(&t);second=config(other);uint64_t next=0;assert(gt__manager_create(m,&second,NULL,NULL,dispose,NULL,&next)==GT_MANAGER_OK && next>id);t.id=next;wait_text(&t,"READY");
  gt_worker_submit_t submit=GT_WORKER_ACCEPTED;assert(gt__manager_submit_internal(m,id,GT_WORKER_INPUT,"go\n",3,NULL,NULL,&submit)==GT_MANAGER_NOT_FOUND && submit==GT_WORKER_INVALID);assert(gt__manager_terminate(m,id)==GT_MANAGER_NOT_FOUND);
  assert(gt__manager_terminate(m,next)==GT_MANAGER_OK);info=wait_result(m,next);assert(info.termination_requested && info.result.kind==GT_WORKER_END_STOPPED && !info.result.has_exit_code);
  assert(gt__manager_submit_internal(m,next,GT_WORKER_INPUT,"x",1,NULL,NULL,&submit)==GT_MANAGER_OK && submit==GT_WORKER_STOPPED);
  assert(gt__manager_reap(m,next)==GT_MANAGER_OK && t.disposed==2);assert(gt__manager_close(m)==GT_MANAGER_OK);destroy(&t);
}
static void startup(void) {
  for(unsigned kind=0;kind<2;kind++) {
    gt_session_manager_t *m=gt__manager_open(1,NULL);assert(m);tracker t;init(&t,m);context *c=make(&t);c->fail_state=!kind;c->fail_backend=kind;gt_worker_config_t spec=config(c);uint64_t id;
    assert(gt__manager_create(m,&spec,NULL,NULL,dispose,NULL,&id)==GT_MANAGER_OK);t.id=id;gt_managed_info_t info=wait_result(m,id);
    assert(info.result.kind==GT_WORKER_END_ERROR && !info.result.has_exit_code);
    assert(info.result.stage==(kind?GT_WORKER_STAGE_BACKEND_OPEN:GT_WORKER_STAGE_STATE_OPEN));assert(info.result.has_sequence==(kind!=0));
    assert(gt__manager_reap(m,id)==GT_MANAGER_OK && t.disposed==1 && t.opened==kind);assert(gt__manager_close(m)==GT_MANAGER_OK);destroy(&t);
  }
}
static void allocation(void) {
  assert(!gt__manager_open(0,NULL) && !gt__manager_open(65,NULL));
  uint64_t invalid=99;assert(gt__manager_create(NULL,NULL,NULL,NULL,NULL,NULL,&invalid)==GT_MANAGER_INVALID && !invalid);
  gt_managed_info_t bad={.id=99,.has_result=true};assert(gt__manager_info(NULL,1,&bad)==GT_MANAGER_INVALID && !bad.id && !bad.has_result);
  allocation_test_budget_t budget={.limit=SIZE_MAX,.fail=1};gt_allocator_t a={&budget,allocation_test_allocate,allocation_test_release};assert(!gt__manager_open(2,&a) && !budget.live);
  budget.fail=0;gt_session_manager_t *m=gt__manager_open(2,&a);assert(m && budget.count==1);tracker t;init(&t,m);context *c=make(&t);gt_worker_config_t spec=config(c);
  allocation_test_budget_t worker={.limit=SIZE_MAX,.fail=1};gt_allocator_t wa={&worker,allocation_test_allocate,allocation_test_release};uint64_t id=33;
  assert(gt__manager_create(m,&spec,&wa,NULL,dispose,NULL,&id)==GT_MANAGER_START_FAILED && !id && !t.disposed && !t.opened && !worker.live);free(c);
  assert(gt__manager_close(m)==GT_MANAGER_OK && !budget.live && !budget.count);destroy(&t);
}
typedef struct {gt_session_manager_t *m;gt_worker_config_t spec;gt_allocator_t *allocator;uint64_t id;gt_manager_rc_t rc;} creator;
static void *create_thread(void *arg) {creator *c=arg;c->rc=gt__manager_create(c->m,&c->spec,c->allocator,NULL,dispose,NULL,&c->id);return NULL;}
static void capacity(void) {
  gt_session_manager_t *m=gt__manager_open(2,NULL);assert(m);tracker t[8];creator c[8];pthread_t threads[8];
  for(unsigned i=0;i<8;i++){init(&t[i],m);c[i]=(creator){.m=m,.spec=config(make(&t[i]))};assert(!pthread_create(&threads[i],NULL,create_thread,&c[i]));}
  unsigned accepted=0;uint64_t previous=0;for(unsigned i=0;i<8;i++){assert(!pthread_join(threads[i],NULL));if(c[i].rc==GT_MANAGER_OK){accepted++;assert(c[i].id && c[i].id!=previous);previous=c[i].id;}else {assert(c[i].rc==GT_MANAGER_FULL && !c[i].id && !t[i].disposed);free(c[i].spec.context);}}
  assert(accepted==2);gt_managed_info_t rows[2];memset(rows,0xa5,sizeof(rows));gt_managed_info_t original[2];memcpy(original,rows,sizeof(rows));size_t count=99;
  assert(gt__manager_list(m,rows,1,&count)==GT_MANAGER_FULL && count==2 && !memcmp(rows,original,sizeof(rows)));
  assert(gt__manager_list(m,rows,2,&count)==GT_MANAGER_OK && count==2 && rows[0].id!=rows[1].id);
  assert(gt__manager_close(m)==GT_MANAGER_OK);for(unsigned i=0;i<8;i++){assert(t[i].disposed==(c[i].rc==GT_MANAGER_OK?1u:0u));destroy(&t[i]);}
}
static void *reap_thread(void *arg) {creator *c=arg;c->rc=gt__manager_reap(c->m,c->id);return NULL;}
static void joining(void) {
  gt_session_manager_t *m=gt__manager_open(1,NULL);assert(m);tracker t;init(&t,m);t.hold_finish=true;context *c=make(&t);gt_worker_config_t spec=config(c);uint64_t id;
  assert(gt__manager_create(m,&spec,NULL,NULL,dispose,NULL,&id)==GT_MANAGER_OK);t.id=id;wait_text(&t,"READY");assert(gt__manager_terminate(m,id)==GT_MANAGER_OK);(void)wait_result(m,id);
  creator reap={.m=m,.id=id};pthread_t thread;assert(!pthread_create(&thread,NULL,reap_thread,&reap));gt_managed_info_t info;
  for(unsigned i=0;;i++){assert(i<5000);assert(gt__manager_info(m,id,&info)==GT_MANAGER_OK);if(info.phase==GT_MANAGED_REAPING)break;pause_one();}
  assert(info.has_result && gt__manager_reap(m,id)==GT_MANAGER_BUSY && gt__manager_forget(m,id)==GT_MANAGER_BUSY && gt__manager_close(m)==GT_MANAGER_BUSY && !t.disposed);
  assert(!pthread_mutex_lock(&t.lock));t.finish_release=true;assert(!pthread_cond_broadcast(&t.changed));assert(!pthread_mutex_unlock(&t.lock));
  assert(!pthread_join(thread,NULL));assert(reap.rc==GT_MANAGER_OK && t.disposed==1);assert(gt__manager_close(m)==GT_MANAGER_OK);destroy(&t);
}
typedef struct {pthread_mutex_t lock;pthread_cond_t changed;bool entered,release;allocation_test_budget_t budget;} gate;
static void *gated_allocate(void *arg,size_t n) {
  gate *g=arg;assert(!pthread_mutex_lock(&g->lock));g->entered=true;assert(!pthread_cond_broadcast(&g->changed));
  while(!g->release)assert(!pthread_cond_wait(&g->changed,&g->lock));
  void *p=allocation_test_allocate(&g->budget,n);assert(!pthread_mutex_unlock(&g->lock));return p;
}
static void gated_release(void *arg,void *p,size_t n) {gate *g=arg;assert(!pthread_mutex_lock(&g->lock));allocation_test_release(&g->budget,p,n);assert(!pthread_mutex_unlock(&g->lock));}
static void creating(void) {
  for(unsigned deny=0;deny<2;deny++) {
    gt_session_manager_t *m=gt__manager_open(1,NULL);assert(m);tracker t;init(&t,m);context *c=make(&t);
    gate g={.budget={.limit=SIZE_MAX,.fail=deny?1:0}};assert(!pthread_mutex_init(&g.lock,NULL));assert(!pthread_cond_init(&g.changed,NULL));
    gt_allocator_t a={&g,gated_allocate,gated_release};creator create={.m=m,.spec=config(c),.allocator=&a};pthread_t thread;assert(!pthread_create(&thread,NULL,create_thread,&create));
    struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;assert(!pthread_mutex_lock(&g.lock));while(!g.entered)assert(!pthread_cond_timedwait(&g.changed,&g.lock,&end));assert(!pthread_mutex_unlock(&g.lock));
    assert(gt__manager_close(m)==GT_MANAGER_BUSY);gt_managed_info_t rows[1];size_t count=99;assert(gt__manager_list(m,rows,1,&count)==GT_MANAGER_OK && !count);
    context *extra=make(&t);gt_worker_config_t spec=config(extra);uint64_t id=99;assert(gt__manager_create(m,&spec,NULL,NULL,dispose,NULL,&id)==GT_MANAGER_FULL && !id);free(extra);
    assert(!pthread_mutex_lock(&g.lock));g.release=true;assert(!pthread_cond_broadcast(&g.changed));assert(!pthread_mutex_unlock(&g.lock));assert(!pthread_join(thread,NULL));
    if(deny){assert(create.rc==GT_MANAGER_START_FAILED && !create.id && !t.disposed);spec=config(c);assert(gt__manager_create(m,&spec,NULL,NULL,dispose,NULL,&id)==GT_MANAGER_OK);}
    else {assert(create.rc==GT_MANAGER_OK && create.id);id=create.id;}
    t.id=id;assert(gt__manager_close(m)==GT_MANAGER_OK && t.disposed==1 && !g.budget.live && !g.budget.count);
    assert(!pthread_cond_destroy(&g.changed));assert(!pthread_mutex_destroy(&g.lock));destroy(&t);
  }
}
int main(int argc,char **argv) {
  assert(argc==2);if(!strcmp(argv[1],"lifecycle"))lifecycle();else if(!strcmp(argv[1],"startup"))startup();else if(!strcmp(argv[1],"allocation"))allocation();else if(!strcmp(argv[1],"capacity"))capacity();else if(!strcmp(argv[1],"creating"))creating();else {assert(!strcmp(argv[1],"joining"));joining();}return 0;
}
