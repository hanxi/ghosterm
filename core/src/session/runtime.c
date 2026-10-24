#include "runtime.h"
#include "io/pty-size.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
struct gt_runtime_session {
  gt_session_runtime_t *runtime;
  uint64_t id,principal;size_t references;
  bool started,joined,has_process;gt_session_process_t process;
  const gt_create_spec_t *spec;
  gt_pty_opts_t options;const char *args[GT_CREATE_MAX_ARGS+1],*env[GT_CREATE_MAX_ENV+1];
  gt_session_attachment_t *attachment;
  bool final_ready;gt_status_t final_status;gt_state_capture_t final;
};
struct gt_session_runtime {
  pthread_mutex_t lock;
  gt_allocator_t allocator;gt_runtime_config_t config;
  gt_session_manager_t *manager;gt_creation_t *creation;bool closing;
  gt_runtime_session_t *sessions[];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool explicit_threads(gt_thread_options_t o){return o.stack_size || o.guard_size;}
static bool instance_valid(const uint8_t instance[16]){uint8_t bits=0;for(unsigned i=0;i<16;i++)bits|=instance[i];return bits!=0;}
static gt_runtime_session_t *find(gt_session_runtime_t *r,uint64_t id) {
  for(size_t i=0;i<r->config.sessions;i++)if(r->sessions[i] && r->sessions[i]->id==id)return r->sessions[i];
  return NULL;
}
static void remove_session(gt_session_runtime_t *r,gt_runtime_session_t *c) {
  for(size_t i=0;i<r->config.sessions;i++)if(r->sessions[i]==c){r->sessions[i]=NULL;break;}
}
static void free_session(gt_runtime_session_t *c) {
  gt_allocator_t a=c->runtime->allocator;
  if(c->final.frozen)c->runtime->config.state.release_frozen(c->runtime->config.state.context,c->final.frozen);
  gt__secure_erase(c,sizeof(*c));a.release(a.context,c,sizeof(*c));
}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t sink,void **out) {
  gt_runtime_session_t *c=ctx;gt_session_runtime_t *r=c->runtime;
  pthread_mutex_lock(&r->lock);c->started=true;pthread_mutex_unlock(&r->lock);
  return r->config.state.open(r->config.state.context,c->principal,c->id,c->spec,sink,out);
}
static gt_io_backend_t *open_backend(void *ctx) {
  gt_runtime_session_t *c=ctx;gt_session_runtime_t *r=c->runtime;
  return gt__pty_open_with_options(&c->options,&r->allocator,
    explicit_threads(r->config.backend_threads)?&r->config.backend_threads:NULL);
}
static void authority_event(void *ctx,const gt_state_event_t *event,uint64_t sequence) {
  gt_runtime_session_t *c=ctx;
  /* Backpressure/failure revokes only this attachment; authority keeps running. */
  (void)gt__attachment_push(c->attachment,sequence,event->kind,event->payload,event->length);
}
static void process_observed(void *ctx,const gt_session_process_t *value) {
  gt_runtime_session_t *c=ctx;gt_session_runtime_t *r=c->runtime;
  pthread_mutex_lock(&r->lock);c->process=*value;c->has_process=true;pthread_mutex_unlock(&r->lock);
}
static gt_status_t authorize(void *ctx,gt_worker_command_t kind,const gt_worker_stamp_t *stamp) {
  (void)kind;gt_runtime_session_t *c=ctx;
  return gt__attachment_authorize(c->attachment,stamp->generation,stamp->command_id,stamp->applied_sequence);
}
static void final_capture(void *ctx,gt_status_t status,gt_state_capture_t capture) {
  gt_runtime_session_t *c=ctx;gt_session_runtime_t *r=c->runtime;
  pthread_mutex_lock(&r->lock);c->final_status=status;c->final=capture;c->final_ready=true;pthread_mutex_unlock(&r->lock);
}
static gt_status_t prepare(void *ctx,uint64_t principal,const gt_create_spec_t *spec,gt_worker_config_t *out) {
  gt_session_runtime_t *r=ctx;
  if(r->closing)return GT_ERR_CANCELLED;
  gt_runtime_session_t *c=r->allocator.allocate(r->allocator.context,sizeof(*c));
  if(!c)return GT_ERR_INTERNAL;
  memset(c,0,sizeof(*c));c->runtime=r;c->principal=principal;c->spec=spec;out->context=c;
  gt_status_t rc=gt__create_spec_pty(spec,&c->options,c->args,c->env);if(rc!=GT_OK)return rc;
  c->attachment=gt__attachment_open_with_allocator(r->config.tasks,r->config.queue_bytes,r->config.queue_events,&r->allocator);
  if(!c->attachment)return GT_ERR_INTERNAL;
  *out=(gt_worker_config_t){.context=c,.open_state=open_state,.open_backend=open_backend,
    .state_ops=r->config.state.ops,.close_state=r->config.state.close,
    .state_invalidated=r->config.state.invalidated,.color_scheme_supported=r->config.state.color_scheme_supported,
    .style_supported=r->config.state.style_supported,
    .event=authority_event,.authorize=authorize,.final_capture=final_capture,.process=process_observed};
  return GT_OK;
}
static gt_status_t bind_id(void *ctx,void *worker,uint64_t principal,uint64_t id) {
  gt_session_runtime_t *r=ctx;gt_runtime_session_t *c=worker;
  if(!c || c->runtime!=r || c->principal!=principal || !id)return GT_ERR_INVALID_ARG;
  pthread_mutex_lock(&r->lock);gt_status_t rc=GT_ERR_INTERNAL;
  if(!r->closing && !c->id && !find(r,id))for(size_t i=0;i<r->config.sessions;i++)if(!r->sessions[i]) {
    c->id=id;r->sessions[i]=c;rc=GT_OK;break;
  }
  pthread_mutex_unlock(&r->lock);return rc;
}
static void dispose(void *ctx,void *worker) {
  gt_session_runtime_t *r=ctx;gt_runtime_session_t *c=worker;if(!c)return;
  pthread_mutex_lock(&r->lock);
  bool retain=c->started;
  if(retain) {
    c->joined=true;c->spec=NULL;memset(&c->options,0,sizeof(c->options));
    memset(c->args,0,sizeof(c->args));memset(c->env,0,sizeof(c->env));
  } else remove_session(r,c);
  pthread_mutex_unlock(&r->lock);
  if(!retain) {
    if(c->attachment)(void)gt__attachment_close(c->attachment);
    free_session(c);
  }
}
gt_status_t gt__runtime_open(const gt_runtime_config_t *config,uint64_t now,const gt_allocator_t *allocator,gt_session_runtime_t **out) {
  if(out)*out=NULL;
  if(!out || !config || !config->sessions || config->sessions>64 || !config->tasks || !instance_valid(config->instance) ||
     !config->creation_records || config->creation_records>64 || !config->creation_retention_ms || config->creation_retention_ms>86400000 ||
     !config->queue_bytes || !config->queue_events || config->queue_events>4096 || config->queue_bytes>16*1024*1024 ||
     !config->state.open || !config->state.ops.apply || !config->state.ops.freeze || !config->state.close || !config->state.release_frozen ||
     !gt__thread_options_valid(explicit_threads(config->worker_threads)?&config->worker_threads:NULL) ||
     !gt__thread_options_valid(explicit_threads(config->backend_threads)?&config->backend_threads:NULL))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  size_t bytes=sizeof(gt_session_runtime_t)+config->sessions*sizeof(gt_runtime_session_t *);
  gt_session_runtime_t *r=a.allocate(a.context,bytes);if(!r)return GT_ERR_INTERNAL;
  memset(r,0,bytes);r->allocator=a;r->config=*config;
  if(pthread_mutex_init(&r->lock,NULL)){gt__secure_erase(r,bytes);a.release(a.context,r,bytes);return GT_ERR_INTERNAL;}
  r->manager=gt__manager_open(config->sessions,&a);
  gt_creation_config_t creation={.manager=r->manager,.capacity=config->creation_records,.retention_ms=config->creation_retention_ms,
    .factory_context=r,.prepare=prepare,.bind_id=bind_id,.dispose=dispose,.worker_allocator=a,.worker_threads=config->worker_threads};
  memcpy(creation.instance,config->instance,16);
  gt_status_t rc=r->manager?gt__creation_open(&creation,now,&a,&r->creation):GT_ERR_INTERNAL;
  if(rc!=GT_OK) {
    if(r->manager)(void)gt__manager_close(r->manager);
    pthread_mutex_destroy(&r->lock);gt__secure_erase(r,bytes);a.release(a.context,r,bytes);return rc;
  }
  *out=r;return GT_OK;
}
gt_session_manager_t *gt__runtime_manager(gt_session_runtime_t *r){return r?r->manager:NULL;}
gt_creation_t *gt__runtime_creation(gt_session_runtime_t *r){return r?r->creation:NULL;}
gt_runtime_rc_t gt__runtime_acquire(gt_session_runtime_t *r,uint64_t principal,uint64_t id,gt_runtime_session_t **out) {
  if(out)*out=NULL;
  if(!r || !principal || !id || !out)return GT_RUNTIME_INVALID;
  pthread_mutex_lock(&r->lock);gt_runtime_session_t *c=find(r,id);
  gt_runtime_rc_t rc=r->closing?GT_RUNTIME_CLOSED:!c || c->principal!=principal?GT_RUNTIME_NOT_FOUND:GT_RUNTIME_OK;
  if(rc==GT_RUNTIME_OK) {
    gt_managed_info_t info;gt_manager_rc_t status=gt__manager_info(r->manager,id,&info);
    if(status!=GT_MANAGER_OK)rc=status==GT_MANAGER_BUSY?GT_RUNTIME_BUSY:GT_RUNTIME_NOT_FOUND;
    else if(c->references==256)rc=GT_RUNTIME_BUSY;
    else {c->references++;*out=c;}
  }
  pthread_mutex_unlock(&r->lock);return rc;
}
void gt__runtime_release(gt_runtime_session_t **lease) {
  if(!lease || !*lease)return;
  gt_runtime_session_t *c=*lease;gt_session_runtime_t *r=c->runtime;
  pthread_mutex_lock(&r->lock);c->references--;*lease=NULL;pthread_mutex_unlock(&r->lock);
}
bool gt__runtime_process(gt_runtime_session_t *c,gt_session_process_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!c || !out)return false;
  gt_session_runtime_t *r=c->runtime;pthread_mutex_lock(&r->lock);
  bool ready=c->has_process;if(ready)*out=c->process;
  pthread_mutex_unlock(&r->lock);return ready;
}
gt_session_attachment_t *gt__runtime_attachment(gt_runtime_session_t *c){return c?c->attachment:NULL;}
gt_runtime_rc_t gt__runtime_final(gt_runtime_session_t *c,gt_runtime_final_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!c || !out)return GT_RUNTIME_INVALID;
  gt_managed_info_t info;gt_manager_rc_t rc=gt__manager_info(c->runtime->manager,c->id,&info);
  if(rc!=GT_MANAGER_OK)return rc==GT_MANAGER_BUSY?GT_RUNTIME_BUSY:GT_RUNTIME_INTERNAL;
  if(!info.has_result)return GT_RUNTIME_BUSY;
  pthread_mutex_lock(&c->runtime->lock);
  if(!c->final_ready){pthread_mutex_unlock(&c->runtime->lock);return GT_RUNTIME_INTERNAL;}
  *out=(gt_runtime_final_t){.result=info.result,.capture_status=c->final_status,.frozen=c->final.frozen,.sequence=c->final.sequence};
  pthread_mutex_unlock(&c->runtime->lock);return GT_RUNTIME_OK;
}
gt_manager_rc_t gt__runtime_capture(gt_runtime_session_t *c,gt_worker_completion_t done,void *ctx,gt_worker_submit_t *result) {
  if(!c){if(result)*result=GT_WORKER_INVALID;return GT_MANAGER_INVALID;}
  return gt__manager_submit_internal(c->runtime->manager,c->id,GT_WORKER_CAPTURE,NULL,0,done,ctx,result);
}
void gt__runtime_release_capture(gt_runtime_session_t *c,gt_state_capture_t *capture) {
  if(!c || !capture)return;
  if(capture->frozen)c->runtime->config.state.release_frozen(c->runtime->config.state.context,capture->frozen);
  memset(capture,0,sizeof(*capture));
}
gt_manager_rc_t gt__runtime_submit_user(gt_runtime_session_t *c,gt_worker_command_t kind,const gt_worker_stamp_t *stamp,
  const void *bytes,size_t n,gt_worker_user_completion_t callback,void *context,gt_worker_submit_t *result) {
  if(!c || !result){if(result)*result=GT_WORKER_INVALID;return GT_MANAGER_INVALID;}
  if(kind==GT_WORKER_RESIZE && stamp && stamp->generation && stamp->command_id && callback && bytes && n==sizeof(gt_state_size_t)) {
    gt_state_size_t size;memcpy(&size,bytes,sizeof(size));
    gt_status_t rc=gt__pty_size_check(size.cols,size.rows,size.px_width,size.px_height,false);
    if(rc!=GT_OK){*result=rc==GT_ERR_UNSUPPORTED?GT_WORKER_UNSUPPORTED:GT_WORKER_INVALID;return GT_MANAGER_OK;}
  }
  return gt__manager_submit_user(c->runtime->manager,c->id,kind,stamp,bytes,n,callback,context,result);
}
gt_manager_rc_t gt__runtime_reap(gt_session_runtime_t *r,uint64_t id) {
  return !r?GT_MANAGER_INVALID:r->closing?GT_MANAGER_CLOSED:gt__manager_reap(r->manager,id);
}
gt_runtime_rc_t gt__runtime_forget(gt_session_runtime_t *r,uint64_t id) {
  if(!r || !id)return GT_RUNTIME_INVALID;
  pthread_mutex_lock(&r->lock);gt_runtime_session_t *c=find(r,id);
  gt_runtime_rc_t rc=r->closing?GT_RUNTIME_CLOSED:!c?GT_RUNTIME_NOT_FOUND:GT_RUNTIME_BUSY;
  if(c && !r->closing && c->joined && !c->references) {
    gt_managed_info_t info;
    if(gt__manager_info(r->manager,id,&info)==GT_MANAGER_OK && info.phase==GT_MANAGED_RETAINED &&
       gt__attachment_close(c->attachment)==GT_TASK_OK) {
      c->attachment=NULL;
      if(gt__manager_forget(r->manager,id)==GT_MANAGER_OK){remove_session(r,c);rc=GT_RUNTIME_OK;}
      else rc=GT_RUNTIME_INTERNAL;
    }
  }
  pthread_mutex_unlock(&r->lock);if(rc==GT_RUNTIME_OK)free_session(c);return rc;
}
gt_runtime_rc_t gt__runtime_close(gt_session_runtime_t *r) {
  if(!r)return GT_RUNTIME_INVALID;
  pthread_mutex_lock(&r->lock);
  if(r->closing){pthread_mutex_unlock(&r->lock);return GT_RUNTIME_CLOSED;}
  for(size_t i=0;i<r->config.sessions;i++)if(r->sessions[i] &&
    (r->sessions[i]->references || !gt__attachment_quiescent(r->sessions[i]->attachment))) {
    pthread_mutex_unlock(&r->lock);return GT_RUNTIME_BUSY;
  }
  r->closing=true;pthread_mutex_unlock(&r->lock);
  gt_manager_rc_t stopped=gt__manager_close(r->manager);
  if(stopped!=GT_MANAGER_OK) {
    pthread_mutex_lock(&r->lock);r->closing=false;pthread_mutex_unlock(&r->lock);return GT_RUNTIME_BUSY;
  }
  gt__creation_close(r->creation);
  for(size_t i=0;i<r->config.sessions;i++)if(r->sessions[i]) {
    (void)gt__attachment_close(r->sessions[i]->attachment);free_session(r->sessions[i]);
  }
  gt_allocator_t a=r->allocator;size_t bytes=sizeof(*r)+r->config.sessions*sizeof(gt_runtime_session_t *);
  pthread_mutex_destroy(&r->lock);gt__secure_erase(r,bytes);a.release(a.context,r,bytes);return GT_RUNTIME_OK;
}
