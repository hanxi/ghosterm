#include "creation.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  bool active;uint64_t principal,issued;size_t length;
  gt_creation_result_t result;uint8_t payload[GT_CREATE_MAX_BYTES];
} record;
struct gt_creation {
  gt_allocator_t allocator;gt_creation_config_t config;uint64_t now;bool failed;
  gt_create_spec_t scratch;char text[GT_CREATE_MAX_BYTES];record records[];
};
typedef struct {
  gt_allocator_t allocator;gt_worker_config_t original;
  void *factory_context;void (*dispose)(void *,void *);
  uint64_t principal;gt_status_t binding_status;
  gt_status_t (*bind_id)(void *,void *,uint64_t,uint64_t);
  gt_create_spec_t spec;char text[GT_CREATE_MAX_BYTES];
} launch;
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static gt_creation_result_t result(gt_creation_status_t status,gt_status_t detail){return (gt_creation_result_t){status,detail,0};}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t sink,void **out){launch *j=ctx;return j->original.open_state(j->original.context,sink,out);}
static gt_io_backend_t *open_backend(void *ctx){launch *j=ctx;return j->original.open_backend(j->original.context);}
static void output(void *ctx,const uint8_t *bytes,size_t n,uint64_t seq){launch *j=ctx;j->original.output(j->original.context,bytes,n,seq);}
static void event(void *ctx,const gt_state_event_t *e,uint64_t seq){launch *j=ctx;j->original.event(j->original.context,e,seq);}
static void process_observed(void *ctx,const gt_session_process_t *value){launch *j=ctx;j->original.process(j->original.context,value);}
static void finished(void *ctx,gt_status_t status){launch *j=ctx;j->original.finished(j->original.context,status);}
static gt_status_t authorize(void *ctx,gt_worker_command_t kind,const gt_worker_stamp_t *stamp){launch *j=ctx;return j->original.authorize(j->original.context,kind,stamp);}
static void final_capture(void *ctx,gt_status_t status,gt_state_capture_t capture){launch *j=ctx;j->original.final_capture(j->original.context,status,capture);}
static gt_status_t register_launch(void *ctx,uint64_t id) {
  launch *j=ctx;j->binding_status=j->bind_id(j->factory_context,j->original.context,j->principal,id);return j->binding_status;
}
static void dispose_launch(void *ctx) {
  launch *j=ctx;j->dispose(j->factory_context,j->original.context);
  gt_allocator_t a=j->allocator;gt__secure_erase(j,sizeof(*j));a.release(a.context,j,sizeof(*j));
}
gt_status_t gt__creation_open(const gt_creation_config_t *config,uint64_t now,const gt_allocator_t *allocator,gt_creation_t **out) {
  if(out)*out=NULL;
  if(!out || !config || !config->manager || !config->prepare || !config->bind_id || !config->dispose || !nonzero(config->instance,16) ||
     !config->capacity || config->capacity>64 || !config->retention_ms || config->retention_ms>86400000 ||
     ((!config->worker_allocator.allocate)!=(!config->worker_allocator.release)) ||
     (config->worker_allocator.context && !config->worker_allocator.allocate) ||
     !gt__thread_options_valid(config->worker_threads.stack_size || config->worker_threads.guard_size?&config->worker_threads:NULL))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  size_t bytes=sizeof(gt_creation_t)+config->capacity*sizeof(record);gt_creation_t *c=a.allocate(a.context,bytes);
  if(!c)return GT_ERR_INTERNAL;
  memset(c,0,bytes);c->allocator=a;c->config=*config;c->now=now;*out=c;return GT_OK;
}
bool gt__creation_matches(const gt_creation_t *c,const gt_session_manager_t *manager,const uint8_t instance[16]) {
  return c && manager && instance && c->config.manager==manager && !memcmp(c->config.instance,instance,16);
}
gt_status_t gt__creation_advance(gt_creation_t *c,uint64_t now) {
  if(!c)return GT_ERR_INVALID_ARG;
  if(c->failed)return GT_ERR_IO;
  if(now<c->now){c->failed=true;gt__secure_erase(c->records,c->config.capacity*sizeof(record));return GT_ERR_IO;}
  c->now=now;
  for(size_t i=0;i<c->config.capacity;i++)if(c->records[i].active && now-c->records[i].issued>=c->config.retention_ms)
    gt__secure_erase(&c->records[i],sizeof(record));
  return GT_OK;
}
static record *find(gt_creation_t *c,uint64_t principal,const uint8_t nonce[16]) {
  for(size_t i=0;i<c->config.capacity;i++)if(c->records[i].active && c->records[i].principal==principal && !memcmp(c->records[i].payload,nonce,16))return &c->records[i];
  return NULL;
}
gt_creation_result_t gt__creation_query(gt_creation_t *c,uint64_t principal,const uint8_t nonce[16],uint64_t now) {
  if(!c || !principal || !nonce || !nonzero(nonce,16))return result(GT_CREATION_INVALID,GT_ERR_INVALID_ARG);
  if(gt__creation_advance(c,now)!=GT_OK)return result(GT_CREATION_CLOCK,GT_ERR_IO);
  record *r=find(c,principal,nonce);return r?r->result:result(GT_CREATION_UNKNOWN,GT_OK);
}
static gt_creation_result_t start(gt_creation_t *c,uint64_t principal,const void *bytes,size_t n) {
  launch *j=c->allocator.allocate(c->allocator.context,sizeof(*j));if(!j)return result(GT_CREATION_NO_MEMORY,GT_ERR_INTERNAL);
  memset(j,0,sizeof(*j));j->allocator=c->allocator;j->factory_context=c->config.factory_context;j->dispose=c->config.dispose;
  j->principal=principal;j->bind_id=c->config.bind_id;
  if(gt__create_spec_decode(bytes,n,&j->spec,j->text,sizeof(j->text))!=GT_OK) {
    gt_allocator_t a=j->allocator;gt__secure_erase(j,sizeof(*j));a.release(a.context,j,sizeof(*j));return result(GT_CREATION_INTERNAL,GT_ERR_PROTOCOL);
  }
  gt_status_t rc=c->config.prepare(c->config.factory_context,principal,&j->spec,&j->original);
  if(rc!=GT_OK) {
    dispose_launch(j);return result(rc==GT_ERR_UNSUPPORTED?GT_CREATION_UNSUPPORTED:GT_CREATION_PREPARE_FAILED,rc);
  }
  gt_worker_config_t wrapped=j->original;wrapped.context=j;wrapped.open_backend=open_backend;wrapped.open_state=open_state;
  if(!j->original.open_backend || !j->original.open_state || !j->original.close_state || !j->original.state_ops.apply || !j->original.state_ops.freeze) {
    dispose_launch(j);return result(GT_CREATION_INTERNAL,GT_ERR_INVALID_ARG);
  }
  wrapped.output=j->original.output?output:NULL;wrapped.event=j->original.event?event:NULL;wrapped.finished=j->original.finished?finished:NULL;
  wrapped.authorize=j->original.authorize?authorize:NULL;
  wrapped.final_capture=j->original.final_capture?final_capture:NULL;
  wrapped.process=j->original.process?process_observed:NULL;
  uint64_t id=0;gt_manager_rc_t status=gt__manager_create(c->config.manager,&wrapped,
    c->config.worker_allocator.allocate?&c->config.worker_allocator:NULL,
    c->config.worker_threads.stack_size?&c->config.worker_threads:NULL,dispose_launch,register_launch,&id);
  if(status==GT_MANAGER_OK)return (gt_creation_result_t){GT_CREATION_ACCEPTED,GT_OK,id};
  if(status==GT_MANAGER_REGISTER_FAILED){gt_status_t reason=j->binding_status;dispose_launch(j);return result(GT_CREATION_PREPARE_FAILED,reason);}
  dispose_launch(j);
  return result(status==GT_MANAGER_FULL?GT_CREATION_BUSY:status==GT_MANAGER_START_FAILED?GT_CREATION_START_FAILED:GT_CREATION_INTERNAL,
    status==GT_MANAGER_FULL?GT_OK:GT_ERR_INTERNAL);
}
gt_creation_result_t gt__creation_submit(gt_creation_t *c,uint64_t principal,const void *bytes,size_t n,uint64_t now) {
  if(!c || !principal)return result(GT_CREATION_INVALID,GT_ERR_INVALID_ARG);
  if(gt__creation_advance(c,now)!=GT_OK)return result(GT_CREATION_CLOCK,GT_ERR_IO);
  gt_status_t decoded=gt__create_spec_decode(bytes,n,&c->scratch,c->text,sizeof(c->text));
  gt__secure_erase(&c->scratch,sizeof(c->scratch));gt__secure_erase(c->text,sizeof(c->text));
  if(decoded!=GT_OK && decoded!=GT_ERR_UNSUPPORTED)return result(GT_CREATION_INVALID,GT_ERR_PROTOCOL);
  record *r=find(c,principal,bytes);
  if(r)return r->length==n && !memcmp(r->payload,bytes,n)?r->result:result(GT_CREATION_CONFLICT,GT_OK);
  for(size_t i=0;i<c->config.capacity;i++)if(!c->records[i].active){r=&c->records[i];break;}
  if(!r)return result(GT_CREATION_BUSY,GT_OK);
  r->active=true;r->principal=principal;r->issued=now;r->length=n;memcpy(r->payload,bytes,n);
  r->result=decoded==GT_ERR_UNSUPPORTED?result(GT_CREATION_UNSUPPORTED,GT_ERR_UNSUPPORTED):start(c,principal,bytes,n);
  return r->result;
}
void gt__creation_close(gt_creation_t *c) {
  if(!c)return;
  gt_allocator_t a=c->allocator;size_t bytes=sizeof(*c)+c->config.capacity*sizeof(record);
  gt__secure_erase(c,bytes);a.release(a.context,c,bytes);
}
