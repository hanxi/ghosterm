#define _POSIX_C_SOURCE 200809L
#include "host.h"
#include "ipc/socket-stream.h"
#include "base/secure-random.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef struct {gt_socket_stream_t stream;uint64_t issuer;} host_peer;
struct gt_session_host {
  gt_allocator_t allocator;gt_session_host_config_t config;
  gt_unix_endpoint_t *endpoint;gt_session_runtime_t *runtime;
  gt_checkpoint_tasks_t *tasks;gt_attachment_owner_t *attachments;
  gt_data_router_t *router;gt_server_t *server;
  pthread_t reaper;bool reaper_started,closing,cleanup_failed;int wake[2];
  atomic_bool stopping;atomic_int failure;
  host_peer peers[];
};
static bool clock_ms(void *context,uint64_t *out) {
  (void)context;struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0 || (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return false;
  *out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;
}
static gt_status_t endpoint_error(gt_endpoint_rc_t rc) {
  return rc==GT_ENDPOINT_OK?GT_OK:rc==GT_ENDPOINT_AUTH?GT_ERR_AUTH:
    rc==GT_ENDPOINT_INVALID?GT_ERR_INVALID_ARG:
    rc==GT_ENDPOINT_UNSUPPORTED?GT_ERR_UNSUPPORTED:GT_ERR_IO;
}
static bool wake_pipe(int fds[2]) {
  if(pipe(fds))return false;
  for(unsigned i=0;i<2;i++) {
    int flags=fcntl(fds[i],F_GETFL);
    if(flags<0 || fcntl(fds[i],F_SETFL,flags|O_NONBLOCK) || fcntl(fds[i],F_SETFD,FD_CLOEXEC)) {
      close(fds[0]);close(fds[1]);fds[0]=fds[1]=-1;return false;
    }
  }
  return true;
}
static bool reap(gt_session_host_t *h) {
  gt_managed_info_t rows[64];size_t count=0;
  if(gt__manager_list(gt__runtime_manager(h->runtime),rows,64,&count)!=GT_MANAGER_OK)return false;
  for(size_t i=0;i<count;i++)if(rows[i].has_result) {
    if(rows[i].phase==GT_MANAGED_WORKER && gt__runtime_reap(h->runtime,rows[i].id)!=GT_MANAGER_OK)return false;
    /* The child is gone; keep the frozen terminal only while a client still
     * references it. BUSY means some attachment holds it (it may still be
     * watching the end screen), so retry and release the Source slot as soon as
     * the last reference drops. */
    gt_runtime_rc_t rc=gt__runtime_forget(h->runtime,rows[i].id);
    if(rc!=GT_RUNTIME_OK && rc!=GT_RUNTIME_BUSY)return false;
  }
  return true;
}
static void *reaper(void *context) {
  gt_session_host_t *h=context;uint64_t previous=0;
  while(!atomic_load(&h->stopping)) {
    uint64_t now;
    if(!clock_ms(NULL,&now) || now<previous || !reap(h)){atomic_store(&h->failure,GT_ERR_IO);break;}
    previous=now;struct pollfd p={.fd=h->wake[0],.events=POLLIN};
    int rc=poll(&p,1,50);
    if(rc<0 && errno!=EINTR){atomic_store(&h->failure,GT_ERR_IO);break;}
    if(rc>0 && (p.revents&(POLLERR|POLLHUP|POLLNVAL))){atomic_store(&h->failure,GT_ERR_IO);break;}
    if(rc>0 && (p.revents&POLLIN)){uint8_t b[64];ssize_t consumed=read(h->wake[0],b,sizeof(b));(void)consumed;}
  }
  return NULL;
}
gt_status_t gt__session_host_open(const gt_session_host_config_t *c,const gt_allocator_t *a,gt_session_host_t **out) {
  if(out)*out=NULL;
  if(!out || !c || !a || !a->allocate || !a->release || !c->directory ||
    !c->sessions || c->sessions>64 || !c->connections || c->connections>64 ||
    !c->snapshot_limit || c->snapshot_limit>64u*1024*1024 || !c->snapshot_format ||
    !gt__thread_options_valid(&c->threads))return GT_ERR_INVALID_ARG;
  size_t n=sizeof(gt_session_host_t)+c->connections*sizeof(host_peer);
  gt_session_host_t *h=a->allocate(a->context,n);if(!h)return GT_ERR_IO;
  memset(h,0,n);h->allocator=*a;h->config=*c;h->wake[0]=h->wake[1]=-1;
  for(size_t i=0;i<c->connections;i++)h->peers[i].stream.fd=-1;
  atomic_init(&h->stopping,false);atomic_init(&h->failure,GT_OK);
  uint64_t now;gt_status_t rc=GT_ERR_IO;
  if(!clock_ms(NULL,&now))goto failed;
  rc=endpoint_error(gt__unix_endpoint_open(c->directory,true,a,&h->endpoint));if(rc!=GT_OK)goto failed;
  h->tasks=gt__tasks_open_with_options(2,a,&c->threads);if(!h->tasks){rc=GT_ERR_IO;goto failed;}
  gt_runtime_config_t runtime={.sessions=c->sessions,.creation_records=64,.creation_retention_ms=60000,
    .queue_bytes=1024*1024,.queue_events=1024,.tasks=h->tasks,.state=c->state,
    .worker_threads=c->threads,.backend_threads=c->threads};memcpy(runtime.instance,c->identity.instance,16);
  rc=gt__runtime_open(&runtime,now,a,&h->runtime);if(rc!=GT_OK)goto failed;
  gt_attachment_owner_config_t owner={.runtime=h->runtime,.capacity=c->connections,.token_ttl_ms=10000};
  memcpy(owner.instance,c->identity.instance,16);rc=gt__attachment_owner_open(&owner,now,a,&h->attachments);if(rc!=GT_OK)goto failed;
  gt_data_router_config_t data={.runtime=h->runtime,.attachments=h->attachments,.connections=c->connections,
    .user_commands=16,.snapshot_limit=c->snapshot_limit,.chunk_bytes=16384,.format=c->snapshot_format,
    .restore_timeout_ms=30000,.error_timeout_ms=3000,.clock=clock_ms,.encoder=c->encoder};
  memcpy(data.instance,c->identity.instance,16);rc=gt__data_router_open(&data,a,&h->router);if(rc!=GT_OK)goto failed;
  gt_server_config_t server={.manager=gt__runtime_manager(h->runtime),.creation=gt__runtime_creation(h->runtime),.attachments=h->attachments,
    .connections=c->connections,.limits={65560,65560,5000},.idle_timeout_ms=15000,
    .data_context=h->router,.data_step=gt__data_router_step,.data_drop=gt__data_router_drop,.data_revoked=gt__data_router_revoked,
    .handshake={.identity=c->identity,.clock=clock_ms,.timeout_ms=5000}};
  rc=gt__server_open(&server,a,&h->server);if(rc!=GT_OK)goto failed;
  if(!wake_pipe(h->wake) || gt__thread_create(&h->reaper,reaper,h,&c->threads)){rc=GT_ERR_IO;goto failed;}
  h->reaper_started=true;
  rc=endpoint_error(gt__unix_endpoint_listen(h->endpoint,16));if(rc!=GT_OK)goto failed;
  *out=h;return GT_OK;
failed:
  (void)gt__session_host_close(&h);return rc;
}
gt_status_t gt__session_host_poll(gt_session_host_t *h,int wake_fd,int timeout_ms) {
  if(!h || h->closing || wake_fd< -1 || timeout_ms<0 || timeout_ms>1000)return GT_ERR_INVALID_ARG;
  if(atomic_load(&h->failure)!=GT_OK || gt__server_step(h->server)!=GT_SERVER_RUNNING)return GT_ERR_IO;
  gt__data_router_reap(h->router);
  for(size_t i=0;i<h->config.connections;i++)if(h->peers[i].issuer && !gt__server_connected(h->server,h->peers[i].issuer))h->peers[i].issuer=0;
  int fd=-1;gt_endpoint_rc_t accepted=gt__unix_endpoint_accept(h->endpoint,&fd);
  if(accepted==GT_ENDPOINT_OK) {
    host_peer *p=NULL;for(size_t i=0;i<h->config.connections;i++)if(!h->peers[i].issuer){p=&h->peers[i];break;}
    if(!p)close(fd);
    else {
      gt_stream_ops_t ops;
      if(gt__socket_stream_adopt(fd,&p->stream,&ops)!=GT_OK)close(fd);
      else (void)gt__server_admit(h->server,&ops,(uint64_t)geteuid()+1,&p->issuer);
    }
  } else if(accepted!=GT_ENDPOINT_WAIT && accepted!=GT_ENDPOINT_REJECTED && accepted!=GT_ENDPOINT_BUSY)return endpoint_error(accepted);
  struct pollfd fds[66];nfds_t count=0;
  fds[count++]=(struct pollfd){.fd=accepted==GT_ENDPOINT_BUSY?-1:gt__unix_endpoint_listener(h->endpoint),.events=POLLIN};
  if(wake_fd>=0)fds[count++]=(struct pollfd){.fd=wake_fd,.events=POLLIN};
  for(size_t i=0;i<h->config.connections;i++)if(h->peers[i].issuer)
    fds[count++]=(struct pollfd){.fd=h->peers[i].stream.fd,.events=(short)(POLLIN|(gt__server_wants_write(h->server,h->peers[i].issuer)?POLLOUT:0))};
  /* Timer also pumps encoder/PTY events that have no socket readiness yet. */
  if(timeout_ms>10)timeout_ms=10;
  int rc=poll(fds,count,timeout_ms);
  if(rc>0)for(nfds_t i=0;i<(wake_fd>=0?2u:1u);i++)if(fds[i].revents&(POLLERR|POLLHUP|POLLNVAL))return GT_ERR_IO;
  return rc<0 && errno!=EINTR?GT_ERR_IO:GT_OK;
}
gt_runtime_rc_t gt__session_host_close(gt_session_host_t **pointer) {
  if(!pointer)return GT_RUNTIME_INVALID;
  gt_session_host_t *h=*pointer;
  if(!h)return GT_RUNTIME_OK;
  if(!h->closing) {
    h->closing=true;
    gt__server_close(h->server);h->server=NULL;
    if(h->endpoint){h->cleanup_failed=gt__unix_endpoint_close(h->endpoint)!=GT_ENDPOINT_OK;h->endpoint=NULL;}
    atomic_store(&h->stopping,true);
    if(h->reaper_started){uint8_t b=1;ssize_t notified=write(h->wake[1],&b,1);(void)notified;pthread_join(h->reaper,NULL);h->reaper_started=false;}
    for(unsigned i=0;i<2;i++)if(h->wake[i]>=0){close(h->wake[i]);h->wake[i]=-1;}
  }
  if(h->router){gt_runtime_rc_t rc=gt__data_router_close(h->router);if(rc!=GT_RUNTIME_OK)return rc;h->router=NULL;}
  if(h->attachments){gt_runtime_rc_t rc=gt__attachment_owner_close(h->attachments);if(rc!=GT_RUNTIME_OK)return rc;h->attachments=NULL;}
  if(h->runtime){gt_runtime_rc_t rc=gt__runtime_close(h->runtime);if(rc!=GT_RUNTIME_OK)return rc;h->runtime=NULL;}
  if(h->tasks){if(gt__tasks_close(h->tasks)!=GT_TASK_OK)return GT_RUNTIME_BUSY;h->tasks=NULL;}
  gt_allocator_t a=h->allocator;size_t n=sizeof(*h)+h->config.connections*sizeof(host_peer);
  bool failed=h->cleanup_failed;gt__secure_erase(h,n);a.release(a.context,h,n);*pointer=NULL;
  return failed?GT_RUNTIME_INTERNAL:GT_RUNTIME_OK;
}
