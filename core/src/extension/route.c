#define _POSIX_C_SOURCE 200809L
#include "extension/route.h"
#include "ssh/wire.h"
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  gt_bridge_stream_t base;
  gt_allocator_t allocator;
  gt_bridge_route_info_t info;
  gt_bridge_route_options_t options;
  gt_ssh_connector_config_t config[GT_BRIDGE_ROUTE_MAX_HOPS];
  char via[GT_BRIDGE_ROUTE_MAX_HOPS][GT_BRIDGE_ROUTE_HOST_MAX+1];
  char command[GT_BRIDGE_SSH_COMMAND_MAX+1];
  gt_ssh_t *material[GT_BRIDGE_ROUTE_MAX_HOPS],*ready;
  gt_ssh_connector_t *auth;
  gt_bridge_stream_t *leaf;
  uint64_t started,now;
  int wake[2];
} route_t;
static const gt_bridge_stream_ops_t ops;
#ifdef GT_HAVE_LIBSSH2
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
#endif
static bool stopping(route_t *r){return atomic_load_explicit(&r->base.stopping,memory_order_acquire);}
static void failure(route_t *r,gt_status_t error) {
  if(r->info.phase!=GT_BRIDGE_ROUTE_FAILED)r->info.failure_phase=r->info.phase;
  r->info.error=error;r->info.phase=error==GT_ERR_CANCELLED?GT_BRIDGE_ROUTE_STOPPED:GT_BRIDGE_ROUTE_FAILED;
}
static bool tick(route_t *r) {
  if(stopping(r)){failure(r,GT_ERR_CANCELLED);return false;}
  uint64_t now;
  if(!r->config[0].clock(r->config[0].clock_context,&now) || now<r->now){failure(r,GT_ERR_INTERNAL);return false;}
  r->now=now;
  if(now-r->started>=r->options.total_ms){r->info.deadline=true;failure(r,GT_ERR_IO);return false;}
  return true;
}
static gt_status_t attach(route_t *r) {
  return gt__bridge_ssh_attach_stop(r->leaf,&r->base.stopping,r->wake[0]);
}
gt_bridge_route_phase_t gt__bridge_route_step(gt_bridge_stream_t *base,int ms) {
  if(!base || base->ops!=&ops || ms<0)return GT_BRIDGE_ROUTE_FAILED;
  route_t *r=(route_t *)base;
  if(base->closed)return GT_BRIDGE_ROUTE_CLOSED;
  if(stopping(r) && r->info.phase<GT_BRIDGE_ROUTE_FAILED){failure(r,GT_ERR_CANCELLED);return r->info.phase;}
  if(r->info.phase>=GT_BRIDGE_ROUTE_READY)return r->info.phase;
  if(!tick(r))return r->info.phase;
  uint64_t step_start=r->now;
  for(;;) {
    if(!tick(r))break;
    int budget=ms;
    uint64_t elapsed=r->now-step_start,remaining=r->options.total_ms-(r->now-r->started);
    if(elapsed>=(uint64_t)ms)budget=0;else budget=ms-(int)elapsed;
    if((uint64_t)budget>remaining)budget=(int)remaining;
    size_t h=r->info.hop;
    if(r->info.phase==GT_BRIDGE_ROUTE_AUTH) {
      gt_ssh_connector_phase_t phase=gt__ssh_connector_step(r->auth);
      (void)gt__ssh_connector_info(r->auth,&r->info.authentication[h]);
      if(stopping(r)){failure(r,GT_ERR_CANCELLED);break;}
      if(phase==GT_SSH_CONNECT_FAILED || phase==GT_SSH_CONNECT_CANCELLED){failure(r,r->info.authentication[h].error);break;}
      if(phase!=GT_SSH_CONNECT_READY) {
        if(phase==GT_SSH_CONNECT_HOSTKEY || phase==GT_SSH_CONNECT_CREDENTIALS || !budget)break;
        int ready=gt__ssh_connector_wait(r->auth,budget,&r->wake[0],1);
        if(!tick(r))break;
        if(ready<0){failure(r,GT_ERR_IO);break;}
        if(!ready)break;
        continue;
      }
      if(!gt__ssh_connector_take(r->auth,&r->ready)){failure(r,GT_ERR_INTERNAL);break;}
      bool released=false;
      if(gt__ssh_connector_dispose(&r->auth,&released)!=GT_OK || !released){failure(r,GT_ERR_INTERNAL);break;}
      gt_status_t status;
      if(h+1<r->info.count) {
        status=gt__bridge_ssh_forward_open(&r->ready,r->via[h+1],gt__ssh_route_port(r->material[h+1]),&r->options.stream,&r->allocator,&r->leaf);
        r->info.phase=GT_BRIDGE_ROUTE_FORWARD;
      } else {
        status=gt__bridge_ssh_open(&r->ready,r->command,&r->options.stream,&r->allocator,&r->leaf);
        r->info.phase=GT_BRIDGE_ROUTE_EXEC;
      }
      if(status!=GT_OK || (status=attach(r))!=GT_OK){failure(r,status);break;}
    } else {
      gt_bridge_ssh_phase_t phase=gt__bridge_ssh_step(r->leaf,budget);
      (void)gt__bridge_ssh_info(r->leaf,&r->info.channel);
      if(stopping(r)){failure(r,GT_ERR_CANCELLED);break;}
      if(phase==GT_BRIDGE_SSH_FAILED || phase==GT_BRIDGE_SSH_STOPPED){failure(r,r->info.channel.error);break;}
      if(phase!=GT_BRIDGE_SSH_READY)break;
      if(r->info.phase==GT_BRIDGE_ROUTE_EXEC){r->info.phase=GT_BRIDGE_ROUTE_READY;break;}
      r->info.hop++;
      gt_status_t status=gt__ssh_connector_open_stream(&r->material[h+1],&r->config[h+1],&r->leaf,&r->allocator,&r->auth);
      if(status!=GT_OK){failure(r,status);break;}
      r->info.phase=GT_BRIDGE_ROUTE_AUTH;
    }
    if(!ms)break;
  }
  return r->info.phase;
}
gt_status_t gt__bridge_route_decide(gt_bridge_stream_t *base,gt_hostkey_decision_t decision) {
  if(!base || base->ops!=&ops)return GT_ERR_INVALID_ARG;
  route_t *r=(route_t *)base;
  if(r->info.phase!=GT_BRIDGE_ROUTE_AUTH || !r->auth)return GT_ERR_INVALID_ARG;
  if(!tick(r))return r->info.error;
  gt_status_t status=gt__ssh_connector_decide(r->auth,decision);
  (void)gt__ssh_connector_info(r->auth,&r->info.authentication[r->info.hop]);
  if(status==GT_OK || decision==GT_HOSTKEY_REJECT)r->info.decisions[r->info.hop]=decision;
  if(r->info.authentication[r->info.hop].phase==GT_SSH_CONNECT_FAILED)failure(r,status);
  return status;
}
gt_status_t gt__bridge_route_supply(gt_bridge_stream_t *base,gt_ssh_t **owned) {
  if(!base || base->ops!=&ops)return GT_ERR_INVALID_ARG;
  route_t *r=(route_t *)base;
  if(r->info.phase!=GT_BRIDGE_ROUTE_AUTH || !r->auth)return GT_ERR_INVALID_ARG;
  if(!tick(r))return r->info.error;
  return gt__ssh_connector_supply(r->auth,owned);
}
bool gt__bridge_route_info(const gt_bridge_stream_t *base,gt_bridge_route_info_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!base || base->ops!=&ops || !out)return false;
  const route_t *r=(const route_t *)base;*out=r->info;
  if(r->leaf)(void)gt__bridge_ssh_info(r->leaf,&out->channel);
  return true;
}
static gt_bridge_io_t transfer(route_t *r,void *p,size_t n,int ms,bool writing) {
  if(r->info.phase!=GT_BRIDGE_ROUTE_READY)return (gt_bridge_io_t){GT_BRIDGE_ERROR,0,r->info.error?r->info.error:GT_ERR_INVALID_ARG};
  gt_bridge_io_t result=writing?gt__bridge_stream_write(r->leaf,p,n,ms):gt__bridge_stream_read(r->leaf,p,n,ms);
  if(stopping(r))failure(r,GT_ERR_CANCELLED);
  else if(result.state==GT_BRIDGE_ERROR)failure(r,result.error);
  return result;
}
static gt_bridge_io_t read_stream(gt_bridge_stream_t *r,void *p,size_t n,int ms){return transfer((route_t *)r,p,n,ms,false);}
static gt_bridge_io_t write_stream(gt_bridge_stream_t *r,const void *p,size_t n,int ms){return transfer((route_t *)r,(void *)p,n,ms,true);}
static void wake(gt_bridge_stream_t *base) {
  route_t *r=(route_t *)base;uint8_t byte=1;
  while(write(r->wake[1],&byte,1)<0 && errno==EINTR){}
}
static gt_bridge_close_t close_step(gt_bridge_stream_t *base,int ms) {
  (void)ms;route_t *r=(route_t *)base;
  if(r->auth) {
    bool released=false;gt_status_t s=gt__ssh_connector_dispose(&r->auth,&released);
    if(s!=GT_OK)return GT_BRIDGE_CLOSE_ERROR;
    if(!released)return GT_BRIDGE_DRAINING;
  }
  if(r->ready) {
    gt__ssh_abort(r->ready);bool released=false;
    if(gt__ssh_dispose_step(&r->ready,&released)!=GT_OK)return GT_BRIDGE_CLOSE_ERROR;
    if(!released)return GT_BRIDGE_DRAINING;
  }
  if(r->leaf) {
    gt_bridge_close_t s=gt__bridge_stream_close(r->leaf,0);
    if(s!=GT_BRIDGE_RELEASED)return s;
    if(gt__bridge_stream_destroy(r->leaf)!=GT_OK)return GT_BRIDGE_CLOSE_ERROR;
    r->leaf=NULL;
  }
  for(size_t i=0;i<r->info.count;i++){gt_ssh_close(r->material[i]);r->material[i]=NULL;}
  r->info.phase=GT_BRIDGE_ROUTE_CLOSED;return GT_BRIDGE_RELEASED;
}
static void destroy(gt_bridge_stream_t *base) {
  route_t *r=(route_t *)base;close(r->wake[0]);close(r->wake[1]);
  gt_allocator_t a=r->allocator;a.release(a.context,r,sizeof(*r));
}
static const gt_bridge_stream_ops_t ops={read_stream,write_stream,wake,close_step,destroy};

