#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "activation.h"
#include "startup-wire.h"
#include "ipc/unix-endpoint.h"
#include "ipc/socket-stream.h"
#include "ipc/client-bootstrap.h"
#include "ipc/control-client.h"
#include "session-contract.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
extern char **environ;
static const char *self,*server,*proxy,*root;static char scratch[1024],directory[1024];
static pid_t service=-1,proxy_child=-1;static bool owned_service,pipe_stdio,restrict_io;static int pipe_output=-1,diagnostic_fd=-1;
static bool clock_ms(void *ctx,uint64_t *out){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static uint64_t now(void){uint64_t t;CHECK(clock_ms(NULL,&t));return t;}
static void pause_one(void){(void)poll(NULL,0,1);}
static int wait_child(pid_t pid){uint64_t end=now()+10000;int status=0;while(waitpid(pid,&status,WNOHANG)!=pid){CHECK(now()<end);pause_one();}return status;}
static void cleanup(void) {
  if(service<0 && directory[0]) {
    /* Only a live kernel-authenticated peer in our fresh private test directory.
     * Never infer liveness from the fixture's metadata report. */
    gt_unix_endpoint_t *e=NULL;int fd=-1;
    if(gt__unix_endpoint_open(directory,false,NULL,&e)==GT_ENDPOINT_OK) {
      if(gt__unix_endpoint_connect(e,0,&fd)==GT_ENDPOINT_OK) {
#ifdef __APPLE__
        pid_t pid=-1;socklen_t n=sizeof(pid);if(!getsockopt(fd,SOL_LOCAL,LOCAL_PEERPID,&pid,&n))service=pid;
#else
        struct ucred cred;socklen_t n=sizeof(cred);if(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&n))service=cred.pid;
#endif
        close(fd);
      }
      (void)gt__unix_endpoint_close(e);
    }
  }
  if(service==getpid())service=-1;
  if(proxy_child>0){kill(proxy_child,SIGTERM);(void)wait_child(proxy_child);proxy_child=-1;}
  if(service>0){kill(service,SIGTERM);if(owned_service)(void)wait_child(service);service=-1;}
}
static void path(char *out,size_t n,const char *leaf){CHECK(snprintf(out,n,"%s/%s",directory,leaf)>0);}
static gt_unix_endpoint_t *endpoint(bool create){gt_unix_endpoint_t *e=NULL;CHECK(gt__unix_endpoint_open(directory,create,NULL,&e)==GT_ENDPOINT_OK);return e;}
static void init(void){CHECK(snprintf(scratch,sizeof(scratch),"%s/tmp/act-XXXXXX",root)>0 && mkdtemp(scratch));CHECK(snprintf(directory,sizeof(directory),"%s/run",scratch)>0);CHECK(!setenv("GT_TEST_ACTUAL_DAEMON",server,1) && !setenv("GT_TEST_ACTIVATION_MODE","normal",1));}
static gt_activation_config_t config(void){gt_activation_config_t c={.directory=directory,.helper=self,.hello={.role=GT_ROLE_MANAGEMENT},.timeout_ms=5000};memcpy(c.hello.compatibility,gt_session_contract,32);return c;}
/* Fresh test executable verifies the spawn boundary then execs the real host.
 * This ownership report is test metadata, never a production liveness source. */
