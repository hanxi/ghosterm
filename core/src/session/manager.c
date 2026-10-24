#include "manager.h"
#include <stdlib.h>
#include <string.h>
typedef enum {EMPTY,CREATING,WORKER,REAPING,RETAINED} entry_phase;
typedef struct {
  entry_phase phase;uint64_t id;bool termination_requested;
  gt_session_worker_t *worker;
  void *context;void (*dispose)(void *);
  gt_worker_result_t result;
} entry;
struct gt_session_manager {
  pthread_mutex_t lock;gt_allocator_t allocator;
  size_t capacity;uint64_t next_id;bool closing;
  entry entries[];
};
static void *allocate(void *c,size_t n){(void)c;return malloc(n);}
static void release(void *c,void *p,size_t n){(void)c;(void)n;free(p);}
static entry *find(gt_session_manager_t *m,uint64_t id) {
  for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase!=EMPTY && m->entries[i].id==id)return &m->entries[i];
  return NULL;
}
static void info(entry *e,gt_managed_info_t *out) {
  *out=(gt_managed_info_t){.id=e->id,.termination_requested=e->termination_requested,
    .phase=e->phase==RETAINED?GT_MANAGED_RETAINED:e->phase==REAPING?GT_MANAGED_REAPING:GT_MANAGED_WORKER};
  if(e->phase==RETAINED || e->phase==REAPING){out->has_result=true;out->result=e->result;}
  else out->has_result=gt__worker_result(e->worker,&out->result)==GT_WORKER_RESULT_READY;
}
gt_session_manager_t *gt__manager_open(size_t capacity,const gt_allocator_t *allocator) {
  if(!capacity || capacity>64)return NULL;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return NULL;
  size_t bytes=sizeof(gt_session_manager_t)+capacity*sizeof(entry);
  gt_session_manager_t *m=a.allocate(a.context,bytes);if(!m)return NULL;
  memset(m,0,bytes);m->allocator=a;m->capacity=capacity;m->next_id=1;
  if(pthread_mutex_init(&m->lock,NULL)){a.release(a.context,m,bytes);return NULL;}
  return m;
}
gt_manager_rc_t gt__manager_create(gt_session_manager_t *m,const gt_worker_config_t *config,
  const gt_allocator_t *allocator,const gt_thread_options_t *options,void (*dispose)(void *),
  gt_status_t (*register_id)(void *,uint64_t),uint64_t *id) {
  if(id)*id=0;
  if(!m || !config || !dispose || !id || !config->open_state || !config->open_backend ||
     !config->close_state || !config->state_ops.apply || !config->state_ops.freeze ||
     !gt__thread_options_valid(options) || (allocator && (!allocator->allocate || !allocator->release)))return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);
  if(m->closing){pthread_mutex_unlock(&m->lock);return GT_MANAGER_CLOSED;}
  if(!m->next_id){pthread_mutex_unlock(&m->lock);return GT_MANAGER_IDS_EXHAUSTED;}
  entry *e=NULL;for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase==EMPTY){e=&m->entries[i];break;}
  if(!e){pthread_mutex_unlock(&m->lock);return GT_MANAGER_FULL;}
  *e=(entry){.phase=CREATING,.id=m->next_id};
  m->next_id=m->next_id==UINT64_MAX?0:m->next_id+1;
  pthread_mutex_unlock(&m->lock);
  if(register_id && register_id(config->context,e->id)!=GT_OK) {
    pthread_mutex_lock(&m->lock);memset(e,0,sizeof(*e));pthread_mutex_unlock(&m->lock);return GT_MANAGER_REGISTER_FAILED;
  }
  gt_session_worker_t *worker=gt__worker_start_with_options(config,allocator,options);
  pthread_mutex_lock(&m->lock);
  if(!worker){memset(e,0,sizeof(*e));pthread_mutex_unlock(&m->lock);return GT_MANAGER_START_FAILED;}
  e->worker=worker;e->context=config->context;e->dispose=dispose;e->phase=WORKER;*id=e->id;
  pthread_mutex_unlock(&m->lock);return GT_MANAGER_OK;
}
gt_manager_rc_t gt__manager_info(gt_session_manager_t *m,uint64_t id,gt_managed_info_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!m || !id || !out)return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);entry *e=find(m,id);
  gt_manager_rc_t rc=m->closing?GT_MANAGER_CLOSED:!e?GT_MANAGER_NOT_FOUND:e->phase==CREATING?GT_MANAGER_BUSY:GT_MANAGER_OK;
  if(rc==GT_MANAGER_OK)info(e,out);
  pthread_mutex_unlock(&m->lock);return rc;
}
gt_manager_rc_t gt__manager_list(gt_session_manager_t *m,gt_managed_info_t *rows,size_t capacity,size_t *count) {
  if(count)*count=0;
  if(!m || !count || (capacity && !rows))return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);
  if(m->closing){pthread_mutex_unlock(&m->lock);return GT_MANAGER_CLOSED;}
  size_t n=0;for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase!=EMPTY && m->entries[i].phase!=CREATING)n++;
  *count=n;if(n>capacity){pthread_mutex_unlock(&m->lock);return GT_MANAGER_FULL;}
  n=0;for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase!=EMPTY && m->entries[i].phase!=CREATING)info(&m->entries[i],&rows[n++]);
  pthread_mutex_unlock(&m->lock);return GT_MANAGER_OK;
}
static gt_manager_rc_t submit(gt_session_manager_t *m,uint64_t id,gt_worker_command_t kind,
  const void *bytes,size_t n,gt_worker_completion_t callback,void *context,gt_worker_submit_t *result,const gt_worker_stamp_t *stamp,gt_worker_user_completion_t user_callback) {
  if(result)*result=GT_WORKER_INVALID;
  if(!m || !id || !result)return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);entry *e=find(m,id);
  gt_manager_rc_t rc=m->closing?GT_MANAGER_CLOSED:!e?GT_MANAGER_NOT_FOUND:e->phase==CREATING || e->phase==REAPING?GT_MANAGER_BUSY:GT_MANAGER_OK;
  if(rc==GT_MANAGER_OK) {
    gt_managed_info_t state;info(e,&state);
    *result=e->phase!=WORKER || e->termination_requested || state.has_result?GT_WORKER_STOPPED:
      stamp?gt__worker_submit_user(e->worker,kind,stamp,bytes,n,user_callback,context):
        gt__worker_submit_internal(e->worker,kind,bytes,n,callback,context);
  }
  pthread_mutex_unlock(&m->lock);return rc;
}
gt_manager_rc_t gt__manager_submit_internal(gt_session_manager_t *m,uint64_t id,gt_worker_command_t kind,
  const void *bytes,size_t n,gt_worker_completion_t callback,void *context,gt_worker_submit_t *result) {
  return submit(m,id,kind,bytes,n,callback,context,result,NULL,NULL);
}
gt_manager_rc_t gt__manager_submit_user(gt_session_manager_t *m,uint64_t id,gt_worker_command_t kind,
  const gt_worker_stamp_t *stamp,const void *bytes,size_t n,gt_worker_user_completion_t callback,void *context,gt_worker_submit_t *result) {
  if(!stamp){if(result)*result=GT_WORKER_INVALID;return GT_MANAGER_INVALID;}
  return submit(m,id,kind,bytes,n,NULL,context,result,stamp,callback);
}
gt_manager_rc_t gt__manager_terminate(gt_session_manager_t *m,uint64_t id) {
  if(!m || !id)return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);entry *e=find(m,id);
  gt_manager_rc_t rc=m->closing?GT_MANAGER_CLOSED:!e?GT_MANAGER_NOT_FOUND:e->phase==CREATING?GT_MANAGER_BUSY:GT_MANAGER_OK;
  if(rc==GT_MANAGER_OK && e->phase==WORKER){e->termination_requested=true;gt__worker_stop(e->worker);}
  pthread_mutex_unlock(&m->lock);return rc;
}
gt_manager_rc_t gt__manager_reap(gt_session_manager_t *m,uint64_t id) {
  if(!m || !id)return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);entry *e=find(m,id);
  gt_manager_rc_t rc=m->closing?GT_MANAGER_CLOSED:!e?GT_MANAGER_NOT_FOUND:e->phase==CREATING || e->phase==REAPING?GT_MANAGER_BUSY:GT_MANAGER_OK;
  if(rc!=GT_MANAGER_OK || e->phase==RETAINED){pthread_mutex_unlock(&m->lock);return rc;}
  if(gt__worker_result(e->worker,&e->result)!=GT_WORKER_RESULT_READY){pthread_mutex_unlock(&m->lock);return GT_MANAGER_BUSY;}
  e->phase=REAPING;gt_session_worker_t *worker=e->worker;void *context=e->context;void (*dispose)(void *)=e->dispose;
  pthread_mutex_unlock(&m->lock);
  gt__worker_join(worker);dispose(context);
  pthread_mutex_lock(&m->lock);e->worker=NULL;e->context=NULL;e->dispose=NULL;e->phase=RETAINED;pthread_mutex_unlock(&m->lock);
  return GT_MANAGER_OK;
}
gt_manager_rc_t gt__manager_forget(gt_session_manager_t *m,uint64_t id) {
  if(!m || !id)return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);entry *e=find(m,id);
  gt_manager_rc_t rc=m->closing?GT_MANAGER_CLOSED:!e?GT_MANAGER_NOT_FOUND:e->phase!=RETAINED?GT_MANAGER_BUSY:GT_MANAGER_OK;
  if(rc==GT_MANAGER_OK)memset(e,0,sizeof(*e));
  pthread_mutex_unlock(&m->lock);return rc;
}
gt_manager_rc_t gt__manager_close(gt_session_manager_t *m) {
  if(!m)return GT_MANAGER_INVALID;
  pthread_mutex_lock(&m->lock);
  if(m->closing){pthread_mutex_unlock(&m->lock);return GT_MANAGER_BUSY;}
  for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase==CREATING || m->entries[i].phase==REAPING){pthread_mutex_unlock(&m->lock);return GT_MANAGER_BUSY;}
  m->closing=true;
  for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase==WORKER)gt__worker_stop(m->entries[i].worker);
  pthread_mutex_unlock(&m->lock);
  for(size_t i=0;i<m->capacity;i++)if(m->entries[i].phase==WORKER){gt__worker_join(m->entries[i].worker);m->entries[i].dispose(m->entries[i].context);}
  gt_allocator_t a=m->allocator;size_t bytes=sizeof(*m)+m->capacity*sizeof(entry);
  pthread_mutex_destroy(&m->lock);a.release(a.context,m,bytes);return GT_MANAGER_OK;
}
