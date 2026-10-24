#include "attachment.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  uint64_t id,generation;
  void *context,*result;
  bool borrowed;
} attachment_job_t;
struct gt_session_attachment {
  gt_allocator_t allocator;
  pthread_mutex_t lock;
  gt_checkpoint_tasks_t *pool;
  uint64_t generation,current_task,last_command;
  bool active,submitted,session_ended,has_terminal_sequence;uint64_t terminal_sequence;
  attachment_job_t jobs[16];
  gt_restore_stream_t stream;
};
static bool current(gt_session_attachment_t *a,uint64_t generation) {
  return a->active && generation && generation==a->generation;
}
static attachment_job_t *job(gt_session_attachment_t *a,uint64_t id) {
  if(!id)return NULL;
  for(size_t i=0;i<16;i++)if(a->jobs[i].id==id)return &a->jobs[i];
  return NULL;
}
static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
gt_session_attachment_t *gt__attachment_open(gt_checkpoint_tasks_t *pool,size_t queue_bytes,size_t queue_events) {
  return gt__attachment_open_with_allocator(pool,queue_bytes,queue_events,NULL);
}
gt_session_attachment_t *gt__attachment_open_with_allocator(gt_checkpoint_tasks_t *pool,size_t queue_bytes,size_t queue_events,const gt_allocator_t *allocator) {
  if(!pool)return NULL;
  gt_allocator_t domain=allocator?*allocator:(gt_allocator_t){NULL,default_allocate,default_release};
  if(!domain.allocate || !domain.release)return NULL;
  gt_session_attachment_t *a=domain.allocate(domain.context,sizeof(*a));if(!a)return NULL;
  memset(a,0,sizeof(*a));a->allocator=domain;
  if(pthread_mutex_init(&a->lock,NULL)) {domain.release(domain.context,a,sizeof(*a));return NULL;}
  if(!gt__restore_init_with_allocator(&a->stream,queue_bytes,queue_events,&domain)) {
    pthread_mutex_destroy(&a->lock);domain.release(domain.context,a,sizeof(*a));return NULL;
  }
  a->pool=pool;return a;
}
gt_task_rc_t gt__attachment_attach(gt_session_attachment_t *a,uint64_t *generation) {
  if(generation)*generation=0;
  if(!a || !generation)return GT_TASK_INVALID;
  pthread_mutex_lock(&a->lock);
  gt_task_rc_t rc=a->active ? GT_TASK_BUSY : a->generation==UINT64_MAX ? GT_TASK_STOPPED : GT_TASK_OK;
  if(rc==GT_TASK_OK) {
    a->generation++;a->active=true;a->submitted=false;a->current_task=0;a->last_command=0;
    *generation=a->generation;gt__restore_reset(&a->stream,a->generation);
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
bool gt__attachment_current(gt_session_attachment_t *a,uint64_t generation) {
  if(!a)return false;
  pthread_mutex_lock(&a->lock);bool valid=current(a,generation);
  pthread_mutex_unlock(&a->lock);return valid;
}
gt_task_rc_t gt__attachment_submit(gt_session_attachment_t *a,uint64_t generation,
                                  const gt_task_spec_t *spec,uint64_t *task) {
  if(task)*task=0;
  if(!a || !spec || !task)return GT_TASK_INVALID;
  pthread_mutex_lock(&a->lock);
  gt_task_rc_t rc=GT_TASK_INVALID;
  if(current(a,generation)) {
    rc=a->submitted ? GT_TASK_BUSY : GT_TASK_FULL;
    if(!a->submitted && (a->stream.phase!=GT_RESTORE_SNAPSHOT || spec->sequence!=a->stream.snapshot))rc=GT_TASK_INVALID;
    if(!a->submitted && a->stream.phase==GT_RESTORE_SNAPSHOT && spec->sequence==a->stream.snapshot) for(size_t i=0;i<16;i++)if(!a->jobs[i].id) {
      gt_task_spec_t bound=*spec;bound.generation=generation;
      rc=gt__tasks_submit(a->pool,&bound,task);
      if(rc==GT_TASK_OK) {
        a->jobs[i]=(attachment_job_t){.id=*task,.generation=generation,.context=spec->context};
        a->submitted=true;a->current_task=*task;
      }
      break;
    }
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_task_rc_t gt__attachment_take(gt_session_attachment_t *a,uint64_t generation,
                                gt_attachment_checkpoint_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!a || !out)return GT_TASK_INVALID;
  pthread_mutex_lock(&a->lock);
  gt_task_rc_t rc=GT_TASK_INVALID;
  attachment_job_t *j=job(a,a->current_task);
  if(current(a,generation) && j && a->stream.phase==GT_RESTORE_SNAPSHOT && !a->stream.offered) {
    uint64_t sequence=0;void *result=NULL;
    rc=gt__tasks_take(a->pool,j->id,generation,&result,&sequence);
    if(rc==GT_TASK_OK) {
      j->borrowed=true;j->result=result;
      if(gt__restore_offer(&a->stream,generation,sequence)!=GT_RESTORE_OK) {
        gt__tasks_release(a->pool,j->id);j->borrowed=false;j->result=NULL;
        a->active=false;pthread_mutex_unlock(&a->lock);return GT_TASK_INVALID;
      }
      *out=(gt_attachment_checkpoint_t){j->id,generation,sequence,result};
    }
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_task_rc_t gt__attachment_detach(gt_session_attachment_t *a,uint64_t generation) {
  if(!a)return GT_TASK_INVALID;
  pthread_mutex_lock(&a->lock);
  gt_task_rc_t rc=GT_TASK_INVALID;
  if(current(a,generation)) {
    a->active=false; /* Revoke before requesting asynchronous cleanup. */
    gt__restore_detach(&a->stream);
    attachment_job_t *j=job(a,a->current_task);
    rc=j && !j->borrowed ? gt__tasks_cancel(a->pool,j->id) : GT_TASK_OK;
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_task_rc_t gt__attachment_release(gt_session_attachment_t *a,gt_attachment_checkpoint_t *lease) {
  if(!a || !lease)return GT_TASK_INVALID;
  pthread_mutex_lock(&a->lock);attachment_job_t *j=job(a,lease->task);
  gt_task_rc_t rc=GT_TASK_INVALID;
  if(j && j->borrowed && j->generation==lease->generation && j->result==lease->result) {
    rc=gt__tasks_release(a->pool,j->id);
    if(rc==GT_TASK_OK) {j->borrowed=false;j->result=NULL;memset(lease,0,sizeof(*lease));}
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
static gt_task_rc_t reap(gt_session_attachment_t *a,uint64_t target,gt_attachment_retired_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!a || !out)return GT_TASK_INVALID;
  pthread_mutex_lock(&a->lock);gt_task_rc_t rc=GT_TASK_BUSY;
  for(size_t i=0;i<16;i++) {
    attachment_job_t *j=&a->jobs[i];if(!j->id || j->borrowed || (target && j->id!=target))continue;
    gt_task_info_t info;
    rc=gt__tasks_info(a->pool,j->id,&info);
    if(rc!=GT_TASK_OK)break;
    rc=GT_TASK_BUSY;
    if(info.state==GT_TASK_CANCELLED || info.state==GT_TASK_FAILED || info.state==GT_TASK_FINISHED) {
      rc=gt__tasks_forget(a->pool,j->id);
      if(rc==GT_TASK_OK) {
        /* Failed encoding cannot leave an active attachment with no snapshot.
         * A retired older generation must never revoke its replacement. */
        if(info.state==GT_TASK_FAILED && current(a,j->generation)) {
          a->active=false;gt__restore_detach(&a->stream);
        }
        *out=(gt_attachment_retired_t){j->id,j->generation,info.state,info.error,j->context};
        if(a->current_task==j->id)a->current_task=0;
        memset(j,0,sizeof(*j));
      }
      break;
    }
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_task_rc_t gt__attachment_reap(gt_session_attachment_t *a,gt_attachment_retired_t *out){return reap(a,0,out);}
gt_task_rc_t gt__attachment_reap_task(gt_session_attachment_t *a,uint64_t id,gt_attachment_retired_t *out) {
  if(!id){if(out)memset(out,0,sizeof(*out));return GT_TASK_INVALID;}
  return reap(a,id,out);
}
bool gt__attachment_quiescent(gt_session_attachment_t *a) {
  if(!a)return false;
  pthread_mutex_lock(&a->lock);
  bool busy=a->active;
  for(size_t i=0;i<16;i++)if(a->jobs[i].id)busy=true;
  pthread_mutex_unlock(&a->lock);
  return !busy;
}
gt_task_rc_t gt__attachment_close(gt_session_attachment_t *a) {
  if(!a)return GT_TASK_INVALID;
  if(!gt__attachment_quiescent(a))return GT_TASK_BUSY;
  gt__restore_destroy(&a->stream);pthread_mutex_destroy(&a->lock);
  gt_allocator_t domain=a->allocator;domain.release(domain.context,a,sizeof(*a));return GT_TASK_OK;
}

static gt_restore_rc_t finish_stream(gt_session_attachment_t *a,gt_restore_rc_t rc) {
  if(a->stream.phase==GT_RESTORE_FAILED && a->active) {
    a->active=false;
    attachment_job_t *j=job(a,a->current_task);
    if(j && !j->borrowed)gt__tasks_cancel(a->pool,j->id);
  }
  return rc;
}
gt_restore_rc_t gt__attachment_begin(gt_session_attachment_t *a,uint64_t g,uint64_t s) {
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);
  gt_restore_rc_t rc;
  if(!current(a,g))rc=GT_RESTORE_STALE;
  else if(a->session_ended && !a->has_terminal_sequence)rc=GT_RESTORE_PROTOCOL;
  else if(a->session_ended && s>a->terminal_sequence)rc=GT_RESTORE_PROTOCOL;
  else rc=finish_stream(a,gt__restore_begin(&a->stream,g,s));
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_push(gt_session_attachment_t *a,uint64_t s,uint32_t kind,const void *p,size_t n) {
  if(!a)return GT_RESTORE_IDLE;
  pthread_mutex_lock(&a->lock);
  gt_restore_rc_t rc;
  if(a->session_ended && !a->has_terminal_sequence) {pthread_mutex_unlock(&a->lock);return GT_RESTORE_PROTOCOL;}
  if(a->session_ended && s>a->terminal_sequence)rc=GT_RESTORE_PROTOCOL;
  else rc=finish_stream(a,gt__restore_push(&a->stream,s,kind,p,n));
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_next(gt_session_attachment_t *a,uint64_t g,gt_restore_event_t *e,void *p,size_t n) {
  if(e)memset(e,0,sizeof(*e));
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);
  gt_restore_rc_t rc=finish_stream(a,gt__restore_next(&a->stream,g,e,p,n));
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_ready_snapshot(gt_session_attachment_t *a,uint64_t g,uint64_t s) {
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);
  gt_restore_rc_t rc=finish_stream(a,gt__restore_ready_snapshot(&a->stream,g,s));
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_live_begin(gt_session_attachment_t *a,uint64_t g,uint64_t *s) {
  if(s)*s=0;
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);
  gt_restore_rc_t rc=finish_stream(a,gt__restore_live_begin(&a->stream,g,s));
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_ready_live(gt_session_attachment_t *a,uint64_t g,uint64_t s) {
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);
  gt_restore_rc_t rc=finish_stream(a,gt__restore_ready_live(&a->stream,g,s));
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_end(gt_session_attachment_t *a,uint64_t g,uint64_t s) {
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);gt_restore_rc_t rc;
  if(!current(a,g)) {rc=GT_RESTORE_STALE;}
  else if(a->session_ended && !a->has_terminal_sequence)rc=GT_RESTORE_PROTOCOL;
  else if((a->session_ended && s!=a->terminal_sequence) || s<a->stream.last)rc=GT_RESTORE_PROTOCOL;
  else {
    a->session_ended=true;a->has_terminal_sequence=true;a->terminal_sequence=s;
    rc=gt__restore_end(&a->stream,g,s);
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_end_unknown(gt_session_attachment_t *a,uint64_t g) {
  if(!a)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);gt_restore_rc_t rc=GT_RESTORE_PROTOCOL;
  if (!current(a,g))rc=GT_RESTORE_STALE;
  else if(a->has_terminal_sequence || a->submitted || a->stream.phase!=GT_RESTORE_CAPTURING)rc=GT_RESTORE_PROTOCOL;
  else {a->session_ended = true;rc=GT_RESTORE_OK;}
  pthread_mutex_unlock(&a->lock);return rc;
}
bool gt__attachment_input_allowed(gt_session_attachment_t *a,uint64_t g,uint64_t s) {
  if(!a)return false;
  pthread_mutex_lock(&a->lock);
  bool valid=a->active && !a->session_ended && gt__restore_input_allowed(&a->stream,g,s);
  pthread_mutex_unlock(&a->lock);return valid;
}
gt_status_t gt__attachment_authorize(gt_session_attachment_t *a,uint64_t g,uint64_t id,uint64_t s) {
  if(!a || !g || !id)return GT_ERR_INVALID_ARG;
  pthread_mutex_lock(&a->lock);gt_status_t rc=GT_ERR_AUTH;
  if(current(a,g)) {
    if(id<=a->last_command)rc=GT_ERR_PROTOCOL;
    else {
      a->last_command=id;
      if(!a->session_ended && gt__restore_input_allowed(&a->stream,g,s))rc=GT_OK;
    }
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
gt_restore_rc_t gt__attachment_restore_info(gt_session_attachment_t *a,uint64_t g,gt_attachment_restore_info_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!a || !out)return GT_RESTORE_STALE;
  pthread_mutex_lock(&a->lock);gt_restore_rc_t rc=GT_RESTORE_STALE;
  if(g && g==a->generation) {
    gt_restore_stream_t *s=&a->stream;
    *out=(gt_attachment_restore_info_t){s->phase,s->failure,s->snapshot,s->last,s->dequeued,s->cut,s->used,s->count,a->session_ended,a->terminal_sequence,a->has_terminal_sequence};
    rc=GT_RESTORE_OK;
  }
  pthread_mutex_unlock(&a->lock);return rc;
}
