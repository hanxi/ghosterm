#define _POSIX_C_SOURCE 200809L
#include "ipc/client-bootstrap.h"
#include "ipc/control-client.h"
#include "ipc/socket-stream.h"
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef union {max_align_t align;size_t n;} block;
typedef struct fixture fixture;
typedef struct {gt_socket_stream_t socket;gt_stream_ops_t base;fixture *f;size_t quota;} stream;
struct fixture {
  stream a,b;gt_connection_t *raw,*peer;gt_client_bootstrap_t *boot;gt_control_client_t *control;
  gt_client_bootstrap_config_t config;gt_control_result_t last;unsigned results;uint64_t now;
  bool clock_fail,fixed_stream_clock;size_t calls,fail,live;
};
static void *admit(void *ctx,size_t n){fixture *f=ctx;if(++f->calls==f->fail)return NULL;block *b=malloc(sizeof(*b)+n);CHECK(b);b->n=n;f->live++;return b+1;}
static void refund(void *ctx,void *p,size_t n){fixture *f=ctx;block *b=(block *)p-1;CHECK(b->n==n && f->live);const uint8_t *bytes=p;for(size_t i=0;i<n;i++)CHECK(!bytes[i]);f->live--;free(b);}
static bool clock_now(void *ctx,uint64_t *now){fixture *f=ctx;if(f->clock_fail)return false;*now=f->now;return true;}
static bool stream_clock(void *ctx,uint64_t *now){stream *s=ctx;*now=s->f->fixed_stream_clock?0:s->f->now;return true;}
static gt_stream_result_t read_stream(void *ctx,void *p,size_t n){stream *s=ctx;return s->base.read(s->base.context,p,n);}
static gt_stream_result_t write_stream(void *ctx,const void *p,size_t n){stream *s=ctx;if(!s->quota)return (gt_stream_result_t){.kind=GT_STREAM_WAIT};if(n>s->quota)n=s->quota;return s->base.write(s->base.context,p,n);}
static void close_stream(void *ctx){stream *s=ctx;s->base.close(s->base.context);}
static void result(void *ctx,const gt_control_result_t *r){fixture *f=ctx;f->last=*r;f->results++;}
static void setup(fixture *f,bool data) {
  memset(f,0,sizeof(*f));f->config=(gt_client_bootstrap_config_t){.hello={.role=data?GT_ROLE_DATA:GT_ROLE_MANAGEMENT,.required_features=3,.compatibility={2},.expected_instance={1}},.timeout_ms=100,.clock_context=f,.clock=clock_now};
  if(data)f->config.binding=(gt_protocol_bind_t){.instance={1},.session=5,.generation=7,.token={{1,0,0,0,0,0,0,0,9}}};
  int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));f->a.f=f->b.f=f;f->a.quota=f->b.quota=SIZE_MAX;
  CHECK(gt__socket_stream_adopt(sockets[0],&f->a.socket,&f->a.base)==GT_OK && gt__socket_stream_adopt(sockets[1],&f->b.socket,&f->b.base)==GT_OK);
  gt_stream_ops_t a={&f->a,read_stream,write_stream,stream_clock,close_stream},b={&f->b,read_stream,write_stream,stream_clock,close_stream};gt_allocator_t domain={f,admit,refund};
  CHECK(gt__connection_open(&a,(gt_connection_limits_t){65560,65560,1000},&domain,&f->raw)==GT_CONNECTION_CREATED && gt__connection_open(&b,(gt_connection_limits_t){65560,65560,1000},&domain,&f->peer)==GT_CONNECTION_CREATED);
}
static void open_boot(fixture *f){gt_allocator_t a={f,admit,refund};CHECK(gt__client_bootstrap_open(f->raw,&f->config,&a,&f->boot)==GT_OK);f->raw=NULL;}
static void pump(fixture *f,unsigned n){for(unsigned i=0;i<n;i++){if(f->peer)(void)gt__connection_step(f->peer);if(f->boot)(void)gt__client_bootstrap_step(f->boot);if(f->control)(void)gt__control_client_step(f->control);}}
static gt_wire_frame_t frame(fixture *f){for(unsigned i=0;i<1000;i++){gt_wire_frame_t r;if(gt__connection_frame(f->peer,&r))return r;pump(f,1);}CHECK(false);return (gt_wire_frame_t){0};}
static void expect(fixture *f,uint16_t type,uint64_t id,const void *bytes,size_t n){gt_wire_frame_t r=frame(f);CHECK(r.header.type==type && r.header.request_id==id && r.header.payload_length==n && (!n || !memcmp(r.payload,bytes,n)));CHECK(gt__connection_release_frame(f->peer));}
static void peer_send(fixture *f,uint16_t type,uint64_t id,const void *bytes,size_t n){gt_wire_header_t h={.type=type,.request_id=id,.payload_length=(uint32_t)n};CHECK(gt__connection_send(f->peer,&h,bytes)==GT_SEND_ACCEPTED);pump(f,8);}
static void put(uint8_t *p,uint64_t v,size_t n){for(size_t i=0;i<n;i++)p[n-1-i]=(uint8_t)(v>>(8*i));}
static void hello_bytes(fixture *f,uint8_t p[80],gt_hello_status_t status){memset(p,0,80);put(p,status,4);p[5]=(uint8_t)f->config.hello.role;p[8]=2;put(p+40,7,8);p[48]=1;p[64]=9;}
static void bind_bytes(uint8_t p[36],gt_hello_status_t status){memset(p,0,36);put(p,status,4);put(p+4,5,8);put(p+12,7,8);p[20]=8;}
static gt_bootstrap_info_t info(fixture *f){gt_bootstrap_info_t r;CHECK(gt__client_bootstrap_info(f->boot,&r));return r;}
static void hello(fixture *f) {
  uint8_t request[60],reply[80];CHECK(gt__hello_encode(&f->config.hello,request));expect(f,GT_MSG_HELLO,f->config.hello.role==GT_ROLE_MANAGEMENT?1:0,request,sizeof(request));
  hello_bytes(f,reply,GT_HELLO_OK);peer_send(f,GT_MSG_HELLO_REPLY,f->config.hello.role==GT_ROLE_MANAGEMENT?1:0,reply,sizeof(reply));
  expect(f,GT_MSG_HELLO_ACK,0,reply+64,16);pump(f,1);
}
static void finish(fixture *f){if(f->boot)gt__client_bootstrap_close(f->boot);if(f->control)gt__control_client_close(f->control);if(f->raw)gt__connection_destroy(f->raw);if(f->peer)gt__connection_destroy(f->peer);CHECK(!f->live);}
static void bootstrap_flow(bool data) {
  fixture f;setup(&f,data);open_boot(&f);hello(&f);
  if(data) {
    uint8_t request[72],reply[36];CHECK(gt__bind_encode(&f.config.binding,request));expect(&f,GT_MSG_BIND,0,request,sizeof(request));
    bind_bytes(reply,GT_HELLO_OK);f.a.quota=0;peer_send(&f,GT_MSG_BIND_REPLY,0,reply,sizeof(reply));CHECK(info(&f).state==GT_BOOTSTRAP_RUNNING);
    f.a.quota=1;pump(&f,8);CHECK(info(&f).state==GT_BOOTSTRAP_RUNNING);f.a.quota=SIZE_MAX;expect(&f,GT_MSG_BIND_ACK,0,reply+20,16);pump(&f,1);
  }
  CHECK(info(&f).state==GT_BOOTSTRAP_READY && info(&f).has_status && info(&f).status==GT_HELLO_OK);
  peer_send(&f,GT_MSG_PING_REPLY,29,NULL,0);gt_bootstrap_result_t r;CHECK(gt__client_bootstrap_take(f.boot,&f.raw,&r) && r.identity.instance[0]==1 && r.identity.compatibility[0]==2 && r.identity.features==7 && r.role==f.config.hello.role);
  CHECK(info(&f).state==GT_BOOTSTRAP_TAKEN);gt_wire_frame_t application;CHECK(gt__connection_frame(f.raw,&application) && application.header.type==GT_MSG_PING_REPLY && application.header.request_id==29);CHECK(gt__connection_release_frame(f.raw));
  CHECK(r.session==(data?5u:0u) && r.generation==(data?7u:0u));gt_connection_t *again=(void *)1;memset(&r,0xa5,sizeof(r));CHECK(!gt__client_bootstrap_take(f.boot,&again,&r) && !again && !r.identity.instance[0]);finish(&f);
}
static void rejections(void) {
  for(unsigned status=1;status<=4;status++) {
    fixture f;setup(&f,false);open_boot(&f);uint8_t request[60],reply[80];CHECK(gt__hello_encode(&f.config.hello,request));expect(&f,GT_MSG_HELLO,1,request,60);
    hello_bytes(&f,reply,(gt_hello_status_t)status);peer_send(&f,GT_MSG_HELLO_REPLY,1,reply,80);expect(&f,GT_MSG_HELLO_ACK,0,reply+64,16);CHECK(info(&f).state==GT_BOOTSTRAP_DRAINING);
    gt__connection_destroy(f.peer);f.peer=NULL;pump(&f,1);CHECK(info(&f).state==GT_BOOTSTRAP_FAILED && info(&f).has_status && info(&f).status==status && info(&f).error==(status==1?GT_BOOTSTRAP_COMPATIBILITY:status==2?GT_BOOTSTRAP_FEATURES:status==3?GT_BOOTSTRAP_INSTANCE:GT_BOOTSTRAP_DENIED));finish(&f);
  }
  fixture f;setup(&f,true);open_boot(&f);hello(&f);uint8_t b[72],reply[36];CHECK(gt__bind_encode(&f.config.binding,b));expect(&f,GT_MSG_BIND,0,b,72);bind_bytes(reply,GT_HELLO_DENIED);peer_send(&f,GT_MSG_BIND_REPLY,0,reply,36);expect(&f,GT_MSG_BIND_ACK,0,reply+20,16);CHECK(info(&f).state==GT_BOOTSTRAP_DRAINING);f.now=100;pump(&f,1);CHECK(info(&f).state==GT_BOOTSTRAP_FAILED && info(&f).error==GT_BOOTSTRAP_DENIED);finish(&f);
}
static void malformed(void) {
  for(unsigned variant=0;variant<8;variant++) {
    fixture f;setup(&f,variant>=5);open_boot(&f);uint8_t bytes[80],req[72];
    if(variant<5) {
      CHECK(gt__hello_encode(&f.config.hello,req));expect(&f,GT_MSG_HELLO,1,req,60);hello_bytes(&f,bytes,GT_HELLO_OK);
      if(!variant)bytes[8]=3;else if(variant==1)put(bytes+40,1,8);else if(variant==2)bytes[48]=2;else if(variant==3)bytes[5]=GT_ROLE_DATA;
      peer_send(&f,GT_MSG_HELLO_REPLY,variant==4?2:1,bytes,80);
    } else {hello(&f);CHECK(gt__bind_encode(&f.config.binding,req));expect(&f,GT_MSG_BIND,0,req,72);bind_bytes(bytes,GT_HELLO_OK);if(variant==5)put(bytes+4,6,8);else if(variant==6)put(bytes+12,8,8);peer_send(&f,GT_MSG_BIND_REPLY,variant==7?1:0,bytes,36);}
    CHECK(info(&f).state==GT_BOOTSTRAP_FAILED);gt_wire_frame_t out;CHECK(!gt__connection_frame(f.peer,&out));finish(&f);
  }
}
static void deadlines(void) {
  fixture f;setup(&f,true);open_boot(&f);hello(&f);uint8_t request[72];CHECK(gt__bind_encode(&f.config.binding,request));expect(&f,GT_MSG_BIND,0,request,72);
  f.now=100;pump(&f,1);CHECK(info(&f).state==GT_BOOTSTRAP_FAILED && info(&f).error==GT_BOOTSTRAP_TIMEOUT);finish(&f);
  setup(&f,false);f.fixed_stream_clock=true;open_boot(&f);f.now=10;pump(&f,1);f.now=9;(void)gt__client_bootstrap_step(f.boot);CHECK(info(&f).error==GT_BOOTSTRAP_CLOCK);finish(&f);
}
static void open_control(fixture *f) {
  setup(f,false);gt_control_client_config_t config={.instance={1},.heartbeat_ms=10,.request_timeout_ms=50,.context=f,.clock=clock_now,.result=result};gt_allocator_t a={f,admit,refund};
  CHECK(gt__control_client_open(f->raw,&config,&a,&f->control)==GT_OK);f->raw=NULL;
}
static uint64_t submit(fixture *f,gt_control_request_t request) {
  uint64_t id=0;gt_status_t error;CHECK(gt__control_client_submit(f->control,&request,&id,&error)==GT_CONTROL_CLIENT_QUEUED && id && error==GT_OK);gt_wire_frame_t r=frame(f);CHECK(r.header.type==request.type && r.header.request_id==id);uint8_t bytes[GT_CREATE_MAX_BYTES]={0};size_t n=0;
  if(request.type==GT_MSG_CREATE)CHECK(gt__create_spec_encode(request.create,bytes,sizeof(bytes),&n)==GT_OK);
  else if(request.type==GT_MSG_QUERY_CREATE){n=16;memcpy(bytes,request.nonce,16);}
  else if(request.type==GT_MSG_INFO || request.type==GT_MSG_ATTACH || request.type==GT_MSG_TERMINATE){n=8;put(bytes,request.session,8);}
  CHECK(r.header.payload_length==n && (!n || !memcmp(r.payload,bytes,n)));CHECK(gt__connection_release_frame(f->peer));return id;
}
static void control_flow(void) {
  fixture f;open_control(&f);uint64_t id=submit(&f,(gt_control_request_t){.type=GT_MSG_LIST});uint8_t bytes[GT_CONTROL_REPLY_BYTES]={0};put(bytes+4,1,4);gt_managed_info_t row={.id=5,.phase=GT_MANAGED_WORKER};CHECK(gt__control_info_encode(&row,bytes+8));peer_send(&f,GT_MSG_LIST_REPLY,id,bytes,72);CHECK(f.results==1 && f.last.count==1 && f.last.rows[0].id==5);
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_INFO,.session=5});memset(bytes,0,sizeof(bytes));CHECK(gt__control_info_encode(&row,bytes+4));peer_send(&f,GT_MSG_INFO_REPLY,id,bytes,68);CHECK(f.last.count==1 && f.last.session==5);
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_TERMINATE,.session=5});memset(bytes,0,sizeof(bytes));put(bytes+4,5,8);peer_send(&f,GT_MSG_TERMINATE_REPLY,id,bytes,12);CHECK(f.last.status==GT_CONTROL_OK && f.last.reply_type==GT_MSG_TERMINATE_REPLY);
  gt_create_spec_t spec={.nonce={9},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,.executable="/bin/sh",.cwd="/",.term="xterm"};
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});memset(bytes,0,sizeof(bytes));bytes[0]=9;put(bytes+24,5,8);peer_send(&f,GT_MSG_CREATE_REPLY,id,bytes,32);CHECK(f.last.creation.status==GT_CREATION_ACCEPTED && f.last.creation.session==5 && f.last.nonce[0]==9);
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_QUERY_CREATE,.nonce={9}});put(bytes+16,GT_CREATION_UNKNOWN,4);put(bytes+24,0,8);peer_send(&f,GT_MSG_QUERY_CREATE_REPLY,id,bytes,32);CHECK(f.last.creation.status==GT_CREATION_UNKNOWN && f.last.delivery==GT_CONTROL_CLIENT_REPLY);
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_ATTACH,.session=5});gt_attach_result_t attach={.status=GT_ATTACH_OK,.session=5,.generation=7,.ttl_ms=50,.instance={1},.token={{1,0,0,0,0,0,0,0,9}}};CHECK(gt__control_attach_encode(&attach,bytes));peer_send(&f,GT_MSG_ATTACH_REPLY,id,bytes,88);CHECK(f.last.attachment.generation==7 && f.last.attachment.token.bytes[8]==9);
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_INFO,.session=99});memset(bytes,0,sizeof(bytes));put(bytes,GT_CONTROL_NOT_FOUND,4);peer_send(&f,GT_MSG_INFO_REPLY,id,bytes,68);CHECK(f.last.status==GT_CONTROL_NOT_FOUND && !f.last.count);
  id=submit(&f,(gt_control_request_t){.type=GT_MSG_LIST});put(bytes,GT_CONTROL_INTERNAL,4);peer_send(&f,GT_MSG_CONTROL_ERROR,id,bytes,4);CHECK(f.last.reply_type==GT_MSG_CONTROL_ERROR && f.last.status==GT_CONTROL_INTERNAL);finish(&f);
}
static void control_bad(void) {
  for(unsigned variant=0;variant<8;variant++) {
    fixture f;open_control(&f);gt_control_request_t request={.type=variant==2?GT_MSG_QUERY_CREATE:variant==3?GT_MSG_INFO:variant==4?GT_MSG_ATTACH:GT_MSG_LIST,.session=5,.nonce={9}};
    uint64_t id=submit(&f,request);uint8_t bytes[160]={0};uint16_t type=GT_MSG_LIST_REPLY;size_t n=8;
    if(!variant)id++;else if(variant==1)type=GT_MSG_PING_REPLY;
    else if(variant==2){type=GT_MSG_QUERY_CREATE_REPLY;n=32;bytes[0]=8;put(bytes+16,GT_CREATION_UNKNOWN,4);}
    else if(variant==3){type=GT_MSG_INFO_REPLY;n=68;gt_managed_info_t row={.id=6,.phase=GT_MANAGED_WORKER};CHECK(gt__control_info_encode(&row,bytes+4));}
    else if(variant==4){type=GT_MSG_ATTACH_REPLY;n=88;gt_attach_result_t r={.status=GT_ATTACH_OK,.session=5,.generation=7,.ttl_ms=50,.instance={2},.token={{1,0,0,0,0,0,0,0,9}}};CHECK(gt__control_attach_encode(&r,bytes));}
    else if(variant==5){put(bytes+4,2,4);n=136;gt_managed_info_t row={.id=5,.phase=GT_MANAGED_WORKER};CHECK(gt__control_info_encode(&row,bytes+8) && gt__control_info_encode(&row,bytes+72));}
    else if(variant==6){put(bytes+4,65,4);}
    else {put(bytes,GT_CONTROL_NOT_FOUND,4);put(bytes+4,1,4);n=72;gt_managed_info_t row={.id=5,.phase=GT_MANAGED_WORKER};CHECK(gt__control_info_encode(&row,bytes+8));}
    peer_send(&f,type,id,bytes,n);CHECK(gt__control_client_error(f.control)==GT_ERR_PROTOCOL && f.results==1 && f.last.delivery==GT_CONTROL_CLIENT_UNKNOWN);finish(&f);
  }
}
static void control_timeout(void) {
  fixture f;open_control(&f);gt_create_spec_t spec={.nonce={9},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,.executable="/bin/sh",.cwd="/",.term="xterm"};(void)submit(&f,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});
  gt_control_request_t next={.type=GT_MSG_LIST};uint64_t id=7;gt_status_t error;CHECK(gt__control_client_submit(f.control,&next,&id,&error)==GT_CONTROL_CLIENT_BACKPRESSURE && !id);f.now=50;pump(&f,1);CHECK(f.results==1 && f.last.delivery==GT_CONTROL_CLIENT_UNKNOWN && f.last.nonce[0]==9 && gt__control_client_error(f.control)==GT_ERR_CANCELLED);finish(&f);
  open_control(&f);(void)submit(&f,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});gt__connection_destroy(f.peer);f.peer=NULL;pump(&f,1);CHECK(f.results==1 && f.last.delivery==GT_CONTROL_CLIENT_UNKNOWN && f.last.nonce[0]==9 && gt__control_client_error(f.control)==GT_ERR_IO);finish(&f);
  open_control(&f);f.fixed_stream_clock=true;(void)submit(&f,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});f.now=10;pump(&f,1);f.now=9;pump(&f,1);CHECK(f.results==1 && f.last.delivery==GT_CONTROL_CLIENT_UNKNOWN && gt__control_client_error(f.control)==GT_ERR_IO);finish(&f);
}
static void heartbeat(void) {
  fixture f;open_control(&f);f.now=10;pump(&f,2);gt_wire_frame_t ping=frame(&f);CHECK(ping.header.type==GT_MSG_PING && ping.header.request_id==1 && !ping.header.payload_length);CHECK(gt__connection_release_frame(f.peer));peer_send(&f,GT_MSG_PING_REPLY,1,NULL,0);CHECK(!f.results);
  uint64_t id=submit(&f,(gt_control_request_t){.type=GT_MSG_PING});CHECK(id==2);peer_send(&f,GT_MSG_PING_REPLY,id,NULL,0);CHECK(f.results==1);finish(&f);
}
static void allocation(void) {
  fixture f;setup(&f,true);gt_allocator_t a={&f,admit,refund};gt_client_bootstrap_t *c=(void *)1;size_t calls=f.calls;
  f.config.binding.instance[0]=2;CHECK(gt__client_bootstrap_open(f.raw,&f.config,&a,&c)==GT_ERR_INVALID_ARG && !c && f.calls==calls);f.config.binding.instance[0]=1;
  f.fail=f.calls+1;CHECK(gt__client_bootstrap_open(f.raw,&f.config,&a,&c)==GT_ERR_IO && !c);f.fail=0;finish(&f);
  open_control(&f);gt_create_spec_t spec={.nonce={9},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=2,.executable="/bin/sh",.cwd="/",.term="xterm"};
  gt_control_request_t req={.type=GT_MSG_CREATE,.create=&spec};uint64_t id=9;gt_status_t error=GT_OK;
  CHECK(gt__control_client_submit(f.control,&req,&id,&error)==GT_CONTROL_CLIENT_UNSUPPORTED && error==GT_ERR_UNSUPPORTED && !id && !f.results);
  req=(gt_control_request_t){.type=GT_MSG_INFO};CHECK(gt__control_client_submit(f.control,&req,&id,&error)==GT_CONTROL_CLIENT_INVALID && error==GT_ERR_INVALID_ARG && !id);
  CHECK(submit(&f,(gt_control_request_t){.type=GT_MSG_PING})==1);finish(&f);
}
int main(int argc,char **argv) {
  CHECK(argc==2);if(!strcmp(argv[1],"management"))bootstrap_flow(false);else if(!strcmp(argv[1],"data"))bootstrap_flow(true);else if(!strcmp(argv[1],"rejections"))rejections();
  else if(!strcmp(argv[1],"malformed"))malformed();else if(!strcmp(argv[1],"deadlines"))deadlines();else if(!strcmp(argv[1],"control"))control_flow();
  else if(!strcmp(argv[1],"control-bad"))control_bad();else if(!strcmp(argv[1],"control-timeout"))control_timeout();else if(!strcmp(argv[1],"heartbeat"))heartbeat();else {CHECK(!strcmp(argv[1],"allocation"));allocation();}
  printf("PASS client bootstrap %s\n",argv[1]);return 0;
}
