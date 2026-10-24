#define _POSIX_C_SOURCE 200809L
#include "ipc/attachment-client.h"
#include "ipc/socket-stream.h"
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef union {max_align_t alignment;size_t n;} allocation;
typedef struct fixture fixture;
typedef struct {gt_socket_stream_t socket;gt_stream_ops_t ops;fixture *f;size_t quota;bool fixed_clock;} stream;
struct fixture {
  gt_attachment_client_t *client;gt_connection_t *connection,*peer;gt_client_config_t config;
  stream a,b;uint64_t now;bool clock_fail,delay_snapshot,delay_event,import_busy,event_busy;
  gt_status_t sink_error;gt_client_applied_t applied;unsigned snapshots,events,invalidated,results;
  gt_user_reply_t replies[32];char text[256];size_t used,calls,fail,live;uint64_t import_latency;
};
static void *admit(void *ctx,size_t n) {
  fixture *f=ctx;if(++f->calls==f->fail)return NULL;
  allocation *a=malloc(sizeof(*a)+n);CHECK(a);a->n=n;f->live++;return a+1;
}
static void refund(void *ctx,void *p,size_t n) {
  fixture *f=ctx;allocation *a=(allocation *)p-1;CHECK(a->n==n && f->live);
  const uint8_t *bytes=p;for(size_t i=0;i<n;i++)CHECK(!bytes[i]);
  f->live--;free(a);
}
static bool clock_now(void *ctx,uint64_t *now){fixture *f=ctx;if(f->clock_fail)return false;*now=f->now;return true;}
static gt_stream_result_t read_stream(void *ctx,void *p,size_t n){stream *s=ctx;return s->ops.read(s->ops.context,p,n);}
static gt_stream_result_t write_stream(void *ctx,const void *p,size_t n) {
  stream *s=ctx;if(!s->quota)return (gt_stream_result_t){.kind=GT_STREAM_WAIT};
  if(n>s->quota)n=s->quota;
  return s->ops.write(s->ops.context,p,n);
}
static bool stream_clock(void *ctx,uint64_t *now){stream *s=ctx;if(s->fixed_clock){*now=0;return true;}return clock_now(s->f,now);}
static void close_stream(void *ctx){stream *s=ctx;s->ops.close(s->ops.context);}
static gt_client_sink_rc_t import(void *ctx,uint64_t generation,uint64_t sequence,uint32_t format,
  const uint8_t *bytes,size_t n,bool readonly,gt_status_t *error) {
  fixture *f=ctx;(void)readonly;CHECK(generation==2 && format==7 && n==4 && !memcmp(bytes,"BASE",4));
  *error=f->sink_error;if(*error!=GT_OK)return GT_CLIENT_SINK_FAILED;
  if(f->import_busy)return GT_CLIENT_SINK_BUSY;
  CHECK(!f->snapshots);f->snapshots++;memcpy(f->text,bytes,n);f->used=n;f->now+=f->import_latency;
  if(!f->delay_snapshot)f->applied=(gt_client_applied_t){true,generation,sequence};
  return GT_CLIENT_SINK_ACCEPTED;
}
static gt_client_sink_rc_t apply(void *ctx,uint64_t generation,uint64_t sequence,const gt_state_event_t *e,gt_status_t *error) {
  fixture *f=ctx;CHECK(generation==2);*error=f->sink_error;
  if(*error!=GT_OK)return GT_CLIENT_SINK_FAILED;
  if(f->event_busy)return GT_CLIENT_SINK_BUSY;
  CHECK(e->kind==GT_STATE_OUTPUT && e->length<sizeof(f->text)-f->used);
  memcpy(f->text+f->used,e->payload,e->length);f->used+=e->length;f->events++;
  if(!f->delay_event)f->applied=(gt_client_applied_t){true,generation,sequence};
  return GT_CLIENT_SINK_ACCEPTED;
}
static gt_status_t applied(void *ctx,gt_client_applied_t *out){fixture *f=ctx;*out=f->applied;return GT_OK;}
static void invalidated(void *ctx,uint64_t generation,gt_status_t error){fixture *f=ctx;CHECK(generation==2 && error!=GT_OK);f->invalidated++;}
static void result(void *ctx,const gt_user_reply_t *reply){fixture *f=ctx;CHECK(f->results<32);f->replies[f->results++]=*reply;}
static void setup_limits(fixture *f,gt_connection_limits_t limits) {
  memset(f,0,sizeof(*f));f->config=(gt_client_config_t){.session=5,.generation=2,.format=7,.snapshot_limit=1024,.commands=2,
    .restore_timeout_ms=1000,.apply_timeout_ms=40,.heartbeat_ms=1000,.peer_timeout_ms=5000,.command_timeout_ms=60,.error_timeout_ms=50,
    .clock_context=f,.clock=clock_now,.sink={f,import,apply,applied,invalidated,result}};
  int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));f->a.f=f->b.f=f;f->a.quota=f->b.quota=SIZE_MAX;
  CHECK(gt__socket_stream_adopt(sockets[0],&f->a.socket,&f->a.ops)==GT_OK && gt__socket_stream_adopt(sockets[1],&f->b.socket,&f->b.ops)==GT_OK);
  gt_stream_ops_t a={&f->a,read_stream,write_stream,stream_clock,close_stream},b={&f->b,read_stream,write_stream,stream_clock,close_stream};
  gt_allocator_t domain={f,admit,refund};
  CHECK(gt__connection_open(&a,limits,&domain,&f->connection)==GT_CONNECTION_CREATED && gt__connection_open(&b,limits,&domain,&f->peer)==GT_CONNECTION_CREATED);
}
static void setup(fixture *f){setup_limits(f,(gt_connection_limits_t){65560,65560,2000});}
static void open_client(fixture *f){gt_allocator_t a={f,admit,refund};CHECK(gt__attachment_client_open(f->connection,&f->config,&a,&f->client)==GT_OK);f->connection=NULL;}
static gt_client_info_t info(fixture *f){gt_client_info_t i;CHECK(gt__attachment_client_info(f->client,&i));return i;}
static void pump(fixture *f,unsigned count) {
  for(unsigned i=0;i<count;i++){if(f->peer)(void)gt__connection_step(f->peer);if(f->client)(void)gt__attachment_client_step(f->client);}
}
static void send_frame(fixture *f,const gt_wire_header_t *h,const void *bytes){CHECK(gt__connection_send(f->peer,h,bytes)==GT_SEND_ACCEPTED);pump(f,12);}
static void send_data(fixture *f,gt_data_message_t m) {
  uint8_t bytes[128];gt_wire_header_t h;if(!m.generation)m.generation=2;
  CHECK(gt__data_encode(&m,bytes,sizeof(bytes),&h));send_frame(f,&h,bytes);
}
static gt_wire_frame_t frame(fixture *f) {
  for(unsigned i=0;i<1000;i++){gt_wire_frame_t r;if(gt__connection_frame(f->peer,&r))return r;pump(f,1);}
  CHECK(false);return (gt_wire_frame_t){0};
}
static gt_data_message_t outgoing(fixture *f,uint16_t type) {
  gt_wire_frame_t r=frame(f);gt_data_message_t m;CHECK(gt__data_decode(&r,&m) && m.type==type && m.generation==2);
  CHECK(gt__connection_release_frame(f->peer));return m;
}
static void begin(fixture *f,uint64_t s,bool readonly) {
  send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.sequence=s,.total=4,.format=7,.flags=readonly?GT_DATA_READONLY:0});
  send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_CHUNK,.offset=0,.bytes=(const uint8_t *)"BA",.length=2});
  send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_CHUNK,.offset=2,.bytes=(const uint8_t *)"SE",.length=2});
  send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_END,.sequence=s,.total=4});
}
static void live(fixture *f,uint64_t s,bool readonly) {
  begin(f,s,readonly);CHECK(outgoing(f,GT_MSG_READY_S).sequence==s);
  send_data(f,(gt_data_message_t){.type=GT_MSG_LIVE_BEGIN,.sequence=s});CHECK(outgoing(f,GT_MSG_READY_L).sequence==s);pump(f,2);
  CHECK(info(f).phase==(readonly?GT_CLIENT_READONLY:GT_CLIENT_LIVE));
}
static void finish(fixture *f) {
  if(f->client)gt__attachment_client_close(f->client);
  if(f->connection)gt__connection_destroy(f->connection);
  if(f->peer)gt__connection_destroy(f->peer);
  CHECK(!f->live && f->invalidated<=1);
}
static gt_user_request_t input(void){return (gt_user_request_t){.kind=GT_WORKER_INPUT,.stamp={999,999,999},.input=(const uint8_t *)"abc",.length=3};}
static gt_user_request_t user_frame(fixture *f) {
  gt_wire_frame_t r=frame(f);gt_user_request_t request;CHECK(gt__user_request_decode(&r,&request));
  CHECK(gt__connection_release_frame(f->peer));return request;
}
static void reply(fixture *f,gt_user_reply_t r){uint8_t bytes[40];gt_wire_header_t h;CHECK(gt__user_reply_encode(&r,bytes,sizeof(bytes),&h));send_frame(f,&h,bytes);}
static gt_managed_info_t ended(uint64_t sequence,bool known) {
  return (gt_managed_info_t){.id=5,.phase=GT_MANAGED_RETAINED,.has_result=true,.result={.kind=GT_WORKER_END_EOF,.status=GT_OK,.has_sequence=known,.sequence=sequence,.termination_status=GT_ERR_NOT_FOUND}};
}
static void flow(void) {
  fixture f;setup(&f);open_client(&f);f.delay_snapshot=true;begin(&f,0,false);CHECK(f.snapshots==1 && !info(&f).has_sequence);
  gt_worker_stamp_t stamp;gt_user_request_t r=input();CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);
  gt_wire_frame_t empty;CHECK(!gt__connection_frame(f.peer,&empty));f.applied=(gt_client_applied_t){true,2,0};pump(&f,8);
  CHECK(outgoing(&f,GT_MSG_READY_S).sequence==0 && info(&f).has_sequence && !info(&f).sequence);
  f.delay_event=true;send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=1,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"+",.length=1});
  send_data(&f,(gt_data_message_t){.type=GT_MSG_LIVE_BEGIN,.sequence=1});CHECK(info(&f).phase==GT_CLIENT_RESTORING && !gt__connection_frame(f.peer,&empty));
  f.applied=(gt_client_applied_t){true,2,1};f.a.quota=0;pump(&f,3);CHECK(info(&f).phase==GT_CLIENT_RESTORING && gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED);
  f.a.quota=1;pump(&f,10);CHECK(info(&f).phase==GT_CLIENT_RESTORING);f.a.quota=SIZE_MAX;pump(&f,8);
  CHECK(outgoing(&f,GT_MSG_READY_L).sequence==1 && info(&f).phase==GT_CLIENT_LIVE && !memcmp(f.text,"BASE+",5));
  CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED && stamp.generation==2 && stamp.command_id==1 && stamp.applied_sequence==1);
  gt_user_request_t sent=user_frame(&f);CHECK(sent.stamp.command_id==1 && sent.length==3);
  reply(&f,(gt_user_reply_t){.kind=sent.kind,.stamp=sent.stamp,.status=GT_OK,.delivery=GT_USER_OPERATION_OK});CHECK(f.results==1 && f.replies[0].delivery==GT_USER_OPERATION_OK);
  send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=2,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"!",.length=1});
  send_data(&f,(gt_data_message_t){.type=GT_MSG_SESSION_END,.terminal=ended(2,true)});CHECK(info(&f).readonly && !info(&f).has_terminal);
  CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED);f.applied.sequence=2;pump(&f,2);CHECK(info(&f).phase==GT_CLIENT_ENDED && info(&f).has_terminal);finish(&f);
}
static void encoded_input(void) {
  fixture f;setup(&f);open_client(&f);live(&f,4,false);
  send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=5,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"!",.length=1});pump(&f,2);CHECK(info(&f).sequence==5);
  gt_user_request_t r=input();gt_worker_stamp_t stamp;
  CHECK(gt__attachment_client_submit_at(f.client,1,4,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);
  CHECK(gt__attachment_client_submit_at(f.client,3,4,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);
  CHECK(gt__attachment_client_submit_at(f.client,2,3,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);
  CHECK(gt__attachment_client_submit_at(f.client,2,6,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);
  CHECK(gt__attachment_client_submit_at(f.client,2,4,&r,&stamp)==GT_CLIENT_QUEUED && stamp.command_id==1 && stamp.applied_sequence==4);
  gt_user_request_t sent=user_frame(&f);CHECK(sent.stamp.generation==2 && sent.stamp.applied_sequence==4);
  reply(&f,(gt_user_reply_t){.kind=sent.kind,.stamp=sent.stamp,.status=GT_OK,.delivery=GT_USER_OPERATION_OK});CHECK(f.results==1 && f.replies[0].stamp.applied_sequence==4);finish(&f);
  setup(&f);open_client(&f);live(&f,0,false);CHECK(gt__attachment_client_submit_at(f.client,2,0,&r,&stamp)==GT_CLIENT_QUEUED && stamp.applied_sequence==0);finish(&f);
}
static void readonly(void) {
  fixture f;setup(&f);open_client(&f);live(&f,0,true);gt_worker_stamp_t stamp;gt_user_request_t r=input();CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED);
  send_data(&f,(gt_data_message_t){.type=GT_MSG_SESSION_END,.terminal=ended(0,true)});CHECK(info(&f).phase==GT_CLIENT_ENDED && info(&f).terminal.result.has_sequence && !info(&f).terminal.result.sequence);finish(&f);
  setup(&f);open_client(&f);send_data(&f,(gt_data_message_t){.type=GT_MSG_SESSION_END,.flags=GT_DATA_NO_SNAPSHOT,.error=GT_ERR_UNSUPPORTED,.terminal=ended(0,false)});
  CHECK(info(&f).phase==GT_CLIENT_ENDED && !info(&f).has_sequence && !info(&f).terminal.result.has_sequence && info(&f).state_error==GT_ERR_UNSUPPORTED && !f.snapshots);finish(&f);
}
static void commands(void) {
  fixture f;setup(&f);open_client(&f);live(&f,4,false);gt_user_request_t r=input();gt_worker_stamp_t a,b,c;
  CHECK(gt__attachment_client_submit(f.client,&r,&a)==GT_CLIENT_QUEUED);(void)user_frame(&f);
  CHECK(gt__attachment_client_submit(f.client,&r,&b)==GT_CLIENT_QUEUED && b.command_id==2);(void)user_frame(&f);
  CHECK(gt__attachment_client_submit(f.client,&r,&c)==GT_CLIENT_BACKPRESSURE && !c.command_id);
  reply(&f,(gt_user_reply_t){.kind=GT_WORKER_INPUT,.stamp=a,.rejected=true,.rejection=GT_USER_REJECT_BACKPRESSURE,.delivery=GT_USER_NOT_EXECUTED});
  CHECK(f.results==1 && f.replies[0].rejected && gt__attachment_client_submit(f.client,&r,&c)==GT_CLIENT_QUEUED && c.command_id==3);
  (void)user_frame(&f);gt__connection_destroy(f.peer);f.peer=NULL;pump(&f,1);
  CHECK(info(&f).phase==GT_CLIENT_FAILED && f.invalidated==1 && f.results==3 && f.replies[1].delivery==GT_USER_OUTCOME_UNKNOWN && f.replies[2].delivery==GT_USER_OUTCOME_UNKNOWN);finish(&f);
}
static void backpressure(void) {
  fixture f;setup(&f);open_client(&f);f.import_busy=true;begin(&f,0,false);CHECK(!f.snapshots && f.live==4);
  f.import_busy=false;pump(&f,8);CHECK(f.snapshots==1 && f.live==3 && outgoing(&f,GT_MSG_READY_S).sequence==0);
  send_data(&f,(gt_data_message_t){.type=GT_MSG_LIVE_BEGIN});(void)outgoing(&f,GT_MSG_READY_L);pump(&f,1);
  f.event_busy=true;send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=1,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"x",.length=1});CHECK(!f.events);
  f.event_busy=false;pump(&f,4);CHECK(f.events==1 && info(&f).sequence==1 && !memcmp(f.text,"BASEx",5));
  gt_user_request_t r=input();gt_worker_stamp_t stamp;f.a.quota=0;
  CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED && stamp.command_id==1);
  CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_BACKPRESSURE && !stamp.command_id);
  f.a.quota=SIZE_MAX;gt_user_request_t first=user_frame(&f);
  reply(&f,(gt_user_reply_t){.kind=first.kind,.stamp=first.stamp,.status=GT_OK,.delivery=GT_USER_OPERATION_OK});
  CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED && stamp.command_id==2);finish(&f);
}
static void bad_replies(void) {
  for(unsigned variant=0;variant<5;variant++) {
    fixture f;setup(&f);open_client(&f);live(&f,4,false);gt_user_request_t r=input();gt_worker_stamp_t stamp;
    CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED);(void)user_frame(&f);
    gt_user_reply_t response={.kind=GT_WORKER_INPUT,.stamp=stamp,.status=GT_OK,.delivery=GT_USER_OPERATION_OK};
    if(!variant)response.stamp.command_id++;else if(variant==1)response.stamp.applied_sequence++;else if(variant==2)response.kind=GT_WORKER_RESIZE;else if(variant==3)response.stamp.generation++;
    else {reply(&f,response);CHECK(f.results==1 && f.replies[0].delivery==GT_USER_OPERATION_OK);}
    reply(&f,response);CHECK(info(&f).phase==GT_CLIENT_FAILED && f.results==1 && f.invalidated==1);
    if(variant!=4)CHECK(f.replies[0].delivery==GT_USER_OUTCOME_UNKNOWN);
    finish(&f);
  }
}
static void malformed(void) {
  for(unsigned variant=0;variant<8;variant++) {
    fixture f;setup(&f);open_client(&f);
    if(variant==0)send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.generation=3,.total=4,.format=7});
    else if(variant==1)send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_CHUNK,.bytes=(const uint8_t *)"x",.length=1});
    else if(variant==2)send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.total=1025,.format=7});
    else if(variant==3){send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.total=4,.format=7});send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_CHUNK,.offset=1,.bytes=(const uint8_t *)"x",.length=1});}
    else if(variant==4){live(&f,1,false);send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=3,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"x",.length=1});}
    else if(variant==5){begin(&f,1,false);(void)outgoing(&f,GT_MSG_READY_S);send_data(&f,(gt_data_message_t){.type=GT_MSG_LIVE_BEGIN,.sequence=2});}
    else if(variant==6)send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.total=4,.format=8});
    else {live(&f,1,false);gt_managed_info_t t=ended(1,true);t.id=6;send_data(&f,(gt_data_message_t){.type=GT_MSG_SESSION_END,.terminal=t});}
    CHECK(info(&f).phase==GT_CLIENT_FAILED && f.invalidated==1);finish(&f);
  }
  fixture f;setup(&f);open_client(&f);gt_wire_header_t h={.type=GT_MSG_SNAPSHOT_BEGIN,.payload_length=40};uint8_t bytes[24];
  CHECK(gt__wire_header_encode(&h,bytes));bytes[0]='X';gt_stream_result_t sent=f.b.ops.write(f.b.ops.context,bytes,sizeof(bytes));CHECK(sent.kind==GT_STREAM_DATA && sent.length==sizeof(bytes));
  pump(&f,2);CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).reason==GT_CLIENT_REASON_PROTOCOL && info(&f).error==GT_ERR_PROTOCOL);finish(&f);
}
static void applied_errors(void) {
  for(unsigned variant=0;variant<4;variant++) {
    fixture f;setup(&f);open_client(&f);live(&f,4,false);
    f.applied=(gt_client_applied_t){true,variant==0?1:variant==1?3:2,variant==2?3:variant==3?5:4};pump(&f,1);
    if(!variant){CHECK(info(&f).phase==GT_CLIENT_LIVE && info(&f).sequence==4);f.applied=(gt_client_applied_t){true,2,4};}
    else CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).reason==GT_CLIENT_REASON_SINK);
    finish(&f);
  }
}
static void deadlines(void) {
  for(unsigned variant=0;variant<5;variant++) {
    fixture f;setup(&f);if(variant==0)f.config.restore_timeout_ms=40;
    if(variant==2){f.config.heartbeat_ms=10;f.config.peer_timeout_ms=50;}
    open_client(&f);
    if(variant==1){f.delay_snapshot=true;begin(&f,0,false);}
    else if(variant>=2)live(&f,0,false);
    if(variant==2){f.now=10;pump(&f,2);CHECK(outgoing(&f,GT_MSG_DATA_PING).nonce==1);f.now=59;send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=1,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"x",.length=1});}
    else if(variant==3){gt_worker_stamp_t stamp;gt_user_request_t r=input();CHECK(gt__attachment_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED);(void)user_frame(&f);f.now=59;}
    else if(variant==4){f.event_busy=true;send_data(&f,(gt_data_message_t){.type=GT_MSG_STATE_EVENT,.sequence=1,.kind=GT_STATE_OUTPUT,.bytes=(const uint8_t *)"x",.length=1});CHECK(!f.events);f.now=39;}
    else {f.now=39;send_data(&f,(gt_data_message_t){.type=GT_MSG_DATA_PING,.nonce=9});CHECK(outgoing(&f,GT_MSG_DATA_PONG).nonce==9);}
    f.now=(variant==2 || variant==3)?60:40;pump(&f,1);CHECK(info(&f).phase==GT_CLIENT_FAILED);
    gt_client_reason_t reasons[]={GT_CLIENT_REASON_RESTORE_DEADLINE,GT_CLIENT_REASON_APPLY_DEADLINE,GT_CLIENT_REASON_PEER_DEADLINE,GT_CLIENT_REASON_COMMAND_DEADLINE,GT_CLIENT_REASON_APPLY_DEADLINE};
    CHECK(info(&f).reason==reasons[variant]);if(variant==3)CHECK(f.results==1 && f.replies[0].delivery==GT_USER_OUTCOME_UNKNOWN);finish(&f);
  }
  fixture f;setup(&f);f.config.heartbeat_ms=10;f.config.peer_timeout_ms=50;open_client(&f);live(&f,0,false);
  f.now=10;pump(&f,2);CHECK(outgoing(&f,GT_MSG_DATA_PING).nonce==1);
  f.now=59;send_data(&f,(gt_data_message_t){.type=GT_MSG_DATA_PONG,.nonce=1});CHECK(info(&f).phase==GT_CLIENT_LIVE);
  f.now=69;pump(&f,2);CHECK(outgoing(&f,GT_MSG_DATA_PING).nonce==2);
  send_data(&f,(gt_data_message_t){.type=GT_MSG_DATA_PONG,.nonce=1});CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).reason==GT_CLIENT_REASON_PROTOCOL);finish(&f);
}
static void error_drain(void) {
  fixture f;setup(&f);open_client(&f);f.a.quota=0;
  send_data(&f,(gt_data_message_t){.type=GT_MSG_RESTORE_ERROR,.error=GT_ERR_CANCELLED,.stage=GT_DATA_ERROR_DEADLINE});
  CHECK(info(&f).phase==GT_CLIENT_CLOSING && f.invalidated==1);f.a.quota=1;pump(&f,10);CHECK(info(&f).phase==GT_CLIENT_CLOSING);
  f.a.quota=SIZE_MAX;gt_data_message_t ack=outgoing(&f,GT_MSG_RESTORE_ERROR_ACK);CHECK(ack.error==GT_ERR_CANCELLED && ack.stage==GT_DATA_ERROR_DEADLINE);
  CHECK(info(&f).phase==GT_CLIENT_CLOSING);gt__connection_destroy(f.peer);f.peer=NULL;pump(&f,1);
  CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).reason==GT_CLIENT_REASON_SERVER && info(&f).error==GT_ERR_CANCELLED && f.invalidated==1);finish(&f);
  setup(&f);open_client(&f);f.a.quota=0;send_data(&f,(gt_data_message_t){.type=GT_MSG_RESTORE_ERROR,.error=GT_ERR_UNSUPPORTED,.stage=GT_DATA_ERROR_ENCODE});
  f.now=49;pump(&f,1);CHECK(info(&f).phase==GT_CLIENT_CLOSING);f.now=50;pump(&f,1);
  CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).reason==GT_CLIENT_REASON_SERVER && info(&f).error==GT_ERR_UNSUPPORTED);finish(&f);
}
static void clocks(void) {
  for(unsigned variant=0;variant<4;variant++) {
    fixture f;setup(&f);open_client(&f);
    if(!variant){f.a.fixed_clock=f.b.fixed_clock=true;f.now=10;pump(&f,1);f.now=9;pump(&f,1);}
    else if(variant==1){f.clock_fail=true;(void)gt__attachment_client_step(f.client);f.clock_fail=false;}
    else if(variant==2){f.import_latency=1000;begin(&f,0,false);}
    else {f.sink_error=GT_ERR_UNSUPPORTED;begin(&f,0,false);}
    CHECK(info(&f).phase==GT_CLIENT_FAILED && f.invalidated==1);
    CHECK(info(&f).reason==(variant<2?GT_CLIENT_REASON_CLOCK:variant==2?GT_CLIENT_REASON_RESTORE_DEADLINE:GT_CLIENT_REASON_SINK));finish(&f);
  }
  fixture f;setup(&f);f.config.restore_timeout_ms=10000;open_client(&f);
  const uint8_t partial[8]={'G','T','S','D',0,1,0,40};gt_stream_result_t sent=f.b.ops.write(f.b.ops.context,partial,sizeof(partial));CHECK(sent.kind==GT_STREAM_DATA && sent.length==sizeof(partial));
  pump(&f,1);f.now=1999;pump(&f,1);CHECK(info(&f).phase==GT_CLIENT_RESTORING);f.now=2000;pump(&f,1);
  CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).error==GT_ERR_CANCELLED && info(&f).reason==GT_CLIENT_REASON_IO);finish(&f);
}
static void invalid(void) {
  fixture f;setup(&f);gt_allocator_t a={&f,admit,refund};gt_attachment_client_t *c=(void *)1;size_t before=f.live,calls=f.calls;
  f.config.commands=17;CHECK(gt__attachment_client_open(f.connection,&f.config,&a,&c)==GT_ERR_INVALID_ARG && !c && f.calls==calls);f.config.commands=2;
  gt_client_config_t good=f.config;
#define BAD(field,value) do {f.config=good;f.config.field=value;size_t start=f.calls;CHECK(gt__attachment_client_open(f.connection,&f.config,&a,&c)==GT_ERR_INVALID_ARG && !c && f.calls==start);} while(0)
  BAD(snapshot_limit,64u*1024u*1024u+1);BAD(snapshot_limit,0);BAD(session,0);BAD(generation,0);BAD(format,0);BAD(commands,0);
  BAD(restore_timeout_ms,0);BAD(restore_timeout_ms,600001);BAD(apply_timeout_ms,0);BAD(apply_timeout_ms,60001);
  BAD(peer_timeout_ms,0);BAD(peer_timeout_ms,60001);BAD(heartbeat_ms,0);BAD(heartbeat_ms,5000);
  BAD(command_timeout_ms,0);BAD(command_timeout_ms,60001);BAD(error_timeout_ms,0);BAD(error_timeout_ms,60001);
#undef BAD
  f.config=good;
  f.clock_fail=true;CHECK(gt__attachment_client_open(f.connection,&f.config,&a,&c)==GT_ERR_IO && !c && f.live==before);f.clock_fail=false;
  f.fail=f.calls+1;CHECK(gt__attachment_client_open(f.connection,&f.config,&a,&c)==GT_ERR_IO && !c && f.live==before);f.fail=0;
  open_client(&f);f.fail=f.calls+1;send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.total=4,.format=7});CHECK(info(&f).phase==GT_CLIENT_FAILED && f.live==before+1);finish(&f);
  gt_connection_limits_t limits={1,2,3};CHECK(!gt__connection_limits(NULL,&limits) && !limits.receive_limit && !limits.send_limit && !limits.frame_timeout_ms);
  for(unsigned variant=0;variant<2;variant++) {
    setup_limits(&f,(gt_connection_limits_t){variant?65560:65551,variant?65559:65560,2000});a=(gt_allocator_t){&f,admit,refund};
    CHECK(gt__connection_limits(f.connection,&limits) && limits.receive_limit==(variant?65560u:65551u));
    CHECK(gt__attachment_client_open(f.connection,&f.config,&a,&c)==GT_ERR_INVALID_ARG && !c);finish(&f);
  }
}
static void process_metadata(void) {
  fixture f;setup(&f);open_client(&f);
  send_data(&f,(gt_data_message_t){.type=GT_MSG_PROCESS_META});
  CHECK(info(&f).phase==GT_CLIENT_FAILED && !info(&f).has_process);finish(&f);
  for(unsigned ro=0;ro<2;ro++) {
    setup(&f);open_client(&f);live(&f,42,ro!=0);
    CHECK(!info(&f).has_process && f.snapshots==1 && !f.events);
    gt_data_message_t m={.type=GT_MSG_PROCESS_META,.process={.child=GT_TRANSPORT_CHILD_UNKNOWN,.has_cwd=true,.cwd="/目录"}};
    send_data(&f,m);gt_client_info_t i=info(&f);
    CHECK(i.has_process && i.process.child==GT_TRANSPORT_CHILD_UNKNOWN && i.process.has_cwd && !strcmp(i.process.cwd,"/目录"));
    CHECK(i.sequence==42 && f.applied.sequence==42 && f.snapshots==1 && !f.events && i.phase==(ro?GT_CLIENT_READONLY:GT_CLIENT_LIVE));
    m.process=(gt_session_process_t){.child=GT_TRANSPORT_CHILD_JOB};send_data(&f,m);i=info(&f);
    CHECK(i.has_process && i.process.child==GT_TRANSPORT_CHILD_JOB && !i.process.has_cwd && !i.process.cwd[0] && i.sequence==42 && !f.events);
    m.generation=1;send_data(&f,m);CHECK(info(&f).phase==GT_CLIENT_FAILED && info(&f).reason==GT_CLIENT_REASON_PROTOCOL);finish(&f);
  }
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"process"))process_metadata();else if(!strcmp(argv[1],"encoded"))encoded_input();else if(!strcmp(argv[1],"flow"))flow();else if(!strcmp(argv[1],"readonly"))readonly();else if(!strcmp(argv[1],"commands"))commands();
  else if(!strcmp(argv[1],"malformed"))malformed();else if(!strcmp(argv[1],"applied"))applied_errors();else if(!strcmp(argv[1],"deadlines"))deadlines();
  else if(!strcmp(argv[1],"error"))error_drain();else if(!strcmp(argv[1],"backpressure"))backpressure();else if(!strcmp(argv[1],"replies"))bad_replies();else if(!strcmp(argv[1],"clocks"))clocks();else {CHECK(!strcmp(argv[1],"invalid"));invalid();}
  printf("PASS attachment client %s\n",argv[1]);return 0;
}
