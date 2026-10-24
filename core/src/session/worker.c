#include "worker.h"
#include "process-meta.h"
#include <time.h>
#include "base/secure-random.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

enum { QUEUE_CAPACITY = 16, INPUT_CAPACITY = 65536 };
typedef struct {
  gt_worker_command_t kind;
  void *payload;
  size_t length;
  gt_worker_completion_t complete;
  void *context;
  bool user;gt_worker_stamp_t stamp;
  gt_worker_user_completion_t complete_user;gt_user_delivery_t delivery;
} command_t;
struct gt_session_worker {
  gt_allocator_t allocator;
  pthread_t thread;
  pthread_mutex_t lock;
  int wake[2];
  gt_worker_config_t config;
  command_t queue[QUEUE_CAPACITY];
  size_t head, count, outstanding, bytes;
  bool stopping,result_ready;
  gt_worker_result_t result;
  pthread_t owner_thread;
  bool reply_window;
  gt_status_t reply_failure;
  uint8_t replies[65536];
  size_t replies_length;
};

/* Both pipe ends live until join; submitters must already be quiesced then.
 * All writers hold lock, so drain is finite even under producer pressure. */
static void notify_locked(gt_session_worker_t *w) {
  const char byte=1;ssize_t n;
  do {n=write(w->wake[1],&byte,1);} while(n<0 && errno==EINTR);
  /* EAGAIN means a wake is already pending. The owner also checks count under
   * the same mutex before waiting, including the queue-before-read race. */
}
static bool pop(gt_session_worker_t *w, command_t *cmd) {
  pthread_mutex_lock(&w->lock);
  char pending[64];ssize_t n;
  do {n=read(w->wake[0],pending,sizeof(pending));} while(n>0 || (n<0 && errno==EINTR));
  bool found = !w->stopping && w->count;
  if (found) {
    *cmd = w->queue[w->head];
    memset(&w->queue[w->head],0,sizeof(command_t));
    w->head = (w->head + 1) % QUEUE_CAPACITY;
    w->count--;
  }
  pthread_mutex_unlock(&w->lock);
  return found;
}
static void complete(gt_session_worker_t *w, command_t *cmd, gt_status_t rc,
                     gt_state_capture_t capture) {
  if(cmd->payload){gt__secure_erase(cmd->payload,cmd->length);w->allocator.release(w->allocator.context,cmd->payload,cmd->length);}
  pthread_mutex_lock(&w->lock);
  w->outstanding--;
  w->bytes -= cmd->length;
  pthread_mutex_unlock(&w->lock);
  if(cmd->user) {
    gt_worker_user_result_t result={.stamp=cmd->stamp,.status=rc,.delivery=cmd->delivery};
    cmd->complete_user(cmd->context,&result);
  } else if (cmd->complete) cmd->complete(cmd->context, rc, capture);
}
static bool stopped(gt_session_worker_t *w) {
  pthread_mutex_lock(&w->lock);
  bool result = w->stopping;
  pthread_mutex_unlock(&w->lock);
  return result;
}
static gt_status_t stage_reply(void *context, const uint8_t *bytes, size_t length) {
  gt_session_worker_t *w = context;
  if (!pthread_equal(w->owner_thread, pthread_self())) return GT_ERR_INVALID_ARG;
  if (w->reply_failure != GT_OK) return w->reply_failure;
  if (!w->reply_window || (length && !bytes))
    return w->reply_failure = GT_ERR_INVALID_ARG;
  if (length > sizeof(w->replies) - w->replies_length)
    return w->reply_failure = GT_ERR_PROTOCOL;
  if (length) memcpy(w->replies + w->replies_length, bytes, length);
  w->replies_length += length;
  return GT_OK;
}
static gt_status_t apply_state(gt_session_worker_t *w, gt_state_owner_t *owner,
                               const gt_state_event_t *event, gt_io_backend_t *backend,gt_worker_stage_t *stage) {
  *stage=GT_WORKER_STAGE_APPLY;
  w->reply_window = true;
  gt_status_t result = gt__state_owner_apply(owner, event);
  w->reply_window = false;
  if (result != GT_OK) return result;
  if (w->reply_failure != GT_OK) {*stage=GT_WORKER_STAGE_REPLY;result=w->reply_failure;}
  else if (w->replies_length) {
    *stage=GT_WORKER_STAGE_REPLY;
    result = backend->ops->write(backend, w->replies, w->replies_length);
    if (result==GT_OK) {w->replies_length = 0;}
  }
  /* Successful apply remains observable even when its reply could not be sent. */
  if (w->config.event) w->config.event(w->config.context, event, owner->sequence);
  return result;
}
static void observe_process(gt_session_worker_t *w,gt_io_backend_t *backend,
  uint64_t *previous,bool *observed,gt_session_process_t *last) {
  if(!w->config.process)return;
  struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0)return;
  uint64_t now=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
  if(*observed && now>=*previous && now-*previous<250)return;
  *previous=now;
  gt_session_process_t value={0};
  if(backend->ops->child_state)value.child=backend->ops->child_state(backend);
  if((unsigned)value.child>GT_TRANSPORT_CHILD_JOB)value.child=GT_TRANSPORT_CHILD_UNKNOWN;
  if(backend->ops->child_cwd) {
    /* Existing backends may NUL-terminate a cap-1 prefix. One extra byte
     * distinguishes every publishable full path from an oversized prefix. */
    char cwd[sizeof(value.cwd)+1]={0};
    if(backend->ops->child_cwd(backend,cwd,sizeof(cwd))) {
      const char *end=memchr(cwd,0,sizeof(cwd));
      if(end && (size_t)(end-cwd)<sizeof(value.cwd)) {
        value.has_cwd=true;memcpy(value.cwd,cwd,(size_t)(end-cwd)+1);
      }
    }
  }
  if(!value.has_cwd || !gt__process_valid(&value)){value.has_cwd=false;memset(value.cwd,0,sizeof(value.cwd));}
  if(!*observed || !gt__process_equal(last,&value)) {
    *last=value;w->config.process(w->config.context,&value);
  }
  *observed=true;
}
static void *run(void *context) {
  gt_session_worker_t *w = context;
  gt_worker_config_t *c = &w->config;
  gt_io_backend_t *backend = NULL;
  void *state = NULL;
  gt_state_owner_t owner={0};bool owner_ready=false;
  gt_worker_end_t end=GT_WORKER_END_STOPPED;gt_worker_stage_t stage=GT_WORKER_STAGE_STATE_OPEN;
  bool has_exit_code=false;int32_t exit_code=0;
  gt_status_t termination_status=GT_ERR_NOT_FOUND;gt_io_exit_t termination={0};
  w->owner_thread = pthread_self();
  gt_worker_reply_sink_t sink = {.context = w, .write = stage_reply};
  gt_status_t rc = c->open_state(c->context, sink, &state);
  if (w->reply_failure != GT_OK) {rc = w->reply_failure;stage=GT_WORKER_STAGE_REPLY;}
  if (rc != GT_OK) goto done;
  if (!state) { rc = GT_ERR_INTERNAL; goto done; }
  rc = gt__state_owner_init(&owner, state, c->state_ops);
  if (rc != GT_OK) goto done;
  owner_ready=true;
  if (stopped(w)) goto done;
  stage=GT_WORKER_STAGE_BACKEND_OPEN;
  backend = c->open_backend(c->context);
  if (!backend) { rc = GT_ERR_IO; goto done; }
  uint8_t output[16384];uint64_t process_time=0;bool process_observed=false;
  gt_session_process_t last_process={0};
  while (!stopped(w)) {
    observe_process(w,backend,&process_time,&process_observed,&last_process);
    command_t cmd;
    /* One command and one bounded read per round: neither continuous output
     * nor producers flooding commands can starve the other direction. */
    if (pop(w, &cmd)) {
      gt_state_capture_t capture = {0};
      gt_status_t result = GT_OK;
      bool rejected=false;
      if(cmd.user) {
        result=c->authorize?c->authorize(c->context,cmd.kind,&cmd.stamp):GT_ERR_UNSUPPORTED;
        if((unsigned)result>GT_ERR_INTERNAL)result=GT_ERR_INTERNAL;
        rejected=result!=GT_OK;
      }
      if(!rejected)cmd.delivery=GT_USER_OUTCOME_UNKNOWN;
      if(rejected) { /* Policy denial never touches backend/state or ends session. */ }
      else if (cmd.kind == GT_WORKER_INPUT) {
        stage=GT_WORKER_STAGE_INPUT;
        result = backend->ops->write(backend, cmd.payload, cmd.length);
      } else if (cmd.kind == GT_WORKER_RESIZE) {
        stage=GT_WORKER_STAGE_RESIZE;
        const gt_state_size_t *size = cmd.payload;
        result = backend->ops->resize(backend, size->cols, size->rows,
                                     size->px_width, size->px_height);
        if (result == GT_OK) {
          gt_state_event_t event = {GT_STATE_RESIZE, size, sizeof(*size)};
          result = apply_state(w, &owner, &event, backend,&stage);
        }
      } else if(cmd.kind==GT_WORKER_CAPTURE) {
        stage=GT_WORKER_STAGE_CAPTURE;
        result = gt__state_owner_capture(&owner, &capture);
      } else {
        gt_state_event_t event={cmd.kind==GT_WORKER_STYLE?GT_STATE_STYLE:GT_STATE_COLOR_SCHEME,cmd.payload,cmd.length};
        result=apply_state(w,&owner,&event,backend,&stage);
      }
      if(!rejected && result==GT_OK)cmd.delivery=GT_USER_OPERATION_OK;
      complete(w, &cmd, result, capture);
      /* A capture allocation failure is task-local; state/backend failures
       * cannot be hidden by continuing to drain the process. */
      if (result != GT_OK && !rejected && cmd.kind != GT_WORKER_CAPTURE) {
        rc = result;
        break;
      }
      if (owner.failure != GT_OK) { rc = owner.failure; break; }
      if (w->reply_failure != GT_OK) { rc = w->reply_failure;stage=GT_WORKER_STAGE_REPLY; break; }
    }
    if (stopped(w)) break;
    stage=GT_WORKER_STAGE_READ;
    pthread_mutex_lock(&w->lock);
    int timeout = w->count || w->stopping ? 0 : 10;
    pthread_mutex_unlock(&w->lock);
    int n = backend->ops->read_with_wake
      ? backend->ops->read_with_wake(backend,output,sizeof(output),timeout,w->wake[0])
      : backend->ops->read(backend,output,sizeof(output),timeout);
    if (n == GT_TRANSPORT_READ_TIMEOUT) continue;
    if (n == GT_TRANSPORT_READ_EOF) {
      end=GT_WORKER_END_EOF;
      termination_status=GT_ERR_UNSUPPORTED;
      if(backend->ops->observe_exit) {
        gt_io_exit_t observed={0};termination_status=backend->ops->observe_exit(backend,&observed);
        if(termination_status==GT_OK) {
          if((observed.kind==GT_IO_EXIT_CODE && observed.code>=0) || (observed.kind==GT_IO_EXIT_SIGNAL && observed.code>0)) {
            termination=observed;has_exit_code=observed.kind==GT_IO_EXIT_CODE;
            exit_code=has_exit_code?observed.code:0;
          } else termination_status=GT_ERR_PROTOCOL;
        }
      }
      break;
    }
    if (n < 0 || (size_t)n > sizeof(output)) { rc = GT_ERR_IO; break; }
    gt_state_event_t event = {GT_STATE_OUTPUT, output, (size_t)n};
    uint64_t previous=owner.sequence;
    rc = apply_state(w, &owner, &event, backend,&stage);
    if (owner.sequence!=previous && c->output) c->output(c->context, output, (size_t)n, owner.sequence);
    if (rc != GT_OK) break;
  }
done:
  /* Invalidate admission before releasing resources or calling completions. */
  gt__worker_stop(w);
  if (backend) backend->ops->close(backend);
  if(c->final_capture) {
    gt_state_capture_t final={0};gt_status_t captured=rc;
    if(rc==GT_OK && owner_ready) {
      captured=gt__state_owner_capture(&owner,&final);
      if(owner.failure!=GT_OK){rc=owner.failure;stage=GT_WORKER_STAGE_CAPTURE;}
    } else if(rc==GT_OK)captured=GT_ERR_NOT_FOUND;
    c->final_capture(c->context,captured,final);
  }
  if (state && owner_ready && owner.failure!=GT_OK && c->state_invalidated)
    c->state_invalidated(state,owner.failure,owner.sequence);
  if (state) c->close_state(state);
  for (;;) {
    pthread_mutex_lock(&w->lock);
    bool found = w->count != 0;
    command_t cmd = {0};
    if (found) {
      cmd = w->queue[w->head];
      memset(&w->queue[w->head],0,sizeof(command_t));
      w->head = (w->head + 1) % QUEUE_CAPACITY;
      w->count--;
    }
    pthread_mutex_unlock(&w->lock);
    if (!found) break;
    complete(w, &cmd, GT_ERR_CANCELLED, (gt_state_capture_t){0});
  }
  gt_worker_result_t result={.kind=rc==GT_OK?end:GT_WORKER_END_ERROR,
    .stage=rc==GT_OK?GT_WORKER_STAGE_NONE:stage,.status=rc,
    .has_sequence=owner_ready,.sequence=owner_ready?owner.sequence:0,
    .has_exit_code=has_exit_code,.exit_code=exit_code,
    .termination_status=termination_status,.termination=termination};
  pthread_mutex_lock(&w->lock);w->result=result;w->result_ready=true;pthread_mutex_unlock(&w->lock);
  if (c->finished) c->finished(c->context, rc);
  return NULL;
}

