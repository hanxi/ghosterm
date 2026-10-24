#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "vt-state.h"
#include "vt-snapshot.h"
#include "host.h"
#include "memory.h"
#include "session-contract.h"
#include "budget-posix.h"
#include "ipc/attachment-client.h"
#include "ipc/client-bootstrap.h"
#include "ipc/control-client.h"
#include "ipc/socket-stream.h"
#include "ipc/ssh-link.h"
#include "ipc/session-client.h"
#include "ipc/ssh-session-provider.h"
#include "ipc/local-session-provider.h"
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define MIB (1024u*1024u)
static void cleanup(void);
static void (*reconnect_cleanup)(void);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);cleanup();exit(1);}} while(0)
extern char **environ;
static pid_t daemon_pid=-1,crash_pid=-1;static gt_session_host_t *local_host;
static gt_ssh_link_t *ssh_owner;static uint8_t expected_instance[16];
static const char *self,*daemon_binary,*root,*proxy_binary;static char directory[1024],scratch[1024];
static void cleanup(void) {
  if(reconnect_cleanup)reconnect_cleanup();
  gt__ssh_link_close(ssh_owner);ssh_owner=NULL;
  if(crash_pid>0){kill(crash_pid,SIGKILL);waitpid(crash_pid,NULL,0);}
  if(daemon_pid>0){kill(daemon_pid,SIGTERM);for(unsigned i=0;i<500;i++){if(waitpid(daemon_pid,NULL,WNOHANG)==daemon_pid){daemon_pid=-1;break;}struct timespec t={0,10000000};nanosleep(&t,NULL);}if(daemon_pid>0){kill(daemon_pid,SIGKILL);waitpid(daemon_pid,NULL,0);}}
}
static bool clock_ms(void *ctx,uint64_t *out){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static void pause_one(void){if(ssh_owner)CHECK(gt__ssh_link_step(ssh_owner)==GT_OK);if(local_host)CHECK(gt__session_host_poll(local_host,-1,0)==GT_OK);struct timespec t={0,1000000};nanosleep(&t,NULL);}
static uint64_t now(void){uint64_t t;CHECK(clock_ms(NULL,&t));return t;}
static int wait_child(pid_t pid) {
  uint64_t end=now()+10000;int status;
  while(waitpid(pid,&status,WNOHANG)!=pid){CHECK(now()<end);pause_one();}return status;
}
static pid_t spawn_server(void) {
  posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));
  char out[1200],err[1200];CHECK(snprintf(out,sizeof(out),"%s/stdout",scratch)>0 && snprintf(err,sizeof(err),"%s/stderr",scratch)>0);
  CHECK(!posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
  CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,out,O_WRONLY|O_CREAT|O_APPEND,0600));
  CHECK(!posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,err,O_WRONLY|O_CREAT|O_APPEND,0600));
  char *args[]={(char *)daemon_binary,"serve","--runtime-dir",directory,NULL};pid_t pid;
  CHECK(!posix_spawn(&pid,daemon_binary,&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);return pid;
}
static gt_unix_endpoint_t *endpoint(void) {
  gt_unix_endpoint_t *e=NULL;CHECK(gt__unix_endpoint_open(directory,false,NULL,&e)==GT_ENDPOINT_OK);return e;
}
static void start_server(void) {
  if(getenv("GT_TEST_REMOTE_RUNTIME")) {
    CHECK(snprintf(scratch,sizeof(scratch),"%s/tmp/host-XXXXXX",root)>0 && mkdtemp(scratch));
    CHECK(snprintf(directory,sizeof(directory),"%s",getenv("GT_TEST_REMOTE_RUNTIME"))>0);return;
  }
  CHECK(snprintf(scratch,sizeof(scratch),"%s/tmp/host-XXXXXX",root)>0 && mkdtemp(scratch));
  CHECK(snprintf(directory,sizeof(directory),"%s/run",scratch)>0);daemon_pid=spawn_server();
  uint64_t end=now()+10000;
  while(true) {
    gt_unix_endpoint_t *e=NULL;gt_endpoint_rc_t rc=gt__unix_endpoint_open(directory,false,NULL,&e);
    if(rc==GT_ENDPOINT_OK){int fd=-1;rc=gt__unix_endpoint_connect(e,0,&fd);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);if(rc==GT_ENDPOINT_OK){close(fd);break;}}
    CHECK(now()<end && waitpid(daemon_pid,NULL,WNOHANG)==0);pause_one();
  }
}
static void stop_server(int signal) {
  if(getenv("GT_TEST_REMOTE_RUNTIME"))return;
  CHECK(!kill(daemon_pid,signal));int status=wait_child(daemon_pid);daemon_pid=-1;CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
  gt_unix_endpoint_t *e=endpoint();int fd=-1;CHECK(gt__unix_endpoint_connect(e,0,&fd)==GT_ENDPOINT_NOT_FOUND && fd==-1);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  char path[1200];struct stat st;CHECK(snprintf(path,sizeof(path),"%s/stdout",scratch)>0 && !stat(path,&st) && !st.st_size);
}
typedef struct {gt_socket_stream_t socket;gt_connection_t *raw;gt_control_client_t *control;gt_control_result_t reply;unsigned results;pid_t proxy;} client;
static void result(void *context,const gt_control_result_t *r){client *c=context;c->reply=*r;c->results++;}
static gt_hostkey_decision_t ssh_accept(void *c,const char *h,uint16_t p,const char *k,const char *f){(void)c;(void)h;(void)p;(void)k;(void)f;return GT_HOSTKEY_ACCEPT;}
static void open_ssh_owner(void) {
  FILE *f=fopen(getenv("GT_TEST_SSH_KEY"),"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));char *key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,f)==(size_t)n);fclose(f);
  char known[1200];CHECK(snprintf(known,sizeof(known),"%s/ssh-known",scratch)>0);
  gt_ssh_opts_t opts={.host=getenv("GT_TEST_SSH_HOST"),.port=(uint16_t)atoi(getenv("GT_TEST_SSH_PORT")),.username=getenv("GT_TEST_SSH_USER"),.known_hosts_path=known,.connect_timeout_ms=3000};
  gt_ssh_t *ssh=gt_ssh_new(&opts);CHECK(ssh && gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK && gt_ssh_set_hostkey_policy(ssh,ssh_accept,NULL)==GT_OK && gt_ssh_connect(ssh)==GT_OK);memset(key,0,(size_t)n);free(key);CHECK(gt__ssh_link_open(&ssh,NULL,&ssh_owner)==GT_OK && !ssh);
}
static void connect_client(client *c,const gt_protocol_bind_t *bind) {
  memset(c,0,sizeof(*c));c->proxy=-1;int fd=-1;gt_stream_ops_t ops;
  if(getenv("GT_TEST_SSH_HOST")) {
    if(!ssh_owner)open_ssh_owner();
    uint64_t end=now()+5000;gt_ssh_link_info_t info;
    do {CHECK(gt__ssh_link_info(ssh_owner,bind?1:0,&info));if(info.phase==GT_SSH_LINK_IDLE || info.phase==GT_SSH_LINK_ENDED)break;CHECK(now()<end);pause_one();}while(true);
    CHECK(gt__ssh_link_stream(ssh_owner,bind?1:0,proxy_binary,directory,daemon_binary,&ops)==GT_OK);
  } else if(proxy_binary) {
    int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_addclose(&actions,fds[0]) && !posix_spawn_file_actions_adddup2(&actions,fds[1],0) && !posix_spawn_file_actions_adddup2(&actions,fds[1],1));
    if(fds[1]>1)CHECK(!posix_spawn_file_actions_addclose(&actions,fds[1]));
    char *argv[]={(char *)proxy_binary,"--runtime-dir",directory,"--role",bind?"data":"management","--daemon",(char *)daemon_binary,NULL};
    CHECK(!posix_spawn(&c->proxy,proxy_binary,&actions,NULL,argv,environ));posix_spawn_file_actions_destroy(&actions);close(fds[1]);fd=fds[0];
  } else {
    gt_unix_endpoint_t *e=endpoint();CHECK(gt__unix_endpoint_connect(e,1000,&fd)==GT_ENDPOINT_OK);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  }
  if(fd>=0){CHECK(gt__socket_stream_adopt(fd,&c->socket,&ops)==GT_OK);}
  CHECK(gt__connection_open(&ops,(gt_connection_limits_t){65560,65560,5000},NULL,&c->raw)==GT_CONNECTION_CREATED);
  gt_client_bootstrap_config_t config={.hello={.role=bind?GT_ROLE_DATA:GT_ROLE_MANAGEMENT},.timeout_ms=5000,.clock=clock_ms};memcpy(config.hello.compatibility,gt_session_contract,32);
  if(bind){config.binding=*bind;memcpy(config.hello.expected_instance,bind->instance,16);}
  else if(ssh_owner)memcpy(config.hello.expected_instance,expected_instance,16);
  gt_client_bootstrap_t *b=NULL;CHECK(gt__client_bootstrap_open(c->raw,&config,NULL,&b)==GT_OK);c->raw=NULL;uint64_t end=now()+10000;
  while(gt__client_bootstrap_step(b)!=GT_BOOTSTRAP_READY){CHECK(now()<end);gt_bootstrap_info_t info;CHECK(gt__client_bootstrap_info(b,&info) && info.state!=GT_BOOTSTRAP_FAILED);pause_one();}
  gt_bootstrap_result_t identity;CHECK(gt__client_bootstrap_take(b,&c->raw,&identity));gt__client_bootstrap_close(b);
  if(!bind && ssh_owner)memcpy(expected_instance,identity.identity.instance,16);
  if(!bind) {
    gt_control_client_config_t management={.heartbeat_ms=1000,.request_timeout_ms=5000,.context=c,.clock=clock_ms,.result=result};memcpy(management.instance,identity.identity.instance,16);
    CHECK(gt__control_client_open(c->raw,&management,NULL,&c->control)==GT_OK);c->raw=NULL;
  }
}
static gt_control_result_t request(client *c,gt_control_request_t req) {
  unsigned before=c->results;uint64_t id;gt_status_t error;CHECK(gt__control_client_submit(c->control,&req,&id,&error)==GT_CONTROL_CLIENT_QUEUED && error==GT_OK);
  uint64_t end=now()+10000;while(c->results==before){CHECK(now()<end && gt__control_client_step(c->control)!=GT_CONTROL_CLIENT_FAILED);pause_one();}
  CHECK(c->results==before+1 && c->reply.delivery==GT_CONTROL_CLIENT_REPLY && c->reply.request_id==id);return c->reply;
}
static void close_client(client *c){gt__control_client_close(c->control);gt__connection_destroy(c->raw);if(c->proxy>0){int status=wait_child(c->proxy);CHECK(WIFEXITED(status));}memset(c,0,sizeof(*c));}
static uint64_t create(client *c,const char *program,unsigned nonce) {
  gt_create_spec_t spec={.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,
    .executable="/bin/sh",.cwd=getenv("GT_TEST_REMOTE_CWD")?getenv("GT_TEST_REMOTE_CWD"):root,.term="xterm-256color",.argc=2,.argv={"-c",program}};spec.nonce[0]=(uint8_t)nonce;
  gt_control_result_t r=request(c,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});CHECK(r.reply_type==GT_MSG_CREATE_REPLY && r.creation.status==GT_CREATION_ACCEPTED);return r.creation.session;
}
typedef struct {
  gt_attachment_client_t *attachment;p0_budget budget;p0_budget_scope scope;GhosttyTerminal terminal;
  gt_vt_snapshot_view_t view;gt_client_applied_t applied;gt_user_reply_t reply;unsigned results;
} replica;
static gt_client_sink_rc_t snapshot(void *context,uint64_t generation,uint64_t sequence,uint32_t format,const uint8_t *bytes,size_t n,bool readonly,gt_status_t *error) {
  replica *r=context;(void)readonly;CHECK(format==GT_VT_SNAPSHOT_FORMAT && !r->terminal && gt_vt_snapshot_parse(bytes,n,8*MIB,&r->view)==GT_OK);
  CHECK(p0_budget_init(&r->budget,64*MIB) && p0_budget_scope_admit(&r->scope,&r->budget,64*MIB));
  GhosttySnapshotDecoder decoder=NULL;CHECK(ghostty_snapshot_decoder_new_buf(p0_budget_allocator(&r->scope,P0_BUDGET_GENERAL),&decoder,r->view.canonical,r->view.canonical_bytes)==GHOSTTY_SUCCESS);
  CHECK(ghostty_snapshot_decoder_p0_page_allocator(decoder,p0_budget_allocator(&r->scope,P0_BUDGET_PAGES))==GHOSTTY_SUCCESS);
  bool retain=true;size_t continuation=(size_t)r->view.continuation_bytes;
  CHECK(ghostty_snapshot_decoder_set(decoder,GHOSTTY_SNAPSHOT_DECODER_OPT_RETAIN_CONTINUATION,&retain)==GHOSTTY_SUCCESS && ghostty_snapshot_decoder_set(decoder,GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES,&continuation)==GHOSTTY_SUCCESS);
  CHECK(ghostty_snapshot_decoder_decode(decoder,&r->terminal)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
  CHECK(!ghostty_terminal_p0_resources_decode(r->terminal,r->view.resources,r->view.resource_bytes,8*MIB));CHECK(ghostty_terminal_set(r->terminal,GHOSTTY_TERMINAL_OPT_WRITE_PTY,NULL)==GHOSTTY_SUCCESS);
  r->view.canonical=r->view.resources=NULL;r->applied=(gt_client_applied_t){true,generation,sequence};*error=GT_OK;return GT_CLIENT_SINK_ACCEPTED;
}
static gt_client_sink_rc_t event(void *context,uint64_t generation,uint64_t sequence,const gt_state_event_t *e,gt_status_t *error) {
  replica *r=context;CHECK(r->applied.generation==generation && r->applied.sequence+1==sequence);
  CHECK(e->kind==GT_STATE_OUTPUT && p0_budget_feed(&r->scope,r->terminal,e->payload,e->length)==GHOSTTY_SUCCESS);
  r->applied.sequence=sequence;*error=GT_OK;return GT_CLIENT_SINK_ACCEPTED;
}
static gt_status_t applied(void *context,gt_client_applied_t *a){*a=((replica *)context)->applied;return GT_OK;}
static void invalidated(void *context,uint64_t generation,gt_status_t error){(void)context;CHECK(generation && error!=GT_OK);}
static void user_result(void *context,const gt_user_reply_t *r){replica *v=context;v->reply=*r;v->results++;}
static gt_client_info_t pump(replica *r) {
  gt_client_phase_t phase=gt__attachment_client_step(r->attachment);CHECK(phase!=GT_CLIENT_FAILED && phase!=GT_CLIENT_CLOSING);
  gt_client_info_t info;CHECK(gt__attachment_client_info(r->attachment,&info));pause_one();return info;
}
static void attach(client *control,client *data,replica *r,uint64_t session,bool readonly) {
  uint64_t end=now()+10000;gt_control_result_t reply;
  do {reply=request(control,(gt_control_request_t){.type=GT_MSG_ATTACH,.session=session});CHECK(now()<end);if(reply.attachment.status==GT_ATTACH_BUSY)pause_one();}while(reply.attachment.status==GT_ATTACH_BUSY);
  CHECK(reply.attachment.status==GT_ATTACH_OK);gt_protocol_bind_t bind={.session=session,.generation=reply.attachment.generation,.token=reply.attachment.token};memcpy(bind.instance,reply.attachment.instance,16);
  connect_client(data,&bind);memset(&bind.token,0,sizeof(bind.token));memset(&reply,0,sizeof(reply));memset(&control->reply,0,sizeof(control->reply));memset(r,0,sizeof(*r));
  gt_client_config_t config={.session=session,.generation=bind.generation,.format=GT_VT_SNAPSHOT_FORMAT,.snapshot_limit=8*MIB,.commands=4,
    .restore_timeout_ms=10000,.apply_timeout_ms=5000,.heartbeat_ms=1000,.peer_timeout_ms=5000,.command_timeout_ms=5000,.error_timeout_ms=3000,
    .clock=clock_ms,.sink={r,snapshot,event,applied,invalidated,user_result}};
  CHECK(gt__attachment_client_open(data->raw,&config,NULL,&r->attachment)==GT_OK);data->raw=NULL;
  for(;;){CHECK(now()<end);gt_client_info_t info=pump(r);if(info.phase==(readonly?GT_CLIENT_READONLY:GT_CLIENT_LIVE) || (readonly && info.phase==GT_CLIENT_ENDED))break;}
}
static char *text(replica *r) {
  GhosttyFormatter fmt=NULL;GhosttyFormatterTerminalOptions options={.size=sizeof(options),.emit=GHOSTTY_FORMATTER_FORMAT_PLAIN,.trim=true};const GhosttyAllocator *a=p0_budget_allocator(&r->scope,P0_BUDGET_GENERAL);
  CHECK(ghostty_formatter_terminal_new(a,&fmt,r->terminal,options)==GHOSTTY_SUCCESS);uint8_t *p=NULL;size_t n=0;CHECK(ghostty_formatter_format_alloc(fmt,a,&p,&n)==GHOSTTY_SUCCESS);
  char *s=malloc(n+1);CHECK(s);if(n)memcpy(s,p,n);s[n]=0;ghostty_free(a,p,n);ghostty_formatter_free(fmt);return s;
}
static bool contains(replica *r,const char *needle){char *s=text(r);bool yes=strstr(s,needle)!=NULL;free(s);return yes;}
static void wait_text(replica *r,const char *needle){uint64_t end=now()+10000;while(!contains(r,needle)){CHECK(now()<end);(void)pump(r);}}
static void close_replica(replica *r){gt__attachment_client_close(r->attachment);ghostty_terminal_free(r->terminal);CHECK(p0_budget_scope_close(&r->scope) && p0_budget_destroy(&r->budget));}
static void input(replica *r,const char *s) {
  gt_user_request_t req={.kind=GT_WORKER_INPUT,.input=(const uint8_t *)s,.length=strlen(s)};gt_worker_stamp_t stamp;unsigned before=r->results;
  CHECK(gt__attachment_client_submit(r->attachment,&req,&stamp)==GT_CLIENT_QUEUED);uint64_t end=now()+10000;
  while(r->results==before){CHECK(now()<end);(void)pump(r);}CHECK(r->results==before+1 && r->reply.delivery==GT_USER_OPERATION_OK);
}
static int crash_client(uint64_t session) {
  CHECK(!fcntl(3,F_SETFD,FD_CLOEXEC));
  client control,data;replica r;connect_client(&control,NULL);attach(&control,&data,&r,session,false);wait_text(&r,"-WAIT");
  char *s=text(&r);char *p=strstr(s,"PID:");CHECK(p);uint64_t report[2]={strtoull(p+4,NULL,10),r.applied.generation};free(s);CHECK(report[0]>1 && write(3,report,sizeof(report))==(ssize_t)sizeof(report));
  for(;;)pause_one();
}
static void flow(void) {
  start_server();client control;connect_client(&control,NULL);
  uint64_t session=create(&control,"stty -echo -icanon min 1 time 0; printf 'PID:%d' \"$$\"; printf '\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); [ \"$a\" = \"$(printf '\\033[?997;1n')\" ] || exit 90; printf '%s' '-WAIT'; IFS= read -r a; [ \"$a\" = go ] || exit 91; printf '%s' '-AFTER'; exit 7",1);close_client(&control);
  /* Reconnection must authenticate a new TCP/SSH session, not reuse parent IO. */
  if(ssh_owner){gt__ssh_link_close(ssh_owner);ssh_owner=NULL;}
  int fds[2];CHECK(!pipe(fds));posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addclose(&actions,fds[0]) && !posix_spawn_file_actions_adddup2(&actions,fds[1],3));
  if(fds[1]!=3)CHECK(!posix_spawn_file_actions_addclose(&actions,fds[1]));
  char id[32];snprintf(id,sizeof(id),"%llu",(unsigned long long)session);
  char *args[]={(char *)self,"crash",(char *)daemon_binary,(char *)root,directory,id,(char *)proxy_binary,NULL};CHECK(!posix_spawn(&crash_pid,self,&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);close(fds[1]);
  uint64_t report[2]={0};struct pollfd ready={.fd=fds[0],.events=POLLIN};CHECK(poll(&ready,1,10000)>0);CHECK(read(fds[0],report,sizeof(report))==(ssize_t)sizeof(report));close(fds[0]);CHECK(!kill(crash_pid,SIGKILL));int status=wait_child(crash_pid);crash_pid=-1;CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL && (getenv("GT_TEST_REMOTE_RUNTIME") || !kill((pid_t)report[0],0)));
  connect_client(&control,NULL);gt_control_result_t info=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=session});CHECK(info.status==GT_CONTROL_OK && !info.rows[0].has_result);
  client data;replica r;attach(&control,&data,&r,session,false);CHECK(r.applied.generation>report[1]);char pid[64];snprintf(pid,sizeof(pid),"PID:%llu",(unsigned long long)report[0]);CHECK(contains(&r,pid) && contains(&r,"-WAIT"));input(&r,"go\n");
  uint64_t end=now()+10000;while(!pump(&r).has_terminal)CHECK(now()<end);CHECK(contains(&r,"-AFTER"));
  /* The attached replica sees the terminal result and refuses further input. */
  gt_worker_stamp_t stamp;gt_user_request_t req={.kind=GT_WORKER_INPUT,.input=(const uint8_t *)"x",.length=1};CHECK(gt__attachment_client_submit(r.attachment,&req,&stamp)==GT_CLIENT_LOCKED);
  do {CHECK(now()<end);info=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=session});pause_one();}while(info.rows[0].phase!=GT_MANAGED_RETAINED);
  CHECK(info.rows[0].result.status==GT_OK && info.rows[0].result.has_sequence);
  /* PTY EOF can precede waiter publication. Preserve explicit unknown rather
   * than inventing exit 0; a known exit must be the program's real exit 7. */
  CHECK(info.rows[0].result.has_exit_code?info.rows[0].result.exit_code==7:info.rows[0].result.termination_status==GT_ERR_NOT_FOUND);
  close_replica(&r);close_client(&data);
  /* Last reference gone: the finished Source is reclaimed immediately instead
   * of holding a Source slot for a retention window. */
  do {CHECK(now()<end);info=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=session});pause_one();}while(info.status!=GT_CONTROL_NOT_FOUND);
  close_client(&control);stop_server(SIGTERM);
}
static void competing(void) {
  start_server();pid_t other=spawn_server();int status=wait_child(other);CHECK(WIFEXITED(status) && WEXITSTATUS(status)!=0 && !kill(daemon_pid,0));
  client c;connect_client(&c,NULL);gt_control_result_t reply=request(&c,(gt_control_request_t){.type=GT_MSG_LIST});CHECK(reply.status==GT_CONTROL_OK && !reply.count);close_client(&c);stop_server(SIGTERM);
}
static void malformed(void) {
  start_server();gt_unix_endpoint_t *e=endpoint();int fd=-1;CHECK(gt__unix_endpoint_connect(e,1000,&fd)==GT_ENDPOINT_OK);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  uint8_t junk[24]={0};CHECK(write(fd,junk,sizeof(junk))==(ssize_t)sizeof(junk));close(fd);
  client c;connect_client(&c,NULL);gt_control_result_t reply=request(&c,(gt_control_request_t){.type=GT_MSG_LIST});CHECK(reply.status==GT_CONTROL_OK);close_client(&c);stop_server(SIGINT);
}
typedef union {max_align_t align;size_t size;} header;
typedef struct {gt_daemon_memory_t memory;size_t calls,fail;} allocation_memory;
static void *admit(void *context,size_t n) {
  allocation_memory *m=context;if(++m->calls==m->fail)return NULL;
  gt_allocator_t a=gt__daemon_memory_allocator(&m->memory);header *h=a.allocate(a.context,sizeof(*h)+n);if(!h)return NULL;h->size=n;return h+1;
}
static void refund(void *context,void *p,size_t n) {
  allocation_memory *m=context;header *h=(header *)p-1;CHECK(h->size==n);gt_allocator_t a=gt__daemon_memory_allocator(&m->memory);a.release(a.context,h,sizeof(*h)+n);
}
static gt_vt_factory_t *local_factory(gt_allocator_t *a) {
  gt_vt_config_t c={.budget_bytes=256*MIB,.state_bytes=32*MIB,.frozen_bytes=32*MIB,.encoder_bytes=32*MIB,.checkpoint_bytes=8*MIB,
    .continuation_bytes=MIB,.history_bytes=MIB,.history_lines=100,.image_bytes=MIB,.cell_width=10,.cell_height=20,
    .light={{255,255,255},{0,0,0}},.dark={{18,18,18},{255,255,255}}};gt_vt_factory_t *f=NULL;CHECK(gt__vt_factory_open(&c,a,&f)==GT_OK);return f;
}
static gt_session_host_config_t local_config(gt_vt_factory_t *f) {
  long page=sysconf(_SC_PAGESIZE);CHECK(page>0);gt_session_host_config_t c={.directory=directory,.sessions=1,.connections=4,.snapshot_limit=8*MIB,
    .snapshot_format=GT_VT_SNAPSHOT_FORMAT,.threads={2*MIB,(size_t)page},.state=gt__vt_factory_state(f),.encoder=gt__vt_snapshot_encoder(f)};
  CHECK(gt__protocol_identity_new(gt_session_contract,0,NULL,&c.identity)==GT_OK);return c;
}
static void local_directory(void) {
  CHECK(snprintf(scratch,sizeof(scratch),"%s/tmp/host-XXXXXX",root)>0 && mkdtemp(scratch));CHECK(snprintf(directory,sizeof(directory),"%s/run",scratch)>0);
}
static void close_local(void) {
  uint64_t end=now()+10000;
  while(local_host){gt_runtime_rc_t rc=gt__session_host_close(&local_host);CHECK(rc==GT_RUNTIME_OK || rc==GT_RUNTIME_BUSY);CHECK(now()<end);(void)poll(NULL,0,1);}
}
static void allocation_test(void) {
  local_directory();allocation_memory m={0};gt__daemon_memory_init(&m.memory,64*MIB);gt_allocator_t a={&m,admit,refund};gt_vt_factory_t *f=local_factory(&a);
  gt_session_host_config_t c=local_config(f);size_t retained_bytes=atomic_load(&m.memory.used);unsigned failures=0;
  for(size_t nth=1;nth<64;nth++) {
    m.fail=m.calls+nth;gt_session_host_t *h=(void *)1;gt_status_t rc=gt__session_host_open(&c,&a,&h);m.fail=0;
    if(rc==GT_OK){CHECK(h);CHECK(gt__session_host_close(&h)==GT_RUNTIME_OK && !h);break;}
    CHECK(!h && atomic_load(&m.memory.used)==retained_bytes);failures++;
  }
  CHECK(failures>=10 && atomic_load(&m.memory.used)==retained_bytes);CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.memory.used));
  gt_daemon_memory_t tiny;gt__daemon_memory_init(&tiny,8);a=gt__daemon_memory_allocator(&tiny);CHECK(!a.allocate(a.context,9) && !atomic_load(&tiny.used));void *v=a.allocate(a.context,8);CHECK(v && !a.allocate(a.context,1));a.release(a.context,v,8);CHECK(!atomic_load(&tiny.used));
}
/* A finished session keeps its frozen terminal only while a client references
 * it: the Source slot returns as soon as the last attachment drops, with no
 * retention expiry to wait for. */
