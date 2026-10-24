#include "session-client.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef enum {CONNECT,MANAGEMENT_HELLO,SEND_REQUEST,WAIT_REPLY,ATTACH_DELAY,DATA_HELLO,ATTACHED,CATALOG,BACKOFF,STOPPED} stage;
struct gt_session_client {
  gt_allocator_t allocator;gt_session_client_config_t config;gt_session_client_info_t info;
  stage stage;uint16_t request;uint64_t now,last_clock,recovery_started,delayed;
  bool recovering,started,create_admitted,reply_ready,cleaning,clock_fault;
  gt_client_bootstrap_t *bootstrap;gt_control_client_t *control;gt_attachment_client_t *attachment;
  gt_control_result_t reply;gt_protocol_bind_t binding;
  bool catalog_known,stop_pending;size_t catalog_count;gt_managed_info_t catalog[GT_CONTROL_MAX_ROWS];
  gt_create_spec_t create;uint8_t create_wire[GT_CREATE_MAX_BYTES];char create_text[GT_CREATE_MAX_BYTES];
};
static void *allocate(void *c,size_t n){(void)c;return malloc(n);}
static void release(void *c,void *p,size_t n){(void)c;(void)n;free(p);}
static bool nonzero(const void *p,size_t n){const uint8_t *s=p;uint8_t b=0;for(size_t i=0;i<n;i++)b|=s[i];return b!=0;}
static bool clock_now(void *ctx,uint64_t *now){gt_session_client_t *c=ctx;if(c->clock_fault || !c->config.clock(c->config.clock_context,now) || *now<c->last_clock){c->clock_fault=true;return false;}c->last_clock=*now;return true;}
static bool stop_blocks(const gt_session_client_t *c) {
  const gt_client_stop_t *s=&c->info.stop;
  return s->session==c->info.session && (s->state==GT_CLIENT_STOP_PENDING || s->state==GT_CLIENT_STOP_UNKNOWN || (s->state==GT_CLIENT_STOP_KNOWN && s->status==GT_CONTROL_OK));
}
static gt_status_t control_error(gt_control_status_t s){return s==GT_CONTROL_OK?GT_OK:s==GT_CONTROL_NOT_FOUND?GT_ERR_NOT_FOUND:s==GT_CONTROL_UNSUPPORTED?GT_ERR_UNSUPPORTED:s==GT_CONTROL_BUSY?GT_ERR_IO:GT_ERR_INTERNAL;}
static void result(void *ctx,const gt_control_result_t *r){
  gt_session_client_t *c=ctx;if(c->cleaning)return;
  if(c->stop_pending && r->request_type==GT_MSG_TERMINATE && r->request_id==c->info.stop.request){
    c->stop_pending=false;c->info.stop.state=r->delivery==GT_CONTROL_CLIENT_REPLY?GT_CLIENT_STOP_KNOWN:GT_CLIENT_STOP_UNKNOWN;
    c->info.stop.status=r->status;c->info.stop.error=r->delivery==GT_CONTROL_CLIENT_REPLY?control_error(r->status):r->error==GT_OK?GT_ERR_IO:r->error;return;
  }
  c->reply=*r;c->reply_ready=true;
}
static void cleanup(gt_session_client_t *c) {
  c->cleaning=true;
  if(c->stop_pending){c->stop_pending=false;c->info.stop.state=GT_CLIENT_STOP_UNKNOWN;c->info.stop.error=c->info.error==GT_OK?GT_ERR_CANCELLED:c->info.error;}
  c->catalog_known=false;c->catalog_count=0;
  gt__attachment_client_close(c->attachment);c->attachment=NULL;
  gt__control_client_close(c->control);c->control=NULL;
  gt__client_bootstrap_close(c->bootstrap);c->bootstrap=NULL;
  if(c->started){c->config.link.reset(c->config.link.context);c->started=false;}
  gt__secure_erase(&c->reply,sizeof(c->reply));gt__secure_erase(&c->binding,sizeof(c->binding));c->reply_ready=false;c->cleaning=false;c->info.has_attachment=false;
}
static void stop(gt_session_client_t *c,gt_status_t error,gt_session_reason_t reason) {
  c->stage=STOPPED;c->info.phase=GT_SESSION_FAILED;c->info.error=error;c->info.reason=reason;cleanup(c);
}
static void retry(gt_session_client_t *c,gt_status_t error,gt_session_reason_t reason) {
  if(error!=GT_ERR_IO && error!=GT_ERR_CANCELLED){stop(c,error==GT_OK?GT_ERR_INTERNAL:error,reason);return;}
  if(!c->recovering){c->recovery_started=c->now;c->recovering=true;}
  c->info.phase=GT_SESSION_RECOVERING;c->info.error=error;c->info.reason=reason;
  cleanup(c);c->delayed=c->now;c->stage=BACKOFF;
}
static bool tick(gt_session_client_t *c) {
  uint64_t now;if(!clock_now(c,&now)){stop(c,GT_ERR_IO,GT_SESSION_REASON_CLOCK);return false;}c->now=now;
  if(c->recovering && now-c->recovery_started>=c->config.recovery_timeout_ms){stop(c,GT_ERR_CANCELLED,GT_SESSION_REASON_DEADLINE);return false;}return true;
}
static bool begin_bootstrap(gt_session_client_t *c,bool data) {
  gt_stream_ops_t ops={0};gt_status_t e=c->config.link.stream(c->config.link.context,data?1:0,&ops);
  if(e!=GT_OK){retry(c,e,GT_SESSION_REASON_LINK);return false;}
  gt_connection_t *connection=NULL;
  gt_connection_open_t rc=gt__connection_open(&ops,(gt_connection_limits_t){65560,65560,c->config.bootstrap_timeout_ms},&c->allocator,&connection);
  if(rc!=GT_CONNECTION_CREATED){if(ops.close)ops.close(ops.context);stop(c,rc==GT_CONNECTION_INVALID?GT_ERR_INVALID_ARG:GT_ERR_IO,GT_SESSION_REASON_LOCAL);return false;}
  gt_client_bootstrap_config_t config={.hello={.role=data?GT_ROLE_DATA:GT_ROLE_MANAGEMENT,.required_features=c->config.required_features},
    .timeout_ms=c->config.bootstrap_timeout_ms,.clock_context=c,.clock=clock_now};
  memcpy(config.hello.compatibility,c->config.compatibility,32);memcpy(config.hello.expected_instance,c->info.instance,16);
  if(data)config.binding=c->binding;
  e=gt__client_bootstrap_open(connection,&config,&c->allocator,&c->bootstrap);gt__secure_erase(&config.binding,sizeof(config.binding));
  if(e!=GT_OK){gt__connection_destroy(connection);stop(c,e,GT_SESSION_REASON_LOCAL);return false;}
  if(data)gt__secure_erase(&c->binding,sizeof(c->binding));
  c->stage=data?DATA_HELLO:MANAGEMENT_HELLO;return true;
}
static bool bootstrap(gt_session_client_t *c,bool data) {
  gt_bootstrap_state_t phase=gt__client_bootstrap_step(c->bootstrap);
  if(phase==GT_BOOTSTRAP_FAILED) {
    gt_bootstrap_info_t info;gt__client_bootstrap_info(c->bootstrap,&info);
    if(info.error==GT_BOOTSTRAP_IO || info.error==GT_BOOTSTRAP_TIMEOUT)retry(c,GT_ERR_IO,GT_SESSION_REASON_BOOTSTRAP);
    else stop(c,info.error==GT_BOOTSTRAP_COMPATIBILITY || info.error==GT_BOOTSTRAP_FEATURES?GT_ERR_UNSUPPORTED:
      info.error==GT_BOOTSTRAP_DENIED?GT_ERR_AUTH:info.error==GT_BOOTSTRAP_CLOCK?GT_ERR_IO:GT_ERR_PROTOCOL,GT_SESSION_REASON_BOOTSTRAP);
    return false;
  }
  if(phase!=GT_BOOTSTRAP_READY)return false;
  gt_connection_t *connection=NULL;gt_bootstrap_result_t identity;
  if(!gt__client_bootstrap_take(c->bootstrap,&connection,&identity)){stop(c,GT_ERR_INTERNAL,GT_SESSION_REASON_LOCAL);return false;}
  gt__client_bootstrap_close(c->bootstrap);c->bootstrap=NULL;
  gt_status_t e;
  if(!data) {
    memcpy(c->info.instance,identity.identity.instance,16);
    gt_control_client_config_t control={.heartbeat_ms=c->config.management_heartbeat_ms,.request_timeout_ms=c->config.management_timeout_ms,.context=c,.clock=clock_now,.result=result};
    memcpy(control.instance,c->info.instance,16);e=gt__control_client_open(connection,&control,&c->allocator,&c->control);
    c->request=c->config.catalog_only?GT_MSG_LIST:c->info.session?GT_MSG_INFO:c->create_admitted?GT_MSG_QUERY_CREATE:GT_MSG_CREATE;c->stage=SEND_REQUEST;
  } else {
    if(identity.session!=c->info.session || identity.generation!=c->info.generation){gt__connection_destroy(connection);stop(c,GT_ERR_PROTOCOL,GT_SESSION_REASON_BOOTSTRAP);return false;}
    gt_client_config_t attachment=c->config.attachment;attachment.session=c->info.session;attachment.generation=c->info.generation;attachment.clock_context=c;attachment.clock=clock_now;
    e=gt__attachment_client_open(connection,&attachment,&c->allocator,&c->attachment);c->stage=ATTACHED;c->info.phase=GT_SESSION_RESTORING;
  }
  if(e!=GT_OK){gt__connection_destroy(connection);stop(c,e,GT_SESSION_REASON_LOCAL);return false;}
  return true;
}
static void send_request(gt_session_client_t *c) {
  gt_control_request_t r={.type=c->request,.session=c->info.session};
  if(c->request==GT_MSG_CREATE)r.create=&c->create;
  if(c->request==GT_MSG_QUERY_CREATE)memcpy(r.nonce,c->create.nonce,16);
  uint64_t id;gt_status_t e;gt_control_submit_t result_code=gt__control_client_submit(c->control,&r,&id,&e);
  if(result_code==GT_CONTROL_CLIENT_BACKPRESSURE)return;
  if(result_code!=GT_CONTROL_CLIENT_QUEUED){stop(c,e==GT_OK?GT_ERR_INTERNAL:e,GT_SESSION_REASON_CONTROL);return;}
  if(c->request==GT_MSG_CREATE)c->create_admitted=true;
  c->stage=WAIT_REPLY;
}
static void reply(gt_session_client_t *c) {
  gt_control_result_t r=c->reply;c->reply_ready=false;gt__secure_erase(&c->reply,sizeof(c->reply));
  if(r.delivery!=GT_CONTROL_CLIENT_REPLY){retry(c,r.error,GT_SESSION_REASON_CONTROL);goto done;}
  if(r.reply_type==GT_MSG_CONTROL_ERROR){stop(c,r.status==GT_CONTROL_UNSUPPORTED?GT_ERR_UNSUPPORTED:GT_ERR_INTERNAL,GT_SESSION_REASON_CONTROL);goto done;}
  if(c->request==GT_MSG_LIST){
    if(r.status!=GT_CONTROL_OK){stop(c,control_error(r.status),GT_SESSION_REASON_CONTROL);goto done;}
    if(r.count>GT_CONTROL_MAX_ROWS){stop(c,GT_ERR_PROTOCOL,GT_SESSION_REASON_CONTROL);goto done;}
    c->catalog_count=r.count;memcpy(c->catalog,r.rows,r.count*sizeof(*c->catalog));c->catalog_known=true;
    c->stage=CATALOG;c->info.phase=GT_SESSION_CATALOG;c->info.error=GT_OK;c->info.reason=GT_SESSION_REASON_NONE;c->recovering=false;
  } else if(c->request==GT_MSG_CREATE || c->request==GT_MSG_QUERY_CREATE) {
    if(r.creation.status!=GT_CREATION_ACCEPTED){stop(c,r.creation.detail!=GT_OK?r.creation.detail:r.creation.status==GT_CREATION_UNKNOWN?GT_ERR_NOT_FOUND:GT_ERR_INTERNAL,
      r.creation.status==GT_CREATION_UNKNOWN?GT_SESSION_REASON_CREATE_UNKNOWN:GT_SESSION_REASON_CONTROL);goto done;}
    c->info.session=r.creation.session;c->request=GT_MSG_ATTACH;c->stage=SEND_REQUEST;
  } else if(c->request==GT_MSG_INFO) {
    if(r.status!=GT_CONTROL_OK){stop(c,r.status==GT_CONTROL_NOT_FOUND?GT_ERR_NOT_FOUND:r.status==GT_CONTROL_UNSUPPORTED?GT_ERR_UNSUPPORTED:GT_ERR_INTERNAL,
      r.status==GT_CONTROL_NOT_FOUND?GT_SESSION_REASON_NOT_FOUND:GT_SESSION_REASON_CONTROL);goto done;}
    c->request=GT_MSG_ATTACH;c->stage=SEND_REQUEST;
  } else {
    gt_attach_result_t *a=&r.attachment;
    if(a->status==GT_ATTACH_BUSY){c->info.busy_replies++;c->stage=ATTACH_DELAY;c->delayed=c->now;goto done;}
    if(a->status!=GT_ATTACH_OK){stop(c,a->detail!=GT_OK?a->detail:a->status==GT_ATTACH_NOT_FOUND?GT_ERR_NOT_FOUND:GT_ERR_INTERNAL,
      a->status==GT_ATTACH_NOT_FOUND?GT_SESSION_REASON_NOT_FOUND:GT_SESSION_REASON_CONTROL);goto done;}
    if(a->generation<=c->info.generation){stop(c,GT_ERR_PROTOCOL,GT_SESSION_REASON_CONTROL);goto done;}
    c->info.generation=a->generation;c->binding=(gt_protocol_bind_t){.session=c->info.session,.generation=a->generation,.token=a->token};memcpy(c->binding.instance,c->info.instance,16);
    (void)begin_bootstrap(c,true);
  }
done:gt__secure_erase(&r,sizeof(r));
}
gt_status_t gt__session_client_open(const gt_session_client_config_t *config,const gt_allocator_t *allocator,gt_session_client_t **out) {
  if(out)*out=NULL;
  if(!config || !out || !config->clock || !config->link.step || !config->link.stream || !config->link.reset ||
    !nonzero(config->compatibility,32) ||
    ((config->create?1:0)+(config->session?1:0)+(nonzero(config->pending_nonce,16)?1:0)+(config->catalog_only?1:0)!=1) ||
    ((config->session || nonzero(config->pending_nonce,16)) && !nonzero(config->instance,16)) ||
    !config->recovery_timeout_ms || config->recovery_timeout_ms>600000 || !config->backoff_ms || config->backoff_ms>5000 ||
    !config->bootstrap_timeout_ms || config->bootstrap_timeout_ms>60000 || !config->management_heartbeat_ms || config->management_heartbeat_ms>60000 ||
    !config->management_timeout_ms || config->management_timeout_ms>60000)return GT_ERR_INVALID_ARG;
  const gt_client_config_t *a=&config->attachment;
  if(!config->catalog_only && (!a->format || !a->snapshot_limit || a->snapshot_limit>64*1024*1024 || !a->commands || a->commands>16 ||
    !a->restore_timeout_ms || a->restore_timeout_ms>600000 || !a->apply_timeout_ms || a->apply_timeout_ms>60000 ||
    !a->heartbeat_ms || !a->peer_timeout_ms || a->heartbeat_ms>=a->peer_timeout_ms || a->peer_timeout_ms>60000 ||
    !a->command_timeout_ms || a->command_timeout_ms>60000 || !a->error_timeout_ms || a->error_timeout_ms>60000 ||
    !a->sink.snapshot || !a->sink.event || !a->sink.applied || !a->sink.invalidated || !a->sink.result))return GT_ERR_INVALID_ARG;
  gt_allocator_t domain=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!domain.allocate || !domain.release)return GT_ERR_INVALID_ARG;
  uint64_t now;if(!config->clock(config->clock_context,&now))return GT_ERR_IO;
  gt_session_client_t *c=domain.allocate(domain.context,sizeof(*c));if(!c)return GT_ERR_IO;
  memset(c,0,sizeof(*c));c->allocator=domain;c->config=*config;c->config.create=NULL;c->now=c->last_clock=c->recovery_started=now;c->recovering=true;
  c->info.phase=GT_SESSION_STARTING;c->info.session=config->session;memcpy(c->info.instance,config->instance,16);
  if(nonzero(config->pending_nonce,16)){memcpy(c->create.nonce,config->pending_nonce,16);c->create_admitted=true;}
  gt_status_t e=GT_OK;size_t n=0;
  if(config->create){e=gt__create_spec_encode(config->create,c->create_wire,sizeof(c->create_wire),&n);if(e==GT_OK)e=gt__create_spec_decode(c->create_wire,n,&c->create,c->create_text,sizeof(c->create_text));}
  if(e!=GT_OK){gt__secure_erase(c,sizeof(*c));domain.release(domain.context,c,sizeof(*c));return e;}
  *out=c;return GT_OK;
}
gt_session_phase_t gt__session_client_step(gt_session_client_t *c) {
  if(!c)return GT_SESSION_FAILED;
  if(c->stage==STOPPED || !tick(c))return c->info.phase;
  if(c->stage==BACKOFF){if(c->now-c->delayed<c->config.backoff_ms)return c->info.phase;c->stage=CONNECT;}
  bool start=!c->started;
  if(start){if(c->info.attempts==UINT32_MAX){stop(c,GT_ERR_INTERNAL,GT_SESSION_REASON_LOCAL);return c->info.phase;}c->info.attempts++;c->started=true;}
  gt_status_t error=GT_OK;gt_session_link_state_t link=c->config.link.step(c->config.link.context,start,c->info.attempts,&error);
  if(!tick(c))return c->info.phase;
  if(link==GT_SESSION_LINK_FATAL){stop(c,error==GT_OK?GT_ERR_INTERNAL:error,GT_SESSION_REASON_LINK);return c->info.phase;}
  if(link==GT_SESSION_LINK_RETRY){retry(c,error,GT_SESSION_REASON_LINK);return c->info.phase;}
  if(error!=GT_OK || (link!=GT_SESSION_LINK_WAIT && link!=GT_SESSION_LINK_READY) || (link==GT_SESSION_LINK_WAIT && c->stage!=CONNECT)){stop(c,GT_ERR_INTERNAL,GT_SESSION_REASON_LINK);return c->info.phase;}
  if(link==GT_SESSION_LINK_WAIT)return c->info.phase;
  gt_control_client_state_t control=c->control?gt__control_client_step(c->control):GT_CONTROL_CLIENT_READY;
  if(!tick(c))return c->info.phase;
  if(control==GT_CONTROL_CLIENT_FAILED){retry(c,gt__control_client_error(c->control),GT_SESSION_REASON_CONTROL);return c->info.phase;}
  switch(c->stage) {
    case CONNECT:(void)begin_bootstrap(c,false);break;
    case MANAGEMENT_HELLO:(void)bootstrap(c,false);break;
    case SEND_REQUEST:send_request(c);break;
    case WAIT_REPLY:if(c->reply_ready)reply(c);break;
    case ATTACH_DELAY:if(c->now-c->delayed>=c->config.backoff_ms){c->request=GT_MSG_ATTACH;c->stage=SEND_REQUEST;}break;
    case DATA_HELLO:(void)bootstrap(c,true);break;
    case ATTACHED: {
      gt_client_phase_t phase=gt__attachment_client_step(c->attachment);gt_client_info_t info;gt__attachment_client_info(c->attachment,&info);
      c->info.attachment=info;c->info.has_attachment=true;
      if(phase==GT_CLIENT_FAILED) {
        if(info.reason==GT_CLIENT_REASON_IO || info.reason==GT_CLIENT_REASON_PEER_DEADLINE || info.reason==GT_CLIENT_REASON_COMMAND_DEADLINE)retry(c,GT_ERR_IO,GT_SESSION_REASON_ATTACHMENT);
        else stop(c,info.error,GT_SESSION_REASON_ATTACHMENT);
      } else if(phase==GT_CLIENT_LIVE || phase==GT_CLIENT_READONLY || phase==GT_CLIENT_ENDED) {
        c->info.phase=phase==GT_CLIENT_LIVE?GT_SESSION_LIVE:phase==GT_CLIENT_READONLY?GT_SESSION_READONLY:GT_SESSION_ENDED;
        c->info.error=GT_OK;c->info.reason=GT_SESSION_REASON_NONE;c->recovering=false;
      }
      break;
    }
    default:break;
  }
  if(c->stage!=STOPPED)(void)tick(c);
  return c->info.phase;
}
bool gt__session_client_info(const gt_session_client_t *c,gt_session_client_info_t *out){if(out)memset(out,0,sizeof(*out));if(!c || !out)return false;*out=c->info;out->has_admitted_create=c->create_admitted;out->input_locked=c->info.phase!=GT_SESSION_LIVE || c->info.attachment.readonly || stop_blocks(c);return true;}
gt_client_submit_t gt__session_client_submit(gt_session_client_t *c,const gt_user_request_t *request,gt_worker_stamp_t *stamp) {
  if(stamp)memset(stamp,0,sizeof(*stamp));
  if(!c || !request || !stamp)return GT_CLIENT_INVALID;
  if(c->info.phase!=GT_SESSION_LIVE || c->stage!=ATTACHED || stop_blocks(c))return GT_CLIENT_LOCKED;
  return gt__attachment_client_submit(c->attachment,request,stamp);
}
gt_client_submit_t gt__session_client_submit_at(gt_session_client_t *c,uint64_t generation,uint64_t sequence,const gt_user_request_t *request,gt_worker_stamp_t *stamp){
  if(stamp)memset(stamp,0,sizeof(*stamp));
  if(!c || !request || !stamp)return GT_CLIENT_INVALID;
  if(c->info.phase!=GT_SESSION_LIVE || c->stage!=ATTACHED || stop_blocks(c))return GT_CLIENT_LOCKED;
  return gt__attachment_client_submit_at(c->attachment,generation,sequence,request,stamp);
}
gt_status_t gt__session_client_catalog(const gt_session_client_t *c,gt_managed_info_t *rows,size_t capacity,size_t *count){
  if(count)*count=0;
  if(!c || !count || capacity>GT_CONTROL_MAX_ROWS || (capacity && !rows))return GT_ERR_INVALID_ARG;
  if(!c->config.catalog_only)return GT_ERR_UNSUPPORTED;
  if(!c->catalog_known)return GT_ERR_NOT_FOUND;
  *count=c->catalog_count;if(capacity<c->catalog_count)return GT_ERR_INVALID_ARG;
  if(c->catalog_count)memcpy(rows,c->catalog,c->catalog_count*sizeof(*rows));
  return GT_OK;
}
gt_client_submit_t gt__session_client_catalog_refresh(gt_session_client_t *c){
  if(!c)return GT_CLIENT_INVALID;
  if(!c->config.catalog_only || c->stage!=CATALOG)return GT_CLIENT_LOCKED;
  if(c->stop_pending)return GT_CLIENT_BACKPRESSURE;
  if(!tick(c))return GT_CLIENT_UNAVAILABLE;
  c->catalog_known=false;c->catalog_count=0;c->request=GT_MSG_LIST;c->stage=SEND_REQUEST;
  c->info.phase=GT_SESSION_STARTING;c->recovering=true;c->recovery_started=c->last_clock;return GT_CLIENT_QUEUED;
}
gt_client_submit_t gt__session_client_terminate(gt_session_client_t *c,uint64_t session){
  if(!c || !session)return GT_CLIENT_INVALID;
  if(c->stage!=ATTACHED && c->stage!=CATALOG)return GT_CLIENT_LOCKED;
  if(c->stop_pending)return GT_CLIENT_BACKPRESSURE;
  gt_control_request_t r={.type=GT_MSG_TERMINATE,.session=session};uint64_t id=0;gt_status_t error=GT_OK;
  gt_control_submit_t rc=gt__control_client_submit(c->control,&r,&id,&error);
  if(rc==GT_CONTROL_CLIENT_BACKPRESSURE)return GT_CLIENT_BACKPRESSURE;
  if(rc!=GT_CONTROL_CLIENT_QUEUED)return rc==GT_CONTROL_CLIENT_INVALID?GT_CLIENT_INVALID:GT_CLIENT_UNAVAILABLE;
  c->info.stop=(gt_client_stop_t){.state=GT_CLIENT_STOP_PENDING,.session=session,.request=id};c->stop_pending=true;return GT_CLIENT_QUEUED;
}
void gt__session_client_cancel(gt_session_client_t *c){if(!c || c->stage==STOPPED)return;c->stage=STOPPED;c->info.phase=GT_SESSION_CLOSED;c->info.error=GT_ERR_CANCELLED;c->info.reason=GT_SESSION_REASON_CANCELLED;cleanup(c);}
void gt__session_client_close(gt_session_client_t *c){if(!c)return;gt__session_client_cancel(c);gt_allocator_t a=c->allocator;gt__secure_erase(c,sizeof(*c));a.release(a.context,c,sizeof(*c));}