static int helper(char **argv) {
  CHECK(fcntl(200,F_GETFD)<0 && errno==EBADF && getsid(0)==getpid() && getpgrp()==getpid());
  sigset_t blocked;CHECK(!sigprocmask(SIG_SETMASK,NULL,&blocked) && !sigismember(&blocked,SIGTERM));
  struct sigaction a;CHECK(!sigaction(SIGPIPE,NULL,&a) && a.sa_handler==SIG_DFL);
  int null=open("/dev/null",O_RDONLY);CHECK(null>=0);struct stat expected;CHECK(!fstat(null,&expected));
  for(int i=0;i<3;i++){struct stat st;CHECK(!fstat(i,&st) && S_ISCHR(st.st_mode) && st.st_rdev==expected.st_rdev);}close(null);
  char name[1200];CHECK(snprintf(name,sizeof(name),"%s/helper",argv[3])>0);int fd=open(name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0);
  char report[80];int n=snprintf(report,sizeof(report),"%ld %ld\n",(long)getpid(),(long)getppid());CHECK(n>0 && write(fd,report,(size_t)n)==n);close(fd);
  if(!strcmp(getenv("GT_TEST_ACTIVATION_MODE"),"stall")){a.sa_handler=SIG_IGN;sigemptyset(&a.sa_mask);CHECK(!sigaction(SIGTERM,&a,NULL));for(;;)pause();}
  const char *binary=getenv("GT_TEST_ACTUAL_DAEMON");CHECK(binary && binary[0]=='/');char *args[]={(char *)binary,"serve-detached","--runtime-dir",argv[3],NULL};execv(binary,args);return 127;
}
typedef struct {gt_socket_stream_t socket;gt_connection_t *raw;gt_control_client_t *control;gt_bootstrap_result_t identity;gt_control_result_t reply;unsigned results;} peer;
static void reply(void *ctx,const gt_control_result_t *r){peer *p=ctx;p->reply=*r;p->results++;}
static void connect_peer(peer *p,int fd,const gt_protocol_hello_t *hello,bool rejected) {
  memset(p,0,sizeof(*p));gt_stream_ops_t ops;CHECK(gt__socket_stream_adopt(fd,&p->socket,&ops)==GT_OK && gt__connection_open(&ops,(gt_connection_limits_t){65560,65560,5000},NULL,&p->raw)==GT_CONNECTION_CREATED);
  gt_client_bootstrap_config_t c={.hello=*hello,.timeout_ms=5000,.clock=clock_ms};gt_client_bootstrap_t *b=NULL;CHECK(gt__client_bootstrap_open(p->raw,&c,NULL,&b)==GT_OK);p->raw=NULL;uint64_t end=now()+10000;
  for(;;){gt_bootstrap_state_t state=gt__client_bootstrap_step(b);CHECK(now()<end);if(rejected){if(state==GT_BOOTSTRAP_FAILED){gt_bootstrap_info_t info;CHECK(gt__client_bootstrap_info(b,&info) && info.has_status && info.status==GT_HELLO_INCOMPATIBLE);break;}}else {CHECK(state!=GT_BOOTSTRAP_FAILED);if(state==GT_BOOTSTRAP_READY)break;}pause_one();}
  if(!rejected){CHECK(gt__client_bootstrap_take(b,&p->raw,&p->identity));gt_control_client_config_t control={.heartbeat_ms=1000,.request_timeout_ms=5000,.context=p,.clock=clock_ms,.result=reply};memcpy(control.instance,p->identity.identity.instance,16);CHECK(gt__control_client_open(p->raw,&control,NULL,&p->control)==GT_OK);p->raw=NULL;}
  gt__client_bootstrap_close(b);
}
static void close_peer(peer *p){gt__control_client_close(p->control);gt__connection_destroy(p->raw);memset(p,0,sizeof(*p));}
static gt_control_result_t request(peer *p,gt_control_request_t req){unsigned before=p->results;uint64_t id;gt_status_t error;CHECK(gt__control_client_submit(p->control,&req,&id,&error)==GT_CONTROL_CLIENT_QUEUED && error==GT_OK);uint64_t end=now()+10000;while(p->results==before){CHECK(now()<end && gt__control_client_step(p->control)!=GT_CONTROL_CLIENT_FAILED);pause_one();}CHECK(p->reply.delivery==GT_CONTROL_CLIENT_REPLY && p->reply.request_id==id);return p->reply;}
static void start_service(void){gt_activation_config_t c=config();gt_activation_result_t result;CHECK(gt__activation_connect(&c,&result)==GT_ACTIVATION_OK && result.fd>=0 && result.child>0 && result.committed);service=result.child;owned_service=true;peer p;connect_peer(&p,result.fd,&c.hello,false);gt_control_result_t r=request(&p,(gt_control_request_t){.type=GT_MSG_LIST});CHECK(r.status==GT_CONTROL_OK && !r.count);close_peer(&p);}
static void stop_service(void) {
  CHECK(service>0 && !kill(service,SIGTERM));if(owned_service){int status=wait_child(service);service=-1;CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);}service=-1;
  gt_unix_endpoint_t *e=endpoint(false);uint64_t end=now()+5000;int fd=-1;while(true){gt_endpoint_rc_t rc=gt__unix_endpoint_connect(e,0,&fd);if(rc==GT_ENDPOINT_NOT_FOUND)break;if(fd>=0)close(fd);CHECK(now()<end);pause_one();}CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
}
static void launch(void) {
  char name[1200];CHECK(snprintf(name,sizeof(name),"%s/sentinel",scratch)>0);int fd=open(name,O_RDWR|O_CREAT,0600);CHECK(fd>=0 && dup2(fd,200)==200);close(fd);CHECK(!fcntl(200,F_SETFD,0));
  sigset_t blocked,saved;sigemptyset(&blocked);sigaddset(&blocked,SIGTERM);CHECK(!sigprocmask(SIG_BLOCK,&blocked,&saved));start_service();CHECK(!sigprocmask(SIG_SETMASK,&saved,NULL));close(200);CHECK(getsid(service)==service && getsid(0)!=service);
  gt_activation_config_t c=config();c.helper="/this/path/is/not/searched";gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_OK && r.child==-1 && !r.committed);peer p;connect_peer(&p,r.fd,&c.hello,false);close_peer(&p);stop_service();
}
static void absent(const char *mode){gt_activation_config_t c=config();if(!strcmp(mode,"data"))c.hello.role=GT_ROLE_DATA;c.hello.expected_instance[0]=9;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_NOT_FOUND && r.fd==-1 && r.child==-1 && !r.committed);char name[1200];path(name,sizeof(name),"helper");CHECK(access(name,F_OK)<0 && errno==ENOENT);}
static bool always_cancel(void *ctx){(void)ctx;return true;}
static void cancellation(void){gt_activation_config_t c=config();c.cancelled=always_cancel;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_CANCELLED && r.child==-1 && r.fd==-1);}
static void locks(void){gt_unix_endpoint_t *e=endpoint(true);CHECK(gt__unix_endpoint_startup_lock(e)==GT_ENDPOINT_OK);gt_activation_config_t c=config();c.timeout_ms=25;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_TIMEOUT && r.child==-1 && r.fd==-1);CHECK(gt__unix_endpoint_startup_unlock(e)==GT_ENDPOINT_OK && gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);start_service();stop_service();}
static void refused(const char *mode){gt_activation_config_t c=config();gt_activation_rc_t expected=GT_ACTIVATION_INCOMPATIBLE;if(!strcmp(mode,"features")){c.hello.required_features=1;expected=GT_ACTIVATION_FEATURES;}else c.hello.compatibility[0]^=1;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==expected && r.fd==-1 && r.child==-1 && !r.committed);char name[1200];path(name,sizeof(name),"helper");CHECK(!unlink(name));start_service();stop_service();}
static void timeout_test(void){CHECK(!setenv("GT_TEST_ACTIVATION_MODE","stall",1));gt_activation_config_t c=config();c.timeout_ms=100;uint64_t begin=now();gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_TIMEOUT && r.fd==-1 && r.child==-1 && !r.committed && now()-begin<2500);}
static void existing(void){start_service();gt_activation_config_t c=config();c.helper="/missing/helper";c.hello.compatibility[0]^=1;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_OK && r.child==-1);peer p;connect_peer(&p,r.fd,&c.hello,true);CHECK(!kill(service,0));stop_service();}
static void trusted(void){char file[1200];CHECK(snprintf(file,sizeof(file),"%s/untrusted",scratch)>0 && !symlink(self,file));gt_activation_config_t c=config();c.helper=file;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_AUTH && r.child==-1);CHECK(!unlink(file));int fd=open(file,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);close(fd);CHECK(!chmod(file,0777));CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_AUTH && r.child==-1);CHECK(!unlink(file));}
static int spawn_proxy(gt_protocol_role_t role) {
  int fds[2],input[2]={-1,-1},output[2]={-1,-1};posix_spawn_file_actions_t a;CHECK(!posix_spawn_file_actions_init(&a));
  if(pipe_stdio){CHECK(!pipe(input) && !pipe(output));CHECK(!posix_spawn_file_actions_addclose(&a,input[1]) && !posix_spawn_file_actions_addclose(&a,output[0]) && !posix_spawn_file_actions_adddup2(&a,input[0],0) && !posix_spawn_file_actions_adddup2(&a,output[1],1) && !posix_spawn_file_actions_addclose(&a,input[0]) && !posix_spawn_file_actions_addclose(&a,output[1]));fds[0]=input[1];fds[1]=-1;}
  else {CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));if(restrict_io){int small=4096;CHECK(!setsockopt(fds[1],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));}CHECK(!posix_spawn_file_actions_addclose(&a,fds[0]) && !posix_spawn_file_actions_adddup2(&a,fds[1],0) && !posix_spawn_file_actions_adddup2(&a,fds[1],1));if(fds[1]>1)CHECK(!posix_spawn_file_actions_addclose(&a,fds[1]));}
  char err[1200];CHECK(snprintf(err,sizeof(err),"%s/proxy.stderr",scratch)>0);if(diagnostic_fd>=0)CHECK(!posix_spawn_file_actions_adddup2(&a,diagnostic_fd,2));else CHECK(!posix_spawn_file_actions_addopen(&a,2,err,O_WRONLY|O_CREAT|O_APPEND,0600));char *args[]={(char *)proxy,"--runtime-dir",directory,"--role",role==GT_ROLE_DATA?"data":"management","--daemon",(char *)self,NULL};CHECK(!posix_spawn(&proxy_child,proxy,&a,NULL,args,environ));posix_spawn_file_actions_destroy(&a);if(pipe_stdio){close(input[0]);close(output[1]);pipe_output=output[0];}else close(fds[1]);return fds[0];
}
static void stdio_test(void) {
  gt_activation_config_t c=config();int fd=spawn_proxy(GT_ROLE_MANAGEMENT);peer p;connect_peer(&p,fd,&c.hello,false);uint8_t instance[16];memcpy(instance,p.identity.identity.instance,16);
  char trace[1200];path(trace,sizeof(trace),"helper");FILE *f=fopen(trace,"r");long child,parent;CHECK(f && fscanf(f,"%ld %ld",&child,&parent)==2 && child>1 && parent==proxy_child);fclose(f);service=(pid_t)child;owned_service=false;
  gt_create_spec_t spec={.nonce={1},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,.executable="/bin/sh",.cwd=root,.term="xterm",.argc=2,.argv={"-c","sleep 30"}};gt_control_result_t r=request(&p,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});CHECK(r.creation.status==GT_CREATION_ACCEPTED);uint64_t session=r.creation.session;
  CHECK(!kill(proxy_child,SIGKILL));int status=wait_child(proxy_child);proxy_child=-1;CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);close_peer(&p);
  fd=spawn_proxy(GT_ROLE_MANAGEMENT);connect_peer(&p,fd,&c.hello,false);CHECK(!memcmp(instance,p.identity.identity.instance,16));r=request(&p,(gt_control_request_t){.type=GT_MSG_INFO,.session=session});CHECK(r.status==GT_CONTROL_OK && !r.rows[0].has_result);
  close_peer(&p);status=wait_child(proxy_child);proxy_child=-1;CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);stop_service();
}
static void malformed(const char *mode){bool data=!strcmp(mode,"role-data");int fd=spawn_proxy(data?GT_ROLE_DATA:GT_ROLE_MANAGEMENT);uint8_t bytes[84]={0};gt_activation_config_t c=config();if(!strcmp(mode,"role")){c.hello.role=GT_ROLE_DATA;c.hello.expected_instance[0]=1;}gt_wire_header_t h={.type=GT_MSG_HELLO,.payload_length=60,.request_id=data?0:1};CHECK(gt__wire_header_encode(&h,bytes) && gt__hello_encode(&c.hello,bytes+24));if(!strcmp(mode,"malformed"))bytes[0]='x';CHECK(write(fd,bytes,sizeof(bytes))==(ssize_t)sizeof(bytes));int status=wait_child(proxy_child);proxy_child=-1;close(fd);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==64+GT_ACTIVATION_INVALID);char trace[1200];path(trace,sizeof(trace),"helper");CHECK(access(trace,F_OK)<0 && errno==ENOENT);}
static void concurrent(void) {
  gt_unix_endpoint_t *e=endpoint(true);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
  int pipes[4][2];pid_t clients[4];for(unsigned i=0;i<4;i++)CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pipes[i]));
  for(unsigned i=0;i<4;i++) {
    clients[i]=fork();CHECK(clients[i]>=0);
    if(!clients[i]) {
      for(unsigned j=0;j<4;j++){close(pipes[j][0]);if(j!=i)close(pipes[j][1]);}
      gt_activation_config_t c=config();gt_activation_result_t r;gt_activation_rc_t rc=gt__activation_connect(&c,&r);if(r.child>0){service=r.child;owned_service=true;}CHECK(rc==GT_ACTIVATION_OK);
      peer p;connect_peer(&p,r.fd,&c.hello,false);uint8_t report[24]={0};memcpy(report,p.identity.identity.instance,16);report[16]=r.child>0;close_peer(&p);
      CHECK(write(pipes[i][1],report,sizeof(report))==(ssize_t)sizeof(report));char command;CHECK(read(pipes[i][1],&command,1)==1);close(pipes[i][1]);if(service>0)stop_service();_exit(0);
    }
  }
  unsigned owners=0;uint8_t instance[16]={0};
  for(unsigned i=0;i<4;i++){close(pipes[i][1]);struct pollfd p={.fd=pipes[i][0],.events=POLLIN};CHECK(poll(&p,1,10000)>0);uint8_t report[24];CHECK(read(pipes[i][0],report,24)==24);if(!i)memcpy(instance,report,16);CHECK(!memcmp(instance,report,16));owners+=report[16];}
  CHECK(owners==1);for(unsigned i=0;i<4;i++){CHECK(write(pipes[i][0],"x",1)==1);close(pipes[i][0]);}
  for(unsigned i=0;i<4;i++){int status=wait_child(clients[i]);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);}
}
static void codec(void) {
  gt_protocol_identity_t id={.instance={1},.compatibility={2},.features=7},out;uint8_t bytes[72],ack[24];gt_status_t status;
  CHECK(gt__startup_ready_encode(GT_OK,&id,bytes) && gt__startup_ready_decode(bytes,&status,&out) && status==GT_OK && out.features==7 && out.instance[0]==1 && out.compatibility[0]==2);
  const unsigned positions[]={0,4,6,8,12,16,48};
  for(size_t i=0;i<sizeof(positions)/sizeof(positions[0]);i++){uint8_t invalid[72];memcpy(invalid,bytes,72);if(positions[i]==16 || positions[i]==48)memset(invalid+positions[i],0,positions[i]==16?32:16);else invalid[positions[i]]^=0x80;CHECK(!gt__startup_ready_decode(invalid,&status,&out) && status==GT_ERR_PROTOCOL && !out.instance[0]);}
  CHECK(gt__startup_ready_encode(GT_ERR_UNSUPPORTED,&id,bytes) && gt__startup_ready_decode(bytes,&status,&out) && status==GT_ERR_UNSUPPORTED && !out.instance[0]);bytes[16]=1;CHECK(!gt__startup_ready_decode(bytes,&status,&out));
  gt__startup_ack_encode(id.instance,ack);CHECK(gt__startup_ack_valid(ack,id.instance) && !gt__startup_commit_valid(ack,id.instance));ack[8]^=1;CHECK(!gt__startup_ack_valid(ack,id.instance));
  gt__startup_commit_encode(id.instance,ack);CHECK(gt__startup_commit_valid(ack,id.instance) && !gt__startup_ack_valid(ack,id.instance));ack[8]^=1;CHECK(!gt__startup_commit_valid(ack,id.instance));
}
static bool before_ack(void *context) {
  gt_activation_result_t *owner=context;gt_unix_endpoint_t *e=NULL;int fd=-1;
  if(gt__unix_endpoint_open(directory,false,NULL,&e)!=GT_ENDPOINT_OK)return false;
  gt_endpoint_rc_t rc=gt__unix_endpoint_connect(e,100,&fd);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);if(rc!=GT_ENDPOINT_OK)return false;
  service=owner->child;owned_service=true;gt_activation_config_t c=config();uint8_t bytes[84];gt_wire_header_t h={.type=GT_MSG_HELLO,.request_id=1,.payload_length=60};CHECK(gt__wire_header_encode(&h,bytes) && gt__hello_encode(&c.hello,bytes+24));CHECK(write(fd,bytes,84)==84);
  struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,100)==0);close(fd);return true;
}
static void barrier(void) {
  gt_activation_result_t r;gt_activation_config_t c=config();c.context=&r;c.cancelled=before_ack;gt_activation_rc_t rc=gt__activation_connect(&c,&r);service=r.child;
  CHECK(rc==GT_ACTIVATION_CANCELLED && !r.committed && r.child==-1 && r.fd==-1);char trace[1200];path(trace,sizeof(trace),"helper");CHECK(!unlink(trace));start_service();stop_service();
}
static uint8_t pattern(size_t i,const uint8_t hello[84]){return i<84?hello[i]:(uint8_t)((i-84)*31+17);}
static void nonblocking(int fd){int flags=fcntl(fd,F_GETFL);CHECK(flags>=0 && !fcntl(fd,F_SETFL,flags|O_NONBLOCK));}
typedef struct {int reader,writer;} pipe_pair;
static gt_stream_result_t pipe_read(void *context,void *bytes,size_t n){pipe_pair *p=context;ssize_t got=read(p->reader,bytes,n);return got>0?(gt_stream_result_t){GT_STREAM_DATA,(size_t)got,0}:!got?(gt_stream_result_t){GT_STREAM_END,0,0}:(gt_stream_result_t){errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR?GT_STREAM_WAIT:GT_STREAM_ERROR,0,errno};}
static gt_stream_result_t pipe_write(void *context,const void *bytes,size_t n){pipe_pair *p=context;ssize_t sent=write(p->writer,bytes,n);return sent>0?(gt_stream_result_t){GT_STREAM_DATA,(size_t)sent,0}:(gt_stream_result_t){errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR?GT_STREAM_WAIT:GT_STREAM_ERROR,0,errno};}
static void pipe_close(void *context){pipe_pair *p=context;close(p->reader);close(p->writer);}
static void bridge(const char *mode) {
  restrict_io=true;
  pipe_stdio=!strcmp(mode,"pipes");if(pipe_stdio){struct sigaction a={0};a.sa_handler=SIG_IGN;sigemptyset(&a.sa_mask);CHECK(!sigaction(SIGPIPE,&a,NULL));}
  gt_unix_endpoint_t *e=endpoint(true);CHECK(gt__unix_endpoint_listen(e,8)==GT_ENDPOINT_OK);int client=spawn_proxy(GT_ROLE_MANAGEMENT);nonblocking(client);gt_socket_stream_t client_stream,remote_stream;gt_stream_ops_t client_ops,remote_ops;pipe_pair pair={pipe_output,client};int small=1024;
  if(pipe_stdio){nonblocking(pipe_output);client_ops=(gt_stream_ops_t){&pair,pipe_read,pipe_write,clock_ms,pipe_close};}
  else {CHECK(gt__socket_stream_adopt(client,&client_stream,&client_ops)==GT_OK && !setsockopt(client,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));}
  gt_activation_config_t c=config();uint8_t hello[84];gt_wire_header_t h={.type=GT_MSG_HELLO,.request_id=1,.payload_length=60};CHECK(gt__wire_header_encode(&h,hello) && gt__hello_encode(&c.hello,hello+24));
  size_t written=0,received=0,total=4*1024*1024+84,queued=0,offset=0;uint8_t echo[4096],bytes[4096];int remote=-1;bool stalled=false;uint64_t start=now(),blocked=0,end=start+15000;
  while(received<total) {
    if(now()>=end){fprintf(stderr,"bridge progress written=%zu received=%zu queued=%zu remote=%d\n",written,received,queued,remote);CHECK(false);}
    if(remote<0){gt_endpoint_rc_t rc=gt__unix_endpoint_accept(e,&remote);CHECK(rc==GT_ENDPOINT_OK || rc==GT_ENDPOINT_WAIT || rc==GT_ENDPOINT_REJECTED);if(remote>=0){CHECK(!setsockopt(remote,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));CHECK(gt__socket_stream_adopt(remote,&remote_stream,&remote_ops)==GT_OK);}}
    bool progress=false;
    if(written<total) {
      size_t n=total-written;if(n>sizeof(bytes))n=sizeof(bytes);for(size_t i=0;i<n;i++)bytes[i]=pattern(written+i,hello);gt_stream_result_t w=client_ops.write(client_ops.context,bytes,n);
      if(w.kind==GT_STREAM_DATA){written+=w.length;blocked=0;progress=true;}else {CHECK(w.kind==GT_STREAM_WAIT);stalled=true;if(!blocked)blocked=now();}
    }
    if(!strcmp(mode,"disconnect")) {
      if(remote>=0 && blocked && now()-blocked>=100)break;
    } else {
      if(remote>=0 && !queued){gt_stream_result_t v=remote_ops.read(remote_ops.context,echo,4093);if(v.kind==GT_STREAM_DATA){queued=v.length;offset=0;progress=true;}else CHECK(v.kind==GT_STREAM_WAIT);}
      if(queued){size_t n=queued-offset;if(n>2039)n=2039;gt_stream_result_t v=remote_ops.write(remote_ops.context,echo+offset,n);if(v.kind==GT_STREAM_DATA){offset+=v.length;if(offset==queued)queued=0;progress=true;}else CHECK(v.kind==GT_STREAM_WAIT);}
      if(now()-start>=1000){gt_stream_result_t v=client_ops.read(client_ops.context,bytes,2047);if(v.kind==GT_STREAM_DATA){for(size_t i=0;i<v.length;i++)CHECK(bytes[i]==pattern(received+i,hello));received+=v.length;progress=true;}else CHECK(v.kind==GT_STREAM_WAIT);}
    }
    if(!progress)(void)poll(NULL,0,1);
  }
  CHECK(stalled && remote>=0);client_ops.close(client_ops.context);
  if(!strcmp(mode,"disconnect")){uint64_t expiry=now()+3000;for(;;){struct pollfd p={.fd=remote,.events=POLLIN};CHECK(poll(&p,1,1)>=0);if(p.revents&POLLHUP)break;CHECK(now()<expiry);}CHECK(remote_ops.read(remote_ops.context,bytes,1).kind==GT_STREAM_END);remote_ops.close(remote_ops.context);}
  else {CHECK(received==total && written==total);remote_ops.close(remote_ops.context);}
  int status=wait_child(proxy_child);proxy_child=-1;CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);
}
static bool recheck_owner(void *context) {
  bool *started=context;if(*started)return false;gt_unix_endpoint_t *e=endpoint(false);gt_endpoint_rc_t rc=gt__unix_endpoint_startup_lock(e);
  if(rc==GT_ENDPOINT_OK){CHECK(gt__unix_endpoint_startup_unlock(e)==GT_ENDPOINT_OK && gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);return false;}
  CHECK(rc==GT_ENDPOINT_BUSY && gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);*started=true;posix_spawn_file_actions_t a;CHECK(!posix_spawn_file_actions_init(&a));for(int i=0;i<3;i++)CHECK(!posix_spawn_file_actions_addopen(&a,i,"/dev/null",i?O_WRONLY:O_RDONLY,0));
  char *argv[]={(char *)server,"serve","--runtime-dir",directory,NULL};CHECK(!posix_spawn(&service,server,&a,NULL,argv,environ));owned_service=true;posix_spawn_file_actions_destroy(&a);e=endpoint(false);uint64_t end=now()+5000;int fd=-1;
  while(gt__unix_endpoint_connect(e,0,&fd)!=GT_ENDPOINT_OK){CHECK(now()<end && waitpid(service,NULL,WNOHANG)==0);pause_one();}close(fd);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);return false;
}
static void recheck(void) {
  bool started=false;gt_activation_config_t c=config();c.context=&started;c.cancelled=recheck_owner;gt_activation_result_t r;CHECK(gt__activation_connect(&c,&r)==GT_ACTIVATION_OK && started && r.child==-1 && !r.committed);peer p;connect_peer(&p,r.fd,&c.hello,false);close_peer(&p);stop_service();
}
typedef struct {gt_activation_result_t *owner;peer p;gt_client_bootstrap_t *boot;uint64_t session;} late_cancel;
static bool after_ack(void *context) {
  late_cancel *c=context;if(c->owner->child>0){service=c->owner->child;owned_service=true;}
  if(!c->boot && !c->p.control) {
    gt_unix_endpoint_t *e=NULL;int fd=-1;if(gt__unix_endpoint_open(directory,false,NULL,&e)!=GT_ENDPOINT_OK)return false;
    gt_endpoint_rc_t rc=gt__unix_endpoint_connect(e,100,&fd);CHECK(gt__unix_endpoint_close(e)==GT_ENDPOINT_OK);if(rc!=GT_ENDPOINT_OK)return false;
    gt_stream_ops_t ops;CHECK(gt__socket_stream_adopt(fd,&c->p.socket,&ops)==GT_OK && gt__connection_open(&ops,(gt_connection_limits_t){65560,65560,5000},NULL,&c->p.raw)==GT_CONNECTION_CREATED);
    gt_activation_config_t cfg=config();gt_client_bootstrap_config_t boot={.hello=cfg.hello,.timeout_ms=5000,.clock=clock_ms};CHECK(gt__client_bootstrap_open(c->p.raw,&boot,NULL,&c->boot)==GT_OK);c->p.raw=NULL;
  }
  if(c->boot) {
    uint64_t end=now()+100;for(;;){gt_bootstrap_state_t state=gt__client_bootstrap_step(c->boot);CHECK(state!=GT_BOOTSTRAP_FAILED);if(state==GT_BOOTSTRAP_READY)break;if(now()>=end)return false;pause_one();}
    CHECK(gt__client_bootstrap_take(c->boot,&c->p.raw,&c->p.identity));gt__client_bootstrap_close(c->boot);c->boot=NULL;
    gt_control_client_config_t cfg={.heartbeat_ms=1000,.request_timeout_ms=5000,.context=&c->p,.clock=clock_ms,.result=reply};memcpy(cfg.instance,c->p.identity.identity.instance,16);CHECK(gt__control_client_open(c->p.raw,&cfg,NULL,&c->p.control)==GT_OK);c->p.raw=NULL;
  }
  gt_create_spec_t spec={.nonce={1},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,.executable="/bin/sh",.cwd=root,.term="xterm",.argc=2,.argv={"-c","sleep 30"}};
  gt_control_result_t r=request(&c->p,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});CHECK(r.creation.status==GT_CREATION_ACCEPTED);c->session=r.creation.session;return true;
}
static void committed_cancel(void) {
  gt_activation_result_t r;late_cancel context={.owner=&r};gt_activation_config_t c=config();c.context=&context;c.cancelled=after_ack;gt_activation_rc_t rc=gt__activation_connect(&c,&r);service=r.child;
  CHECK(rc==GT_ACTIVATION_CANCELLED && r.fd==-1 && r.child>0 && r.committed && context.session);gt_control_result_t info=request(&context.p,(gt_control_request_t){.type=GT_MSG_INFO,.session=context.session});CHECK(info.status==GT_CONTROL_OK && !info.rows[0].has_result);close_peer(&context.p);stop_service();
}
static void empty_input(void){int fd=spawn_proxy(GT_ROLE_MANAGEMENT);close(fd);int status=wait_child(proxy_child);proxy_child=-1;CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);char name[1200];path(name,sizeof(name),"helper");CHECK(access(name,F_OK)<0 && errno==ENOENT);}
static void diagnostic_pressure(void) {
  int fds[2];CHECK(!pipe(fds) && !fcntl(fds[0],F_SETFD,FD_CLOEXEC) && !fcntl(fds[1],F_SETFD,FD_CLOEXEC));int flags=fcntl(fds[1],F_GETFL);nonblocking(fds[1]);uint8_t fill[4096]={0};while(write(fds[1],fill,sizeof(fill))>0){}CHECK(errno==EAGAIN || errno==EWOULDBLOCK);CHECK(!fcntl(fds[1],F_SETFL,flags));diagnostic_fd=fds[1];
  int fd=spawn_proxy(GT_ROLE_MANAGEMENT);uint8_t invalid[24]={0};CHECK(write(fd,invalid,24)==24);uint64_t end=now()+2000;int status=0;bool exited=false;
  while(now()<end){if(waitpid(proxy_child,&status,WNOHANG)==proxy_child){exited=true;break;}pause_one();}
  close(fds[0]);close(fds[1]);diagnostic_fd=-1;if(!exited)status=wait_child(proxy_child);proxy_child=-1;close(fd);CHECK(exited && WIFEXITED(status) && WEXITSTATUS(status)==64+GT_ACTIVATION_INVALID);
}
static void unsafe_entry(void) {
  posix_spawn_file_actions_t a;CHECK(!posix_spawn_file_actions_init(&a));for(int i=0;i<4;i++)CHECK(!posix_spawn_file_actions_addopen(&a,i,"/dev/null",i?O_RDWR:O_RDONLY,0));
  char *argv[]={(char *)server,"serve-detached","--runtime-dir",directory,NULL};CHECK(!posix_spawn(&proxy_child,server,&a,NULL,argv,environ));posix_spawn_file_actions_destroy(&a);int status=wait_child(proxy_child);proxy_child=-1;CHECK(WIFEXITED(status) && WEXITSTATUS(status)==2);
}
static void publish_command(const char *source,const char *destination,int expected) {
  int pipefd[2];CHECK(!pipe(pipefd));posix_spawn_file_actions_t a;CHECK(!posix_spawn_file_actions_init(&a));
  CHECK(!posix_spawn_file_actions_adddup2(&a,pipefd[1],1) && !posix_spawn_file_actions_addclose(&a,pipefd[0]));
  if(pipefd[1]!=1)CHECK(!posix_spawn_file_actions_addclose(&a,pipefd[1]));
  char *args[]={(char *)proxy,"publish","--from",(char *)source,"--to",(char *)destination,NULL};
  CHECK(!posix_spawn(&proxy_child,proxy,&a,NULL,args,environ));posix_spawn_file_actions_destroy(&a);close(pipefd[1]);
  char bytes[64]={0};ssize_t n=read(pipefd[0],bytes,sizeof(bytes)-1);CHECK(n>=0);close(pipefd[0]);int status=wait_child(proxy_child);proxy_child=-1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status)==expected);
  CHECK(expected?n==0:!strcmp(bytes,"GT_PUBLISH_OK\n"));
}
static void publication(void) {
  CHECK(!mkdir(directory,0700));char releases[1200],stage[1200],final[1200],receipt[1400],empty[1200],alias[1200],alias_source[1400],alias_final[1400];
  path(releases,sizeof(releases),"releases");path(stage,sizeof(stage),".stage-000000000000000000000000");path(final,sizeof(final),"releases/version-one");path(empty,sizeof(empty),"releases/empty-existing");path(alias,sizeof(alias),"alias");
  CHECK(!mkdir(releases,0700) && !mkdir(stage,0700));CHECK(snprintf(receipt,sizeof(receipt),"%s/receipt",stage)>0);int fd=open(receipt,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0 && write(fd,"x",1)==1);close(fd);
  publish_command(stage,final,0);struct stat st;CHECK(lstat(stage,&st)<0 && errno==ENOENT);CHECK(snprintf(receipt,sizeof(receipt),"%s/receipt",final)>0);fd=open(receipt,O_RDONLY);char value;CHECK(fd>=0 && read(fd,&value,1)==1 && value=='x');close(fd);
  CHECK(!mkdir(stage,0700) && !mkdir(empty,0700));publish_command(stage,empty,1);CHECK(!lstat(stage,&st) && !lstat(empty,&st));
  CHECK(!symlink(directory,alias));CHECK(snprintf(alias_source,sizeof(alias_source),"%s/.stage-000000000000000000000000",alias)>0 && snprintf(alias_final,sizeof(alias_final),"%s/releases/alias-new",alias)>0);publish_command(alias_source,alias_final,1);
  CHECK(!chmod(directory,0777));path(final,sizeof(final),"releases/permissions-new");publish_command(stage,final,1);CHECK(!chmod(directory,0700));
}
int main(int argc,char **argv) {
  if(argc==4 && !strcmp(argv[1],"serve-detached"))return helper(argv);
  CHECK(argc==5);self=argv[0];server=argv[2];proxy=argv[3];root=argv[4];CHECK(!atexit(cleanup));init();
  if(!strcmp(argv[1],"publish"))publication();else if(!strcmp(argv[1],"launch"))launch();else if(!strcmp(argv[1],"data") || !strcmp(argv[1],"old-instance"))absent(argv[1]);else if(!strcmp(argv[1],"cancel"))cancellation();else if(!strcmp(argv[1],"locks"))locks();else if(!strcmp(argv[1],"incompatible") || !strcmp(argv[1],"features"))refused(argv[1]);else if(!strcmp(argv[1],"timeout"))timeout_test();else if(!strcmp(argv[1],"existing"))existing();else if(!strcmp(argv[1],"trusted"))trusted();else if(!strcmp(argv[1],"stdio"))stdio_test();else if(!strcmp(argv[1],"concurrent"))concurrent();else if(!strcmp(argv[1],"codec"))codec();else if(!strcmp(argv[1],"recheck"))recheck();else if(!strcmp(argv[1],"committed-cancel"))committed_cancel();else if(!strcmp(argv[1],"empty"))empty_input();else if(!strcmp(argv[1],"stderr"))diagnostic_pressure();else if(!strcmp(argv[1],"unsafe-entry"))unsafe_entry();else if(!strcmp(argv[1],"barrier"))barrier();else if(!strcmp(argv[1],"backpressure") || !strcmp(argv[1],"disconnect") || !strcmp(argv[1],"pipes"))bridge(argv[1]);else {malformed(argv[1]);if(!strcmp(argv[1],"role"))malformed("role-data");}
#ifdef GT_TEST_SHORT_IO
  extern size_t gt_test_short_calls(void);CHECK(gt_test_short_calls()>100);
#endif
  printf("PASS activation %s\n",argv[1]);return 0;
}
