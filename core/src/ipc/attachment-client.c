#include "attachment-client.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef enum {WAIT_BEGIN,CHUNKS,IMPORTING,WAIT_S,WAIT_CUT,WAIT_L,READY_L_PENDING,STREAM,NO_STATE,ERROR_DRAIN,FAILED} stage;
typedef struct {bool used;gt_worker_command_t kind;gt_worker_stamp_t stamp;uint64_t started;} command;
struct gt_attachment_client {
  gt_allocator_t allocator;gt_client_config_t config;gt_connection_t *connection;
  gt_client_info_t info;stage stage;uint16_t marker;
  bool invalidated,submitted_known,ready_s_sent,terminal_pending,applying,blocked,ping_pending,pong_pending,error_ack_sent;
  uint64_t now,start,snapshot,submitted,cut,total,offset,apply_started,blocked_started,
    last_ping,ping_started,ping_nonce,pong_nonce,last_id,error_started;
  uint8_t *snapshot_bytes;command commands[16];uint8_t tx[GT_USER_PREFIX_BYTES+GT_USER_MAX_INPUT];
  gt_data_message_t server_error;
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static void free_snapshot(gt_attachment_client_t *c) {
  if(c->snapshot_bytes){gt__secure_erase(c->snapshot_bytes,(size_t)c->total);c->allocator.release(c->allocator.context,c->snapshot_bytes,(size_t)c->total);c->snapshot_bytes=NULL;}
}
static void invalidate(gt_attachment_client_t *c,gt_status_t error) {
  if(c->invalidated)return;
  c->invalidated=true;c->config.sink.invalidated(c->config.sink.context,c->config.generation,error);
}
static void unknown(gt_attachment_client_t *c,gt_status_t error) {
  for(size_t i=0;i<c->config.commands;i++)if(c->commands[i].used) {
    command *p=&c->commands[i];gt_user_reply_t reply={.kind=p->kind,.stamp=p->stamp,.status=error,.delivery=GT_USER_OUTCOME_UNKNOWN};
    memset(p,0,sizeof(*p));c->config.sink.result(c->config.sink.context,&reply);
  }
}
static void fail(gt_attachment_client_t *c,gt_status_t error,gt_client_reason_t reason) {
  if(c->stage==FAILED)return;
  c->stage=FAILED;c->info.phase=GT_CLIENT_FAILED;c->info.error=error;c->info.reason=reason;c->info.readonly=true;
  gt__connection_cancel(c->connection);free_snapshot(c);invalidate(c,error);unknown(c,error);
}
static bool tick(gt_attachment_client_t *c) {
  uint64_t now;
  if(!c->config.clock(c->config.clock_context,&now) || now<c->now){fail(c,GT_ERR_IO,GT_CLIENT_REASON_CLOCK);return false;}
  c->now=now;
  if(c->stage==FAILED)return false;
  if(c->stage==ERROR_DRAIN) {
    if(now-c->error_started>=c->config.error_timeout_ms){fail(c,c->info.error,GT_CLIENT_REASON_SERVER);return false;}
    return true;
  }
  if(c->info.phase==GT_CLIENT_RESTORING && now-c->start>=c->config.restore_timeout_ms){fail(c,GT_ERR_CANCELLED,GT_CLIENT_REASON_RESTORE_DEADLINE);return false;}
  if((c->applying && now-c->apply_started>=c->config.apply_timeout_ms) ||
    (c->blocked && now-c->blocked_started>=c->config.apply_timeout_ms)){fail(c,GT_ERR_CANCELLED,GT_CLIENT_REASON_APPLY_DEADLINE);return false;}
  if(c->ping_pending && now-c->ping_started>=c->config.peer_timeout_ms){fail(c,GT_ERR_IO,GT_CLIENT_REASON_PEER_DEADLINE);return false;}
  for(size_t i=0;i<c->config.commands;i++)if(c->commands[i].used && now-c->commands[i].started>=c->config.command_timeout_ms) {
    fail(c,GT_ERR_IO,GT_CLIENT_REASON_COMMAND_DEADLINE);return false;
  }
  return true;
}
static void flushed(gt_attachment_client_t *c) {
  if(!c->marker || gt__connection_wants_write(c->connection))return;
  uint16_t marker=c->marker;c->marker=0;
  if(marker==GT_MSG_READY_S)c->ready_s_sent=true;
  else if(marker==GT_MSG_READY_L){c->stage=STREAM;c->info.phase=c->info.readonly?GT_CLIENT_READONLY:GT_CLIENT_LIVE;}
  else if(marker==GT_MSG_RESTORE_ERROR_ACK)c->error_ack_sent=true;
}
static bool poll_applied(gt_attachment_client_t *c) {
  if(!c->submitted_known || c->stage==ERROR_DRAIN)return true;
  gt_client_applied_t a={0};gt_status_t rc=c->config.sink.applied(c->config.sink.context,&a);
  if(rc!=GT_OK){fail(c,rc,GT_CLIENT_REASON_SINK);return false;}
  if(!tick(c))return false;
  if(a.has_sequence && a.generation<c->config.generation)return true;
  if(!a.has_sequence) {
    if(c->info.has_sequence || a.generation || a.sequence){fail(c,GT_ERR_PROTOCOL,GT_CLIENT_REASON_SINK);return false;}
    return true;
  }
  if(a.generation!=c->config.generation || a.sequence>c->submitted || (c->info.has_sequence && a.sequence<c->info.sequence)) {
    fail(c,GT_ERR_PROTOCOL,GT_CLIENT_REASON_SINK);return false;
  }
  bool progress=!c->info.has_sequence || a.sequence>c->info.sequence;
  c->info.has_sequence=true;c->info.sequence=a.sequence;
  c->applying=a.sequence<c->submitted;if(c->applying && progress)c->apply_started=c->now;
  if(c->terminal_pending && a.sequence==c->info.terminal.result.sequence){c->terminal_pending=false;c->info.has_terminal=true;c->info.phase=GT_CLIENT_ENDED;}
  return true;
}
static bool admission(gt_attachment_client_t *c,gt_client_sink_rc_t rc,gt_status_t error) {
  if(rc==GT_CLIENT_SINK_FAILED && error>GT_OK && error<=GT_ERR_INTERNAL){fail(c,error,GT_CLIENT_REASON_SINK);return false;}
  if((rc!=GT_CLIENT_SINK_ACCEPTED && rc!=GT_CLIENT_SINK_BUSY) || error!=GT_OK){fail(c,GT_ERR_INTERNAL,GT_CLIENT_REASON_SINK);return false;}
  if(!tick(c))return false;
  if(rc==GT_CLIENT_SINK_BUSY){if(!c->blocked){c->blocked=true;c->blocked_started=c->now;}return false;}
  c->blocked=false;return true;
}
static bool import_snapshot(gt_attachment_client_t *c) {
  if(c->stage!=IMPORTING)return true;
  gt_status_t error=GT_OK;gt_client_sink_rc_t rc=c->config.sink.snapshot(c->config.sink.context,c->config.generation,c->snapshot,
    c->config.format,c->snapshot_bytes,(size_t)c->total,c->info.readonly,&error);
  if(!admission(c,rc,error))return c->stage!=FAILED;
  c->submitted_known=true;c->submitted=c->snapshot;c->applying=true;c->apply_started=c->now;
  free_snapshot(c);c->stage=WAIT_S;return true;
}
static bool result(gt_attachment_client_t *c,const gt_wire_frame_t *frame) {
  gt_user_reply_t reply;if(!gt__user_reply_decode(frame,&reply) || reply.stamp.generation!=c->config.generation)return false;
  for(size_t i=0;i<c->config.commands;i++) {
    command *p=&c->commands[i];if(!p->used || p->stamp.command_id!=reply.stamp.command_id)continue;
    if(p->kind!=reply.kind || p->stamp.applied_sequence!=reply.stamp.applied_sequence)return false;
    memset(p,0,sizeof(*p));c->config.sink.result(c->config.sink.context,&reply);return true;
  }
  return false;
}
static bool receive(gt_attachment_client_t *c) {
  gt_wire_frame_t frame;if(!gt__connection_frame(c->connection,&frame))return true;
  if(frame.header.type==GT_MSG_USER_RESULT || frame.header.type==GT_MSG_USER_REJECT) {
    if(!result(c,&frame))return false;
    return gt__connection_release_frame(c->connection);
  }
  gt_data_message_t m;if(!gt__data_decode(&frame,&m) || m.generation!=c->config.generation)return false;
  if(m.type==GT_MSG_DATA_PONG) {
    if(!c->ping_pending || m.nonce!=c->ping_nonce)return false;
    c->ping_pending=false;c->last_ping=c->now;
  } else if(m.type==GT_MSG_DATA_PING) {
    if(c->pong_pending)return false;
    c->pong_pending=true;c->pong_nonce=m.nonce;
  } else if(m.type==GT_MSG_RESTORE_ERROR) {
    c->server_error=m;c->error_started=c->now;c->stage=ERROR_DRAIN;c->info.phase=GT_CLIENT_CLOSING;
    c->info.error=m.error;c->info.reason=GT_CLIENT_REASON_SERVER;c->info.readonly=true;
    free_snapshot(c);invalidate(c,m.error);unknown(c,m.error);
  } else if(m.type==GT_MSG_SNAPSHOT_BEGIN) {
    if(c->stage!=WAIT_BEGIN || m.total>c->config.snapshot_limit)return false;
    if(m.format!=c->config.format){fail(c,GT_ERR_UNSUPPORTED,GT_CLIENT_REASON_PROTOCOL);return true;}
    c->total=m.total;c->snapshot=m.sequence;c->info.readonly=(m.flags&GT_DATA_READONLY)!=0;
    c->snapshot_bytes=c->allocator.allocate(c->allocator.context,(size_t)m.total);
    if(!c->snapshot_bytes){fail(c,GT_ERR_IO,GT_CLIENT_REASON_SINK);return true;}
    c->stage=CHUNKS;
  } else if(m.type==GT_MSG_SNAPSHOT_CHUNK) {
    if(c->stage!=CHUNKS || m.offset!=c->offset || m.length>c->total-c->offset)return false;
    memcpy(c->snapshot_bytes+(size_t)c->offset,m.bytes,m.length);c->offset+=m.length;
  } else if(m.type==GT_MSG_SNAPSHOT_END) {
    if(c->stage!=CHUNKS || c->offset!=c->total || m.total!=c->total || m.sequence!=c->snapshot)return false;
    c->stage=IMPORTING;
  } else if(m.type==GT_MSG_PROCESS_META) {
    if(!c->ready_s_sent || (c->stage!=WAIT_CUT && c->stage!=WAIT_L && c->stage!=READY_L_PENDING && c->stage!=STREAM) || c->info.has_terminal)return false;
    c->info.process=m.process;c->info.has_process=true;
  } else if(m.type==GT_MSG_STATE_EVENT) {
    if(!c->ready_s_sent || (c->stage!=WAIT_CUT && c->stage!=WAIT_L && c->stage!=READY_L_PENDING && c->stage!=STREAM) ||
      c->terminal_pending || c->info.has_terminal || c->submitted==UINT64_MAX || m.sequence!=c->submitted+1)return false;
    gt_state_event_t event={.kind=m.kind,.payload=m.bytes,.length=m.length};
    if(m.kind==GT_STATE_RESIZE){event.payload=&m.size;event.length=sizeof(m.size);}
    else if(m.kind==GT_STATE_STYLE){event.payload=&m.style;event.length=sizeof(m.style);}
    else if(m.kind==GT_STATE_COLOR_SCHEME){event.payload=&m.color;event.length=sizeof(m.color);}
    gt_status_t error=GT_OK;gt_client_sink_rc_t rc=c->config.sink.event(c->config.sink.context,c->config.generation,m.sequence,&event,&error);
    if(!admission(c,rc,error))return c->stage!=FAILED;
    if(!c->applying){c->applying=true;c->apply_started=c->now;}
    c->submitted=m.sequence;
  } else if(m.type==GT_MSG_LIVE_BEGIN) {
    if(c->stage!=WAIT_CUT || !c->ready_s_sent || m.sequence!=c->submitted)return false;
    c->cut=m.sequence;c->stage=WAIT_L;
  } else if(m.type==GT_MSG_SESSION_END) {
    if(m.terminal.id!=c->config.session || c->info.has_terminal || c->terminal_pending)return false;
    if(m.flags&GT_DATA_NO_SNAPSHOT) {
      if(c->stage!=WAIT_BEGIN)return false;
      c->stage=NO_STATE;c->info.has_terminal=true;c->info.phase=GT_CLIENT_ENDED;c->info.state_error=m.error;
    } else {
      if(c->stage!=STREAM || m.terminal.result.sequence!=c->submitted)return false;
      c->terminal_pending=true;
    }
    c->info.terminal=m.terminal;c->info.readonly=true;
  } else return false;
  if(c->stage==FAILED)return true;
  return gt__connection_release_frame(c->connection);
}
static bool send_message(gt_attachment_client_t *c,gt_data_message_t m) {
  gt_wire_header_t h;m.generation=c->config.generation;
  if(!gt__data_encode(&m,c->tx,sizeof(c->tx),&h) || gt__connection_send(c->connection,&h,c->tx)!=GT_SEND_ACCEPTED) {
    fail(c,GT_ERR_IO,GT_CLIENT_REASON_IO);return false;
  }
  gt__secure_erase(c->tx,h.payload_length);return true;
}
static void output(gt_attachment_client_t *c) {
  if(gt__connection_wants_write(c->connection))return;
  gt_data_message_t m={0};
  if(c->stage==ERROR_DRAIN){if(c->error_ack_sent)return;m=c->server_error;m.type=GT_MSG_RESTORE_ERROR_ACK;c->marker=m.type;}
  else if(c->stage==WAIT_S && c->info.has_sequence && c->info.sequence>=c->snapshot) {
    m.type=GT_MSG_READY_S;m.sequence=c->snapshot;c->marker=m.type;c->stage=WAIT_CUT;
  } else if(c->stage==WAIT_L && c->info.has_sequence && c->info.sequence>=c->cut) {
    m.type=GT_MSG_READY_L;m.sequence=c->cut;c->marker=m.type;c->stage=READY_L_PENDING;
  } else if(c->pong_pending){m.type=GT_MSG_DATA_PONG;m.nonce=c->pong_nonce;c->pong_pending=false;}
  else if(!c->ping_pending && c->now-c->last_ping>=c->config.heartbeat_ms) {
    if(c->ping_nonce==UINT64_MAX){fail(c,GT_ERR_PROTOCOL,GT_CLIENT_REASON_PROTOCOL);return;}
    m.type=GT_MSG_DATA_PING;m.nonce=++c->ping_nonce;c->ping_pending=true;c->ping_started=c->now;
  }
  if(m.type)(void)send_message(c,m);
}
gt_status_t gt__attachment_client_open(gt_connection_t *connection,const gt_client_config_t *config,
  const gt_allocator_t *allocator,gt_attachment_client_t **out) {
  if(out)*out=NULL;
  gt_connection_limits_t limits={0};
  if(!out || !connection || !config || !config->session || !config->generation || !config->format ||
    !config->snapshot_limit || config->snapshot_limit>64*1024*1024 || !config->commands || config->commands>16 ||
    !config->restore_timeout_ms || config->restore_timeout_ms>600000 ||
    !config->apply_timeout_ms || config->apply_timeout_ms>60000 || !config->heartbeat_ms || config->heartbeat_ms>60000 ||
    !config->peer_timeout_ms || config->peer_timeout_ms>60000 || config->heartbeat_ms>=config->peer_timeout_ms ||
    !config->command_timeout_ms || config->command_timeout_ms>60000 || !config->error_timeout_ms || config->error_timeout_ms>60000 ||
    !config->clock || !config->sink.snapshot || !config->sink.event || !config->sink.applied || !config->sink.invalidated || !config->sink.result ||
    gt__connection_state(connection)!=GT_CONNECTION_OPEN || gt__connection_wants_write(connection) ||
    !gt__connection_limits(connection,&limits) || limits.receive_limit<65552 || limits.send_limit<65560)return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  uint64_t now;if(!config->clock(config->clock_context,&now))return GT_ERR_IO;
  gt_attachment_client_t *c=a.allocate(a.context,sizeof(*c));if(!c)return GT_ERR_IO;
  memset(c,0,sizeof(*c));c->allocator=a;c->config=*config;c->connection=connection;c->now=c->start=c->last_ping=now;
  c->stage=WAIT_BEGIN;c->info.phase=GT_CLIENT_RESTORING;*out=c;return GT_OK;
}
gt_client_phase_t gt__attachment_client_step(gt_attachment_client_t *c) {
  if(!c)return GT_CLIENT_FAILED;
  if(!tick(c))return c->info.phase;
  gt_connection_state_t state=gt__connection_step(c->connection);
  if(state!=GT_CONNECTION_OPEN){
    if(c->stage==ERROR_DRAIN)fail(c,c->info.error,GT_CLIENT_REASON_SERVER);
    else if(state==GT_CONNECTION_PROTOCOL_ERROR)fail(c,GT_ERR_PROTOCOL,GT_CLIENT_REASON_PROTOCOL);
    else if(state==GT_CONNECTION_CLOCK_ERROR)fail(c,GT_ERR_IO,GT_CLIENT_REASON_CLOCK);
    else if(state==GT_CONNECTION_TIMEOUT)fail(c,GT_ERR_CANCELLED,GT_CLIENT_REASON_IO);
    else fail(c,GT_ERR_IO,GT_CLIENT_REASON_IO);
    return c->info.phase;
  }
  if(!tick(c))return c->info.phase;
  flushed(c);if(c->stage==FAILED)return c->info.phase;
  if(c->stage!=ERROR_DRAIN) {
    if(!poll_applied(c) || !import_snapshot(c))return c->info.phase;
    if(!receive(c)){fail(c,GT_ERR_PROTOCOL,GT_CLIENT_REASON_PROTOCOL);return c->info.phase;}
  }
  else {gt_wire_frame_t frame;if(gt__connection_frame(c->connection,&frame))(void)gt__connection_release_frame(c->connection);}
  if(tick(c))output(c);
  return c->info.phase;
}
bool gt__attachment_client_info(const gt_attachment_client_t *c,gt_client_info_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!c || !out)return false;
  *out=c->info;return true;
}
static gt_client_submit_t submit(gt_attachment_client_t *c,const gt_user_request_t *request,gt_worker_stamp_t *out,bool encoded,uint64_t generation,uint64_t sequence) {
  if(out)memset(out,0,sizeof(*out));
  if(!c || !request || !out)return GT_CLIENT_INVALID;
  if(!tick(c))return GT_CLIENT_UNAVAILABLE;
  if(c->info.phase!=GT_CLIENT_LIVE || c->info.readonly || !c->info.has_sequence)return GT_CLIENT_LOCKED;
  if(encoded && (generation!=c->config.generation || sequence<c->cut || sequence>c->info.sequence))return GT_CLIENT_LOCKED;
  if(gt__connection_wants_write(c->connection))return GT_CLIENT_BACKPRESSURE;
  size_t index=0;while(index<c->config.commands && c->commands[index].used)index++;
  if(index==c->config.commands)return GT_CLIENT_BACKPRESSURE;
  if(c->last_id==UINT64_MAX){fail(c,GT_ERR_PROTOCOL,GT_CLIENT_REASON_PROTOCOL);return GT_CLIENT_UNAVAILABLE;}
  gt_user_request_t r=*request;r.stamp=(gt_worker_stamp_t){c->config.generation,c->last_id+1,encoded?sequence:c->info.sequence};gt_wire_header_t h;
  if(!gt__user_request_encode(&r,c->tx,sizeof(c->tx),&h))return GT_CLIENT_INVALID;
  gt_connection_send_t rc=gt__connection_send(c->connection,&h,c->tx);gt__secure_erase(c->tx,h.payload_length);
  if(rc==GT_SEND_BUSY)return GT_CLIENT_BACKPRESSURE;
  if(rc!=GT_SEND_ACCEPTED){fail(c,GT_ERR_IO,GT_CLIENT_REASON_IO);return GT_CLIENT_UNAVAILABLE;}
  c->last_id=r.stamp.command_id;c->commands[index]=(command){.used=true,.kind=r.kind,.stamp=r.stamp,.started=c->now};*out=r.stamp;
  return GT_CLIENT_QUEUED;
}
gt_client_submit_t gt__attachment_client_submit(gt_attachment_client_t *c,const gt_user_request_t *request,gt_worker_stamp_t *out){return submit(c,request,out,false,0,0);}
gt_client_submit_t gt__attachment_client_submit_at(gt_attachment_client_t *c,uint64_t generation,uint64_t sequence,const gt_user_request_t *request,gt_worker_stamp_t *out){return submit(c,request,out,true,generation,sequence);}
void gt__attachment_client_cancel(gt_attachment_client_t *c){if(c)fail(c,GT_ERR_CANCELLED,GT_CLIENT_REASON_CANCELLED);}
void gt__attachment_client_close(gt_attachment_client_t *c) {
  if(!c)return;
  gt__attachment_client_cancel(c);gt__connection_destroy(c->connection);gt_allocator_t a=c->allocator;
  gt__secure_erase(c,sizeof(*c));a.release(a.context,c,sizeof(*c));
}