static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
gt_session_worker_t *gt__worker_start(const gt_worker_config_t *config) {
  return gt__worker_start_with_allocator(config,NULL);
}
gt_session_worker_t *gt__worker_start_with_allocator(const gt_worker_config_t *config,const gt_allocator_t *allocator) {
  return gt__worker_start_with_options(config,allocator,NULL);
}
gt_session_worker_t *gt__worker_start_with_options(const gt_worker_config_t *config,const gt_allocator_t *allocator,const gt_thread_options_t *thread_options) {
  if(!gt__thread_options_valid(thread_options))return NULL;
  if (!config || !config->open_backend || !config->open_state ||
      !config->close_state || !config->state_ops.apply || !config->state_ops.freeze)
    return NULL;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,default_allocate,default_release};
  if(!a.allocate || !a.release)return NULL;
  gt_session_worker_t *w = a.allocate(a.context,sizeof(*w));
  if (!w) return NULL;
  memset(w,0,sizeof(*w));w->allocator=a;
  w->config = *config;
  if (pthread_mutex_init(&w->lock, NULL)) { a.release(a.context,w,sizeof(*w)); return NULL; }
  w->wake[0]=w->wake[1]=-1;
  bool pipe_ready=pipe(w->wake)==0;
  for(unsigned i=0;pipe_ready && i<2;i++) {
    int fdflags=fcntl(w->wake[i],F_GETFD),flags=fcntl(w->wake[i],F_GETFL);
    pipe_ready=fdflags>=0 && flags>=0 &&
      fcntl(w->wake[i],F_SETFD,fdflags|FD_CLOEXEC)==0 &&
      fcntl(w->wake[i],F_SETFL,flags|O_NONBLOCK)==0;
  }
  if (!pipe_ready || gt__thread_create(&w->thread,run,w,thread_options)) {
    for(unsigned i=0;i<2;i++)if(w->wake[i]>=0)close(w->wake[i]);
    pthread_mutex_destroy(&w->lock);
    a.release(a.context,w,sizeof(*w));
    return NULL;
  }
  return w;
}

