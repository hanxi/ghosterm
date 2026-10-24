#define _POSIX_C_SOURCE 200809L
#include "ipc/session-client.h"
#include "ipc/socket-stream.h"
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct fixture fixture;
typedef struct {gt_socket_stream_t socket;gt_stream_ops_t ops;fixture *f;bool closed;size_t quota;} stream;
typedef union {max_align_t align;size_t n;} block;
struct fixture {
  gt_session_client_t *client;gt_session_client_config_t config;gt_create_spec_t create;
  gt_connection_t *peer[2];stream a[2],b[2];uint64_t now,generation;
  gt_session_link_state_t link;gt_status_t link_error,sink_error;
  gt_client_applied_t applied;bool clock_fail,delay_apply;unsigned starts,resets,streams,snapshots,invalidations,results,clocks,fail_clock;
  gt_user_reply_t last;size_t calls,fail,live;char executable[32];
};
static void *admit(void *ctx,size_t n){fixture *f=ctx;if(++f->calls==f->fail)return NULL;block *b=malloc(sizeof(*b)+n);CHECK(b);b->n=n;f->live++;return b+1;}
static void refund(void *ctx,void *p,size_t n){fixture *f=ctx;block *b=(block *)p-1;CHECK(f->live && b->n==n);for(size_t i=0;i<n;i++)CHECK(!((uint8_t *)p)[i]);f->live--;free(b);}
static bool clock_now(void *ctx,uint64_t *out){fixture *f=ctx;if(++f->clocks==f->fail_clock || f->clock_fail)return false;*out=f->now;return true;}
static bool stream_clock(void *ctx,uint64_t *out){stream *s=ctx;*out=s->f->now;return true;}
static gt_stream_result_t read_stream(void *ctx,void *p,size_t n){stream *s=ctx;CHECK(!s->closed);return s->ops.read(s->ops.context,p,n);}
static gt_stream_result_t write_stream(void *ctx,const void *p,size_t n){stream *s=ctx;CHECK(!s->closed);if(!s->quota)return (gt_stream_result_t){.kind=GT_STREAM_WAIT};if(n>s->quota)n=s->quota;return s->ops.write(s->ops.context,p,n);}
static void close_stream(void *ctx){stream *s=ctx;CHECK(!s->closed);s->closed=true;s->ops.close(s->ops.context);}
static gt_session_link_state_t link_step(void *ctx,bool start,uint32_t attempt,gt_status_t *error) {
  fixture *f=ctx;if(start){CHECK(f->starts==f->resets);f->starts++;}CHECK(f->starts==attempt);*error=f->link_error;return f->link;
}
static gt_status_t stream_open(void *ctx,unsigned slot,gt_stream_ops_t *ops) {
  fixture *f=ctx;CHECK(slot<2 && !f->peer[slot]);int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
  f->a[slot]=(stream){.f=f,.quota=SIZE_MAX};f->b[slot]=(stream){.f=f,.quota=SIZE_MAX};
  stream *a=&f->a[slot],*b=&f->b[slot];CHECK(gt__socket_stream_adopt(fd[0],&a->socket,&a->ops)==GT_OK && gt__socket_stream_adopt(fd[1],&b->socket,&b->ops)==GT_OK);
  *ops=(gt_stream_ops_t){a,read_stream,write_stream,stream_clock,close_stream};gt_stream_ops_t peer={b,read_stream,write_stream,stream_clock,close_stream};
  CHECK(gt__connection_open(&peer,(gt_connection_limits_t){65560,65560,1000},NULL,&f->peer[slot])==GT_CONNECTION_CREATED);f->streams++;return GT_OK;
}
static void reset(void *ctx){fixture *f=ctx;CHECK(f->resets+1==f->starts);for(unsigned i=0;i<2;i++){if(f->peer[i]){CHECK(f->a[i].closed);gt__connection_destroy(f->peer[i]);f->peer[i]=NULL;}}f->resets++;}
static gt_client_sink_rc_t snapshot(void *ctx,uint64_t generation,uint64_t sequence,uint32_t format,const uint8_t *p,size_t n,bool readonly,gt_status_t *error) {
  fixture *f=ctx;(void)readonly;CHECK(generation==f->generation && format==7 && n==4 && !memcmp(p,"BASE",4));*error=f->sink_error;
  if(*error!=GT_OK)return GT_CLIENT_SINK_FAILED;
  f->snapshots++;if(!f->delay_apply)f->applied=(gt_client_applied_t){true,generation,sequence};return GT_CLIENT_SINK_ACCEPTED;
}
static gt_client_sink_rc_t event(void *ctx,uint64_t generation,uint64_t sequence,const gt_state_event_t *e,gt_status_t *error){fixture *f=ctx;CHECK(e->kind==GT_STATE_OUTPUT);f->applied=(gt_client_applied_t){true,generation,sequence};*error=GT_OK;return GT_CLIENT_SINK_ACCEPTED;}
static gt_status_t applied(void *ctx,gt_client_applied_t *out){fixture *f=ctx;*out=f->applied;return GT_OK;}
static void invalidated(void *ctx,uint64_t generation,gt_status_t error){fixture *f=ctx;CHECK(generation && error!=GT_OK);f->invalidations++;}
static void user_result(void *ctx,const gt_user_reply_t *r){fixture *f=ctx;f->last=*r;f->results++;}
static void setup(fixture *f,bool resume) {
  memset(f,0,sizeof(*f));f->link=GT_SESSION_LINK_READY;f->generation=7;strcpy(f->executable,"/bin/sh");
  f->create=(gt_create_spec_t){.nonce={9},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,.executable=f->executable,.cwd="/",.term="xterm"};
  f->config=(gt_session_client_config_t){.link={f,link_step,stream_open,reset},.compatibility={2},.session=resume?5:0,.create=resume?NULL:&f->create,
    .recovery_timeout_ms=500,.backoff_ms=10,.bootstrap_timeout_ms=100,.management_heartbeat_ms=20,.management_timeout_ms=50,.clock_context=f,.clock=clock_now,
    .attachment={.format=7,.snapshot_limit=1024,.commands=2,.restore_timeout_ms=400,.apply_timeout_ms=40,.heartbeat_ms=20,.peer_timeout_ms=60,.command_timeout_ms=50,.error_timeout_ms=10,
    .sink={f,snapshot,event,applied,invalidated,user_result}}};if(resume)f->config.instance[0]=1;
}
static void open_client(fixture *f){gt_allocator_t a={f,admit,refund};CHECK(gt__session_client_open(&f->config,&a,&f->client)==GT_OK);}
static gt_session_client_info_t info(fixture *f){gt_session_client_info_t r;CHECK(gt__session_client_info(f->client,&r));return r;}
static void pump(fixture *f,unsigned n){for(unsigned i=0;i<n;i++){(void)gt__session_client_step(f->client);for(unsigned j=0;j<2;j++)if(f->peer[j])(void)gt__connection_step(f->peer[j]);}}
static gt_wire_frame_t frame(fixture *f,unsigned slot){for(unsigned i=0;i<1000;i++){gt_wire_frame_t r;if(f->peer[slot] && gt__connection_frame(f->peer[slot],&r))return r;pump(f,1);}CHECK(false);return (gt_wire_frame_t){0};}
static void peer_send(fixture *f,unsigned slot,uint16_t type,uint64_t id,const void *bytes,size_t n){gt_wire_header_t h={.type=type,.request_id=id,.payload_length=(uint32_t)n};CHECK(gt__connection_send(f->peer[slot],&h,bytes)==GT_SEND_ACCEPTED);pump(f,12);}
static void put(uint8_t *p,uint64_t v,size_t n){for(size_t i=0;i<n;i++)p[n-i-1]=(uint8_t)(v>>(8*i));}
static uint64_t request(fixture *f,uint16_t type){gt_wire_frame_t r=frame(f,0);CHECK(r.header.type==type);uint64_t id=r.header.request_id;if(type==GT_MSG_CREATE){gt_create_spec_t s;char scratch[8192];CHECK(gt__create_spec_decode(r.payload,r.header.payload_length,&s,scratch,sizeof(scratch))==GT_OK && !strcmp(s.executable,"/bin/sh") && s.nonce[0]==9);}else if(type==GT_MSG_QUERY_CREATE)CHECK(r.header.payload_length==16 && r.payload[0]==9);CHECK(gt__connection_release_frame(f->peer[0]));return id;}
static void hello(fixture *f,unsigned slot,uint8_t instance) {
  gt_wire_frame_t r=frame(f,slot);gt_protocol_hello_t h;CHECK(r.header.type==GT_MSG_HELLO && gt__hello_decode(r.payload,r.header.payload_length,&h));
  CHECK(h.role==(slot?GT_ROLE_DATA:GT_ROLE_MANAGEMENT));CHECK(h.expected_instance[0]==info(f).instance[0]);CHECK(gt__connection_release_frame(f->peer[slot]));
  uint8_t p[80]={0};p[5]=(uint8_t)h.role;p[8]=2;p[48]=instance;p[64]=9;peer_send(f,slot,GT_MSG_HELLO_REPLY,slot?0:1,p,80);
  if(info(f).phase==GT_SESSION_FAILED)return;
  r=frame(f,slot);CHECK(r.header.type==GT_MSG_HELLO_ACK && r.payload[0]==9);CHECK(gt__connection_release_frame(f->peer[slot]));pump(f,3);
}
static void creation(fixture *f,bool query,gt_creation_status_t status){uint64_t id=request(f,query?GT_MSG_QUERY_CREATE:GT_MSG_CREATE);uint8_t p[32]={9};put(p+16,status,4);if(status==GT_CREATION_ACCEPTED)put(p+24,5,8);peer_send(f,0,query?GT_MSG_QUERY_CREATE_REPLY:GT_MSG_CREATE_REPLY,id,p,sizeof(p));}
static void resume_info(fixture *f,bool found){uint64_t id=request(f,GT_MSG_INFO);uint8_t p[68]={0};if(found){gt_managed_info_t row={.id=5,.phase=GT_MANAGED_WORKER};CHECK(gt__control_info_encode(&row,p+4));}else put(p,GT_CONTROL_NOT_FOUND,4);peer_send(f,0,GT_MSG_INFO_REPLY,id,p,sizeof(p));}
static void attach(fixture *f,bool busy) {
  uint64_t id=request(f,GT_MSG_ATTACH);gt_attach_result_t a={.status=busy?GT_ATTACH_BUSY:GT_ATTACH_OK,.session=5};
  if(!busy){a.generation=f->generation;a.ttl_ms=100;a.instance[0]=1;a.token.bytes[0]=1;a.token.bytes[8]=9;}
  uint8_t p[88];CHECK(gt__control_attach_encode(&a,p));peer_send(f,0,GT_MSG_ATTACH_REPLY,id,p,sizeof(p));
}
static void data_hello(fixture *f) {
  hello(f,1,1);gt_wire_frame_t r=frame(f,1);CHECK(r.header.type==GT_MSG_BIND && r.header.payload_length==72 && r.payload[0]==1);CHECK(gt__connection_release_frame(f->peer[1]));
  uint8_t p[36]={0};put(p+4,5,8);put(p+12,f->generation,8);p[20]=8;peer_send(f,1,GT_MSG_BIND_REPLY,0,p,sizeof(p));
  if(info(f).phase==GT_SESSION_FAILED)return;
  r=frame(f,1);CHECK(r.header.type==GT_MSG_BIND_ACK && r.payload[0]==8);CHECK(gt__connection_release_frame(f->peer[1]));pump(f,3);CHECK(info(f).phase==GT_SESSION_RESTORING);
}
static void send_data(fixture *f,gt_data_message_t m){m.generation=f->generation;uint8_t p[128];gt_wire_header_t h;CHECK(gt__data_encode(&m,p,sizeof(p),&h));peer_send(f,1,h.type,0,p,h.payload_length);}
static void snapshot_begin(fixture *f,bool readonly){send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.total=4,.format=7,.flags=readonly?GT_DATA_READONLY:0});send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_CHUNK,.bytes=(const uint8_t *)"BASE",.length=4});send_data(f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_END,.total=4});}
static void live(fixture *f,bool readonly) {
  snapshot_begin(f,readonly);gt_wire_frame_t r=frame(f,1);CHECK(r.header.type==GT_MSG_READY_S);CHECK(gt__connection_release_frame(f->peer[1]));
  send_data(f,(gt_data_message_t){.type=GT_MSG_LIVE_BEGIN});r=frame(f,1);CHECK(r.header.type==GT_MSG_READY_L);CHECK(gt__connection_release_frame(f->peer[1]));pump(f,3);CHECK(info(f).phase==(readonly?GT_SESSION_READONLY:GT_SESSION_LIVE));
}
static void connect_live(fixture *f,bool resume,bool readonly){hello(f,0,1);if(resume)resume_info(f,true);else creation(f,false,GT_CREATION_ACCEPTED);attach(f,false);data_hello(f);live(f,readonly);}
static gt_user_request_t input(void){return (gt_user_request_t){.kind=GT_WORKER_INPUT,.input=(const uint8_t *)"abc",.length=3};}
static void finish(fixture *f){gt__session_client_close(f->client);f->client=NULL;CHECK(!f->live && f->starts==f->resets);for(unsigned i=0;i<2;i++)CHECK(!f->peer[i]);}
static void flow(void) {
  fixture f;setup(&f,false);open_client(&f);strcpy(f.executable,"/bad");connect_live(&f,false,false);CHECK(info(&f).session==5 && info(&f).instance[0]==1 && info(&f).generation==7);
  gt_worker_stamp_t stamp;gt_user_request_t r=input();CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED && stamp.generation==7);gt_wire_frame_t w=frame(&f,1);CHECK(w.header.type==GT_MSG_USER_INPUT);CHECK(gt__connection_release_frame(f.peer[1]));
  f.link=GT_SESSION_LINK_RETRY;f.link_error=GT_ERR_IO;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_RECOVERING && f.invalidations==1 && f.results==1 && f.last.delivery==GT_USER_OUTCOME_UNKNOWN);
  CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);unsigned starts=f.starts;f.link=GT_SESSION_LINK_READY;f.link_error=GT_OK;f.now=9;pump(&f,1);CHECK(f.starts==starts);f.now=10;pump(&f,1);CHECK(f.starts==starts+1);f.generation=8;connect_live(&f,true,false);CHECK(f.snapshots==2 && f.results==1);
  pump(&f,10);CHECK(!gt__connection_frame(f.peer[1],&w));finish(&f);
}
static void uncertain(void) {
  for(unsigned variant=0;variant<2;variant++) {
    fixture f;setup(&f,false);open_client(&f);hello(&f,0,1);(void)request(&f,GT_MSG_CREATE);
    f.link=GT_SESSION_LINK_RETRY;f.link_error=GT_ERR_IO;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_RECOVERING);f.link=GT_SESSION_LINK_READY;f.link_error=GT_OK;f.now=10;hello(&f,0,1);
    creation(&f,true,variant?GT_CREATION_UNKNOWN:GT_CREATION_ACCEPTED);
    if(variant){CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_CREATE_UNKNOWN && info(&f).error==GT_ERR_NOT_FOUND && f.starts==2);}
    else{attach(&f,false);data_hello(&f);live(&f,false);CHECK(info(&f).session==5 && f.starts==2);}finish(&f);
  }
}
static void deadlines(void) {
  fixture f;setup(&f,true);f.link=GT_SESSION_LINK_WAIT;open_client(&f);pump(&f,3);CHECK(f.starts==1);f.now=500;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_DEADLINE && f.resets==1);finish(&f);
  setup(&f,true);f.link=GT_SESSION_LINK_RETRY;f.link_error=GT_ERR_IO;open_client(&f);for(unsigned i=0;i<50;i++){f.now=i*10;pump(&f,1);}CHECK(info(&f).phase==GT_SESSION_RECOVERING);f.now=500;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_DEADLINE);finish(&f);
}
static void busy(void) {
  fixture f;setup(&f,true);open_client(&f);hello(&f,0,1);resume_info(&f,true);attach(&f,true);CHECK(info(&f).busy_replies==1);unsigned streams=f.streams;pump(&f,5);CHECK(f.streams==streams);f.now=10;attach(&f,false);data_hello(&f);live(&f,false);finish(&f);
}
static void heartbeat(void) {
  fixture f;setup(&f,true);open_client(&f);hello(&f,0,1);resume_info(&f,true);attach(&f,false);data_hello(&f);f.now=20;uint64_t id=request(&f,GT_MSG_PING);peer_send(&f,0,GT_MSG_PING_REPLY,id,NULL,0);gt_wire_frame_t first=frame(&f,1);gt_data_message_t ping;CHECK(gt__data_decode(&first,&ping) && ping.type==GT_MSG_DATA_PING);CHECK(gt__connection_release_frame(f.peer[1]));send_data(&f,(gt_data_message_t){.type=GT_MSG_DATA_PONG,.nonce=ping.nonce});live(&f,false);
  f.now=40;id=request(&f,GT_MSG_PING);peer_send(&f,0,GT_MSG_PING_REPLY,id,NULL,0);gt_wire_frame_t r=frame(&f,1);gt_data_message_t m;CHECK(gt__data_decode(&r,&m) && m.type==GT_MSG_DATA_PING);CHECK(gt__connection_release_frame(f.peer[1]));send_data(&f,(gt_data_message_t){.type=GT_MSG_DATA_PONG,.nonce=m.nonce});CHECK(info(&f).phase==GT_SESSION_LIVE);finish(&f);
}
static void identity(void) {
  fixture f;setup(&f,true);open_client(&f);hello(&f,0,2);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).error==GT_ERR_PROTOCOL && f.resets==1);f.now=20;pump(&f,1);CHECK(f.starts==1);finish(&f);
  setup(&f,true);open_client(&f);hello(&f,0,1);resume_info(&f,false);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_NOT_FOUND && f.starts==1);finish(&f);
}
static void sink_barrier(void) {
  fixture f;setup(&f,true);open_client(&f);hello(&f,0,1);resume_info(&f,true);attach(&f,false);data_hello(&f);f.delay_apply=true;snapshot_begin(&f,false);CHECK(info(&f).phase==GT_SESSION_RESTORING && f.snapshots==1);gt_user_request_t r=input();gt_worker_stamp_t stamp;CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED);gt_wire_frame_t out;CHECK(!gt__connection_frame(f.peer[1],&out));
  f.applied=(gt_client_applied_t){true,f.generation,0};pump(&f,8);out=frame(&f,1);CHECK(out.header.type==GT_MSG_READY_S);CHECK(gt__connection_release_frame(f.peer[1]));send_data(&f,(gt_data_message_t){.type=GT_MSG_LIVE_BEGIN});out=frame(&f,1);CHECK(out.header.type==GT_MSG_READY_L);CHECK(gt__connection_release_frame(f.peer[1]));pump(&f,3);CHECK(info(&f).phase==GT_SESSION_LIVE);finish(&f);
  setup(&f,true);open_client(&f);hello(&f,0,1);resume_info(&f,true);attach(&f,false);data_hello(&f);f.sink_error=GT_ERR_UNSUPPORTED;snapshot_begin(&f,false);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_ATTACHMENT && f.starts==1 && f.invalidations==1);finish(&f);
}
static void readonly(void) {
  fixture f;setup(&f,true);open_client(&f);connect_live(&f,true,true);gt_user_request_t r=input();gt_worker_stamp_t stamp;CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED);
  gt_managed_info_t terminal={.id=5,.phase=GT_MANAGED_RETAINED,.has_result=true,.result={.kind=GT_WORKER_END_EOF,.status=GT_OK,.has_sequence=true,.termination_status=GT_ERR_NOT_FOUND}};
  send_data(&f,(gt_data_message_t){.type=GT_MSG_SESSION_END,.terminal=terminal});CHECK(info(&f).phase==GT_SESSION_ENDED);finish(&f);
}
static void fatal(void) {
  for(unsigned variant=0;variant<5;variant++){fixture f;setup(&f,true);f.link=variant==4?GT_SESSION_LINK_RETRY:GT_SESSION_LINK_FATAL;f.link_error=variant==0?GT_ERR_AUTH:variant==1?GT_ERR_HOST_KEY:variant==2?GT_ERR_UNSUPPORTED:variant==3?GT_ERR_IO:GT_OK;open_client(&f);pump(&f,1);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).error!=GT_OK && f.resets==1);f.now=10;pump(&f,1);CHECK(f.starts==1);finish(&f);}
}
static void clock_error(void) {
  for(unsigned variant=0;variant<2;variant++){fixture f;setup(&f,true);f.link=GT_SESSION_LINK_WAIT;open_client(&f);f.now=10;pump(&f,1);if(variant)f.clock_fail=true;else f.now=9;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_CLOCK && f.resets==1);finish(&f);}
  fixture f;setup(&f,true);open_client(&f);connect_live(&f,true,false);f.fail_clock=f.clocks+3;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_CLOCK && f.resets==1);finish(&f);
}
static void allocation(void) {
  for(size_t fail=1;fail<=8;fail++) {
    fixture f;setup(&f,true);f.fail=fail;gt_allocator_t a={&f,admit,refund};gt_status_t rc=gt__session_client_open(&f.config,&a,&f.client);
    if(rc!=GT_OK){CHECK(!f.client && !f.live && !f.starts);continue;}
    for(unsigned n=0;n<200 && info(&f).phase!=GT_SESSION_FAILED;n++) {
      pump(&f,1);if(f.peer[0]){gt_wire_frame_t w;if(gt__connection_frame(f.peer[0],&w))break;}
    }
    if(info(&f).phase!=GT_SESSION_FAILED){hello(&f,0,1);if(info(&f).phase!=GT_SESSION_FAILED){resume_info(&f,true);attach(&f,false);if(info(&f).phase!=GT_SESSION_FAILED)data_hello(&f);}}
    if(fail==8 && info(&f).phase!=GT_SESSION_FAILED)send_data(&f,(gt_data_message_t){.type=GT_MSG_SNAPSHOT_BEGIN,.total=4,.format=7});
    CHECK(info(&f).phase==GT_SESSION_FAILED && f.calls>=fail);finish(&f);
  }
}
static void network(void) {
  for(unsigned variant=0;variant<3;variant++) {
    fixture f;setup(&f,true);open_client(&f);connect_live(&f,true,false);
    if(variant<2){gt__connection_destroy(f.peer[variant]);f.peer[variant]=NULL;}
    else {
      f.now=20;uint64_t id=request(&f,GT_MSG_PING);peer_send(&f,0,GT_MSG_PING_REPLY,id,NULL,0);
      f.now=40;id=request(&f,GT_MSG_PING);peer_send(&f,0,GT_MSG_PING_REPLY,id,NULL,0);
      f.now=80;
    }
    pump(&f,1);CHECK(info(&f).phase==GT_SESSION_RECOVERING && f.resets==1 && f.invalidations==1);
    f.now+=10;f.generation=8;connect_live(&f,true,false);CHECK(f.starts==2 && info(&f).session==5);finish(&f);
  }
}
static void generation(void) {
  fixture f;setup(&f,true);open_client(&f);connect_live(&f,true,false);
  f.link=GT_SESSION_LINK_RETRY;f.link_error=GT_ERR_IO;pump(&f,1);f.link=GT_SESSION_LINK_READY;f.link_error=GT_OK;f.now=10;
  hello(&f,0,1);resume_info(&f,true);attach(&f,false);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).error==GT_ERR_PROTOCOL && f.starts==2);finish(&f);
}
static void restore_deadline(void) {
  fixture f;setup(&f,true);f.config.recovery_timeout_ms=30;open_client(&f);hello(&f,0,1);resume_info(&f,true);attach(&f,false);data_hello(&f);
  f.now=30;pump(&f,1);CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).reason==GT_SESSION_REASON_DEADLINE && f.invalidations==1);finish(&f);
}
static void compatibility(void) {
  fixture f;setup(&f,true);open_client(&f);gt_wire_frame_t r=frame(&f,0);CHECK(r.header.type==GT_MSG_HELLO);CHECK(gt__connection_release_frame(f.peer[0]));
  uint8_t p[80]={0};p[5]=GT_ROLE_MANAGEMENT;p[8]=3;p[48]=1;p[64]=9;peer_send(&f,0,GT_MSG_HELLO_REPLY,1,p,80);
  CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).error==GT_ERR_UNSUPPORTED && f.starts==1 && f.resets==1);finish(&f);
}
static void validation(void) {
  fixture f;setup(&f,false);f.create.environment=2;gt_allocator_t a={&f,admit,refund};
  CHECK(gt__session_client_open(&f.config,&a,&f.client)==GT_ERR_UNSUPPORTED && !f.client && !f.starts && !f.live);
  f.config.session=5;CHECK(gt__session_client_open(&f.config,&a,&f.client)==GT_ERR_INVALID_ARG && !f.client && !f.starts && !f.live);
}
static void cancel(void) {
  fixture f;setup(&f,true);f.link=GT_SESSION_LINK_WAIT;open_client(&f);pump(&f,1);gt__session_client_cancel(f.client);CHECK(info(&f).phase==GT_SESSION_CLOSED && f.resets==1);pump(&f,5);CHECK(f.starts==1);finish(&f);
  setup(&f,true);open_client(&f);connect_live(&f,true,false);gt_user_request_t r=input();gt_worker_stamp_t stamp;CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_QUEUED);gt__session_client_cancel(f.client);CHECK(info(&f).phase==GT_SESSION_CLOSED && f.invalidations==1 && f.results==1 && f.last.delivery==GT_USER_OUTCOME_UNKNOWN);finish(&f);
}
int main(int argc,char **argv){CHECK(argc==2);if(!strcmp(argv[1],"flow"))flow();else if(!strcmp(argv[1],"uncertain"))uncertain();else if(!strcmp(argv[1],"deadlines"))deadlines();else if(!strcmp(argv[1],"busy"))busy();else if(!strcmp(argv[1],"heartbeat"))heartbeat();else if(!strcmp(argv[1],"identity"))identity();else if(!strcmp(argv[1],"sink"))sink_barrier();else if(!strcmp(argv[1],"readonly"))readonly();else if(!strcmp(argv[1],"fatal"))fatal();else if(!strcmp(argv[1],"clock"))clock_error();else if(!strcmp(argv[1],"allocation"))allocation();else if(!strcmp(argv[1],"network"))network();else if(!strcmp(argv[1],"generation"))generation();else if(!strcmp(argv[1],"restore-deadline"))restore_deadline();else if(!strcmp(argv[1],"compatibility"))compatibility();else if(!strcmp(argv[1],"validation"))validation();else{CHECK(!strcmp(argv[1],"cancel"));cancel();}printf("PASS session client %s\n",argv[1]);return 0;}