static void retention(void) {
  local_directory();gt_daemon_memory_t m;gt__daemon_memory_init(&m,64*MIB);gt_allocator_t a=gt__daemon_memory_allocator(&m);gt_vt_factory_t *f=local_factory(&a);gt_session_host_config_t c=local_config(f);
  CHECK(gt__session_host_open(&c,&a,&local_host)==GT_OK);client control,data;connect_client(&control,NULL);
  uint64_t id=create(&control,"printf RETAINED; IFS= read -r line; exit 4",1);gt_control_result_t info;uint64_t end=now()+5000;
  replica r;attach(&control,&data,&r,id,false);wait_text(&r,"RETAINED");input(&r,"go\n");
  do {CHECK(now()<end);info=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=id});pause_one();}while(info.rows[0].phase!=GT_MANAGED_RETAINED);
  /* While a client is attached the frozen terminal must stay inspectable. */
  uint64_t hold=now()+300;while(now()<hold)(void)pump(&r);
  info=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=id});CHECK(info.status==GT_CONTROL_OK && info.rows[0].phase==GT_MANAGED_RETAINED && info.rows[0].has_result);
  close_replica(&r);close_client(&data);
  /* Last reference gone: with capacity one, the slot must come back here or the
   * next create cannot be admitted. */
  do {CHECK(now()<end);info=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=id});pause_one();}while(info.status!=GT_CONTROL_NOT_FOUND);
  info=request(&control,(gt_control_request_t){.type=GT_MSG_LIST});CHECK(info.status==GT_CONTROL_OK && !info.count);CHECK(create(&control,"exit 0",2)>id);close_client(&control);close_local();CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.used));
}
static void shutdown_test(void) {
  start_server();client control,d1,d2;replica r1,r2;connect_client(&control,NULL);
  const char *program="stty -echo; printf 'PID:%d-WAIT' \"$$\"; IFS= read -r line; exit 7";
  uint64_t a=create(&control,program,1),b=create(&control,program,2);attach(&control,&d1,&r1,a,false);attach(&control,&d2,&r2,b,false);wait_text(&r1,"-WAIT");wait_text(&r2,"-WAIT");
  char *s1=text(&r1),*s2=text(&r2);char *p1=strstr(s1,"PID:"),*p2=strstr(s2,"PID:");CHECK(p1 && p2);pid_t first=(pid_t)strtol(p1+4,NULL,10),second=(pid_t)strtol(p2+4,NULL,10);free(s1);free(s2);CHECK(first>1 && second>1 && first!=second);
  gt_control_result_t reply=request(&control,(gt_control_request_t){.type=GT_MSG_TERMINATE,.session=a});CHECK(reply.status==GT_CONTROL_OK);
  uint64_t end=now()+5000;while(!pump(&r1).has_terminal){CHECK(now()<end);(void)pump(&r2);}
  reply=request(&control,(gt_control_request_t){.type=GT_MSG_INFO,.session=b});CHECK(reply.status==GT_CONTROL_OK && !reply.rows[0].has_result && !kill(second,0));
  stop_server(SIGINT);CHECK(kill(first,0)<0 && kill(second,0)<0);close_replica(&r1);close_replica(&r2);close_client(&d1);close_client(&d2);close_client(&control);
}
static void sessions(void) {
  start_server();client control,data[6];replica replicas[6];uint64_t ids[6];connect_client(&control,NULL);
  for(unsigned i=0;i<6;i++) {
    ids[i]=create(&control,"printf SOURCE-READY; IFS= read -r hold",i+1);
    for(unsigned j=0;j<i;j++)CHECK(ids[i]!=ids[j]);
    attach(&control,&data[i],&replicas[i],ids[i],false);wait_text(&replicas[i],"SOURCE-READY");
  }
  gt_control_result_t listed=request(&control,(gt_control_request_t){.type=GT_MSG_LIST});CHECK(listed.status==GT_CONTROL_OK && listed.count==6);
  for(unsigned i=0;i<6;i++){close_replica(&replicas[i]);close_client(&data[i]);}
  close_client(&control);stop_server(SIGTERM);
}
static void capacity(void) {
  start_server();client peers[64];for(unsigned i=0;i<64;i++)connect_client(&peers[i],NULL);
  gt_unix_endpoint_t *e=endpoint();int fd=-1;CHECK(gt__unix_endpoint_connect(e,1000,&fd)==GT_ENDPOINT_OK);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,3000)>0);char b;CHECK(read(fd,&b,1)==0);close(fd);
  for(unsigned i=0;i<64;i++)close_client(&peers[i]);
  for(unsigned i=0;i<20;i++){client c;connect_client(&c,NULL);gt_control_result_t r=request(&c,(gt_control_request_t){.type=GT_MSG_PING});CHECK(r.reply_type==GT_MSG_PING_REPLY);close_client(&c);}
  stop_server(SIGTERM);
}
/* Native fixture supplies already resolved loopback/credential material after
 * callback return. Shared production provider drives TCP/SSH with no worker. */