gt_status_t gt__bridge_route_open(gt_ssh_t **owned,const gt_bridge_route_hop_t *hops,size_t count,
 const char *command,const gt_bridge_route_options_t *options,const gt_allocator_t *allocator,gt_bridge_stream_t **out) {
  if(out)*out=NULL;
#ifndef GT_HAVE_LIBSSH2
  (void)owned;(void)hops;(void)count;(void)command;(void)options;(void)allocator;
  return GT_ERR_UNSUPPORTED;
#else
  if(!owned || !hops || !count || count>GT_BRIDGE_ROUTE_MAX_HOPS || !command || !out)return GT_ERR_INVALID_ARG;
  size_t length=strnlen(command,GT_BRIDGE_SSH_COMMAND_MAX+1);
  if(!length || length>GT_BRIDGE_SSH_COMMAND_MAX)return GT_ERR_INVALID_ARG;
  gt_bridge_route_options_t o=options?*options:(gt_bridge_route_options_t){30000,{5000,262144}};
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!o.total_ms || o.total_ms>60000 || !o.stream.admission_ms || o.stream.admission_ms>60000 ||
     !o.stream.stderr_limit || o.stream.stderr_limit>GT_BRIDGE_SSH_STDERR_MAX || !a.allocate || !a.release ||
     !hops[0].connector.clock || hops[0].via_host)return GT_ERR_INVALID_ARG;
  for(size_t i=0;i<count;i++) {
    if(!gt__ssh_route_fresh(owned[i]) || !hops[i].connector.timeout_ms || hops[i].connector.timeout_ms>60000 ||
       hops[i].connector.clock!=hops[0].connector.clock || hops[i].connector.clock_context!=hops[0].connector.clock_context ||
       (i==0?(hops[i].connector.family!=4 && hops[i].connector.family!=6):hops[i].connector.family!=0))return GT_ERR_INVALID_ARG;
    for(size_t j=0;j<i;j++)if(owned[j]==owned[i])return GT_ERR_INVALID_ARG;
    if(i) {
      if(!hops[i].via_host || !hops[i].via_host[0] || strnlen(hops[i].via_host,256)>255)return GT_ERR_INVALID_ARG;
      for(const unsigned char *p=(const unsigned char *)hops[i].via_host;*p;p++)if(*p<33 || *p==127)return GT_ERR_INVALID_ARG;
    }
  }
  uint64_t now;if(!hops[0].connector.clock(hops[0].connector.clock_context,&now))return GT_ERR_IO;
  route_t *r=a.allocate(a.context,sizeof(*r));if(!r)return GT_ERR_IO;
  memset(r,0,sizeof(*r));r->allocator=a;r->options=o;r->started=r->now=now;r->wake[0]=r->wake[1]=-1;
  r->info.count=count;r->info.phase=GT_BRIDGE_ROUTE_AUTH;memcpy(r->command,command,length+1);
  (void)gt__bridge_stream_init(&r->base,&ops);
  for(size_t i=0;i<count;i++) {
    r->material[i]=owned[i];r->config[i]=hops[i].connector;r->config[i].stop=&r->base.stopping;
    r->info.decisions[i]=GT_HOSTKEY_UNKNOWN;
    if(i)memcpy(r->via[i],hops[i].via_host,strlen(hops[i].via_host)+1);
  }
  if(pipe(r->wake))goto failed;
  for(size_t i=0;i<2;i++)if(fcntl(r->wake[i],F_SETFL,O_NONBLOCK)<0 || fcntl(r->wake[i],F_SETFD,FD_CLOEXEC)<0)goto failed;
  gt_status_t status=gt__ssh_connector_open(&r->material[0],&r->config[0],&a,&r->auth);
  if(status!=GT_OK) {
    close(r->wake[0]);close(r->wake[1]);a.release(a.context,r,sizeof(*r));return status;
  }
  for(size_t i=0;i<count;i++)owned[i]=NULL;
  *out=&r->base;return GT_OK;
failed:
  if(r->wake[0]>=0)close(r->wake[0]);if(r->wake[1]>=0)close(r->wake[1]);
  a.release(a.context,r,sizeof(*r));return GT_ERR_IO;
#endif
}
