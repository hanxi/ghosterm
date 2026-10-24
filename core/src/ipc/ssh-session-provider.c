#include "ssh-session-provider.h"
#include "ssh-link.h"
#include "internal.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
#ifdef GT_HAVE_LIBSSH2
struct gt_ssh_session_provider {
  gt_allocator_t allocator;gt_ssh_session_provider_config_t config;gt_ssh_provider_info_t info;
  char proxy[1025],daemon[1025],runtime[1025];
  gt_ssh_connector_t *connector;gt_ssh_link_t *owner;
  uint64_t now,started,last_request;bool pending,callback,notified,clock_fault;
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool clock_now(void *ctx,uint64_t *out){gt_ssh_session_provider_t *p=ctx;if(p->clock_fault || !p->config.clock(p->config.clock_context,out) || *out<p->now){p->clock_fault=true;return false;}p->now=*out;return true;}
static void cancel_request(gt_ssh_session_provider_t *p){if(p->pending){p->pending=false;p->callback=true;p->config.cancelled(p->config.context,p->info.request);p->callback=false;}}
static void fail(gt_ssh_session_provider_t *p,gt_status_t error) {
  p->info.phase=GT_SSH_PROVIDER_FAILED;p->info.error=error;cancel_request(p);
  gt__ssh_connector_close(p->connector);p->connector=NULL;
}
static bool tick(gt_ssh_session_provider_t *p) {
  uint64_t now;if(!clock_now(p,&now)){fail(p,GT_ERR_INTERNAL);return false;}
  if(p->info.phase!=GT_SSH_PROVIDER_ACTIVE && now-p->started>=p->config.authentication_timeout_ms){p->info.deadline=true;fail(p,GT_ERR_IO);return false;}return true;
}
static gt_session_link_state_t state(gt_ssh_session_provider_t *p,gt_status_t *error) {
  *error=p->info.error;return *error==GT_ERR_IO || *error==GT_ERR_CANCELLED?GT_SESSION_LINK_RETRY:GT_SESSION_LINK_FATAL;
}
static gt_session_link_state_t step(void *context,bool start,uint32_t attempt,gt_status_t *error) {
  gt_ssh_session_provider_t *p=context;*error=GT_OK;
  if(p->callback){*error=GT_ERR_INTERNAL;return GT_SESSION_LINK_FATAL;}
  if(start) {
    if(p->info.phase!=GT_SSH_PROVIDER_IDLE || p->last_request==UINT64_MAX){*error=GT_ERR_INTERNAL;return GT_SESSION_LINK_FATAL;}
    uint64_t now;if(!clock_now(p,&now)){fail(p,GT_ERR_INTERNAL);return state(p,error);}p->started=now;
    p->info=(gt_ssh_provider_info_t){.phase=GT_SSH_PROVIDER_REQUESTED,.request=++p->last_request};p->pending=true;p->notified=false;
    p->callback=true;p->config.requested(p->config.context,p->info.request,attempt);p->callback=false;
  }
  if(p->info.phase==GT_SSH_PROVIDER_IDLE){*error=GT_ERR_INTERNAL;return GT_SESSION_LINK_FATAL;}
  if(p->info.phase==GT_SSH_PROVIDER_FAILED)return state(p,error);
  if(!tick(p))return state(p,error);
  if(p->info.phase==GT_SSH_PROVIDER_REQUESTED)return GT_SESSION_LINK_WAIT;
  if(p->connector) {
    gt_ssh_connector_phase_t phase=gt__ssh_connector_step(p->connector);gt__ssh_connector_info(p->connector,&p->info.authentication);
    if(phase==GT_SSH_CONNECT_HOSTKEY && !p->notified){p->notified=true;p->callback=true;p->config.hostkey(p->config.context,p->info.request,&p->info.authentication);p->callback=false;}
    if(phase==GT_SSH_CONNECT_FAILED || phase==GT_SSH_CONNECT_CANCELLED){p->info.deadline=p->info.authentication.deadline;fail(p,p->clock_fault?GT_ERR_INTERNAL:p->info.authentication.error);return state(p,error);}
    if(!tick(p))return state(p,error);
    if(phase!=GT_SSH_CONNECT_READY)return GT_SESSION_LINK_WAIT;
    gt_ssh_t *ssh=NULL;if(!gt__ssh_connector_take(p->connector,&ssh)){fail(p,GT_ERR_INTERNAL);return state(p,error);}
    gt_status_t rc=gt__ssh_link_open(&ssh,&p->allocator,&p->owner);
    if(rc!=GT_OK){gt__ssh_abort(ssh);gt_ssh_close(ssh);fail(p,rc);return state(p,error);}
    gt__ssh_connector_close(p->connector);p->connector=NULL;p->pending=false;p->info.phase=GT_SSH_PROVIDER_ACTIVE;
  }
  gt_status_t rc=gt__ssh_link_step(p->owner);if(rc!=GT_OK){p->info.phase=GT_SSH_PROVIDER_FAILED;p->info.error=rc;return state(p,error);}
  return GT_SESSION_LINK_READY;
}
static gt_status_t stream(void *context,unsigned slot,gt_stream_ops_t *out){gt_ssh_session_provider_t *p=context;if(!out || p->callback || p->info.phase!=GT_SSH_PROVIDER_ACTIVE)return GT_ERR_INVALID_ARG;return gt__ssh_link_stream(p->owner,slot,p->proxy,p->runtime,p->daemon,out);}
static void reset(void *context) {
  gt_ssh_session_provider_t *p=context;cancel_request(p);gt__ssh_connector_close(p->connector);p->connector=NULL;
  gt__ssh_link_close(p->owner);p->owner=NULL;p->info=(gt_ssh_provider_info_t){.phase=GT_SSH_PROVIDER_IDLE};p->notified=false;
}
static bool path(const char *text){if(!text || text[0]!='/' || strlen(text)>1024)return false;for(size_t i=0;text[i];i++)if((unsigned char)text[i]<32 || (unsigned char)text[i]==127)return false;return true;}
gt_status_t gt__ssh_session_provider_open(const gt_ssh_session_provider_config_t *config,const gt_allocator_t *allocator,gt_ssh_session_provider_t **out) {
  if(out)*out=NULL;
  if(!out || !config || !path(config->proxy) || !path(config->daemon) || !path(config->runtime) || !config->clock || !config->requested || !config->cancelled || !config->hostkey || !config->authentication_timeout_ms || config->authentication_timeout_ms>60000)return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  uint64_t now;if(!config->clock(config->clock_context,&now))return GT_ERR_IO;
  gt_ssh_session_provider_t *p=a.allocate(a.context,sizeof(*p));if(!p)return GT_ERR_IO;
  memset(p,0,sizeof(*p));p->allocator=a;p->config=*config;p->now=now;strcpy(p->proxy,config->proxy);strcpy(p->daemon,config->daemon);strcpy(p->runtime,config->runtime);p->config.proxy=p->proxy;p->config.daemon=p->daemon;p->config.runtime=p->runtime;*out=p;return GT_OK;
}
bool gt__ssh_session_provider_link(gt_ssh_session_provider_t *p,gt_session_link_t *out){if(out)memset(out,0,sizeof(*out));if(!p || !out)return false;*out=(gt_session_link_t){p,step,stream,reset};return true;}
static gt_status_t scope(gt_ssh_session_provider_t *p,uint64_t request,bool requested_only) {
  if(!p || p->callback)return GT_ERR_INVALID_ARG;
  if(!request || request!=p->info.request || !p->pending || p->info.phase==GT_SSH_PROVIDER_IDLE || p->info.phase==GT_SSH_PROVIDER_FAILED || (requested_only && p->info.phase!=GT_SSH_PROVIDER_REQUESTED))return GT_ERR_NOT_FOUND;
  if(!tick(p))return p->info.error;
  return GT_OK;
}
gt_status_t gt__ssh_session_provider_supply(gt_ssh_session_provider_t *p,uint64_t request,gt_ssh_t **ssh,const gt_ssh_connector_config_t *resolved) {
  gt_status_t e=scope(p,request,true);if(e!=GT_OK)return e;if(!resolved || !ssh || !*ssh)return GT_ERR_INVALID_ARG;
  gt_ssh_connector_config_t c=*resolved;c.defer_credentials=false;c.timeout_ms=p->config.authentication_timeout_ms-(uint32_t)(p->now-p->started);c.clock=clock_now;c.clock_context=p;
  e=gt__ssh_connector_open(ssh,&c,&p->allocator,&p->connector);if(e==GT_OK)p->info.phase=GT_SSH_PROVIDER_AUTHENTICATING;return e;
}
gt_status_t gt__ssh_session_provider_adopt(gt_ssh_session_provider_t *p,uint64_t request,gt_ssh_t **ssh) {
  gt_status_t error=scope(p,request,true);if(error!=GT_OK)return error;
  if(!ssh || !*ssh || p->connector || p->owner)return GT_ERR_INVALID_ARG;
  error=gt__ssh_link_open(ssh,&p->allocator,&p->owner);
  if(error==GT_OK){p->pending=false;p->info.phase=GT_SSH_PROVIDER_ACTIVE;}
  return error;
}
gt_status_t gt__ssh_session_provider_reject(gt_ssh_session_provider_t *p,uint64_t request,gt_status_t error){gt_status_t e=scope(p,request,true);if(e!=GT_OK)return e;if(error==GT_OK || error>GT_ERR_INTERNAL)return GT_ERR_INVALID_ARG;fail(p,error);return GT_OK;}
gt_status_t gt__ssh_session_provider_decide(gt_ssh_session_provider_t *p,uint64_t request,gt_hostkey_decision_t decision){gt_status_t e=scope(p,request,false);if(e!=GT_OK)return e;if(!p->connector)return GT_ERR_INVALID_ARG;e=gt__ssh_connector_decide(p->connector,decision);if(e!=GT_OK){gt__ssh_connector_info(p->connector,&p->info.authentication);if(p->info.authentication.phase==GT_SSH_CONNECT_FAILED)fail(p,e);}return e;}
bool gt__ssh_session_provider_info(const gt_ssh_session_provider_t *p,gt_ssh_provider_info_t *out){if(out)memset(out,0,sizeof(*out));if(!p || !out)return false;*out=p->info;return true;}
void gt__ssh_session_provider_close(gt_ssh_session_provider_t *p){if(!p)return;reset(p);gt_allocator_t a=p->allocator;gt__secure_erase(p,sizeof(*p));a.release(a.context,p,sizeof(*p));}

#else
struct gt_ssh_session_provider {int unused;};
gt_status_t gt__ssh_session_provider_open(const gt_ssh_session_provider_config_t *c,const gt_allocator_t *a,gt_ssh_session_provider_t **out){(void)c;(void)a;if(out)*out=NULL;return GT_ERR_UNSUPPORTED;}
bool gt__ssh_session_provider_link(gt_ssh_session_provider_t *p,gt_session_link_t *out){(void)p;if(out)memset(out,0,sizeof(*out));return false;}
gt_status_t gt__ssh_session_provider_supply(gt_ssh_session_provider_t *p,uint64_t r,gt_ssh_t **s,const gt_ssh_connector_config_t *c){(void)p;(void)r;(void)s;(void)c;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_session_provider_adopt(gt_ssh_session_provider_t *p,uint64_t r,gt_ssh_t **s){(void)p;(void)r;(void)s;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_session_provider_reject(gt_ssh_session_provider_t *p,uint64_t r,gt_status_t e){(void)p;(void)r;(void)e;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_session_provider_decide(gt_ssh_session_provider_t *p,uint64_t r,gt_hostkey_decision_t d){(void)p;(void)r;(void)d;return GT_ERR_UNSUPPORTED;}
bool gt__ssh_session_provider_info(const gt_ssh_session_provider_t *p,gt_ssh_provider_info_t *out){(void)p;if(out)memset(out,0,sizeof(*out));return false;}
void gt__ssh_session_provider_close(gt_ssh_session_provider_t *p){(void)p;}
#endif
