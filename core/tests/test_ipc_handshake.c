#define _POSIX_C_SOURCE 200809L
#include "ipc/handshake.h"
#include "ipc/socket-stream.h"
#include "session/attachment.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct {
  gt_connection_t *server,*client;gt_socket_stream_t ss,cs;
  gt_handshake_t *handshake;gt_token_store_t *tokens;
  gt_checkpoint_tasks_t *tasks;gt_session_attachment_t *attachment;
  gt_stream_ops_t server_ops;gt_handshake_config_t config;
  uint64_t now,generation;unsigned random_calls,drops;
  bool clock_ok,native;gt_status_t entropy_status;
} fixture;
static gt_status_t random_fill(void *ctx,uint8_t *bytes,size_t n) {
  fixture *f=ctx;f->random_calls++;memset(bytes,0x40+f->random_calls,n);return f->entropy_status;
}
static bool clock_ms(void *ctx,uint64_t *out) {
  fixture *f=ctx;if(!f->clock_ok)return false;
  if(f->native)return f->server_ops.clock(f->server_ops.context,out);
  *out=f->now;return true;
}
static bool current(void *ctx,const gt_token_scope_t *scope) {
  fixture *f=ctx;return scope->session==3 && gt__attachment_current(f->attachment,scope->generation);
}
static void drop(void *ctx,const gt_token_scope_t *scope) {
  fixture *f=ctx;CHECK(scope->session==3);f->drops++;(void)gt__attachment_detach(f->attachment,scope->generation);
}
static void open_fixture_config(fixture *f,bool native,bool token_service) {
  memset(f,0,sizeof(*f));f->native=native;f->clock_ok=true;
  int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));gt_stream_ops_t client_ops;
  CHECK(gt__socket_stream_adopt(pair[0],&f->ss,&f->server_ops)==GT_OK);
  CHECK(gt__socket_stream_adopt(pair[1],&f->cs,&client_ops)==GT_OK);
  CHECK(gt__connection_open(&f->server_ops,(gt_connection_limits_t){128,128,1000},NULL,&f->server)==GT_CONNECTION_CREATED);
  CHECK(gt__connection_open(&client_ops,(gt_connection_limits_t){128,128,1000},NULL,&f->client)==GT_CONNECTION_CREATED);
  f->tasks=gt__tasks_open(1);CHECK(f->tasks);f->attachment=gt__attachment_open(f->tasks,128,8);CHECK(f->attachment);
  CHECK(gt__attachment_attach(f->attachment,&f->generation)==GT_TASK_OK);
  f->config=(gt_handshake_config_t){.principal=1,.timeout_ms=1000,.clock_context=f,.clock=clock_ms,
    .random={f,native?NULL:random_fill},.binding_context=f,.binding_current=current,.binding_drop=drop};
  uint8_t compatibility[GT_COMPAT_BYTES]={0x11};gt_token_random_t random={f,random_fill};
  CHECK(gt__protocol_identity_new(compatibility,5,native?NULL:&random,&f->config.identity)==GT_OK);
  CHECK(clock_ms(f,&f->now));gt_token_owner_t owner={f,drop};
  CHECK(gt__tokens_open(f->config.identity.instance,4,f->now,native?NULL:&random,owner,NULL,&f->tokens)==GT_TOKEN_OK);
  f->config.tokens=token_service?f->tokens:NULL;
  CHECK(gt__handshake_open(f->server,&f->config,NULL,&f->handshake)==GT_OK);
}
static void open_fixture(fixture *f,bool native){open_fixture_config(f,native,true);}
static void close_fixture(fixture *f) {
  gt__handshake_close(f->handshake);gt__connection_destroy(f->client);gt__connection_destroy(f->server);
  gt__tokens_close(f->tokens);(void)gt__attachment_detach(f->attachment,f->generation);
  CHECK(gt__attachment_close(f->attachment)==GT_TASK_OK && gt__tasks_close(f->tasks)==GT_TASK_OK);
}
static void send_frame(fixture *f,uint16_t type,uint64_t id,const void *bytes,size_t n) {
  gt_wire_header_t h={.type=type,.request_id=id,.payload_length=(uint32_t)n};
  CHECK(gt__connection_send(f->client,&h,bytes)==GT_SEND_ACCEPTED);
}
static gt_wire_frame_t receive_frame(fixture *f,uint16_t type) {
  for(unsigned i=0;i<1000;i++) {
    CHECK(gt__connection_step(f->client)==GT_CONNECTION_OPEN);
    CHECK(gt__handshake_step(f->handshake)!=GT_HANDSHAKE_FAILED);
    gt_wire_frame_t frame;if(gt__connection_frame(f->client,&frame)){CHECK(frame.header.type==type);return frame;}
  }
  CHECK(false);return (gt_wire_frame_t){0};
}
static gt_protocol_hello_t request(fixture *f,gt_protocol_role_t role) {
  gt_protocol_hello_t h={.role=role,.required_features=1};memcpy(h.compatibility,f->config.identity.compatibility,GT_COMPAT_BYTES);
  if(role==GT_ROLE_DATA)memcpy(h.expected_instance,f->config.identity.instance,GT_INSTANCE_BYTES);
  return h;
}
static gt_protocol_hello_reply_t hello(fixture *f,gt_protocol_hello_t h,bool acknowledge) {
  uint8_t bytes[GT_HELLO_BYTES];CHECK(gt__hello_encode(&h,bytes));send_frame(f,GT_MSG_HELLO,h.role==GT_ROLE_MANAGEMENT?7:0,bytes,sizeof(bytes));
  gt_wire_frame_t frame=receive_frame(f,GT_MSG_HELLO_REPLY);gt_protocol_hello_reply_t reply;
  CHECK(gt__hello_reply_decode(frame.payload,frame.header.payload_length,&reply) && frame.header.request_id==(h.role==GT_ROLE_MANAGEMENT?7u:0u));
  CHECK(gt__connection_release_frame(f->client));
  if(acknowledge)send_frame(f,GT_MSG_HELLO_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);
  return reply;
}
static gt_protocol_bind_reply_t bind_channel(fixture *f,gt_attachment_token_t token,bool acknowledge) {
  gt_protocol_bind_t b={.session=3,.generation=f->generation,.token=token};memcpy(b.instance,f->config.identity.instance,GT_INSTANCE_BYTES);
  uint8_t bytes[GT_BIND_BYTES];CHECK(gt__bind_encode(&b,bytes));send_frame(f,GT_MSG_BIND,0,bytes,sizeof(bytes));
  gt_wire_frame_t frame=receive_frame(f,GT_MSG_BIND_REPLY);gt_protocol_bind_reply_t reply;
  CHECK(gt__bind_reply_decode(frame.payload,frame.header.payload_length,&reply));CHECK(gt__connection_release_frame(f->client));
  if(acknowledge)send_frame(f,GT_MSG_BIND_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);
  return reply;
}
static void pump_until(fixture *f,gt_handshake_state_t expected) {
  for(unsigned i=0;i<1000;i++) {
    (void)gt__connection_step(f->client);gt_handshake_state_t state=gt__handshake_step(f->handshake);
    if(state==expected)return;
    CHECK(state!=GT_HANDSHAKE_FAILED);
  }
  CHECK(false);
}
static gt_attachment_token_t issue(fixture *f,uint32_t ttl) {
  gt_attachment_token_t token;uint64_t now;CHECK(clock_ms(f,&now));
  CHECK(gt__tokens_issue(f->tokens,(gt_token_scope_t){1,2,3,f->generation},now,ttl,&token)==GT_TOKEN_OK);return token;
}
static void management(void) {
  fixture f;open_fixture(&f,false);CHECK(hello(&f,request(&f,GT_ROLE_MANAGEMENT),true).status==GT_HELLO_OK);
  pump_until(&f,GT_HANDSHAKE_READY);gt_protocol_role_t role;gt_token_scope_t scope;
  CHECK(gt__handshake_take(f.handshake,&role,&scope) && role==GT_ROLE_MANAGEMENT && !scope.session);
  CHECK(gt__connection_state(f.server)==GT_CONNECTION_OPEN && !f.drops);close_fixture(&f);
}
static void data(bool native) {
  fixture f;open_fixture(&f,native);gt_attachment_token_t token=issue(&f,1000);
  CHECK(hello(&f,request(&f,GT_ROLE_DATA),true).status==GT_HELLO_OK);pump_until(&f,GT_HANDSHAKE_BIND);
  CHECK(bind_channel(&f,token,true).status==GT_HELLO_OK);pump_until(&f,GT_HANDSHAKE_READY);
  gt_protocol_role_t role;gt_token_scope_t scope;CHECK(gt__handshake_take(f.handshake,&role,&scope));
  CHECK(role==GT_ROLE_DATA && scope.principal==1 && scope.issuer==2 && scope.session==3 && scope.generation==f.generation);
  CHECK(!f.drops && gt__attachment_current(f.attachment,f.generation) && !gt__attachment_input_allowed(f.attachment,f.generation,0));
  CHECK(!gt__handshake_take(f.handshake,&role,&scope));close_fixture(&f);
}
static void rejection(const char *mode) {
  fixture f;open_fixture(&f,false);gt_protocol_hello_t h=request(&f,GT_ROLE_MANAGEMENT);gt_hello_status_t expected;gt_handshake_error_t error;
  if(!strcmp(mode,"compatibility")){h.compatibility[0]^=1;expected=GT_HELLO_INCOMPATIBLE;error=GT_HANDSHAKE_COMPATIBILITY;}
  else if(!strcmp(mode,"features")){h.required_features=2;expected=GT_HELLO_UNSUPPORTED;error=GT_HANDSHAKE_FEATURES;}
  else{h.expected_instance[0]=0x7f;expected=GT_HELLO_WRONG_INSTANCE;error=GT_HANDSHAKE_INSTANCE;}
  gt_protocol_hello_reply_t reply=hello(&f,h,false);CHECK(reply.status==expected && gt__connection_state(f.server)==GT_CONNECTION_OPEN);
  CHECK(!memcmp(reply.identity.instance,f.config.identity.instance,GT_INSTANCE_BYTES));
  send_frame(&f,GT_MSG_HELLO_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);pump_until(&f,GT_HANDSHAKE_FAILED);
  CHECK(gt__handshake_error(f.handshake)==error && gt__attachment_current(f.attachment,f.generation));close_fixture(&f);
}
static void challenge_test(void) {
  fixture f;open_fixture(&f,false);gt_protocol_hello_reply_t reply=hello(&f,request(&f,GT_ROLE_MANAGEMENT),false);
  reply.challenge[0]^=1;send_frame(&f,GT_MSG_HELLO_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);pump_until(&f,GT_HANDSHAKE_FAILED);
  CHECK(gt__handshake_error(f.handshake)==GT_HANDSHAKE_PROTOCOL);close_fixture(&f);
}
static void reservation(const char *mode) {
  fixture f;open_fixture(&f,false);gt_attachment_token_t token=issue(&f,100);
  CHECK(hello(&f,request(&f,GT_ROLE_DATA),true).status==GT_HELLO_OK);pump_until(&f,GT_HANDSHAKE_BIND);
  CHECK(bind_channel(&f,token,false).status==GT_HELLO_OK);
  if(!strcmp(mode,"disconnect")) {
    gt__connection_cancel(f.client);pump_until(&f,GT_HANDSHAKE_FAILED);
    CHECK(f.drops==1 && !gt__attachment_current(f.attachment,f.generation));
  } else if(!strcmp(mode,"stale")) {
    uint64_t old=f.generation;CHECK(gt__attachment_detach(f.attachment,old)==GT_TASK_OK);
    CHECK(gt__attachment_attach(f.attachment,&f.generation)==GT_TASK_OK && f.generation>old);
    CHECK(gt__handshake_step(f.handshake)==GT_HANDSHAKE_FAILED && f.drops==1 && gt__attachment_current(f.attachment,f.generation));
  } else {
    gt__handshake_close(f.handshake);f.handshake=NULL;CHECK(f.drops==1 && !gt__attachment_current(f.attachment,f.generation));
  }
  close_fixture(&f);CHECK(f.drops==1);
}
static void expired(void) {
  fixture f;open_fixture(&f,false);gt_attachment_token_t token=issue(&f,10);
  CHECK(hello(&f,request(&f,GT_ROLE_DATA),true).status==GT_HELLO_OK);pump_until(&f,GT_HANDSHAKE_BIND);
  f.now=10;CHECK(bind_channel(&f,token,true).status==GT_HELLO_DENIED);pump_until(&f,GT_HANDSHAKE_FAILED);
  CHECK(f.drops==1 && gt__handshake_error(f.handshake)==GT_HANDSHAKE_TOKEN);close_fixture(&f);
}
static void deadlines(const char *mode) {
  fixture f;open_fixture_config(&f,false,false);(void)issue(&f,100);
  if(!strcmp(mode,"timeout")){f.now=1000;CHECK(gt__handshake_step(f.handshake)==GT_HANDSHAKE_FAILED && gt__handshake_error(f.handshake)==GT_HANDSHAKE_TIMEOUT);}
  else{f.now=5;CHECK(gt__handshake_step(f.handshake)==GT_HANDSHAKE_HELLO);f.now=4;CHECK(gt__handshake_step(f.handshake)==GT_HANDSHAKE_FAILED && gt__handshake_error(f.handshake)==GT_HANDSHAKE_CLOCK);}
  close_fixture(&f);CHECK(f.drops==1);
}
static void entropy(void) {
  fixture f;open_fixture(&f,false);f.entropy_status=GT_ERR_IO;gt_protocol_hello_t h=request(&f,GT_ROLE_MANAGEMENT);uint8_t bytes[GT_HELLO_BYTES];
  CHECK(gt__hello_encode(&h,bytes));send_frame(&f,GT_MSG_HELLO,7,bytes,sizeof(bytes));pump_until(&f,GT_HANDSHAKE_FAILED);
  CHECK(gt__handshake_error(f.handshake)==GT_HANDSHAKE_ENTROPY);close_fixture(&f);
}
static void transaction_deadline(void) {
  fixture f;open_fixture(&f,false);gt_attachment_token_t token=issue(&f,3000);
  f.now=900;CHECK(hello(&f,request(&f,GT_ROLE_DATA),true).status==GT_HELLO_OK);
  pump_until(&f,GT_HANDSHAKE_BIND);f.now=999;CHECK(bind_channel(&f,token,false).status==GT_HELLO_OK);
  f.now=1000;CHECK(gt__handshake_step(f.handshake)==GT_HANDSHAKE_FAILED);
  CHECK(gt__handshake_error(f.handshake)==GT_HANDSHAKE_TIMEOUT && f.drops==1);close_fixture(&f);
}
static void schema(void) {
  fixture f;open_fixture(&f,false);gt_protocol_hello_t h=request(&f,GT_ROLE_MANAGEMENT);uint8_t bytes[GT_HELLO_BYTES];
  CHECK(gt__hello_encode(&h,bytes));send_frame(&f,GT_MSG_HELLO,0,bytes,sizeof(bytes));
  pump_until(&f,GT_HANDSHAKE_FAILED);CHECK(gt__handshake_error(f.handshake)==GT_HANDSHAKE_PROTOCOL);close_fixture(&f);
  open_fixture(&f,false);send_frame(&f,GT_MSG_BIND,0,bytes,sizeof(bytes));
  pump_until(&f,GT_HANDSHAKE_FAILED);CHECK(gt__handshake_error(f.handshake)==GT_HANDSHAKE_PROTOCOL);close_fixture(&f);
}
static void invalid(void) {
  fixture f;open_fixture(&f,false);gt_handshake_config_t config=f.config;gt_handshake_t *out=(gt_handshake_t *)1;
  config.principal=0;CHECK(gt__handshake_open(f.server,&config,NULL,&out)==GT_ERR_INVALID_ARG && !out);
  config=f.config;config.timeout_ms=60001;CHECK(gt__handshake_open(f.server,&config,NULL,&out)==GT_ERR_INVALID_ARG);
  config=f.config;config.binding_drop=NULL;CHECK(gt__handshake_open(f.server,&config,NULL,&out)==GT_ERR_INVALID_ARG);
  f.clock_ok=false;CHECK(gt__handshake_open(f.server,&f.config,NULL,&out)==GT_ERR_IO && !out);
  CHECK(gt__connection_state(f.server)==GT_CONNECTION_OPEN && !f.drops);f.clock_ok=true;close_fixture(&f);
}
static void codec(void) {
  gt_protocol_hello_t h={.role=GT_ROLE_MANAGEMENT,.required_features=UINT64_C(0x0102030405060708)},decoded;
  h.compatibility[0]=0x11;uint8_t bytes[GT_HELLO_BYTES];CHECK(gt__hello_encode(&h,bytes));
  CHECK(bytes[0]==0 && bytes[1]==1 && !bytes[2] && !bytes[3] && bytes[4]==0x11 && bytes[36]==1 && bytes[43]==8 && !bytes[44]);
  CHECK(gt__hello_decode(bytes,sizeof(bytes),&decoded) && decoded.required_features==h.required_features);
  for(unsigned i=0;i<GT_HELLO_BYTES;i++)CHECK(!gt__hello_decode(bytes,i,&decoded));
  bytes[2]=1;CHECK(!gt__hello_decode(bytes,sizeof(bytes),&decoded));
  CHECK(!gt__hello_reply_decode(bytes,sizeof(bytes),(gt_protocol_hello_reply_t *)&(gt_protocol_hello_reply_t){0}));
  gt_protocol_bind_t b={.session=UINT64_C(0x0102030405060708),.generation=9};b.instance[0]=1;b.token.bytes[0]=1;
  uint8_t bind_bytes[GT_BIND_BYTES];CHECK(gt__bind_encode(&b,bind_bytes));CHECK(bind_bytes[0]==1 && bind_bytes[16]==1 && bind_bytes[23]==8 && bind_bytes[31]==9 && bind_bytes[32]==1);
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"management"))management();else if(!strcmp(argv[1],"data"))data(false);else if(!strcmp(argv[1],"native"))data(true);
  else if(!strcmp(argv[1],"compatibility") || !strcmp(argv[1],"features") || !strcmp(argv[1],"instance"))rejection(argv[1]);
  else if(!strcmp(argv[1],"challenge"))challenge_test();else if(!strcmp(argv[1],"disconnect") || !strcmp(argv[1],"stale") || !strcmp(argv[1],"unclaimed"))reservation(argv[1]);
  else if(!strcmp(argv[1],"expired"))expired();else if(!strcmp(argv[1],"timeout") || !strcmp(argv[1],"clock"))deadlines(argv[1]);
  else if(!strcmp(argv[1],"entropy"))entropy();else if(!strcmp(argv[1],"codec"))codec();
  else if(!strcmp(argv[1],"transaction-deadline"))transaction_deadline();else if(!strcmp(argv[1],"schema"))schema();
  else if(!strcmp(argv[1],"invalid"))invalid();else CHECK(false);
  printf("PASS shared bootstrap handshake %s\n",argv[1]);return 0;
}
