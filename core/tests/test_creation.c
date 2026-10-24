#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "session/creation.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct {atomic_size_t calls,fail,live,bytes;} budget;
typedef union {max_align_t align;size_t size;} allocation;
static void *allocate(void *ctx,size_t n) {
  budget *b=ctx;size_t call=atomic_fetch_add(&b->calls,1)+1;if(call==atomic_load(&b->fail))return NULL;
  allocation *a=malloc(sizeof(*a)+n);CHECK(a);a->size=n;atomic_fetch_add(&b->live,1);atomic_fetch_add(&b->bytes,n);return a+1;
}
static void release(void *ctx,void *p,size_t n) {
  budget *b=ctx;allocation *a=(allocation *)p-1;CHECK(a->size==n);
  for(size_t i=0;i<n;i++)CHECK(((uint8_t *)p)[i]==0);
  CHECK(atomic_fetch_sub(&b->live,1)>0 && atomic_fetch_sub(&b->bytes,n)>=n);free(a);
}
typedef struct {
  gt_creation_t *creation;gt_session_manager_t *manager;gt_creation_config_t config;budget memory;
  pthread_mutex_t lock;pthread_cond_t changed;pthread_t control;
  char text[8192];size_t length;unsigned prepared,disposed,opened,closed,finished,outputs,events;
  gt_status_t prepare_status;bool state_fail,invalid_config;gt_status_t bind_status;unsigned bound;uint8_t instance[16];
  uint64_t expected_principal,last_bound;bool observe_binding,binding_entered,binding_observed;
} fixture;
typedef struct {fixture *f;const gt_create_spec_t *spec;gt_pty_opts_t opts;const char *args[33],*env[65];pthread_t worker;uint64_t principal,session;bool state_fail;} context;
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static gt_io_backend_t *open_backend(void *ctx) {
  context *c=ctx;CHECK(pthread_equal(c->worker,pthread_self()) && !pthread_equal(c->f->control,pthread_self()));
  return gt__pty_open(&c->opts);
}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t reply,void **out) {
  (void)reply;context *c=ctx;CHECK(c->principal && c->session);c->worker=pthread_self();CHECK(!pthread_equal(c->f->control,c->worker));
  if(c->state_fail){*out=NULL;return GT_ERR_UNSUPPORTED;}
  CHECK(!pthread_mutex_lock(&c->f->lock));c->f->opened++;CHECK(!pthread_mutex_unlock(&c->f->lock));*out=c;return GT_OK;
}
static gt_status_t apply(void *ctx,const gt_state_event_t *event) {
  context *c=ctx;fixture *f=c->f;CHECK(pthread_equal(c->worker,pthread_self()) && event->kind==GT_STATE_OUTPUT);
  CHECK(!pthread_mutex_lock(&f->lock));CHECK(event->length<sizeof(f->text)-f->length);
  memcpy(f->text+f->length,event->payload,event->length);f->length+=event->length;f->text[f->length]=0;
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));return GT_OK;
}
static gt_status_t freeze(void *ctx,void **out){(void)ctx;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void close_state(void *ctx){context *c=ctx;CHECK(pthread_equal(c->worker,pthread_self()));CHECK(!pthread_mutex_lock(&c->f->lock));c->f->closed++;CHECK(!pthread_mutex_unlock(&c->f->lock));}
static void output(void *ctx,const uint8_t *p,size_t n,uint64_t seq){context *c=ctx;CHECK(p && n && seq && pthread_equal(c->worker,pthread_self()));CHECK(!pthread_mutex_lock(&c->f->lock));c->f->outputs++;CHECK(!pthread_mutex_unlock(&c->f->lock));}
static void event(void *ctx,const gt_state_event_t *e,uint64_t seq){context *c=ctx;CHECK(e && seq && pthread_equal(c->worker,pthread_self()));CHECK(!pthread_mutex_lock(&c->f->lock));c->f->events++;CHECK(!pthread_mutex_unlock(&c->f->lock));}
static void finished(void *ctx,gt_status_t status){context *c=ctx;CHECK(pthread_equal(c->worker,pthread_self()) && status==(c->state_fail?GT_ERR_UNSUPPORTED:GT_OK));CHECK(!pthread_mutex_lock(&c->f->lock));c->f->finished++;CHECK(!pthread_cond_broadcast(&c->f->changed));CHECK(!pthread_mutex_unlock(&c->f->lock));}
static gt_status_t prepare(void *arg,uint64_t principal,const gt_create_spec_t *spec,gt_worker_config_t *out) {
  fixture *f=arg;CHECK(principal==f->expected_principal && pthread_equal(f->control,pthread_self()));f->prepared++;
  context *c=calloc(1,sizeof(*c));CHECK(c);c->f=f;c->spec=spec;c->principal=principal;c->state_fail=f->state_fail;
  out->context=c;
  if(f->prepare_status!=GT_OK)return f->prepare_status;
  gt_status_t rc=gt__create_spec_pty(spec,&c->opts,c->args,c->env);if(rc!=GT_OK)return rc;
  *out=(gt_worker_config_t){.context=c,.open_backend=open_backend,.open_state=open_state,.close_state=close_state,
    .state_ops={apply,freeze},.output=output,.event=event,.finished=finished};
  if(f->invalid_config)out->state_ops.apply=NULL;
  return GT_OK;
}
static gt_status_t bind_id(void *arg,void *ctx,uint64_t principal,uint64_t id) {
  fixture *f=arg;context *c=ctx;CHECK(c && id && principal==f->expected_principal && c->principal==principal && pthread_equal(f->control,pthread_self()));
  c->session=id;f->bound++;f->last_bound=id;
  if(f->observe_binding) {
    /* Test-only rendezvous: the prestarted observer proves registry operations
     * can run while this callback is active. A mutex regression fails here in
     * ordinary CHECK, rather than relying on the external process watchdog. */
    struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=3;
    CHECK(!pthread_mutex_lock(&f->lock));CHECK(!f->opened);f->binding_entered=true;
    CHECK(!pthread_cond_broadcast(&f->changed));
    while(!f->binding_observed)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));
    CHECK(!pthread_mutex_unlock(&f->lock));
  } else {gt_managed_info_t info;CHECK(gt__manager_info(f->manager,id,&info)==GT_MANAGER_BUSY);}
  return f->bind_status;
}
static void dispose(void *arg,void *ctx) {
  fixture *f=arg;context *c=ctx;CHECK(pthread_equal(f->control,pthread_self()));
  CHECK(c && c->f==f && c->spec->nonce[0] && c->spec->executable[0]=='/');f->disposed++;free(c);
}
static void open_fixture(fixture *f,size_t capacity,size_t manager_capacity,uint32_t ttl) {
  memset(f,0,sizeof(*f));atomic_init(&f->memory.calls,0);atomic_init(&f->memory.fail,0);atomic_init(&f->memory.live,0);atomic_init(&f->memory.bytes,0);f->control=pthread_self();CHECK(!pthread_mutex_init(&f->lock,NULL));CHECK(!pthread_cond_init(&f->changed,NULL));
  f->manager=gt__manager_open(manager_capacity,NULL);CHECK(f->manager);f->instance[0]=1;
  f->config=(gt_creation_config_t){.manager=f->manager,.capacity=capacity,.retention_ms=ttl,.factory_context=f,.prepare=prepare,.bind_id=bind_id,.dispose=dispose};
  memcpy(f->config.instance,f->instance,16);gt_allocator_t a={&f->memory,allocate,release};
  CHECK(gt__creation_open(&f->config,0,&a,&f->creation)==GT_OK);
}
static void close_fixture(fixture *f) {
  gt__creation_close(f->creation);f->creation=NULL;CHECK(gt__manager_close(f->manager)==GT_MANAGER_OK);
  CHECK(!atomic_load(&f->memory.live) && !atomic_load(&f->memory.bytes) && f->prepared==f->disposed);
  CHECK(!pthread_cond_destroy(&f->changed));CHECK(!pthread_mutex_destroy(&f->lock));
}
static gt_create_spec_t spec(const char *cwd,unsigned nonce) {
  gt_create_spec_t s={.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,
    .executable="/bin/sh",.cwd=cwd,.term="xterm-256color",.argc=4,.envc=1,
    .argv={"-c","stty -echo; printf 'CTX:%s:%s:%s\n' \"$GTERM_CREATE_TEST\" \"$PWD\" \"$1\"; read a; printf DETACHED; read a; printf DONE; exit 7","zero","literal ;$() 中文"},
    .env={"GTERM_CREATE_TEST=one"}};s.nonce[0]=(uint8_t)nonce;return s;
}
static size_t encode(const gt_create_spec_t *s,uint8_t bytes[GT_CREATE_MAX_BYTES]){size_t n=0;CHECK(gt__create_spec_encode(s,bytes,GT_CREATE_MAX_BYTES,&n)==GT_OK);return n;}
static gt_creation_result_t submit(fixture *f,gt_create_spec_t s,uint64_t principal,uint64_t now) {
  f->expected_principal=principal;
  uint8_t bytes[GT_CREATE_MAX_BYTES];size_t n=encode(&s,bytes);gt_creation_result_t r=gt__creation_submit(f->creation,principal,bytes,n,now);
  memset(bytes,0xa5,n);return r;
}
static void wait_text(fixture *f,const char *text) {
  struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&f->lock));while(!strstr(f->text,text))CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void input(fixture *f,uint64_t id){gt_worker_submit_t status;CHECK(gt__manager_submit_internal(f->manager,id,GT_WORKER_INPUT,"go\n",3,NULL,NULL,&status)==GT_MANAGER_OK && status==GT_WORKER_ACCEPTED);}
