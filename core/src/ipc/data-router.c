#include "data-router.h"
#include "session/process-meta.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef enum {START,CAPTURING,FINAL_WAIT,ENCODING,BEGIN,CHUNKS,END,WAIT_S,STREAM,NO_STATE,FAILED} phase;
typedef struct data_peer {
  struct gt_data_router *router;gt_token_scope_t scope;gt_runtime_session_t *lease;
  bool active,pending,owned_input;gt_state_capture_t capture;const void *source;
  uint64_t task,sequence,offset,total,start,error_start;gt_status_t capture_error,error;
  gt_status_t final_error;
  gt_data_error_stage_t error_stage;phase phase;
  gt_attachment_checkpoint_t view;const gt_snapshot_blob_t *blob;
  uint16_t marker;bool end_sent,live_sent,error_sent,terminal_sent,pong_pending,turn;
  gt_session_process_t process;bool process_sent;
  uint64_t pong;bool has_terminal;gt_managed_info_t terminal;
} data_peer;
struct gt_data_router {
  gt_allocator_t allocator;gt_data_router_config_t config;pthread_mutex_t lock;
  gt_user_dispatcher_t *users;uint8_t *output;size_t output_capacity;uint64_t now;
  bool clock_failed;uint8_t event[GT_DATA_MAX_OUTPUT];data_peer peers[];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool same(gt_token_scope_t a,gt_token_scope_t b){return a.principal==b.principal && a.issuer==b.issuer && a.session==b.session && a.generation==b.generation;}
static bool valid_scope(const gt_token_scope_t *s){return s && s->principal && s->issuer && s->session && s->generation;}
static data_peer *find(gt_data_router_t *d,const gt_token_scope_t *s){for(size_t i=0;i<d->config.connections;i++)if(d->peers[i].lease && same(d->peers[i].scope,*s))return &d->peers[i];
  return NULL;}
static bool tick(gt_data_router_t *d){uint64_t now;if(d->clock_failed || !d->config.clock(d->config.clock_context,&now) || now<d->now){d->clock_failed=true;return false;}d->now=now;return true;}
static gt_status_t encode(void *ctx,void **out) {
  data_peer *p=ctx;gt_data_router_t *d=p->router;gt_snapshot_blob_t *blob=NULL;
  gt_status_t rc=d->config.encoder.encode(d->config.encoder.context,p->source,d->config.snapshot_limit,&d->allocator,&blob);
  *out=blob;
  if(rc==GT_OK && (!blob || !blob->bytes || !blob->length || blob->length>d->config.snapshot_limit))return GT_ERR_INTERNAL;
  return (unsigned)rc<=GT_ERR_INTERNAL?rc:GT_ERR_INTERNAL;
}
static void release_input(void *ctx){data_peer *p=ctx;if(p->owned_input)gt__runtime_release_capture(p->lease,&p->capture);p->owned_input=false;p->source=NULL;}
static void release_result(void *ctx,void *result){data_peer *p=ctx;p->router->config.encoder.release(p->router->config.encoder.context,result);}
static gt_task_rc_t submit(data_peer *p) {
  gt_task_spec_t spec={.context=p,.encode=encode,.release_input=release_input,.release_result=release_result,.sequence=p->sequence};
  return gt__attachment_submit(gt__runtime_attachment(p->lease),p->scope.generation,&spec,&p->task);
}
static void captured(void *ctx,gt_status_t status,gt_state_capture_t capture) {
  data_peer *p=ctx;gt_data_router_t *d=p->router;
  if(status==GT_OK && capture.frozen) {
    p->capture=capture;p->source=capture.frozen;p->owned_input=true;p->sequence=capture.sequence;
    if(gt__attachment_begin(gt__runtime_attachment(p->lease),p->scope.generation,p->sequence)!=GT_RESTORE_OK)status=GT_ERR_CANCELLED;
    else if(submit(p)!=GT_TASK_OK)status=GT_ERR_INTERNAL;
    if(status!=GT_OK)release_input(p);
  } else if(status==GT_OK)status=GT_ERR_INTERNAL;
  pthread_mutex_lock(&d->lock);p->capture_error=status;p->pending=false;pthread_mutex_unlock(&d->lock);
}
static bool busy_capture(gt_data_router_t *d,data_peer *p){pthread_mutex_lock(&d->lock);bool busy=p->pending;pthread_mutex_unlock(&d->lock);return busy;}
void gt__data_router_reap(gt_data_router_t *d) {
  if(!d)return;
  gt__user_dispatcher_reap(d->users);
  for(size_t i=0;i<d->config.connections;i++) {
    data_peer *p=&d->peers[i];if(!p->lease || busy_capture(d,p))continue;
    if(p->task && !p->view.result) {
      gt_attachment_retired_t retired;
      if(gt__attachment_reap_task(gt__runtime_attachment(p->lease),p->task,&retired)==GT_TASK_OK) {
        p->task=0;
        if(p->phase!=FAILED && (retired.state==GT_TASK_FAILED || retired.state==GT_TASK_CANCELLED)){p->error=retired.error==GT_OK?GT_ERR_INTERNAL:retired.error;p->error_stage=GT_DATA_ERROR_ENCODE;}
      }
    }
    if(!p->active && !p->task){gt_runtime_session_t *lease=p->lease;gt__secure_erase(p,sizeof(*p));gt__runtime_release(&lease);}
  }
}
gt_status_t gt__data_router_open(const gt_data_router_config_t *c,const gt_allocator_t *allocator,gt_data_router_t **out) {
  if(out)*out=NULL;
  if(!out || !c || !c->runtime || !c->attachments || !c->connections || c->connections>64 || !c->user_commands || c->user_commands>16 ||
    !c->snapshot_limit || c->snapshot_limit>64*1024*1024 || !c->chunk_bytes || c->chunk_bytes>GT_DATA_MAX_CHUNK || !c->format ||
    !c->restore_timeout_ms || c->restore_timeout_ms>600000 || !c->error_timeout_ms || c->error_timeout_ms>60000 || !c->clock || !c->encoder.encode || !c->encoder.release ||
    !gt__attachment_owner_matches(c->attachments,gt__runtime_manager(c->runtime),gt__runtime_creation(c->runtime),c->instance))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  size_t n=sizeof(gt_data_router_t)+c->connections*sizeof(data_peer);gt_data_router_t *d=a.allocate(a.context,n);if(!d)return GT_ERR_INTERNAL;
  memset(d,0,n);d->allocator=a;d->config=*c;
  if(pthread_mutex_init(&d->lock,NULL)){a.release(a.context,d,n);return GT_ERR_INTERNAL;}
  size_t chunk=c->chunk_bytes>GT_DATA_MAX_OUTPUT?c->chunk_bytes:GT_DATA_MAX_OUTPUT;d->output_capacity=24+chunk;d->output=a.allocate(a.context,d->output_capacity);
  gt_user_dispatcher_config_t users={.runtime=c->runtime,.attachments=c->attachments,.connections=c->connections,.commands=c->user_commands};memcpy(users.instance,c->instance,16);
  gt_status_t status=d->output?gt__user_dispatcher_open(&users,&a,&d->users):GT_ERR_INTERNAL;
  if(status==GT_OK && !tick(d))status=GT_ERR_IO;
  if(status!=GT_OK){gt__user_dispatcher_close(d->users);if(d->output)a.release(a.context,d->output,d->output_capacity);pthread_mutex_destroy(&d->lock);gt__secure_erase(d,n);a.release(a.context,d,n);return status;}
  *out=d;return GT_OK;
}
static void failure(gt_data_router_t *d,data_peer *p,gt_status_t status,gt_data_error_stage_t stage) {
  if(p->phase==FAILED)return;
  p->phase=FAILED;p->error=status==GT_OK?GT_ERR_INTERNAL:status;p->error_stage=stage;p->error_start=d->now;p->marker=0;
  (void)gt__attachment_detach(gt__runtime_attachment(p->lease),p->scope.generation);
  gt__user_dispatcher_drop(d->users,&p->scope);
  if(p->view.result){(void)gt__attachment_release(gt__runtime_attachment(p->lease),&p->view);p->blob=NULL;}
}
static bool send(gt_data_router_t *d,data_peer *p,gt_connection_t *c,const gt_data_message_t *m) {
  gt_wire_header_t h;if(!gt__data_encode(m,d->output,d->output_capacity,&h))return false;
  gt_connection_send_t status=gt__connection_send(c,&h,d->output);gt__secure_erase(d->output,h.payload_length);
  if(status!=GT_SEND_ACCEPTED)return false;
  if(m->type==GT_MSG_SNAPSHOT_END || m->type==GT_MSG_LIVE_BEGIN || m->type==GT_MSG_RESTORE_ERROR)p->marker=m->type;
  return true;
}
static void flushed(data_peer *p,gt_connection_t *c) {
  if(p->marker && !gt__connection_wants_write(c)) {
    if(p->marker==GT_MSG_SNAPSHOT_END)p->end_sent=true;
    else if(p->marker==GT_MSG_LIVE_BEGIN)p->live_sent=true;
    else p->error_sent=true;
    p->marker=0;
  }
}
void gt__data_router_drop(void *ctx,const gt_token_scope_t *scope) {
  gt_data_router_t *d=ctx;if(!d || !valid_scope(scope))return;
  gt__user_dispatcher_drop(d->users,scope);data_peer *p=find(d,scope);if(!p)return;
  p->active=false;if(p->view.result){(void)gt__attachment_release(gt__runtime_attachment(p->lease),&p->view);p->blob=NULL;}
  gt__data_router_reap(d);
}
gt_server_data_rc_t gt__data_router_revoked(void *ctx,gt_connection_t *c,const gt_token_scope_t *scope) {
  gt_data_router_t *d=ctx;if(!d || !c || !valid_scope(scope) || !tick(d))return GT_SERVER_DATA_CLOSE;
  gt__data_router_reap(d);data_peer *p=find(d,scope);if(!p || !p->active)return GT_SERVER_DATA_CLOSE;
  failure(d,p,p->error==GT_OK?GT_ERR_CANCELLED:p->error,p->error_stage?p->error_stage:GT_DATA_ERROR_REVOKED);flushed(p,c);
  if(d->now-p->error_start>=d->config.error_timeout_ms)return GT_SERVER_DATA_CLOSE;
  gt_wire_frame_t f;
  if(gt__connection_frame(c,&f)) {
    if(f.header.type==GT_MSG_DETACH)return GT_SERVER_DATA_CLOSE;
    if(f.header.type==GT_MSG_RESTORE_ERROR_ACK) {
      gt_data_message_t m;
      if(!gt__data_decode(&f,&m) || m.generation!=scope->generation || !p->error_sent || m.error!=p->error || m.stage!=p->error_stage)return GT_SERVER_DATA_CLOSE;
      return GT_SERVER_DATA_CLOSE;
    }
    /* In-flight pre-revocation requests/heartbeats cannot run or hide ERROR.
     * Discard one frame, without peer-progress or deadline renewal. */
    if(!gt__connection_release_frame(c))return GT_SERVER_DATA_CLOSE;
  }
  if(!p->error_sent && !p->marker && !gt__connection_wants_write(c)) {
    gt_data_message_t m={.type=GT_MSG_RESTORE_ERROR,.generation=scope->generation,.error=p->error,.stage=p->error_stage};
    if(!send(d,p,c,&m))return GT_SERVER_DATA_CLOSE;
  }
  return GT_SERVER_DATA_IDLE;
}
static data_peer *adopt(gt_data_router_t *d,const gt_token_scope_t *s) {
  data_peer *p=find(d,s);if(p)return p->active?p:NULL;
  for(size_t i=0;i<d->config.connections;i++)if(!d->peers[i].lease) {
    gt_runtime_session_t *lease=NULL;if(gt__runtime_acquire(d->config.runtime,s->principal,s->session,&lease)!=GT_RUNTIME_OK)return NULL;
    p=&d->peers[i];p->router=d;p->active=true;p->scope=*s;p->lease=lease;p->start=d->now;return p;
  }
  return NULL;
}
static bool terminal(data_peer *p) {
  gt_managed_info_t info;
  if(gt__manager_info(gt__runtime_manager(p->router->config.runtime),p->scope.session,&info)!=GT_MANAGER_OK)return false;
  if(!info.has_result)return true;
  p->has_terminal=true;p->terminal=info;
  gt_session_attachment_t *a=gt__runtime_attachment(p->lease);
  if(info.result.has_sequence)return gt__attachment_end(a,p->scope.generation,info.result.sequence)!=GT_RESTORE_PROTOCOL;
  return gt__attachment_end_unknown(a,p->scope.generation)!=GT_RESTORE_PROTOCOL;
}
static bool start_capture(gt_data_router_t *d,data_peer *p) {
  gt_runtime_final_t final;gt_runtime_rc_t ended=gt__runtime_final(p->lease,&final);
  if(ended==GT_RUNTIME_OK) {
    if(final.capture_status!=GT_OK || !final.frozen){p->final_error=final.capture_status==GT_OK?GT_ERR_INTERNAL:final.capture_status;p->phase=NO_STATE;return true;}
    p->source=final.frozen;p->sequence=final.sequence;
    if(gt__attachment_begin(gt__runtime_attachment(p->lease),p->scope.generation,p->sequence)!=GT_RESTORE_OK || submit(p)!=GT_TASK_OK)return false;
    p->phase=ENCODING;return true;
  }
  if(ended!=GT_RUNTIME_BUSY)return false;
  pthread_mutex_lock(&d->lock);p->pending=true;pthread_mutex_unlock(&d->lock);
  gt_worker_submit_t admission;gt_manager_rc_t rc=gt__runtime_capture(p->lease,captured,p,&admission);
  if(rc==GT_MANAGER_OK && admission==GT_WORKER_ACCEPTED){p->phase=CAPTURING;return true;}
  pthread_mutex_lock(&d->lock);p->pending=false;pthread_mutex_unlock(&d->lock);
  if(rc==GT_MANAGER_OK && admission==GT_WORKER_STOPPED)return true; /* Result may publish next round. */
  return false;
}
static gt_server_data_rc_t output(gt_data_router_t *d,data_peer *p,gt_connection_t *c) {
  if(gt__connection_wants_write(c))return GT_SERVER_DATA_IDLE;
  gt_data_message_t m={.generation=p->scope.generation};gt_session_attachment_t *a=gt__runtime_attachment(p->lease);
  if(p->pong_pending){m.type=GT_MSG_DATA_PONG;m.nonce=p->pong;p->pong_pending=false;return send(d,p,c,&m)?GT_SERVER_DATA_IDLE:GT_SERVER_DATA_CLOSE;}
  if(p->phase==BEGIN){m.type=GT_MSG_SNAPSHOT_BEGIN;m.sequence=p->sequence;m.total=p->total;m.format=d->config.format;m.flags=p->has_terminal?GT_DATA_READONLY:0;p->phase=CHUNKS;}
  else if(p->phase==CHUNKS) {
    if(p->offset==p->total)p->phase=END;
    else {size_t n=(size_t)(p->total-p->offset);if(n>d->config.chunk_bytes)n=d->config.chunk_bytes;m.type=GT_MSG_SNAPSHOT_CHUNK;m.offset=p->offset;m.bytes=p->blob->bytes+p->offset;m.length=n;p->offset+=n;}
  }
  if(p->phase==END){m.type=GT_MSG_SNAPSHOT_END;m.sequence=p->sequence;m.total=p->total;p->phase=WAIT_S;}
  if(m.type) {
    if(!send(d,p,c,&m))return GT_SERVER_DATA_CLOSE;
    if(m.type==GT_MSG_SNAPSHOT_END && p->view.result){if(gt__attachment_release(a,&p->view)!=GT_TASK_OK)return GT_SERVER_DATA_CLOSE;p->blob=NULL;}
    return GT_SERVER_DATA_IDLE;
  }
  if(p->phase==NO_STATE && !p->terminal_sent) {m.type=GT_MSG_SESSION_END;m.flags=GT_DATA_NO_SNAPSHOT;m.error=p->final_error;m.terminal=p->terminal;p->terminal_sent=true;return send(d,p,c,&m)?GT_SERVER_DATA_IDLE:GT_SERVER_DATA_CLOSE;}
  if(p->phase!=STREAM)return GT_SERVER_DATA_IDLE;
  gt_attachment_restore_info_t state;if(gt__attachment_restore_info(a,p->scope.generation,&state)!=GT_RESTORE_OK)return GT_SERVER_DATA_CLOSE;
  if(state.phase==GT_RESTORE_DRAINING) {
    uint64_t cut;gt_restore_rc_t live=gt__attachment_live_begin(a,p->scope.generation,&cut);
    if(live==GT_RESTORE_OK){m.type=GT_MSG_LIVE_BEGIN;m.sequence=cut;return send(d,p,c,&m)?GT_SERVER_DATA_IDLE:GT_SERVER_DATA_CLOSE;}
    if(live!=GT_RESTORE_WAIT)return GT_SERVER_DATA_CLOSE;
  }
  gt_session_process_t process;
  if(gt__runtime_process(p->lease,&process) && (!p->process_sent || !gt__process_equal(&p->process,&process))) {
    m.type=GT_MSG_PROCESS_META;m.process=process;
    if(!send(d,p,c,&m))return GT_SERVER_DATA_CLOSE;
    p->process=process;p->process_sent=true;return GT_SERVER_DATA_IDLE;
  }
  gt_restore_event_t event;gt_restore_rc_t next=gt__attachment_next(a,p->scope.generation,&event,d->event,sizeof(d->event));
  if(next==GT_RESTORE_OK) {
    m.type=GT_MSG_STATE_EVENT;m.sequence=event.sequence;m.kind=event.kind;
    if(event.kind==GT_STATE_OUTPUT){m.bytes=d->event;m.length=event.length;}
    else if(event.kind==GT_STATE_RESIZE && event.length==sizeof(m.size))memcpy(&m.size,d->event,sizeof(m.size));
    else if(event.kind==GT_STATE_STYLE && event.length==sizeof(m.style))memcpy(&m.style,d->event,sizeof(m.style));
    else if(event.kind==GT_STATE_COLOR_SCHEME && event.length==sizeof(m.color))memcpy(&m.color,d->event,sizeof(m.color));
    else return GT_SERVER_DATA_CLOSE;
    return send(d,p,c,&m)?GT_SERVER_DATA_IDLE:GT_SERVER_DATA_CLOSE;
  }
  if(next!=GT_RESTORE_WAIT)return GT_SERVER_DATA_CLOSE;
  if(p->has_terminal && !p->terminal_sent && gt__attachment_restore_info(a,p->scope.generation,&state)==GT_RESTORE_OK && state.phase==GT_RESTORE_ENDED && state.dequeued==p->terminal.result.sequence) {
    m.type=GT_MSG_SESSION_END;m.terminal=p->terminal;p->terminal_sent=true;return send(d,p,c,&m)?GT_SERVER_DATA_IDLE:GT_SERVER_DATA_CLOSE;
  }
  return GT_SERVER_DATA_IDLE;
}
gt_server_data_rc_t gt__data_router_step(void *ctx,gt_connection_t *c,const gt_token_scope_t *scope) {
  gt_data_router_t *d=ctx;if(!d || !c || !valid_scope(scope) || !tick(d))return GT_SERVER_DATA_CLOSE;
  gt__data_router_reap(d);data_peer *p=adopt(d,scope);if(!p)return GT_SERVER_DATA_CLOSE;flushed(p,c);
  if(p->phase==FAILED || !gt__attachment_owner_current(d->config.attachments,scope))return gt__data_router_revoked(d,c,scope);
  if(!terminal(p))return GT_SERVER_DATA_CLOSE;
  if(p->phase==FINAL_WAIT) {
    gt_runtime_final_t final;gt_runtime_rc_t ready=gt__runtime_final(p->lease,&final);
    if(ready==GT_RUNTIME_OK)p->phase=START;
    else if(ready!=GT_RUNTIME_BUSY)return GT_SERVER_DATA_CLOSE;
  }
  if(p->phase==START && !start_capture(d,p)){failure(d,p,GT_ERR_INTERNAL,GT_DATA_ERROR_CAPTURE);return gt__data_router_revoked(d,c,scope);}
  if(p->phase==CAPTURING && !busy_capture(d,p)) {
    if(p->capture_error!=GT_OK){
      if(p->capture_error==GT_ERR_CANCELLED && !p->task){p->phase=FINAL_WAIT;p->capture_error=GT_OK;return GT_SERVER_DATA_IDLE;}
      failure(d,p,p->capture_error,GT_DATA_ERROR_CAPTURE);return gt__data_router_revoked(d,c,scope);
    }
    p->phase=ENCODING;
  }
  if(p->error!=GT_OK){failure(d,p,p->error,p->error_stage);return gt__data_router_revoked(d,c,scope);}
  if(p->phase==ENCODING) {
    gt_task_rc_t ready=gt__attachment_take(gt__runtime_attachment(p->lease),scope->generation,&p->view);
    if(ready==GT_TASK_OK){p->blob=p->view.result;p->total=p->blob->length;p->phase=BEGIN;}
    else if(ready!=GT_TASK_BUSY){failure(d,p,GT_ERR_INTERNAL,GT_DATA_ERROR_ENCODE);return gt__data_router_revoked(d,c,scope);}
  }
  gt_attachment_restore_info_t state;
  if(p->phase!=NO_STATE && gt__attachment_restore_info(gt__runtime_attachment(p->lease),scope->generation,&state)==GT_RESTORE_OK && state.phase!=GT_RESTORE_LIVE && state.phase!=GT_RESTORE_ENDED && d->now-p->start>=d->config.restore_timeout_ms) {
    failure(d,p,GT_ERR_CANCELLED,GT_DATA_ERROR_DEADLINE);return gt__data_router_revoked(d,c,scope);
  }
  gt_server_data_rc_t progress=GT_SERVER_DATA_IDLE;gt_wire_frame_t frame;
  if(gt__connection_frame(c,&frame)) {
    if((frame.header.type>=GT_MSG_USER_INPUT && frame.header.type<=GT_MSG_USER_COLOR) || frame.header.type==GT_MSG_USER_STYLE) {
      progress=gt__user_dispatcher_step(d->users,c,scope);if(progress==GT_SERVER_DATA_CLOSE)return progress;
    } else {
      gt_data_message_t m;if(!gt__data_decode(&frame,&m) || m.generation!=scope->generation)return GT_SERVER_DATA_CLOSE;
      if(m.type==GT_MSG_READY_S) {
        if(!p->end_sent || gt__attachment_ready_snapshot(gt__runtime_attachment(p->lease),scope->generation,m.sequence)!=GT_RESTORE_OK)return GT_SERVER_DATA_CLOSE;
        p->phase=STREAM;
      } else if(m.type==GT_MSG_READY_L) {
        if(!p->live_sent || gt__attachment_ready_live(gt__runtime_attachment(p->lease),scope->generation,m.sequence)!=GT_RESTORE_OK)return GT_SERVER_DATA_CLOSE;
      } else if(m.type==GT_MSG_DATA_PING){if(p->pong_pending)return GT_SERVER_DATA_CLOSE;p->pong=m.nonce;p->pong_pending=true;}
      else return GT_SERVER_DATA_CLOSE;
      if(!gt__connection_release_frame(c))return GT_SERVER_DATA_CLOSE;
      progress=GT_SERVER_DATA_PROGRESS;
    }
  } else {
    /* Alternate USER completion and restore output, sharing one connection TX.
     * Caller routed non-USER input first, so this cannot consume a READY frame. */
    p->turn=!p->turn;
    if(p->turn && gt__user_dispatcher_step(d->users,c,scope)==GT_SERVER_DATA_CLOSE)return GT_SERVER_DATA_CLOSE;
  }
  gt_server_data_rc_t sent=output(d,p,c);return sent==GT_SERVER_DATA_CLOSE?sent:progress;
}
gt_runtime_rc_t gt__data_router_close(gt_data_router_t *d) {
  if(!d)return GT_RUNTIME_OK;
  gt__data_router_reap(d);
  for(size_t i=0;i<d->config.connections;i++)if(d->peers[i].lease)return GT_RUNTIME_BUSY;
  if(gt__user_dispatcher_close(d->users)!=GT_RUNTIME_OK)return GT_RUNTIME_BUSY;
  gt_allocator_t a=d->allocator;size_t n=sizeof(*d)+d->config.connections*sizeof(data_peer);
  pthread_mutex_destroy(&d->lock);gt__secure_erase(d->output,d->output_capacity);a.release(a.context,d->output,d->output_capacity);gt__secure_erase(d,n);a.release(a.context,d,n);return GT_RUNTIME_OK;
}