static gt_worker_submit_t submit(gt_session_worker_t *w,
                                    gt_worker_command_t kind, const void *payload,
                                    size_t length, gt_worker_completion_t callback,
                                    void *context,const gt_worker_stamp_t *stamp,gt_worker_user_completion_t user_callback) {
  if (!w || (length && !payload) || kind > GT_WORKER_STYLE ||
      kind < GT_WORKER_INPUT ||
      (kind == GT_WORKER_RESIZE && length != sizeof(gt_state_size_t)) ||
      (kind == GT_WORKER_CAPTURE && (length || !callback)) ||
      (kind == GT_WORKER_COLOR_SCHEME && length != sizeof(gt_state_color_scheme_t)) ||
      (kind == GT_WORKER_STYLE && length != sizeof(gt_session_style_t)))
    return GT_WORKER_INVALID;
  if (kind == GT_WORKER_RESIZE) {
    gt_state_size_t size;
    memcpy(&size, payload, sizeof(size));
    if (!size.cols || !size.rows) return GT_WORKER_INVALID;
  }
  if(kind==GT_WORKER_COLOR_SCHEME) {
    gt_state_color_scheme_t scheme;memcpy(&scheme,payload,sizeof(scheme));
    if(scheme.dark>1)return GT_WORKER_INVALID;
  }
  if(kind==GT_WORKER_STYLE) {
    gt_session_style_t style;memcpy(&style,payload,sizeof(style));
    if(style.dark>1)return GT_WORKER_INVALID;
  }
  pthread_mutex_lock(&w->lock);
  gt_worker_submit_t result = GT_WORKER_ACCEPTED;
  if (w->stopping) result = GT_WORKER_STOPPED;
  else if(stamp && !w->config.authorize)result=GT_WORKER_UNSUPPORTED;
  else if(kind==GT_WORKER_COLOR_SCHEME && !w->config.color_scheme_supported)result=GT_WORKER_UNSUPPORTED;
  else if(kind==GT_WORKER_STYLE && !w->config.style_supported)result=GT_WORKER_UNSUPPORTED;
  else if (w->outstanding == QUEUE_CAPACITY || length > INPUT_CAPACITY - w->bytes)
    result = GT_WORKER_BACKPRESSURE;
  else {
    void *copy = length ? w->allocator.allocate(w->allocator.context,length) : NULL;
    if (length && !copy) result = GT_WORKER_NO_MEMORY;
    else {
      if (length) memcpy(copy, payload, length);
      size_t tail = (w->head + w->count) % QUEUE_CAPACITY;
      w->queue[tail] = (command_t){.kind=kind,.payload=copy,.length=length,.complete=callback,.context=context,
        .user=stamp!=NULL,.stamp=stamp?*stamp:(gt_worker_stamp_t){0},.complete_user=user_callback};
      w->count++;
      w->outstanding++;
      w->bytes += length;
      notify_locked(w);
    }
  }
  pthread_mutex_unlock(&w->lock);
  return result;
}
gt_worker_submit_t gt__worker_submit_internal(gt_session_worker_t *w,gt_worker_command_t kind,
  const void *payload,size_t length,gt_worker_completion_t callback,void *context) {
  return submit(w,kind,payload,length,callback,context,NULL,NULL);
}
gt_worker_submit_t gt__worker_submit_user(gt_session_worker_t *w,gt_worker_command_t kind,const gt_worker_stamp_t *stamp,
  const void *payload,size_t length,gt_worker_user_completion_t callback,void *context) {
  if(!stamp || !stamp->generation || !stamp->command_id || !callback || kind==GT_WORKER_CAPTURE)return GT_WORKER_INVALID;
  return submit(w,kind,payload,length,NULL,context,stamp,callback);
}
void gt__worker_stop(gt_session_worker_t *w) {
  if (!w) return;
  pthread_mutex_lock(&w->lock);
  w->stopping = true;
  notify_locked(w);
  pthread_mutex_unlock(&w->lock);
}
gt_worker_result_rc_t gt__worker_result(gt_session_worker_t *w,gt_worker_result_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!w || !out)return GT_WORKER_RESULT_INVALID;
  pthread_mutex_lock(&w->lock);bool ready=w->result_ready;
  if(ready)*out=w->result;
  pthread_mutex_unlock(&w->lock);return ready?GT_WORKER_RESULT_READY:GT_WORKER_RESULT_PENDING;
}
void gt__worker_join(gt_session_worker_t *w) {
  if (!w) return;
  pthread_join(w->thread, NULL);
  close(w->wake[0]);close(w->wake[1]);
  pthread_mutex_destroy(&w->lock);
  gt_allocator_t a=w->allocator;gt__secure_erase(w,sizeof(*w));a.release(a.context,w,sizeof(*w));
}
