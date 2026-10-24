#include "checkpoint-task.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint64_t id;
  gt_task_spec_t spec;
  gt_task_state_t state;
  gt_status_t error;
  bool cancel, input_owned;
  void *result;
} task_t;
struct gt_checkpoint_tasks {
  gt_allocator_t allocator;
  pthread_mutex_t lock;
  pthread_cond_t changed;
  pthread_t thread;
  task_t slots[16];
  size_t capacity;
  uint64_t next_id;
  bool stopping;
};
static task_t *find(gt_checkpoint_tasks_t *pool, uint64_t id) {
  if (!id) return NULL;
  for (size_t i=0;i<pool->capacity;i++)
    if (pool->slots[i].id==id) return &pool->slots[i];
  return NULL;
}
static bool terminal(gt_task_state_t state) {
  return state==GT_TASK_FINISHED || state==GT_TASK_FAILED || state==GT_TASK_CANCELLED;
}
static void cancel_locked(task_t *t) {
  if (terminal(t->state) || t->state==GT_TASK_RELEASING) return;
  t->cancel=true;
  if (t->state!=GT_TASK_RUNNING) t->state=GT_TASK_CANCELLING;
}
static task_t *select_task(gt_checkpoint_tasks_t *pool) {
  task_t *queued=NULL;
  for (size_t i=0;i<pool->capacity;i++) {
    task_t *t=&pool->slots[i];
    if (!t->id) continue;
    if (t->state==GT_TASK_CANCELLING || t->state==GT_TASK_RELEASING) return t;
    if (t->state==GT_TASK_QUEUED && (!queued || t->id<queued->id)) queued=t;
  }
  return queued;
}
static void *run(void *context) {
  gt_checkpoint_tasks_t *pool=context;
  pthread_mutex_lock(&pool->lock);
  for (;;) {
    task_t *t=select_task(pool);
    if (!t) {
      if (pool->stopping) break;
      pthread_cond_wait(&pool->changed,&pool->lock);
      continue;
    }
    bool encode=t->state==GT_TASK_QUEUED;
    bool releasing=t->state==GT_TASK_RELEASING;
    /* RUNNING also covers cleanup, preventing forget while callbacks run. */
    t->state=GT_TASK_RUNNING;
    pthread_mutex_unlock(&pool->lock);
    void *result=t->result;
    gt_status_t error=GT_OK;
    if (encode) {
      error=t->spec.encode(t->spec.context,&result);
      if (error==GT_OK && !result) error=GT_ERR_INTERNAL;
    }
    if (t->input_owned) {
      t->spec.release_input(t->spec.context);
      t->input_owned=false;
    }
    pthread_mutex_lock(&pool->lock);
    bool discard=t->cancel || releasing || error!=GT_OK;
    if (discard) {
      pthread_mutex_unlock(&pool->lock);
      if (result) t->spec.release_result(t->spec.context,result);
      pthread_mutex_lock(&pool->lock);
      t->result=NULL;
      t->error=t->cancel && !releasing ? GT_ERR_CANCELLED : error;
      t->state=releasing ? GT_TASK_FINISHED : t->cancel ? GT_TASK_CANCELLED : GT_TASK_FAILED;
    } else {
      t->result=result;
      t->error=GT_OK;
      t->state=GT_TASK_READY;
    }
    pthread_cond_broadcast(&pool->changed);
  }
  pthread_mutex_unlock(&pool->lock);
  return NULL;
}
static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
gt_checkpoint_tasks_t *gt__tasks_open(size_t capacity) {
  return gt__tasks_open_with_allocator(capacity,NULL);
}
gt_checkpoint_tasks_t *gt__tasks_open_with_allocator(size_t capacity,const gt_allocator_t *allocator) {
  return gt__tasks_open_with_options(capacity,allocator,NULL);
}
gt_checkpoint_tasks_t *gt__tasks_open_with_options(size_t capacity,const gt_allocator_t *allocator,const gt_thread_options_t *thread_options) {
  if(!gt__thread_options_valid(thread_options))return NULL;
  if (!capacity || capacity>16) return NULL;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,default_allocate,default_release};
  if(!a.allocate || !a.release)return NULL;
  gt_checkpoint_tasks_t *pool=a.allocate(a.context,sizeof(*pool));
  if (!pool) return NULL;
  memset(pool,0,sizeof(*pool));pool->allocator=a;
  pool->capacity=capacity;pool->next_id=1;
  if (pthread_mutex_init(&pool->lock,NULL)) {a.release(a.context,pool,sizeof(*pool));return NULL;}
  if (pthread_cond_init(&pool->changed,NULL)) {
    pthread_mutex_destroy(&pool->lock);a.release(a.context,pool,sizeof(*pool));return NULL;
  }
  if (gt__thread_create(&pool->thread,run,pool,thread_options)) {
    pthread_cond_destroy(&pool->changed);pthread_mutex_destroy(&pool->lock);a.release(a.context,pool,sizeof(*pool));return NULL;
  }
  return pool;
}
gt_task_rc_t gt__tasks_submit(gt_checkpoint_tasks_t *pool,const gt_task_spec_t *spec,uint64_t *id) {
  if (id) *id=0;
  if (!pool || !spec || !id || !spec->encode || !spec->release_input ||
      !spec->release_result || !spec->generation) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);
  gt_task_rc_t rc=GT_TASK_FULL;
  if (pool->stopping || !pool->next_id) rc=GT_TASK_STOPPED;
  else for (size_t i=0;i<pool->capacity;i++) if (!pool->slots[i].id) {
    task_t *t=&pool->slots[i];
    *t=(task_t){.id=pool->next_id++,.spec=*spec,.state=GT_TASK_QUEUED,.input_owned=true};
    *id=t->id;rc=GT_TASK_OK;pthread_cond_signal(&pool->changed);break;
  }
  pthread_mutex_unlock(&pool->lock);return rc;
}
gt_task_rc_t gt__tasks_info(gt_checkpoint_tasks_t *pool,uint64_t id,gt_task_info_t *info) {
  if (info) memset(info,0,sizeof(*info));
  if (!pool || !info) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);task_t *t=find(pool,id);
  if (t) *info=(gt_task_info_t){t->cancel && t->state==GT_TASK_RUNNING ? GT_TASK_CANCELLING : t->state,
                               t->error,t->spec.generation,t->spec.sequence};
  pthread_mutex_unlock(&pool->lock);return t ? GT_TASK_OK : GT_TASK_INVALID;
}
gt_task_rc_t gt__tasks_cancel(gt_checkpoint_tasks_t *pool,uint64_t id) {
  if (!pool) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);task_t *t=find(pool,id);
  gt_task_rc_t rc=!t ? GT_TASK_INVALID : t->state==GT_TASK_TAKEN ? GT_TASK_BUSY : GT_TASK_OK;
  if (rc==GT_TASK_OK) {cancel_locked(t);pthread_cond_signal(&pool->changed);}
  pthread_mutex_unlock(&pool->lock);return rc;
}
gt_task_rc_t gt__tasks_take(gt_checkpoint_tasks_t *pool,uint64_t id,uint64_t generation,
                           void **result,uint64_t *sequence) {
  if (result) *result=NULL;
  if (sequence) *sequence=0;
  if (!pool || !result || !sequence) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);task_t *t=find(pool,id);
  gt_task_rc_t rc=!t || t->spec.generation!=generation ? GT_TASK_INVALID : GT_TASK_BUSY;
  if (t && t->spec.generation==generation && t->state==GT_TASK_READY && !t->cancel) {
    *result=t->result;*sequence=t->spec.sequence;t->state=GT_TASK_TAKEN;rc=GT_TASK_OK;
  }
  pthread_mutex_unlock(&pool->lock);return rc;
}
gt_task_rc_t gt__tasks_release(gt_checkpoint_tasks_t *pool,uint64_t id) {
  if (!pool) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);task_t *t=find(pool,id);
  gt_task_rc_t rc=!t ? GT_TASK_INVALID : t->state!=GT_TASK_TAKEN ? GT_TASK_BUSY : GT_TASK_OK;
  if (rc==GT_TASK_OK) {t->state=GT_TASK_RELEASING;pthread_cond_signal(&pool->changed);}
  pthread_mutex_unlock(&pool->lock);return rc;
}
gt_task_rc_t gt__tasks_forget(gt_checkpoint_tasks_t *pool,uint64_t id) {
  if (!pool) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);task_t *t=find(pool,id);
  gt_task_rc_t rc=!t ? GT_TASK_INVALID : !terminal(t->state) ? GT_TASK_BUSY : GT_TASK_OK;
  if (rc==GT_TASK_OK) memset(t,0,sizeof(*t));
  pthread_mutex_unlock(&pool->lock);return rc;
}
gt_task_rc_t gt__tasks_close(gt_checkpoint_tasks_t *pool) {
  if (!pool) return GT_TASK_INVALID;
  pthread_mutex_lock(&pool->lock);
  for (size_t i=0;i<pool->capacity;i++) if (pool->slots[i].id && pool->slots[i].state==GT_TASK_TAKEN) {
    pthread_mutex_unlock(&pool->lock);return GT_TASK_BUSY;
  }
  pool->stopping=true;
  for (size_t i=0;i<pool->capacity;i++) if (pool->slots[i].id) cancel_locked(&pool->slots[i]);
  pthread_cond_signal(&pool->changed);pthread_mutex_unlock(&pool->lock);
  pthread_join(pool->thread,NULL);
  pthread_cond_destroy(&pool->changed);pthread_mutex_destroy(&pool->lock);
  gt_allocator_t a=pool->allocator;a.release(a.context,pool,sizeof(*pool));return GT_TASK_OK;
}