typedef struct {
  gt_ssh_session_provider_t *ssh;gt_local_session_provider_t *local;gt_session_link_t base;gt_socket_stream_t sockets[2];
  gt_stream_ops_t local_streams[2];bool disconnected;
  char *key;char known[1200];uint64_t request,hostkey_request;unsigned starts,resets;
} reconnect_provider;
static reconnect_provider reconnect_link;static gt_session_client_t *reconnect_session;
static void authentication_requested(void *context,uint64_t request,uint32_t attempt){reconnect_provider *p=context;(void)attempt;p->request=request;}
static void authentication_cancelled(void *context,uint64_t request){reconnect_provider *p=context;if(p->request==request)p->request=0;if(p->hostkey_request==request)p->hostkey_request=0;}
static void fixture_hostkey(void *context,uint64_t request,const gt_ssh_connector_info_t *info){reconnect_provider *p=context;CHECK(info->host==GT_SSH_HOST_UNKNOWN);p->hostkey_request=request;}
static void native_completions(void) {
  reconnect_provider *p=&reconnect_link;
  if(p->request) {
    uint64_t request=p->request;p->request=0;
    gt_ssh_opts_t options={.host=getenv("GT_TEST_SSH_HOST"),.port=(uint16_t)atoi(getenv("GT_TEST_SSH_PORT")),.username=getenv("GT_TEST_SSH_USER"),.known_hosts_path=p->known,.connect_timeout_ms=3000};
    gt_ssh_t *ssh=gt_ssh_new(&options);CHECK(ssh && gt_ssh_auth_private_key(ssh,p->key,NULL)==GT_OK);
    gt_ssh_connector_config_t resolved={.family=4};CHECK(inet_pton(AF_INET,options.host,resolved.address)==1);
    gt_status_t status=gt__ssh_session_provider_supply(p->ssh,request,&ssh,&resolved);CHECK(status==GT_OK || status==GT_ERR_NOT_FOUND);gt_ssh_close(ssh);
  }
  /* Test-only explicit trust of the generated disposable sshd's hostkey. */
  if(p->hostkey_request){uint64_t request=p->hostkey_request;p->hostkey_request=0;gt_status_t status=gt__ssh_session_provider_decide(p->ssh,request,GT_HOSTKEY_ACCEPT);CHECK(status==GT_OK || status==GT_ERR_NOT_FOUND);}
}
static gt_session_link_state_t reconnect_step(void *context,bool start,uint32_t attempt,gt_status_t *error) {
  reconnect_provider *p=context;if(start)p->starts++;
  if(p->ssh || p->local)return p->base.step(p->base.context,start,attempt,error);
  *error=GT_OK;return GT_SESSION_LINK_READY;
}
static gt_stream_result_t local_read(void *context,void *bytes,size_t n){gt_stream_ops_t *s=context;if(reconnect_link.disconnected)return (gt_stream_result_t){.kind=GT_STREAM_ERROR};return s->read(s->context,bytes,n);}
static gt_stream_result_t local_write(void *context,const void *bytes,size_t n){gt_stream_ops_t *s=context;if(reconnect_link.disconnected)return (gt_stream_result_t){.kind=GT_STREAM_ERROR};return s->write(s->context,bytes,n);}
static bool local_clock(void *context,uint64_t *out){gt_stream_ops_t *s=context;return s->clock(s->context,out);}
static void local_close(void *context){gt_stream_ops_t *s=context;s->close(s->context);}
static gt_status_t reconnect_stream(void *context,unsigned slot,gt_stream_ops_t *ops) {
  reconnect_provider *p=context;if(p->ssh)return p->base.stream(p->base.context,slot,ops);
  if(p->local){gt_status_t e=p->base.stream(p->base.context,slot,&p->local_streams[slot]);if(e==GT_OK)*ops=(gt_stream_ops_t){&p->local_streams[slot],local_read,local_write,local_clock,local_close};return e;}
  gt_unix_endpoint_t *e=endpoint();int fd=-1;gt_endpoint_rc_t rc=gt__unix_endpoint_connect(e,0,&fd);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  if(rc!=GT_ENDPOINT_OK)return GT_ERR_IO;
  return gt__socket_stream_adopt(fd,&p->sockets[slot],ops);
}
static void reconnect_reset(void *context){reconnect_provider *p=context;p->resets++;if(p->ssh || p->local)p->base.reset(p->base.context);p->disconnected=false;}
static void reconnect_finish(void) {
  reconnect_cleanup=NULL;gt__session_client_close(reconnect_session);reconnect_session=NULL;
  reconnect_provider *p=&reconnect_link;gt__ssh_session_provider_close(p->ssh);p->ssh=NULL;
  if(p->local){uint64_t end=now()+3000;while(!gt__local_session_provider_quiesce(p->local) && now()<end)pause_one();CHECK(gt__local_session_provider_destroy(p->local));p->local=NULL;}
  if(p->key){memset(p->key,0,strlen(p->key));free(p->key);p->key=NULL;}
}
static gt_client_sink_rc_t reconnect_snapshot(void *context,uint64_t generation,uint64_t sequence,uint32_t format,const uint8_t *bytes,size_t n,bool readonly,gt_status_t *error) {
  replica *r=context;if(r->terminal){ghostty_terminal_free(r->terminal);r->terminal=NULL;CHECK(p0_budget_scope_close(&r->scope) && p0_budget_destroy(&r->budget));}
  return snapshot(context,generation,sequence,format,bytes,n,readonly,error);
}
static void reconnect_pump(void) {
  uint64_t started=now();gt_session_phase_t phase=gt__session_client_step(reconnect_session);CHECK(phase!=GT_SESSION_FAILED);CHECK(now()-started<100);native_completions();
  if(local_host)CHECK(gt__session_host_poll(local_host,-1,0)==GT_OK);
  struct timespec t={0,1000000};nanosleep(&t,NULL);
}
static gt_session_client_info_t reconnect_info(void){gt_session_client_info_t i;CHECK(gt__session_client_info(reconnect_session,&i));return i;}
static void reconnect_wait(replica *r,const char *needle,uint32_t attempts) {
  uint64_t deadline=now()+(attempts>2?60000:10000);
  for(;;){gt_session_client_info_t i=reconnect_info();if(i.phase==GT_SESSION_LIVE && i.attempts>=attempts && r->terminal && contains(r,needle))return;CHECK(now()<deadline);reconnect_pump();}
}
static void relay_command(const char *command) {
  const char *path=getenv("GT_TEST_RELAY_CONTROL");CHECK(path);int fd=socket(AF_UNIX,SOCK_STREAM,0);CHECK(fd>=0);struct sockaddr_un address={.sun_family=AF_UNIX};CHECK(strlen(path)<sizeof(address.sun_path));strcpy(address.sun_path,path);
  CHECK(!connect(fd,(struct sockaddr *)&address,sizeof(address)));CHECK(write(fd,command,strlen(command))==(ssize_t)strlen(command));char result[3]={0};CHECK(read(fd,result,2)==2 && !strcmp(result,"OK"));close(fd);
}
static uint64_t replica_pid(replica *r){char *s=text(r);char *p=strstr(s,"PID:");CHECK(p);uint64_t pid=strtoull(p+4,NULL,10);free(s);CHECK(pid>1);return pid;}
static void reconnect_input(const char *input) {
  gt_user_request_t r={.kind=GT_WORKER_INPUT,.input=(const uint8_t *)input,.length=strlen(input)};gt_worker_stamp_t stamp;CHECK(gt__session_client_submit(reconnect_session,&r,&stamp)==GT_CLIENT_QUEUED);
}
static void automatic_reconnect(void) {
  start_server();memset(&reconnect_link,0,sizeof(reconnect_link));reconnect_cleanup=reconnect_finish;
  if(getenv("GT_TEST_SSH_HOST")) {
    CHECK(snprintf(reconnect_link.known,sizeof(reconnect_link.known),"%s/reconnect-known",scratch)>0);
    FILE *f=fopen(getenv("GT_TEST_SSH_KEY"),"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));reconnect_link.key=calloc(1,(size_t)n+1);CHECK(reconnect_link.key && fread(reconnect_link.key,1,(size_t)n,f)==(size_t)n);fclose(f);
    gt_ssh_session_provider_config_t provider={.proxy=proxy_binary,.daemon=daemon_binary,.runtime=directory,.authentication_timeout_ms=5000,.clock=clock_ms,.context=&reconnect_link,.requested=authentication_requested,.cancelled=authentication_cancelled,.hostkey=fixture_hostkey};
    CHECK(gt__ssh_session_provider_open(&provider,NULL,&reconnect_link.ssh)==GT_OK && gt__ssh_session_provider_link(reconnect_link.ssh,&reconnect_link.base));
  }
  if(proxy_binary && !reconnect_link.ssh){gt_local_session_provider_config_t provider={.proxy=proxy_binary,.daemon=daemon_binary,.runtime=directory,.clock=clock_ms};CHECK(gt__local_session_provider_open(&provider,NULL,&reconnect_link.local)==GT_OK && gt__local_session_provider_link(reconnect_link.local,&reconnect_link.base));}
  replica r={0};gt_create_spec_t spec={.nonce={81},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,.executable="/bin/sh",.cwd=getenv("GT_TEST_REMOTE_CWD")?getenv("GT_TEST_REMOTE_CWD"):root,.term="xterm-256color",.argc=2,
    .argv={"-c","stty -echo; printf 'PID:%d-WAIT' \"$$\"; IFS= read -r a; [ \"$a\" = go ] || exit 91; printf '%s' '-AFTER'; IFS= read -r a; [ \"$a\" = end ] || exit 92; exit 7"}};
  gt_session_client_config_t config={.link={&reconnect_link,reconnect_step,reconnect_stream,reconnect_reset},.create=&spec,
    .recovery_timeout_ms=60000,.backoff_ms=100,.bootstrap_timeout_ms=5000,.management_heartbeat_ms=250,.management_timeout_ms=1500,.clock=clock_ms,
    .attachment={.format=GT_VT_SNAPSHOT_FORMAT,.snapshot_limit=8*MIB,.commands=4,.restore_timeout_ms=30000,.apply_timeout_ms=5000,.heartbeat_ms=250,.peer_timeout_ms=1500,.command_timeout_ms=3000,.error_timeout_ms=3000,
      .sink={&r,reconnect_snapshot,event,applied,invalidated,user_result}}};memcpy(config.compatibility,gt_session_contract,32);
  CHECK(gt__session_client_open(&config,NULL,&reconnect_session)==GT_OK);reconnect_wait(&r,"-WAIT",1);gt_session_client_info_t original=reconnect_info();uint64_t pid=replica_pid(&r);
  if(getenv("GT_TEST_RELAY_CONTROL"))relay_command("reset");else if(reconnect_link.local)reconnect_link.disconnected=true;else {CHECK(!reconnect_link.ssh);CHECK(reconnect_link.sockets[0].fd>=0 && !shutdown(reconnect_link.sockets[0].fd,SHUT_RDWR));}
  reconnect_wait(&r,"-WAIT",2);CHECK(reconnect_info().session==original.session && !memcmp(reconnect_info().instance,original.instance,16) && reconnect_info().generation>original.generation && replica_pid(&r)==pid);
  if(getenv("GT_TEST_RELAY_CONTROL")) {
    uint32_t previous=reconnect_info().attempts,busy_before=reconnect_info().busy_replies;relay_command("blackhole");uint64_t deadline=now()+10000;
    while(reconnect_info().phase==GT_SESSION_LIVE){CHECK(now()<deadline);reconnect_pump();}
    gt_user_request_t denied={.kind=GT_WORKER_INPUT,.input=(const uint8_t *)"bad",.length=3};gt_worker_stamp_t stamp;CHECK(gt__session_client_submit(reconnect_session,&denied,&stamp)==GT_CLIENT_LOCKED);
    relay_command("online");reconnect_wait(&r,"-WAIT",previous+1);CHECK(reconnect_info().busy_replies>busy_before);CHECK(reconnect_info().session==original.session && replica_pid(&r)==pid);
  }
  unsigned before=r.results;reconnect_input("go\n");reconnect_wait(&r,"-AFTER",2);uint64_t receipt_deadline=now()+5000;while(r.results==before){CHECK(now()<receipt_deadline);reconnect_pump();}CHECK(r.results==before+1 && r.reply.delivery==GT_USER_OPERATION_OK);reconnect_input("end\n");uint64_t deadline=now()+10000;
  while(reconnect_info().phase!=GT_SESSION_ENDED){CHECK(now()<deadline);reconnect_pump();}gt_worker_result_t ended=reconnect_info().attachment.terminal.result;CHECK(reconnect_info().attachment.has_terminal && ended.kind==GT_WORKER_END_EOF && ended.status==GT_OK);CHECK(ended.has_exit_code?ended.exit_code==7:ended.termination_status==GT_ERR_NOT_FOUND);
  printf("automatic reconnect: attempts=%u session=%llu PID=%llu exit=%s\n",reconnect_info().attempts,(unsigned long long)original.session,(unsigned long long)pid,ended.has_exit_code?"7":"unknown");
  reconnect_finish();CHECK(reconnect_link.starts==reconnect_link.resets);ghostty_terminal_free(r.terminal);CHECK(p0_budget_scope_close(&r.scope) && p0_budget_destroy(&r.budget));stop_server(SIGTERM);
}
/* Public ABI end-to-end: same daemon/PTY/VT, fresh handle resumes original PID. */
static gt_session_t *public_session;
static bool public_reentry,public_readonly;static uint64_t public_request,public_hostkey_request;
static unsigned public_resizes,public_colors;
static void public_finish(void){uint64_t end=now()+3000;while(public_session && !gt_session_close(public_session)){CHECK(now()<end);pause_one();}public_session=NULL;reconnect_cleanup=NULL;}
static gt_session_sink_result_t public_snapshot(void *ctx,uint64_t g,uint64_t s,uint32_t f,const uint8_t *bytes,size_t n,bool ro,gt_status_t *error){
  if(ro)public_readonly=true;
  if(!public_reentry){public_reentry=true;CHECK(!gt_session_close(public_session));gt_session_stamp_t stamp;CHECK(gt_session_input(public_session,g,s,"bad",3,&stamp)==GT_SESSION_SUBMIT_INVALID);gt_session_info_t info;CHECK(!gt_session_info(public_session,&info) && !info.session);}
  return (gt_session_sink_result_t)snapshot(ctx,g,s,f,bytes,n,ro,error);
}
static gt_session_sink_result_t public_event(void *ctx,uint64_t g,uint64_t s,const gt_session_event_t *e,gt_status_t *error){
  replica *r=ctx;CHECK(r->applied.generation==g && r->applied.sequence+1==s);GhosttyResult rc;
  if(e->kind==GT_SESSION_OUTPUT)rc=p0_budget_feed(&r->scope,r->terminal,e->bytes,e->length);
  else if(e->kind==GT_SESSION_RESIZE){public_resizes++;rc=p0_budget_resize_cells(&r->scope,r->terminal,e->size.cols,e->size.rows,r->view.cell_width,r->view.cell_height);}
  else {CHECK(e->kind==GT_SESSION_COLOR);public_colors++;gt_vt_colors_t c=e->dark?r->view.dark:r->view.light;GhosttyColorRgb bg={c.background[0],c.background[1],c.background[2]},fg={c.foreground[0],c.foreground[1],c.foreground[2]};rc=ghostty_terminal_set(r->terminal,GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,&bg);if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(r->terminal,GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,&fg);}
  CHECK(rc==GHOSTTY_SUCCESS);r->applied.sequence=s;*error=GT_OK;return GT_SESSION_SINK_ACCEPTED;
}
static gt_status_t public_applied(void *ctx,gt_session_applied_t *out){replica *r=ctx;*out=(gt_session_applied_t){r->applied.has_sequence,r->applied.generation,r->applied.sequence};return GT_OK;}
static void public_receipt(void *ctx,const gt_session_receipt_t *receipt){replica *r=ctx;r->results++;CHECK(receipt->delivery==GT_SESSION_OPERATION_OK && receipt->status==GT_OK && !receipt->rejected);}
static gt_session_info_t public_info(void){gt_session_info_t i;CHECK(gt_session_info(public_session,&i));return i;}
static void public_auth_requested(void *ctx,uint64_t request,uint32_t attempt){(void)ctx;CHECK(request && attempt);public_request=request;}
static void public_auth_cancelled(void *ctx,uint64_t request){(void)ctx;if(public_request==request)public_request=0;if(public_hostkey_request==request)public_hostkey_request=0;}
static void public_auth_hostkey(void *ctx,uint64_t request,const char *type,const char *fingerprint){(void)ctx;CHECK(type && type[0] && fingerprint && !strncmp(fingerprint,"SHA256:",7));public_hostkey_request=request;}
static void public_native_completions(void){
  if(public_request){uint64_t request=public_request;public_request=0;FILE *f=fopen(getenv("GT_TEST_SSH_KEY"),"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));char *key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,f)==(size_t)n);fclose(f);
    char known[1200];CHECK(snprintf(known,sizeof(known),"%s/public-known",scratch)>0);gt_ssh_opts_t options={.host=getenv("GT_TEST_SSH_HOST"),.port=(uint16_t)atoi(getenv("GT_TEST_SSH_PORT")),.username=getenv("GT_TEST_SSH_USER"),.known_hosts_path=known,.connect_timeout_ms=3000};gt_ssh_t *ssh=gt_ssh_new(&options);CHECK(ssh && gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK);memset(key,0,(size_t)n);free(key);uint8_t address[16]={0};CHECK(inet_pton(AF_INET,options.host,address)==1);CHECK(gt_session_auth_supply(public_session,request,&ssh,4,address,0)==GT_OK && !ssh);
  }
  if(public_hostkey_request){uint64_t request=public_hostkey_request;public_hostkey_request=0;CHECK(gt_session_auth_decide(public_session,request,GT_HOSTKEY_ACCEPT)==GT_OK);}
}
static void public_pump(void){CHECK(gt_session_step(public_session)!=GT_SESSION_STATE_FAILED);public_native_completions();pause_one();}
static void public_wait(replica *r,const char *needle){uint64_t end=now()+10000;for(;;){gt_session_info_t i=public_info();if(i.state==GT_SESSION_STATE_LIVE && r->terminal && contains(r,needle))return;CHECK(now()<end);public_pump();}}
static void public_input(const char *bytes){gt_session_info_t i=public_info();gt_session_stamp_t stamp;CHECK(gt_session_input(public_session,i.generation,i.sequence,bytes,strlen(bytes),&stamp)==GT_SESSION_SUBMIT_QUEUED && stamp.generation==i.generation && stamp.sequence==i.sequence);}
static void public_flow(void){
  start_server();reconnect_cleanup=public_finish;replica r={0};const char *args[]={"-c","stty -echo; printf 'PID:%d-WAIT' \"$$\"; IFS= read -r a; [ \"$a\" = go ] || exit 91; printf '%s' '-AFTER'; IFS= read -r a; [ \"$a\" = end ] || exit 92; exit 7",NULL};
  gt_session_create_t create={.nonce={93},.process={.executable="/bin/sh",.cwd=root,.term="xterm-256color",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24}};
  gt_session_options_t o={.kind=getenv("GT_TEST_SSH_HOST")?GT_SESSION_SSH:GT_SESSION_LOCAL,.authentication={NULL,public_auth_requested,public_auth_cancelled,public_auth_hostkey},.proxy=proxy_binary,.daemon=daemon_binary,.runtime=directory,.create=&create,.snapshot_format=GT_VT_SNAPSHOT_FORMAT,.snapshot_limit=8*MIB,.sink={&r,public_snapshot,public_event,public_applied,invalidated,public_receipt}};memcpy(o.compatibility,gt_session_contract,32);
  CHECK(gt_session_open(&o,&public_session)==GT_OK);public_wait(&r,"-WAIT");gt_session_info_t first=public_info();uint64_t pid=replica_pid(&r);CHECK(public_reentry && first.session && first.generation && !first.readonly);
  gt_session_stamp_t stamp;CHECK(gt_session_resize(public_session,first.generation,first.sequence,(gt_session_size_t){90,30,900,600},&stamp)==GT_SESSION_SUBMIT_QUEUED);uint64_t end=now()+5000;while(!public_resizes || r.results<1){CHECK(now()<end);public_pump();}
  gt_session_info_t resized=public_info();CHECK(gt_session_color(public_session,resized.generation,resized.sequence,true,&stamp)==GT_SESSION_SUBMIT_QUEUED);end=now()+5000;while(!public_colors || r.results<2){CHECK(now()<end);public_pump();}
  public_finish();ghostty_terminal_free(r.terminal);CHECK(p0_budget_scope_close(&r.scope) && p0_budget_destroy(&r.budget));memset(&r,0,sizeof(r));
  o.create=NULL;o.session=first.session;memcpy(o.instance,first.instance,16);reconnect_cleanup=public_finish;CHECK(gt_session_open(&o,&public_session)==GT_OK);public_wait(&r,"-WAIT");gt_session_info_t resumed=public_info();CHECK(resumed.session==first.session && !memcmp(resumed.instance,first.instance,16) && resumed.generation>first.generation && replica_pid(&r)==pid);
  CHECK(gt_session_input(public_session,first.generation,first.sequence,"bad\n",4,&stamp)==GT_SESSION_SUBMIT_LOCKED && !stamp.command_id);
  public_input("go\n");public_wait(&r,"-AFTER");end=now()+5000;while(r.results<1){CHECK(now()<end);public_pump();}public_input("end\n");end=now()+10000;while(public_info().state!=GT_SESSION_STATE_ENDED){CHECK(now()<end);public_pump();}
  gt_session_info_t done=public_info();CHECK(done.has_terminal && done.readonly && done.terminal_kind==GT_WORKER_END_EOF && done.terminal_status==GT_OK && (done.has_exit_code?done.exit_code==7:done.termination_status==GT_ERR_NOT_FOUND));
  public_finish();ghostty_terminal_free(r.terminal);CHECK(p0_budget_scope_close(&r.scope) && p0_budget_destroy(&r.budget));memset(&r,0,sizeof(r));
  /* The finished Source had no remaining client reference, so it is reclaimed;
   * a resumed handle must not resurrect it. */
  const char *stop_args[]={"-c","stty -echo; printf 'PID:%d-STOP' \"$$\"; IFS= read -r a",NULL};create.nonce[0]=94;create.process.argv=stop_args;
  o.create=&create;o.session=0;memset(o.instance,0,16);reconnect_cleanup=public_finish;CHECK(gt_session_open(&o,&public_session)==GT_OK);public_wait(&r,"-STOP");gt_session_info_t active=public_info();uint64_t active_pid=replica_pid(&r);CHECK(active.has_admitted_create);
  public_finish();ghostty_terminal_free(r.terminal);CHECK(p0_budget_scope_close(&r.scope) && p0_budget_destroy(&r.budget));memset(&r,0,sizeof(r));
  o.create=NULL;memcpy(o.instance,active.instance,16);o.pending_nonce[0]=94;reconnect_cleanup=public_finish;CHECK(gt_session_open(&o,&public_session)==GT_OK);public_wait(&r,"-STOP");CHECK(public_info().session==active.session && public_info().has_admitted_create && replica_pid(&r)==active_pid);
  public_finish();ghostty_terminal_free(r.terminal);CHECK(p0_budget_scope_close(&r.scope) && p0_budget_destroy(&r.budget));memset(&r,0,sizeof(r));
  memset(o.pending_nonce,0,16);o.catalog_only=true;o.snapshot_format=0;o.sink=(gt_session_sink_t){0};reconnect_cleanup=public_finish;CHECK(gt_session_open(&o,&public_session)==GT_OK);
  gt_session_row_t rows[64];size_t count=99;CHECK(gt_session_catalog(public_session,rows,64,&count)==GT_ERR_NOT_FOUND && !count);
  end=now()+10000;while(public_info().state!=GT_SESSION_STATE_CATALOG){CHECK(now()<end);public_pump();}
  CHECK(gt_session_catalog(public_session,rows,64,&count)==GT_OK && count>=1);bool found=false;for(size_t i=0;i<count;i++)if(rows[i].session==active.session){CHECK(!rows[i].has_result && !rows[i].terminating);found=true;}CHECK(found);
  CHECK(gt_session_terminate(public_session,active.session)==GT_SESSION_SUBMIT_QUEUED && public_info().stop.state==GT_SESSION_STOP_PENDING);
  end=now()+10000;while(public_info().stop.state==GT_SESSION_STOP_PENDING){CHECK(now()<end);public_pump();}
  CHECK(public_info().stop.state==GT_SESSION_STOP_KNOWN && public_info().stop.status==GT_SESSION_CONTROL_OK && public_info().state==GT_SESSION_STATE_CATALOG);
  found=false;while(!found){CHECK(now()<end);CHECK(gt_session_catalog_refresh(public_session)==GT_SESSION_SUBMIT_QUEUED);CHECK(gt_session_catalog(public_session,rows,64,&count)==GT_ERR_NOT_FOUND);while(public_info().state!=GT_SESSION_STATE_CATALOG){CHECK(now()<end);public_pump();}CHECK(gt_session_catalog(public_session,rows,64,&count)==GT_OK);for(size_t i=0;i<count;i++)if(rows[i].session==active.session && rows[i].has_result){CHECK(rows[i].terminating && (rows[i].has_exit_code || !rows[i].exit_code));found=true;}}
  public_finish();stop_server(SIGTERM);puts("PASS public session ABI: readonly barrier, resize/color, original PID, pending nonce lookup, engine-free catalog and explicit termination");
}

int main(int argc,char **argv) {
  char *ssh_args[6]={argv[0],getenv("GT_TEST_PUBLIC_SESSION")?"public-proxy":getenv("GT_TEST_RECONNECT")?"auto-proxy":"proxy-flow",NULL,NULL,NULL,NULL};
  if(argc==6 && getenv("GT_TEST_SESSIOND")) {
    CHECK(!setenv("GT_TEST_SSH_HOST",argv[1],1) && !setenv("GT_TEST_SSH_PORT",argv[2],1) && !setenv("GT_TEST_SSH_USER",argv[3],1) && !setenv("GT_TEST_SSH_KEY",argv[4],1));
    ssh_args[2]=getenv("GT_TEST_SESSIOND");ssh_args[3]=getenv("GT_TEST_PROJECT");ssh_args[4]=getenv("GT_TEST_PROXY");argv=ssh_args;argc=5;
  }
  CHECK(argc>=4);self=argv[0];daemon_binary=argv[2];root=argv[3];
  if(!strcmp(argv[1],"crash")){CHECK(argc==6 || argc==7);if(argc==7)proxy_binary=argv[6];CHECK(snprintf(directory,sizeof(directory),"%s",argv[4])>0);return crash_client(strtoull(argv[5],NULL,10));}
  CHECK(argc==4 || (argc==5 && (!strcmp(argv[1],"proxy-flow") || !strcmp(argv[1],"auto-proxy") || !strcmp(argv[1],"public-proxy"))));if(argc==5)proxy_binary=argv[4];CHECK(!atexit(cleanup));if(!strcmp(argv[1],"public-proxy"))public_flow();else if(!strcmp(argv[1],"auto") || !strcmp(argv[1],"auto-proxy"))automatic_reconnect();else if(!strcmp(argv[1],"flow") || !strcmp(argv[1],"proxy-flow"))flow();else if(!strcmp(argv[1],"competing"))competing();else if(!strcmp(argv[1],"malformed"))malformed();else if(!strcmp(argv[1],"allocation"))allocation_test();else if(!strcmp(argv[1],"retention"))retention();else if(!strcmp(argv[1],"capacity"))capacity();else if(!strcmp(argv[1],"sessions"))sessions();else {CHECK(!strcmp(argv[1],"shutdown"));shutdown_test();}
  cleanup();printf("PASS host process %s\n",argv[1]);return 0;
}
