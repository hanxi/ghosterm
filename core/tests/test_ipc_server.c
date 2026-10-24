#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ipc/server.h"
#include "ipc/socket-stream.h"
#include "ipc/unix-endpoint.h"
#include "session/attachment.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct {size_t calls,fail,live,bytes;} budget;
typedef struct {gt_stream_ops_t ops;bool blocked;unsigned reads,writes,closed;} bridge;
static gt_stream_result_t read_bridge(void *ctx,void *p,size_t n){bridge *b=ctx;b->reads++;return b->ops.read(b->ops.context,p,n);}
static gt_stream_result_t write_bridge(void *ctx,const void *p,size_t n){bridge *b=ctx;b->writes++;return b->blocked?(gt_stream_result_t){GT_STREAM_WAIT,0,0}:b->ops.write(b->ops.context,p,n);}
static bool clock_bridge(void *ctx,uint64_t *now){bridge *b=ctx;return b->ops.clock(b->ops.context,now);}
static void close_bridge(void *ctx){bridge *b=ctx;b->closed++;b->ops.close(b->ops.context);}
typedef union {max_align_t alignment;size_t size;} allocation_header;
static void *admit(void *ctx,size_t n) {
  budget *b=ctx;if(++b->calls==b->fail)return NULL;
  allocation_header *h=malloc(sizeof(*h)+n);CHECK(h);h->size=n;b->live++;b->bytes+=n;return h+1;
}
static void refund(void *ctx,void *p,size_t n) {
  budget *b=ctx;allocation_header *h=(allocation_header *)p-1;
  CHECK(h->size==n && b->live && b->bytes>=n);b->live--;b->bytes-=n;free(h);
}
typedef struct {
  gt_server_t *server;gt_server_config_t config;gt_session_manager_t *manager;
  gt_unix_endpoint_t *endpoint;gt_socket_stream_t streams[5],client_streams[5];
  bridge bridges[5];gt_server_admit_t admissions[5];
  gt_connection_t *clients[5];uint64_t ids[5],now,session,generation;bool clock_ok;
  gt_token_store_t *tokens;gt_checkpoint_tasks_t *tasks;gt_session_attachment_t *attachment;
  gt_creation_t *creation;unsigned prepared;budget creation_memory;void *ledger;size_t ledger_bytes;
  pthread_mutex_t lock;pthread_cond_t changed;char output[4096];size_t length;
  unsigned disposed,drops,data_calls;bool data_failed;budget memory;char directory[4096];
} fixture;
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static bool clock_ms(void *ctx,uint64_t *now){fixture *f=ctx;*now=f->now;return f->clock_ok;}
static gt_status_t random_fill(void *ctx,uint8_t *bytes,size_t n){(void)ctx;memset(bytes,0x55,n);return GT_OK;}
static bool current(void *ctx,const gt_token_scope_t *scope) {
  fixture *f=ctx;return scope->session==f->session && gt__attachment_current(f->attachment,scope->generation);
}
static void drop(void *ctx,const gt_token_scope_t *scope) {
  fixture *f=ctx;f->drops++;CHECK(scope->session==f->session);(void)gt__attachment_detach(f->attachment,scope->generation);
}
static gt_server_data_rc_t data_step(void *ctx,gt_connection_t *c,const gt_token_scope_t *scope) {
  fixture *f=ctx;CHECK(gt__connection_state(c)==GT_CONNECTION_OPEN && current(f,scope));f->data_calls++;return f->data_failed?GT_SERVER_DATA_CLOSE:GT_SERVER_DATA_IDLE;
}
static void open_fixture_config(fixture *f,const char *root,size_t capacity,bool data,bool tokens) {
  memset(f,0,sizeof(*f));f->clock_ok=true;CHECK(!pthread_mutex_init(&f->lock,NULL));CHECK(!pthread_cond_init(&f->changed,NULL));
  CHECK(snprintf(f->directory,sizeof(f->directory),"%s/server-XXXXXX",root)>0 && mkdtemp(f->directory));
  CHECK(gt__unix_endpoint_open(f->directory,false,NULL,&f->endpoint)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_listen(f->endpoint,8)==GT_ENDPOINT_OK);
  f->manager=gt__manager_open(64,NULL);CHECK(f->manager);
  f->tasks=gt__tasks_open(1);CHECK(f->tasks);f->attachment=gt__attachment_open(f->tasks,128,8);CHECK(f->attachment);
  gt_handshake_config_t h={.timeout_ms=1000,.clock_context=f,.clock=clock_ms,.random={f,random_fill},
    .binding_context=f,.binding_current=current,.binding_drop=drop};
  h.identity.instance[0]=1;h.identity.compatibility[0]=2;
  gt_token_random_t rng={f,random_fill};CHECK(gt__tokens_open(h.identity.instance,4,0,&rng,(gt_token_owner_t){f,drop},NULL,&f->tokens)==GT_TOKEN_OK);
  h.tokens=tokens?f->tokens:NULL;
  f->config=(gt_server_config_t){.manager=f->manager,.handshake=h,.connections=capacity,
    .limits={128,GT_CONTROL_REPLY_BYTES,1000},.idle_timeout_ms=2000,.data_context=f,.data_step=data?data_step:NULL};
  gt_allocator_t a={&f->memory,admit,refund};CHECK(gt__server_open(&f->config,&a,&f->server)==GT_OK);
  for(unsigned i=0;i<5;i++){f->streams[i].fd=-1;f->client_streams[i].fd=-1;}
}
static void open_fixture(fixture *f,const char *root,size_t capacity,bool data){open_fixture_config(f,root,capacity,data,true);}
static void close_fixture(fixture *f) {
  gt__server_close(f->server);CHECK(!f->memory.live && !f->memory.bytes);
  gt__creation_close(f->creation);
  for(unsigned i=0;i<5;i++){CHECK(f->bridges[i].closed<=1);gt__connection_destroy(f->clients[i]);}
  gt__tokens_close(f->tokens);(void)gt__attachment_detach(f->attachment,f->generation);
  CHECK(gt__attachment_close(f->attachment)==GT_TASK_OK && gt__tasks_close(f->tasks)==GT_TASK_OK);
  CHECK(gt__manager_close(f->manager)==GT_MANAGER_OK);
  CHECK(!f->creation_memory.live && !f->creation_memory.bytes);
  CHECK(gt__unix_endpoint_close(f->endpoint)==GT_ENDPOINT_OK);
  char name[4200];CHECK(snprintf(name,sizeof(name),"%s/startup.lock",f->directory)>0);CHECK(!unlink(name) || errno==ENOENT);
  CHECK(snprintf(name,sizeof(name),"%s/daemon.lock",f->directory)>0);CHECK(!unlink(name));CHECK(!rmdir(f->directory));
  CHECK(!pthread_cond_destroy(&f->changed));CHECK(!pthread_mutex_destroy(&f->lock));
}
static bool connect_peer_as(fixture *f,unsigned i,uint64_t principal) {
  int client=-1,server=-1;CHECK(gt__unix_endpoint_connect(f->endpoint,100,&client)==GT_ENDPOINT_OK);
  gt_endpoint_rc_t rc=GT_ENDPOINT_WAIT;
  for(unsigned n=0;n<1000 && rc==GT_ENDPOINT_WAIT;n++){rc=gt__unix_endpoint_accept(f->endpoint,&server);if(rc==GT_ENDPOINT_WAIT)pause_one();}
  CHECK(rc==GT_ENDPOINT_OK);gt_stream_ops_t client_ops,server_ops;
  CHECK(gt__socket_stream_adopt(client,&f->client_streams[i],&client_ops)==GT_OK);
  CHECK(gt__socket_stream_adopt(server,&f->streams[i],&server_ops)==GT_OK);
  CHECK(gt__connection_open(&client_ops,(gt_connection_limits_t){GT_CONTROL_REPLY_BYTES,f->creation?GT_CREATE_MAX_BYTES:128,1000},NULL,&f->clients[i])==GT_CONNECTION_CREATED);
  f->bridges[i].ops=server_ops;gt_stream_ops_t bridged={&f->bridges[i],read_bridge,write_bridge,clock_bridge,close_bridge};
  f->admissions[i]=gt__server_admit(f->server,&bridged,principal,&f->ids[i]);return f->admissions[i]==GT_SERVER_ADMITTED;
}
static bool connect_peer(fixture *f,unsigned i){return connect_peer_as(f,i,1);}
static void send_frame(fixture *f,unsigned i,uint16_t type,uint64_t request,const void *bytes,size_t n) {
  gt_wire_header_t h={.type=type,.request_id=request,.payload_length=(uint32_t)n};
  CHECK(gt__connection_send(f->clients[i],&h,bytes)==GT_SEND_ACCEPTED);
}
static gt_wire_frame_t receive_frame(fixture *f,unsigned i,uint16_t type,uint64_t id) {
  for(unsigned n=0;n<5000;n++) {
    CHECK(gt__server_step(f->server)==GT_SERVER_RUNNING);
    CHECK(gt__connection_step(f->clients[i])==GT_CONNECTION_OPEN);
    gt_wire_frame_t frame;if(gt__connection_frame(f->clients[i],&frame)) {
      CHECK(frame.header.type==type && frame.header.request_id==id);return frame;
    }
    pause_one();
  }
  CHECK(false);return (gt_wire_frame_t){0};
}
static void authenticate(fixture *f,unsigned i,bool data) {
  gt_protocol_hello_t hello={.role=data?GT_ROLE_DATA:GT_ROLE_MANAGEMENT};
  memcpy(hello.compatibility,f->config.handshake.identity.compatibility,GT_COMPAT_BYTES);
  if(data)memcpy(hello.expected_instance,f->config.handshake.identity.instance,GT_INSTANCE_BYTES);
  uint8_t bytes[GT_HELLO_BYTES];CHECK(gt__hello_encode(&hello,bytes));send_frame(f,i,GT_MSG_HELLO,data?0:7,bytes,sizeof(bytes));
  gt_wire_frame_t frame=receive_frame(f,i,GT_MSG_HELLO_REPLY,data?0:7);gt_protocol_hello_reply_t reply;
  CHECK(gt__hello_reply_decode(frame.payload,frame.header.payload_length,&reply) && reply.status==GT_HELLO_OK);
  CHECK(gt__connection_release_frame(f->clients[i]));send_frame(f,i,GT_MSG_HELLO_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);
  for(unsigned n=0;n<8;n++){CHECK(gt__connection_step(f->clients[i])==GT_CONNECTION_OPEN);CHECK(gt__server_step(f->server)==GT_SERVER_RUNNING);}
}
static uint32_t u32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void session_bytes(uint64_t id,uint8_t p[8]){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(id>>(56-8*i));}
static gt_managed_info_t request_info(fixture *f,unsigned i,uint64_t request) {
  uint8_t bytes[8];session_bytes(f->session,bytes);send_frame(f,i,GT_MSG_INFO,request,bytes,8);
  gt_wire_frame_t frame=receive_frame(f,i,GT_MSG_INFO_REPLY,request);gt_managed_info_t row;
  CHECK(frame.header.payload_length==68 && u32(frame.payload)==GT_CONTROL_OK);
  CHECK(gt__control_info_decode(frame.payload+4,64,&row));CHECK(gt__connection_release_frame(f->clients[i]));return row;
}
static gt_io_backend_t *open_backend(void *ctx) {
  (void)ctx;const char *args[]={"-c","stty -echo; printf '%d READY' $$; read answer; printf DETACHED; read answer; printf DONE; exit 7",NULL};
  gt_pty_opts_t opts={.executable="/bin/sh",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};return gt__pty_open(&opts);
}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t reply,void **out){(void)reply;*out=ctx;return GT_OK;}
static gt_status_t apply(void *ctx,const gt_state_event_t *event) {
  fixture *f=ctx;CHECK(event->kind==GT_STATE_OUTPUT);CHECK(!pthread_mutex_lock(&f->lock));
  CHECK(event->length<sizeof(f->output)-f->length);memcpy(f->output+f->length,event->payload,event->length);f->length+=event->length;f->output[f->length]=0;
  CHECK(!pthread_cond_broadcast(&f->changed));CHECK(!pthread_mutex_unlock(&f->lock));return GT_OK;
}
static gt_status_t freeze(void *ctx,void **out){(void)ctx;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void close_state(void *ctx){(void)ctx;}
static void dispose(void *ctx){fixture *f=ctx;f->disposed++;}
static void wait_text(fixture *f,const char *text) {
  struct timespec until;CHECK(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&f->lock));while(!strstr(f->output,text))CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&until));CHECK(!pthread_mutex_unlock(&f->lock));
}
static void create_session(fixture *f) {
  gt_worker_config_t c={.context=f,.open_backend=open_backend,.open_state=open_state,.close_state=close_state,.state_ops={apply,freeze}};
  CHECK(gt__manager_create(f->manager,&c,NULL,NULL,dispose,NULL,&f->session)==GT_MANAGER_OK);wait_text(f,"READY");
}
static void input(fixture *f){gt_worker_submit_t result;CHECK(gt__manager_submit_internal(f->manager,f->session,GT_WORKER_INPUT,"go\n",3,NULL,NULL,&result)==GT_MANAGER_OK && result==GT_WORKER_ACCEPTED);}
static void wait_disconnected(fixture *f,unsigned i) {
  for(unsigned n=0;n<1000 && gt__server_connected(f->server,f->ids[i]);n++){CHECK(gt__server_step(f->server)==GT_SERVER_RUNNING);pause_one();}
  CHECK(!gt__server_connected(f->server,f->ids[i]));
}
static void management(const char *root) {
  fixture f;open_fixture(&f,root,3,false);create_session(&f);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  send_frame(&f,0,GT_MSG_LIST,1,NULL,0);gt_wire_frame_t frame=receive_frame(&f,0,GT_MSG_LIST_REPLY,1);gt_managed_info_t row;
  CHECK(frame.header.payload_length==72 && u32(frame.payload)==GT_CONTROL_OK && u32(frame.payload+4)==1);
  CHECK(gt__control_info_decode(frame.payload+8,64,&row) && row.id==f.session && !row.has_result && row.phase==GT_MANAGED_WORKER);
  CHECK(gt__connection_release_frame(f.clients[0]));gt__connection_cancel(f.clients[0]);wait_disconnected(&f,0);
  input(&f);wait_text(&f,"DETACHED");CHECK(connect_peer(&f,1) && f.ids[1]>f.ids[0]);authenticate(&f,1,false);
  row=request_info(&f,1,1);CHECK(row.id==f.session && !row.has_result && !row.termination_requested);
  uint8_t bytes[8];session_bytes(f.session,bytes);send_frame(&f,1,GT_MSG_TERMINATE,2,bytes,8);
  frame=receive_frame(&f,1,GT_MSG_TERMINATE_REPLY,2);CHECK(frame.header.payload_length==12 && u32(frame.payload)==GT_CONTROL_OK && !memcmp(frame.payload+4,bytes,8));
  CHECK(gt__connection_release_frame(f.clients[1]));
  for(uint64_t id=3;;id++) {CHECK(id<1000);row=request_info(&f,1,id);if(row.has_result)break;pause_one();}
  CHECK(row.termination_requested && row.result.kind==GT_WORKER_END_STOPPED && !row.result.has_exit_code);
  CHECK(!f.disposed);close_fixture(&f);CHECK(f.disposed==1);
}
static void requests(const char *root,const char *mode) {
  fixture f;open_fixture(&f,root,2,false);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  if(!strcmp(mode,"unsupported")) {
    send_frame(&f,0,0x1234,1,NULL,0);gt_wire_frame_t frame=receive_frame(&f,0,GT_MSG_CONTROL_ERROR,1);
    CHECK(frame.header.payload_length==4 && u32(frame.payload)==GT_CONTROL_UNSUPPORTED);CHECK(gt__connection_release_frame(f.clients[0]));
    send_frame(&f,0,GT_MSG_PING,2,NULL,0);frame=receive_frame(&f,0,GT_MSG_PING_REPLY,2);CHECK(!frame.header.payload_length);CHECK(gt__connection_release_frame(f.clients[0]));
  } else if(!strcmp(mode,"request-id")) {
    send_frame(&f,0,GT_MSG_PING,1,NULL,0);(void)receive_frame(&f,0,GT_MSG_PING_REPLY,1);CHECK(gt__connection_release_frame(f.clients[0]));
    send_frame(&f,0,GT_MSG_PING,1,NULL,0);for(unsigned n=0;n<4;n++)CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN);
    wait_disconnected(&f,0);
  } else {
    create_session(&f);uint8_t bytes[9]={0};session_bytes(f.session,bytes);
    send_frame(&f,0,GT_MSG_TERMINATE,1,bytes,9);for(unsigned n=0;n<4;n++)CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN);
    wait_disconnected(&f,0);input(&f);wait_text(&f,"DETACHED");
    CHECK(connect_peer(&f,1));authenticate(&f,1,false);memset(bytes,0,8);send_frame(&f,1,GT_MSG_TERMINATE,1,bytes,8);
    for(unsigned n=0;n<4;n++)CHECK(gt__connection_step(f.clients[1])==GT_CONNECTION_OPEN);
    wait_disconnected(&f,1);gt_managed_info_t row;CHECK(gt__manager_info(f.manager,f.session,&row)==GT_MANAGER_OK && !row.termination_requested && !row.has_result);
  }
  close_fixture(&f);
}
static void capacity(const char *root) {
  fixture f;open_fixture(&f,root,1,false);CHECK(connect_peer(&f,0));CHECK(!connect_peer(&f,1) && f.admissions[1]==GT_SERVER_ADMIT_FULL && f.streams[1].fd==-1 && !f.ids[1]);
  CHECK(gt__server_connections(f.server)==1);gt__connection_cancel(f.clients[0]);wait_disconnected(&f,0);
  CHECK(connect_peer(&f,2) && f.ids[2]>f.ids[0]);authenticate(&f,2,false);close_fixture(&f);
}
static void deadlines(const char *root,const char *mode) {
  fixture f;open_fixture(&f,root,1,false);create_session(&f);CHECK(connect_peer(&f,0));
  if(!strcmp(mode,"idle")){authenticate(&f,0,false);f.now=2000;}
  else f.now=1000;
  CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING && !gt__server_connections(f.server));
  input(&f);wait_text(&f,"DETACHED");close_fixture(&f);
}
static void clock_test(const char *root) {
  fixture f;open_fixture_config(&f,root,2,false,false);create_session(&f);CHECK(connect_peer(&f,0) && connect_peer(&f,1));authenticate(&f,0,false);
  f.now=5;CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING);f.now=4;
  CHECK(gt__server_step(f.server)==GT_SERVER_CLOCK_FAILED && !gt__server_connections(f.server));
  CHECK(gt__server_step(f.server)==GT_SERVER_CLOCK_FAILED);CHECK(!connect_peer(&f,2) && f.admissions[2]==GT_SERVER_ADMIT_CLOSED && f.bridges[2].closed==1);input(&f);wait_text(&f,"DETACHED");close_fixture(&f);
}
static void fairness(const char *root) {
  fixture f;open_fixture(&f,root,2,false);CHECK(connect_peer(&f,0) && connect_peer(&f,1));authenticate(&f,0,false);authenticate(&f,1,false);
  f.bridges[0].blocked=true;send_frame(&f,0,GT_MSG_LIST,1,NULL,0);
  for(unsigned n=0;n<8;n++){CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN);CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING);}
  CHECK(gt__server_wants_write(f.server,f.ids[0]));unsigned reads=f.bridges[0].reads;
  send_frame(&f,0,GT_MSG_PING,2,NULL,0);
  for(unsigned n=0;n<8;n++){CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN);CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING);}
  CHECK(f.bridges[0].reads>reads);
  send_frame(&f,1,GT_MSG_PING,1,NULL,0);(void)receive_frame(&f,1,GT_MSG_PING_REPLY,1);CHECK(gt__connection_release_frame(f.clients[1]));
  CHECK(gt__server_connected(f.server,f.ids[0]) && gt__server_wants_write(f.server,f.ids[0]));
  f.bridges[0].blocked=false;(void)receive_frame(&f,0,GT_MSG_LIST_REPLY,1);CHECK(gt__connection_release_frame(f.clients[0]));
  (void)receive_frame(&f,0,GT_MSG_PING_REPLY,2);CHECK(gt__connection_release_frame(f.clients[0]));close_fixture(&f);
}
static void shutdown_test(const char *root) {
  fixture f;open_fixture(&f,root,1,false);create_session(&f);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  gt__server_close(f.server);f.server=NULL;CHECK(f.bridges[0].closed==1 && !f.disposed);
  input(&f);wait_text(&f,"DETACHED");close_fixture(&f);CHECK(f.disposed==1);
}
static void killed(const char *root) {
  fixture f;open_fixture(&f,root,1,false);create_session(&f);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  long descriptor_limit=sysconf(_SC_OPEN_MAX);CHECK(descriptor_limit>0 && descriptor_limit<=INT_MAX);
  int ready[2];CHECK(!pipe(ready));pid_t child=fork();CHECK(child>=0);
  if(!child) {
    alarm(10);close(ready[0]);close(f.streams[0].fd);close(gt__unix_endpoint_listener(f.endpoint));
    /* Only async-signal-safe operations after a multithreaded fork. Close every
     * inherited fd except this client's socket, readiness pipe and stderr. */
    for(int fd=0;fd<descriptor_limit;fd++)if(fd!=f.client_streams[0].fd && fd!=ready[1] && fd!=2)close(fd);
    if(write(ready[1],"R",1)!=1)_exit(2);
    for(;;)pause();
  }
  close(ready[1]);struct pollfd p={ready[0],POLLIN,0};CHECK(poll(&p,1,5000)==1);char byte;CHECK(read(ready[0],&byte,1)==1 && byte=='R');close(ready[0]);
  gt__connection_destroy(f.clients[0]);f.clients[0]=NULL;
  CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING && gt__server_connected(f.server,f.ids[0]));
  CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
  wait_disconnected(&f,0);input(&f);wait_text(&f,"DETACHED");
  CHECK(connect_peer(&f,1));authenticate(&f,1,false);gt_managed_info_t row=request_info(&f,1,1);
  CHECK(row.id==f.session && !row.has_result && !row.termination_requested);close_fixture(&f);
}
static void allocation(const char *root) {
  for(unsigned fail=1;fail<=2;fail++) {
    fixture f;open_fixture(&f,root,1,false);f.memory.fail=f.memory.calls+fail;
    CHECK(!connect_peer(&f,0) && f.admissions[0]==GT_SERVER_ADMIT_NO_MEMORY && !f.ids[0] && f.streams[0].fd==-1 && gt__server_connections(f.server)==0);
    CHECK(f.memory.live==1);close_fixture(&f);
  }
  fixture f;open_fixture(&f,root,1,false);f.clock_ok=false;
  CHECK(!connect_peer(&f,0) && f.admissions[0]==GT_SERVER_ADMIT_CLOCK && f.bridges[0].closed==1 && f.memory.live==1);
  f.clock_ok=true;close_fixture(&f);
}
static void invalid(const char *root) {
  fixture f;open_fixture(&f,root,1,false);create_session(&f);gt_server_config_t config=f.config;gt_server_t *out=(gt_server_t *)1;
  config.connections=0;CHECK(gt__server_open(&config,NULL,&out)==GT_ERR_INVALID_ARG && !out);
  config.connections=65;CHECK(gt__server_open(&config,NULL,&out)==GT_ERR_INVALID_ARG && !out);
  config=f.config;config.limits.send_limit=GT_CONTROL_REPLY_BYTES-1;CHECK(gt__server_open(&config,NULL,&out)==GT_ERR_INVALID_ARG);
  config=f.config;config.data_step=data_step;config.handshake.tokens=NULL;CHECK(gt__server_open(&config,NULL,&out)==GT_ERR_INVALID_ARG);
  f.clock_ok=false;CHECK(gt__server_open(&f.config,NULL,&out)==GT_ERR_IO && !out);f.clock_ok=true;
  gt_allocator_t allocator={&f.memory,admit,refund};f.memory.fail=f.memory.calls+1;
  CHECK(gt__server_open(&f.config,&allocator,&out)==GT_ERR_INTERNAL && !out && f.memory.live==1);
  uint8_t request[8],scratch[GT_CONTROL_REPLY_BYTES];session_bytes(f.session,request);
  gt_wire_frame_t frame={.header={.type=GT_MSG_TERMINATE,.request_id=1,.payload_length=8},.payload=request};gt_wire_header_t reply;
  CHECK(gt__control_process(&(gt_control_context_t){.manager=f.manager,.principal=1,.issuer=1},&frame,scratch,4,&reply)==GT_CONTROL_NO_SPACE);
  frame.header.type=GT_MSG_INFO_REPLY;CHECK(gt__control_process(&(gt_control_context_t){.manager=f.manager,.principal=1,.issuer=1},&frame,scratch,sizeof(scratch),&reply)==GT_CONTROL_MALFORMED);
  gt_managed_info_t info;CHECK(gt__manager_info(f.manager,f.session,&info)==GT_MANAGER_OK && !info.termination_requested && !info.has_result);close_fixture(&f);
}
static void missing(const char *root) {
  fixture f;open_fixture(&f,root,1,false);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  uint8_t id[8]={0,0,0,0,0,0,0,1};send_frame(&f,0,GT_MSG_INFO,1,id,8);
  gt_wire_frame_t frame=receive_frame(&f,0,GT_MSG_INFO_REPLY,1);CHECK(frame.header.payload_length==68 && u32(frame.payload)==GT_CONTROL_NOT_FOUND);
  for(unsigned i=4;i<68;i++)CHECK(!frame.payload[i]);
  CHECK(gt__connection_release_frame(f.clients[0]));
  send_frame(&f,0,GT_MSG_TERMINATE,2,id,8);frame=receive_frame(&f,0,GT_MSG_TERMINATE_REPLY,2);
  CHECK(u32(frame.payload)==GT_CONTROL_NOT_FOUND && !memcmp(frame.payload+4,id,8));CHECK(gt__connection_release_frame(f.clients[0]));close_fixture(&f);
}
static void data_disabled(const char *root) {
  fixture f;open_fixture(&f,root,1,false);create_session(&f);CHECK(connect_peer(&f,0));
  gt_protocol_hello_t hello={.role=GT_ROLE_DATA};memcpy(hello.compatibility,f.config.handshake.identity.compatibility,GT_COMPAT_BYTES);
  memcpy(hello.expected_instance,f.config.handshake.identity.instance,GT_INSTANCE_BYTES);uint8_t bytes[GT_HELLO_BYTES];CHECK(gt__hello_encode(&hello,bytes));
  send_frame(&f,0,GT_MSG_HELLO,0,bytes,sizeof(bytes));gt_wire_frame_t frame=receive_frame(&f,0,GT_MSG_HELLO_REPLY,0);gt_protocol_hello_reply_t reply;
  CHECK(gt__hello_reply_decode(frame.payload,frame.header.payload_length,&reply) && reply.status==GT_HELLO_UNSUPPORTED);CHECK(gt__connection_release_frame(f.clients[0]));
  send_frame(&f,0,GT_MSG_HELLO_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);
  for(unsigned n=0;n<4;n++)CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN);
  wait_disconnected(&f,0);input(&f);wait_text(&f,"DETACHED");close_fixture(&f);
}
static void ended(const char *root) {
  fixture f;open_fixture(&f,root,1,false);create_session(&f);input(&f);wait_text(&f,"DETACHED");input(&f);wait_text(&f,"DONE");
  CHECK(connect_peer(&f,0));authenticate(&f,0,false);gt_managed_info_t info;
  for(uint64_t n=1;;n++){CHECK(n<1000);info=request_info(&f,0,n);if(info.has_result)break;pause_one();}
  gt_managed_info_t actual;CHECK(gt__manager_info(f.manager,f.session,&actual)==GT_MANAGER_OK && actual.has_result);
  CHECK(info.result.kind==GT_WORKER_END_EOF && !info.termination_requested && info.result.has_sequence);
  CHECK(info.result.has_exit_code==actual.result.has_exit_code && info.result.termination_status==actual.result.termination_status);
  if(info.result.has_exit_code)CHECK(info.result.exit_code==7);
  else CHECK(info.result.termination_status==GT_ERR_NOT_FOUND || info.result.termination_status==GT_ERR_UNSUPPORTED);
  close_fixture(&f);
}
static gt_status_t unavailable_state(void *ctx,gt_worker_reply_sink_t sink,void **out){(void)ctx;(void)sink;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void full_list(const char *root) {
  fixture f;open_fixture(&f,root,1,false);gt_worker_config_t c={.context=&f,.open_backend=open_backend,.open_state=unavailable_state,.close_state=close_state,.state_ops={apply,freeze}};
  uint64_t ids[64];for(unsigned i=0;i<64;i++)CHECK(gt__manager_create(f.manager,&c,NULL,NULL,dispose,NULL,&ids[i])==GT_MANAGER_OK);
  for(unsigned n=0;;n++) {
    CHECK(n<5000);gt_managed_info_t rows[64];size_t count;CHECK(gt__manager_list(f.manager,rows,64,&count)==GT_MANAGER_OK && count==64);
    unsigned ready=0;for(unsigned i=0;i<64;i++)if(rows[i].has_result)ready++;
    if(ready==64)break;
    pause_one();
  }
  CHECK(connect_peer(&f,0));authenticate(&f,0,false);send_frame(&f,0,GT_MSG_LIST,1,NULL,0);
  gt_wire_frame_t frame=receive_frame(&f,0,GT_MSG_LIST_REPLY,1);CHECK(frame.header.payload_length==4104 && u32(frame.payload)==GT_CONTROL_OK && u32(frame.payload+4)==64);
  for(unsigned i=0;i<64;i++) {
    gt_managed_info_t row;CHECK(gt__control_info_decode(frame.payload+8+i*64,64,&row));
    CHECK(row.id==ids[i] && row.has_result && row.result.kind==GT_WORKER_END_ERROR && row.result.status==GT_ERR_UNSUPPORTED && !row.result.has_exit_code);
  }
  CHECK(gt__connection_release_frame(f.clients[0]));close_fixture(&f);CHECK(f.disposed==64);
}
typedef struct {fixture *f;uint64_t principal,session;gt_pty_opts_t options;const char *args[33],*env[65];} create_context;
static gt_io_backend_t *created_backend(void *ctx){create_context *c=ctx;CHECK(c->session && c->principal);return gt__pty_open(&c->options);}
static gt_status_t created_state(void *ctx,gt_worker_reply_sink_t reply,void **out){(void)reply;create_context *c=ctx;*out=c->f;return GT_OK;}
static gt_status_t prepare_created(void *ctx,uint64_t principal,const gt_create_spec_t *spec,gt_worker_config_t *out) {
  fixture *f=ctx;create_context *c=calloc(1,sizeof(*c));CHECK(c);c->f=f;c->principal=principal;out->context=c;f->prepared++;
  gt_status_t status=gt__create_spec_pty(spec,&c->options,c->args,c->env);if(status!=GT_OK)return status;
  *out=(gt_worker_config_t){.context=c,.open_backend=created_backend,.open_state=created_state,.close_state=close_state,.state_ops={apply,freeze}};return GT_OK;
}
static gt_status_t bind_created(void *ctx,void *worker,uint64_t principal,uint64_t id){create_context *c=worker;CHECK(c && c->f==ctx && c->principal==principal && id);c->session=id;return GT_OK;}
static void dispose_created(void *ctx,void *worker){fixture *f=ctx;f->disposed++;free(worker);}
static void *creation_admit(void *ctx,size_t n){fixture *f=ctx;void *p=admit(&f->creation_memory,n);if(!f->ledger){f->ledger=p;f->ledger_bytes=n;}return p;}
static void creation_refund(void *ctx,void *p,size_t n){fixture *f=ctx;if(p==f->ledger)f->ledger=NULL;refund(&f->creation_memory,p,n);}
static bool contains(const void *memory,size_t bytes,const char *text) {
  const uint8_t *p=memory;size_t n=strlen(text);for(size_t i=0;i+n<=bytes;i++)if(!memcmp(p+i,text,n))return true;return false;
}
static void enable_creation(fixture *f) {
  gt__server_close(f->server);f->server=NULL;
  gt_creation_config_t c={.manager=f->manager,.capacity=4,.retention_ms=100,.factory_context=f,.prepare=prepare_created,.bind_id=bind_created,.dispose=dispose_created};
  memcpy(c.instance,f->config.handshake.identity.instance,16);gt_allocator_t domain={f,creation_admit,creation_refund};
  CHECK(gt__creation_open(&c,0,&domain,&f->creation)==GT_OK);
  f->config.creation=f->creation;f->config.limits.receive_limit=GT_CREATE_MAX_BYTES;gt_allocator_t a={&f->memory,admit,refund};
  CHECK(gt__server_open(&f->config,&a,&f->server)==GT_OK);
}
static gt_create_spec_t wire_spec(fixture *f) {
  gt_create_spec_t s={.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,
    .executable="/bin/sh",.cwd=f->directory,.term="xterm-256color",.argc=2,.envc=1,
    .argv={"-c","stty -echo; printf 'CREATE_READY:%s:%s\n' \"$GTERM_CREATE_TEST\" \"$PWD\"; read a; printf DETACHED; read a; exit 7"},.env={"GTERM_CREATE_TEST=wire"}};
  s.nonce[0]=1;return s;
}
static gt_creation_result_t receive_creation(fixture *f,unsigned i,uint16_t type,uint64_t request,const uint8_t nonce[16]) {
  gt_wire_frame_t frame=receive_frame(f,i,type,request);gt_creation_result_t result;uint8_t received[16];
  CHECK(gt__control_creation_decode(frame.payload,frame.header.payload_length,received,&result) && !memcmp(received,nonce,16));
  CHECK(gt__connection_release_frame(f->clients[i]));return result;
}
static void wire_creation(const char *root,const char *mode) {
  fixture f;open_fixture(&f,root,3,false);enable_creation(&f);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  gt_create_spec_t s=wire_spec(&f);uint8_t bytes[GT_CREATE_MAX_BYTES];size_t n;CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_OK);
  if(!strcmp(mode,"create-malformed")) {
    bytes[n-1]=0;send_frame(&f,0,GT_MSG_CREATE,1,bytes,n);
    for(unsigned k=0;k<5 && gt__server_connected(f.server,f.ids[0]);k++){CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN);CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING);}
    wait_disconnected(&f,0);CHECK(!f.prepared);CHECK(connect_peer(&f,1));authenticate(&f,1,false);
    send_frame(&f,1,GT_MSG_QUERY_CREATE,1,s.nonce,16);CHECK(receive_creation(&f,1,GT_MSG_QUERY_CREATE_REPLY,1,s.nonce).status==GT_CREATION_UNKNOWN);
    close_fixture(&f);return;
  }
  bool lost=!strcmp(mode,"create-lost");f.bridges[0].blocked=lost;send_frame(&f,0,GT_MSG_CREATE,1,bytes,n);
  unsigned active=0;
  if(lost) {
    for(unsigned k=0;k<5000;k++) {
      CHECK(gt__connection_step(f.clients[0])==GT_CONNECTION_OPEN && gt__server_step(f.server)==GT_SERVER_RUNNING);
      gt_managed_info_t row[2];size_t count=0;CHECK(gt__manager_list(f.manager,row,2,&count)==GT_MANAGER_OK);
      if(count){CHECK(count==1);f.session=row[0].id;break;}
      pause_one();
    }
    CHECK(f.session && f.prepared==1 && gt__server_wants_write(f.server,f.ids[0]));
    gt__connection_cancel(f.clients[0]);wait_disconnected(&f,0);CHECK(connect_peer(&f,1));authenticate(&f,1,false);active=1;
  } else {
    gt_creation_result_t result=receive_creation(&f,0,GT_MSG_CREATE_REPLY,1,s.nonce);CHECK(result.status==GT_CREATION_ACCEPTED && result.session);f.session=result.session;
  }
  wait_text(&f,"CREATE_READY:wire:");
  if(!strcmp(mode,"create-expiry")) {
    CHECK(contains(f.ledger,f.ledger_bytes,"GTERM_CREATE_TEST=wire"));f.now=100;CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING);
    CHECK(!contains(f.ledger,f.ledger_bytes,"GTERM_CREATE_TEST=wire"));
  }
  send_frame(&f,active,GT_MSG_QUERY_CREATE,2,s.nonce,16);gt_creation_result_t result=receive_creation(&f,active,GT_MSG_QUERY_CREATE_REPLY,2,s.nonce);
  if(!strcmp(mode,"create-expiry"))CHECK(result.status==GT_CREATION_UNKNOWN && !result.session);
  else CHECK(result.status==GT_CREATION_ACCEPTED && result.session==f.session);
  if(!strcmp(mode,"create-principal")) {
    CHECK(connect_peer_as(&f,1,2));authenticate(&f,1,false);send_frame(&f,1,GT_MSG_QUERY_CREATE,1,s.nonce,16);
    CHECK(receive_creation(&f,1,GT_MSG_QUERY_CREATE_REPLY,1,s.nonce).status==GT_CREATION_UNKNOWN);
  } else if(!strcmp(mode,"create")) {
    send_frame(&f,0,GT_MSG_CREATE,3,bytes,n);CHECK(receive_creation(&f,0,GT_MSG_CREATE_REPLY,3,s.nonce).session==f.session);
    s.env[0]="GTERM_CREATE_TEST=other";CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_OK);
    send_frame(&f,0,GT_MSG_CREATE,4,bytes,n);CHECK(receive_creation(&f,0,GT_MSG_CREATE_REPLY,4,s.nonce).status==GT_CREATION_CONFLICT);
  }
  CHECK(f.prepared==1);input(&f);wait_text(&f,"DETACHED");close_fixture(&f);CHECK(f.disposed==1);
}
static void creation_disabled(const char *root) {
  fixture f;open_fixture(&f,root,1,false);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  gt_create_spec_t s={.command=GT_PTY_CMD_DIRECT,.cols=1,.rows=1,.environment=GT_CREATE_HOST_ENV,.executable="/bin/sh",.cwd="/",.term="x"};s.nonce[0]=1;
  uint8_t bytes[GT_CREATE_MAX_BYTES];size_t n;CHECK(gt__create_spec_encode(&s,bytes,sizeof(bytes),&n)==GT_OK);send_frame(&f,0,GT_MSG_CREATE,1,bytes,n);
  CHECK(receive_creation(&f,0,GT_MSG_CREATE_REPLY,1,s.nonce).status==GT_CREATION_UNSUPPORTED);close_fixture(&f);
}
static void creation_config(const char *root) {
  fixture f;open_fixture(&f,root,1,false);enable_creation(&f);gt_server_config_t c=f.config;gt_server_t *server;
  c.limits.receive_limit=GT_CREATE_MAX_BYTES-1;CHECK(gt__server_open(&c,NULL,&server)==GT_ERR_INVALID_ARG && !server);
  c=f.config;c.handshake.identity.instance[0]^=2;CHECK(gt__server_open(&c,NULL,&server)==GT_ERR_INVALID_ARG && !server);
  c=f.config;c.manager=gt__manager_open(1,NULL);CHECK(c.manager && gt__server_open(&c,NULL,&server)==GT_ERR_INVALID_ARG && !server);
  CHECK(gt__manager_close(c.manager)==GT_MANAGER_OK);close_fixture(&f);
}
static void creation_codec(void) {
  uint8_t bytes[33]={1},nonce[16];gt_creation_result_t result;bytes[31]=9;
  CHECK(gt__control_creation_decode(bytes,32,nonce,&result) && nonce[0]==1 && result.status==GT_CREATION_ACCEPTED && result.session==9);
  for(unsigned i=0;i<32;i++)CHECK(!gt__control_creation_decode(bytes,i,nonce,&result) && !result.session && !nonce[0]);
  CHECK(!gt__control_creation_decode(bytes,33,nonce,&result));
  bytes[19]=1;CHECK(!gt__control_creation_decode(bytes,32,nonce,&result));bytes[31]=0;
  CHECK(gt__control_creation_decode(bytes,32,nonce,&result) && result.status==GT_CREATION_UNKNOWN && !result.session);
  bytes[19]=0;CHECK(!gt__control_creation_decode(bytes,32,nonce,&result));bytes[31]=9;bytes[23]=3;
  CHECK(!gt__control_creation_decode(bytes,32,nonce,&result));bytes[23]=0;bytes[31]=0;bytes[19]=255;
  CHECK(!gt__control_creation_decode(bytes,32,nonce,&result));bytes[19]=0;bytes[31]=9;bytes[0]=0;
  CHECK(!gt__control_creation_decode(bytes,32,nonce,&result));
}
/* Only the isolated counter fixture initializes server next_id at UINT64_MAX. */
static void exhaustion(const char *root) {
  fixture f;open_fixture(&f,root,1,false);CHECK(connect_peer(&f,0) && f.ids[0]==UINT64_MAX);
  gt__connection_cancel(f.clients[0]);wait_disconnected(&f,0);
  CHECK(!connect_peer(&f,1) && f.admissions[1]==GT_SERVER_ADMIT_IDS_EXHAUSTED && !f.ids[1] && f.bridges[1].closed==1 && !gt__server_connections(f.server));close_fixture(&f);
}
static void reservation(const char *root,const char *mode) {
  fixture f;open_fixture(&f,root,2,true);create_session(&f);CHECK(connect_peer(&f,0));authenticate(&f,0,false);
  CHECK(gt__attachment_attach(f.attachment,&f.generation)==GT_TASK_OK);gt_attachment_token_t token;
  gt_token_scope_t scope={1,f.ids[0],f.session,f.generation};CHECK(gt__tokens_issue(f.tokens,scope,0,100,&token)==GT_TOKEN_OK);
  if(!strcmp(mode,"issuer")) {gt__connection_cancel(f.clients[0]);wait_disconnected(&f,0);CHECK(f.drops==1 && !gt__attachment_current(f.attachment,f.generation));}
  else if(!strcmp(mode,"expiry")) {f.now=100;CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING && f.drops==1 && !gt__attachment_current(f.attachment,f.generation));}
  else {
    CHECK(connect_peer(&f,1));authenticate(&f,1,true);gt_protocol_bind_t bind={.session=f.session,.generation=f.generation,.token=token};
    memcpy(bind.instance,f.config.handshake.identity.instance,GT_INSTANCE_BYTES);uint8_t bytes[GT_BIND_BYTES];CHECK(gt__bind_encode(&bind,bytes));send_frame(&f,1,GT_MSG_BIND,0,bytes,sizeof(bytes));
    gt_wire_frame_t frame=receive_frame(&f,1,GT_MSG_BIND_REPLY,0);gt_protocol_bind_reply_t reply;
    CHECK(gt__bind_reply_decode(frame.payload,frame.header.payload_length,&reply) && reply.status==GT_HELLO_OK);CHECK(gt__connection_release_frame(f.clients[1]));
    send_frame(&f,1,GT_MSG_BIND_ACK,0,reply.challenge,GT_CHALLENGE_BYTES);
    for(unsigned n=0;n<8;n++){CHECK(gt__connection_step(f.clients[1])==GT_CONNECTION_OPEN);CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING);}
    CHECK(f.data_calls && !f.drops && !gt__attachment_input_allowed(f.attachment,f.generation,0));
    gt__connection_cancel(f.clients[0]);wait_disconnected(&f,0);CHECK(!f.drops && gt__attachment_current(f.attachment,f.generation));
    if(!strcmp(mode,"stale")) {
      uint64_t old=f.generation;CHECK(gt__attachment_detach(f.attachment,old)==GT_TASK_OK);CHECK(gt__attachment_attach(f.attachment,&f.generation)==GT_TASK_OK);
      wait_disconnected(&f,1);CHECK(f.drops==1 && gt__attachment_current(f.attachment,f.generation));
    } else {
      if(!strcmp(mode,"data-failure"))f.data_failed=true;
      else if(!strcmp(mode,"data-lease")){f.now=1999;CHECK(gt__server_step(f.server)==GT_SERVER_RUNNING && gt__server_connected(f.server,f.ids[1]));f.now=2000;}
      else gt__connection_cancel(f.clients[1]);
      wait_disconnected(&f,1);CHECK(f.drops==1 && !gt__attachment_current(f.attachment,f.generation));
    }
  }
  input(&f);wait_text(&f,"DETACHED");close_fixture(&f);CHECK(f.drops==1);
}
static void codec(void) {
  gt_managed_info_t row={.id=UINT64_C(0x0102030405060708),.phase=GT_MANAGED_RETAINED,.has_result=true,
    .result={.kind=GT_WORKER_END_EOF,.has_sequence=true,.sequence=9,.termination_status=GT_ERR_NOT_FOUND}};
  uint8_t bytes[65]={0};CHECK(gt__control_info_encode(&row,bytes));
  gt_managed_info_t oversized;CHECK(!gt__control_info_decode(bytes,65,&oversized));
  CHECK(bytes[0]==1 && bytes[7]==8 && bytes[11]==3 && bytes[15]==6 && bytes[19]==2 && bytes[31]==GT_ERR_NOT_FOUND && bytes[39]==9 && !bytes[43]);
  gt_managed_info_t decoded;CHECK(gt__control_info_decode(bytes,64,&decoded) && decoded.has_result && !decoded.result.has_exit_code && decoded.result.termination_status==GT_ERR_NOT_FOUND);
  for(size_t n=0;n<64;n++)CHECK(!gt__control_info_decode(bytes,n,&decoded) && !decoded.id);
  bytes[63]=1;CHECK(!gt__control_info_decode(bytes,64,&decoded) && !decoded.id);bytes[63]=0;
  row.result.has_exit_code=true;row.result.exit_code=0;row.result.termination_status=GT_OK;row.result.termination=(gt_io_exit_t){GT_IO_EXIT_CODE,0};
  CHECK(gt__control_info_encode(&row,bytes) && gt__control_info_decode(bytes,64,&decoded) && decoded.result.has_exit_code && decoded.result.exit_code==0);
  bytes[40]=bytes[41]=bytes[42]=bytes[43]=255;CHECK(!gt__control_info_decode(bytes,64,&decoded));
  row.result.has_exit_code=false;row.result.termination=(gt_io_exit_t){GT_IO_EXIT_SIGNAL,15};
  CHECK(gt__control_info_encode(&row,bytes) && gt__control_info_decode(bytes,64,&decoded) && !decoded.result.has_exit_code && decoded.result.termination.kind==GT_IO_EXIT_SIGNAL && decoded.result.termination.code==15);
}
int main(int argc,char **argv) {
  CHECK(argc==3);
  if(!strcmp(argv[1],"management"))management(argv[2]);
  else if(!strcmp(argv[1],"unsupported") || !strcmp(argv[1],"request-id") || !strcmp(argv[1],"malformed"))requests(argv[2],argv[1]);
  else if(!strcmp(argv[1],"capacity"))capacity(argv[2]);else if(!strcmp(argv[1],"idle") || !strcmp(argv[1],"handshake-timeout"))deadlines(argv[2],argv[1]);
  else if(!strcmp(argv[1],"clock"))clock_test(argv[2]);else if(!strcmp(argv[1],"allocation"))allocation(argv[2]);
  else if(!strcmp(argv[1],"fairness"))fairness(argv[2]);else if(!strcmp(argv[1],"shutdown"))shutdown_test(argv[2]);else if(!strcmp(argv[1],"kill"))killed(argv[2]);
  else if(!strcmp(argv[1],"invalid"))invalid(argv[2]);else if(!strcmp(argv[1],"missing"))missing(argv[2]);else if(!strcmp(argv[1],"ended"))ended(argv[2]);
  else if(!strcmp(argv[1],"data-disabled"))data_disabled(argv[2]);
  else if(!strcmp(argv[1],"full-list"))full_list(argv[2]);else if(!strcmp(argv[1],"exhaustion"))exhaustion(argv[2]);
  else if(!strcmp(argv[1],"create") || !strcmp(argv[1],"create-lost") || !strcmp(argv[1],"create-malformed") || !strcmp(argv[1],"create-expiry") || !strcmp(argv[1],"create-principal"))wire_creation(argv[2],argv[1]);
  else if(!strcmp(argv[1],"create-disabled"))creation_disabled(argv[2]);else if(!strcmp(argv[1],"create-config"))creation_config(argv[2]);
  else if(!strcmp(argv[1],"create-codec"))creation_codec();
  else if(!strcmp(argv[1],"issuer") || !strcmp(argv[1],"expiry") || !strcmp(argv[1],"data") || !strcmp(argv[1],"stale") || !strcmp(argv[1],"data-lease") || !strcmp(argv[1],"data-failure"))reservation(argv[2],argv[1]);
  else if(!strcmp(argv[1],"codec"))codec();else CHECK(false);
  printf("PASS shared service owner %s\n",argv[1]);return 0;
}