static gt_managed_info_t terminal(fixture *f,uint64_t id) {
  gt_managed_info_t info;for(unsigned i=0;i<5000;i++){CHECK(gt__manager_info(f->manager,id,&info)==GT_MANAGER_OK);if(info.has_result)return info;pause_one();}CHECK(false);return info;
}
static void lifecycle(const char *cwd) {
  fixture f;open_fixture(&f,2,2,100);gt_create_spec_t s=spec(cwd,1);gt_creation_result_t r=submit(&f,s,1,0);
  CHECK(r.status==GT_CREATION_ACCEPTED && r.session && f.prepared==1 && !f.disposed);wait_text(&f,"CTX:one:");wait_text(&f,"literal ;$() 中文");
  CHECK(!pthread_mutex_lock(&f.lock));CHECK(strstr(f.text,cwd));CHECK(!pthread_mutex_unlock(&f.lock));
  gt__creation_close(f.creation);f.creation=NULL;
  CHECK(atomic_load(&f.memory.live)==1);input(&f,r.session);wait_text(&f,"DETACHED");input(&f,r.session);wait_text(&f,"DONE");
  gt_managed_info_t info=terminal(&f,r.session);CHECK(info.result.kind==GT_WORKER_END_EOF && !info.termination_requested);
  CHECK(gt__manager_reap(f.manager,r.session)==GT_MANAGER_OK && f.disposed==1 && !atomic_load(&f.memory.live));
  CHECK(f.opened==1 && f.closed==1 && f.finished==1 && f.outputs && f.events);close_fixture(&f);
}
static void replay(const char *cwd) {
  fixture f;open_fixture(&f,4,4,100);gt_create_spec_t s=spec(cwd,1);gt_creation_result_t a=submit(&f,s,1,0),b=submit(&f,s,1,1);
  CHECK(a.status==GT_CREATION_ACCEPTED && b.status==GT_CREATION_ACCEPTED && a.session==b.session && f.prepared==1);
  s.env[0]="GTERM_CREATE_TEST=two";b=submit(&f,s,1,2);CHECK(b.status==GT_CREATION_CONFLICT && !b.session && f.prepared==1);
  b=gt__creation_query(f.creation,2,s.nonce,2);CHECK(b.status==GT_CREATION_UNKNOWN && !b.session);
  b=submit(&f,s,2,3);CHECK(b.status==GT_CREATION_ACCEPTED && b.session!=a.session && f.prepared==2);
  CHECK(gt__creation_query(f.creation,1,s.nonce,3).session==a.session);close_fixture(&f);
}
static void expiry(const char *cwd) {
  fixture f;open_fixture(&f,1,2,10);gt_create_spec_t s=spec(cwd,1);gt_creation_result_t a=submit(&f,s,1,0);
  CHECK(a.status==GT_CREATION_ACCEPTED);s.nonce[0]=2;CHECK(submit(&f,s,1,9).status==GT_CREATION_BUSY && f.prepared==1);
  CHECK(gt__creation_advance(f.creation,10)==GT_OK);s.nonce[0]=1;CHECK(gt__creation_query(f.creation,1,s.nonce,10).status==GT_CREATION_UNKNOWN);
  wait_text(&f,"CTX:one:");input(&f,a.session);wait_text(&f,"DETACHED");s.nonce[0]=2;
  CHECK(submit(&f,s,1,10).status==GT_CREATION_ACCEPTED && f.prepared==2);close_fixture(&f);
}
static void failures(const char *cwd) {
  fixture f;open_fixture(&f,4,1,100);gt_create_spec_t s=spec(cwd,1);f.prepare_status=GT_ERR_UNSUPPORTED;
  gt_creation_result_t a=submit(&f,s,1,0);CHECK(a.status==GT_CREATION_UNSUPPORTED && a.detail==GT_ERR_UNSUPPORTED && !a.session && f.disposed==1);
  CHECK(submit(&f,s,1,0).status==GT_CREATION_UNSUPPORTED && f.prepared==1);
  f.prepare_status=GT_OK;s.nonce[0]=2;a=submit(&f,s,1,0);CHECK(a.status==GT_CREATION_ACCEPTED);
  s.nonce[0]=3;CHECK(submit(&f,s,1,0).status==GT_CREATION_BUSY && f.prepared==3 && f.disposed==2);
  CHECK(gt__creation_query(f.creation,1,s.nonce,1).status==GT_CREATION_BUSY);close_fixture(&f);
}
static void asynchronous(const char *cwd) {
  fixture f;open_fixture(&f,1,1,100);f.state_fail=true;gt_create_spec_t s=spec(cwd,1);gt_creation_result_t a=submit(&f,s,1,0);
  CHECK(a.status==GT_CREATION_ACCEPTED && a.session);gt_managed_info_t info=terminal(&f,a.session);
  CHECK(info.result.kind==GT_WORKER_END_ERROR && info.result.stage==GT_WORKER_STAGE_STATE_OPEN && info.result.status==GT_ERR_UNSUPPORTED && !info.result.has_exit_code);
  CHECK(gt__creation_query(f.creation,1,s.nonce,1).status==GT_CREATION_ACCEPTED);close_fixture(&f);
}
static void clock_test(const char *cwd) {
  fixture f;open_fixture(&f,2,2,100);gt_create_spec_t s=spec(cwd,1);gt_creation_result_t a=submit(&f,s,1,5);
  CHECK(a.status==GT_CREATION_ACCEPTED && gt__creation_advance(f.creation,4)==GT_ERR_IO);
  CHECK(gt__creation_query(f.creation,1,s.nonce,10).status==GT_CREATION_CLOCK);s.nonce[0]=2;CHECK(submit(&f,s,1,10).status==GT_CREATION_CLOCK && f.prepared==1);
  wait_text(&f,"CTX:one:");input(&f,a.session);wait_text(&f,"DETACHED");close_fixture(&f);
}
static void allocation_test(const char *cwd) {
  fixture f;open_fixture(&f,2,2,100);gt_create_spec_t s=spec(cwd,1);atomic_store(&f.memory.fail,atomic_load(&f.memory.calls)+1);
  CHECK(submit(&f,s,1,0).status==GT_CREATION_NO_MEMORY && !f.prepared && atomic_load(&f.memory.live)==1);
  CHECK(submit(&f,s,1,1).status==GT_CREATION_NO_MEMORY && !f.prepared);s.nonce[0]=2;
  CHECK(submit(&f,s,1,1).status==GT_CREATION_ACCEPTED);close_fixture(&f);
}
static void *deny_worker(void *ctx,size_t n){(void)ctx;(void)n;return NULL;}
static void no_refund(void *ctx,void *p,size_t n){(void)ctx;(void)p;(void)n;CHECK(false);}
static void launch_failures(const char *cwd) {
  fixture f;open_fixture(&f,2,2,100);gt__creation_close(f.creation);f.creation=NULL;
  f.config.worker_allocator=(gt_allocator_t){NULL,deny_worker,no_refund};gt_allocator_t a={&f.memory,allocate,release};
  CHECK(gt__creation_open(&f.config,0,&a,&f.creation)==GT_OK);gt_create_spec_t s=spec(cwd,1);
  gt_creation_result_t r=submit(&f,s,1,0);CHECK(r.status==GT_CREATION_START_FAILED && !r.session && f.prepared==1 && f.bound==1 && f.disposed==1);
  CHECK(submit(&f,s,1,1).status==GT_CREATION_START_FAILED && f.prepared==1);close_fixture(&f);
  open_fixture(&f,1,1,100);f.invalid_config=true;r=submit(&f,s,1,0);
  CHECK(r.status==GT_CREATION_INTERNAL && r.detail==GT_ERR_INVALID_ARG && !r.session && !f.bound && f.disposed==1);close_fixture(&f);
}
static void *observe_binding(void *arg) {
  fixture *f=arg;struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=3;
  CHECK(!pthread_mutex_lock(&f->lock));
  while(!f->binding_entered)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));
  uint64_t id=f->last_bound;CHECK(!pthread_mutex_unlock(&f->lock));
  gt_managed_info_t info;size_t count=99;gt_worker_submit_t submitted;
  CHECK(gt__manager_info(f->manager,id,&info)==GT_MANAGER_BUSY && !info.id);
  CHECK(gt__manager_list(f->manager,&info,1,&count)==GT_MANAGER_OK && !count);
  CHECK(gt__manager_submit_internal(f->manager,id,GT_WORKER_INPUT,"x",1,NULL,NULL,&submitted)==GT_MANAGER_BUSY && submitted==GT_WORKER_INVALID);
  CHECK(gt__manager_terminate(f->manager,id)==GT_MANAGER_BUSY && gt__manager_reap(f->manager,id)==GT_MANAGER_BUSY && gt__manager_forget(f->manager,id)==GT_MANAGER_BUSY);
  CHECK(gt__manager_close(f->manager)==GT_MANAGER_BUSY);
  CHECK(!pthread_mutex_lock(&f->lock));f->binding_observed=true;CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));return NULL;
}
static void *count_worker(void *ctx,size_t n){atomic_fetch_add((atomic_uint *)ctx,1);return malloc(n);}
static void release_worker(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static void registration(const char *cwd) {
  fixture f;open_fixture(&f,4,1,100);gt__creation_close(f.creation);f.creation=NULL;
  atomic_uint worker_allocations;atomic_init(&worker_allocations,0);
  f.config.worker_allocator=(gt_allocator_t){&worker_allocations,count_worker,release_worker};gt_allocator_t a={&f.memory,allocate,release};
  CHECK(gt__creation_open(&f.config,0,&a,&f.creation)==GT_OK);
  f.observe_binding=true;f.bind_status=GT_ERR_UNSUPPORTED;pthread_t observer;CHECK(!pthread_create(&observer,NULL,observe_binding,&f));
  gt_create_spec_t s=spec(cwd,1);gt_creation_result_t r=submit(&f,s,37,0);CHECK(!pthread_join(observer,NULL));
  uint64_t rejected=f.last_bound;CHECK(rejected && r.status==GT_CREATION_PREPARE_FAILED && r.detail==GT_ERR_UNSUPPORTED && !r.session);
  CHECK(f.bound==1 && f.prepared==1 && f.disposed==1 && !f.opened && !f.closed && !f.finished && !atomic_load(&worker_allocations));
  CHECK(atomic_load(&f.memory.live)==1);
  CHECK(submit(&f,s,37,1).status==GT_CREATION_PREPARE_FAILED && f.bound==1 && f.prepared==1 && f.disposed==1);
  CHECK(gt__creation_query(f.creation,37,s.nonce,1).detail==GT_ERR_UNSUPPORTED);
  gt_managed_info_t info;CHECK(gt__manager_info(f.manager,rejected,&info)==GT_MANAGER_NOT_FOUND);
  f.bind_status=GT_OK;f.binding_entered=false;f.binding_observed=false;s.nonce[0]=2;
  CHECK(!pthread_create(&observer,NULL,observe_binding,&f));r=submit(&f,s,91,2);CHECK(!pthread_join(observer,NULL));
  CHECK(r.status==GT_CREATION_ACCEPTED && r.session>rejected && f.bound==2 && f.prepared==2 && f.disposed==1 && atomic_load(&worker_allocations));
  wait_text(&f,"CTX:one:");CHECK(gt__manager_info(f.manager,r.session,&info)==GT_MANAGER_OK && !info.has_result);
  close_fixture(&f);CHECK(f.disposed==2 && f.opened==1 && f.closed==1 && f.finished==1);
}
static void limits(void) {
  gt_create_spec_t s={.command=GT_PTY_CMD_DIRECT,.cols=1,.rows=1,.environment=GT_CREATE_HOST_ENV,.executable="/",.cwd="/",.term="x",.argc=1};s.nonce[0]=1;
  char *argument=malloc(8127);CHECK(argument);memset(argument,'X',8126);argument[8125]=0;s.argv[0]=argument;
  uint8_t bytes[8193];size_t n;CHECK(gt__create_spec_encode(&s,bytes,8192,&n)==GT_OK && n==8192);
  gt_create_spec_t out;char storage[GT_CREATE_MAX_BYTES];CHECK(gt__create_spec_decode(bytes,n,&out,storage,sizeof(storage))==GT_OK && strlen(out.argv[0])==8125);
  argument[8125]='X';argument[8126]=0;CHECK(gt__create_spec_encode(&s,bytes,8192,&n)==GT_ERR_INVALID_ARG && !n);
  argument[8125]=0;CHECK(gt__create_spec_encode(&s,bytes,8192,&n)==GT_OK);
  bytes[65]=0x1f;bytes[66]=0xbe;bytes[8192]='X';CHECK(gt__create_spec_decode(bytes,8193,&out,storage,sizeof(storage))==GT_ERR_PROTOCOL);
  s.argc=0;CHECK(gt__create_spec_encode(&s,bytes,8192,&n)==GT_OK && n==63);bytes[39]=33;memset(bytes+63,0,33*4);
  CHECK(gt__create_spec_decode(bytes,195,&out,storage,sizeof(storage))==GT_ERR_PROTOCOL);free(argument);
}
static void unsupported(const char *cwd) {
  fixture f;open_fixture(&f,1,1,100);gt_create_spec_t s=spec(cwd,1);uint8_t bytes[8192];size_t n=encode(&s,bytes);bytes[35]=2;
  gt_creation_result_t r=gt__creation_submit(f.creation,1,bytes,n,0);
  CHECK(r.status==GT_CREATION_UNSUPPORTED && r.detail==GT_ERR_UNSUPPORTED && !f.prepared);
  CHECK(gt__creation_query(f.creation,1,s.nonce,1).status==GT_CREATION_UNSUPPORTED);
  bytes[35]=1;CHECK(gt__creation_submit(f.creation,1,bytes,n,2).status==GT_CREATION_CONFLICT && !f.prepared);close_fixture(&f);
}
static void codec(const char *cwd) {
  gt_create_spec_t s=spec(cwd,1),out;uint8_t bytes[GT_CREATE_MAX_BYTES+1];size_t n=encode(&s,bytes);char text[GT_CREATE_MAX_BYTES];
  CHECK(bytes[0]==1 && bytes[19]==1 && bytes[20]==0 && bytes[21]==80 && bytes[23]==24 && bytes[35]==1 && bytes[39]==4 && bytes[43]==1 && !bytes[47] && bytes[51]==7 && !memcmp(bytes+52,"/bin/sh",7));
  CHECK(gt__create_spec_decode(bytes,n,&out,text,sizeof(text))==GT_OK && !strcmp(out.cwd,cwd) && !strcmp(out.argv[3],s.argv[3]));
  for(size_t i=0;i<n;i++)CHECK(gt__create_spec_decode(bytes,i,&out,text,sizeof(text))==GT_ERR_PROTOCOL && !out.executable);
  bytes[n]=0;CHECK(gt__create_spec_decode(bytes,n+1,&out,text,sizeof(text))==GT_ERR_PROTOCOL);
  bytes[35]=2;CHECK(gt__create_spec_decode(bytes,n,&out,text,sizeof(text))==GT_ERR_UNSUPPORTED);bytes[35]=1;
  bytes[47]=1;CHECK(gt__create_spec_decode(bytes,n,&out,text,sizeof(text))==GT_ERR_PROTOCOL);bytes[47]=0;
  s.envc=2;s.env[1]="GTERM_CREATE_TEST=two";CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_ERR_INVALID_ARG && !n);
  s.envc=1;s.env[0]="TERM=wrong";CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_ERR_INVALID_ARG);
  s.env[0]="=wrong";CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_ERR_INVALID_ARG);
  s=spec(cwd,1);s.argv[3]="";CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_OK && gt__create_spec_decode(bytes,n,&out,text,sizeof(text))==GT_OK && !out.argv[3][0]);
  const char *invalid[]={"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe4\xb8"};
  for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++){s.argv[3]=invalid[i];CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_ERR_INVALID_ARG);}
  s=spec(cwd,1);s.px_width=65536;gt_pty_opts_t opts;const char *args[33],*env[65];CHECK(gt__create_spec_pty(&s,&opts,args,env)==GT_ERR_UNSUPPORTED);
}
static void invalid(const char *cwd) {
  fixture f;open_fixture(&f,1,1,100);gt_creation_t *out=(gt_creation_t *)1;gt_creation_config_t c=f.config;
  c.capacity=0;CHECK(gt__creation_open(&c,0,NULL,&out)==GT_ERR_INVALID_ARG && !out);c=f.config;c.retention_ms=86400001;CHECK(gt__creation_open(&c,0,NULL,&out)==GT_ERR_INVALID_ARG);
  c=f.config;c.bind_id=NULL;CHECK(gt__creation_open(&c,0,NULL,&out)==GT_ERR_INVALID_ARG && !out);
  CHECK(gt__creation_matches(f.creation,f.manager,f.instance));uint8_t other[16]={2};CHECK(!gt__creation_matches(f.creation,f.manager,other));
  gt_create_spec_t s=spec(cwd,1);uint8_t bytes[GT_CREATE_MAX_BYTES];size_t n=encode(&s,bytes);bytes[n-1]=0;
  gt_creation_result_t r=gt__creation_submit(f.creation,1,bytes,n,0);CHECK(r.status==GT_CREATION_INVALID && r.detail==GT_ERR_PROTOCOL && !f.prepared);
  CHECK(gt__creation_query(f.creation,1,s.nonce,0).status==GT_CREATION_UNKNOWN);close_fixture(&f);
}
int main(int argc,char **argv) {
  CHECK(argc==3);
  if(!strcmp(argv[1],"lifecycle"))lifecycle(argv[2]);else if(!strcmp(argv[1],"replay"))replay(argv[2]);
  else if(!strcmp(argv[1],"expiry"))expiry(argv[2]);else if(!strcmp(argv[1],"failures"))failures(argv[2]);
  else if(!strcmp(argv[1],"asynchronous"))asynchronous(argv[2]);else if(!strcmp(argv[1],"clock"))clock_test(argv[2]);
  else if(!strcmp(argv[1],"allocation"))allocation_test(argv[2]);else if(!strcmp(argv[1],"codec"))codec(argv[2]);else if(!strcmp(argv[1],"invalid"))invalid(argv[2]);
  else if(!strcmp(argv[1],"launch-failures"))launch_failures(argv[2]);else if(!strcmp(argv[1],"limits"))limits();else if(!strcmp(argv[1],"unsupported"))unsupported(argv[2]);else if(!strcmp(argv[1],"registration"))registration(argv[2]);else CHECK(false);
  printf("PASS shared creation %s\n",argv[1]);return 0;
}
