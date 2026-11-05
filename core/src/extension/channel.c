#define _POSIX_C_SOURCE 200809L
#include "ghosterm/extension.h"
#include "extension/route.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct gt_ext_channel {
  gt_bridge_stream_t *stream;bool ssh,closed;
  gt_ext_channel_info_t info;
  uint64_t challenge_counter;
  uint32_t wait_hop;gt_ssh_auth_phase_t wait_phase;bool waiting;
  uint8_t write_buffer[GT_EXT_IO_MAX];size_t write_size,write_at;
};
static atomic_uint channel_count;
static bool channel_stopped(const gt_ext_channel_t *c){return atomic_load_explicit(&c->stream->stopping,memory_order_acquire);}
#ifdef GT_HAVE_PLUGIN_CHANNEL
static bool channel_clock(void *ctx,uint64_t *out){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static gt_ext_channel_t *channel_new(void){
  unsigned count=atomic_load_explicit(&channel_count,memory_order_relaxed);
  do{if(count>=GT_EXT_CHANNEL_LIMIT)return NULL;}while(!atomic_compare_exchange_weak_explicit(&channel_count,&count,count+1,memory_order_acq_rel,memory_order_relaxed));
  gt_ext_channel_t *c=calloc(1,sizeof(*c));if(!c)atomic_fetch_sub_explicit(&channel_count,1,memory_order_release);return c;
}
#endif
static void channel_free(gt_ext_channel_t *c){gt__secure_erase(c,sizeof(*c));free(c);atomic_fetch_sub_explicit(&channel_count,1,memory_order_release);}
static gt_ext_stage_t channel_stage(gt_bridge_route_phase_t p){switch(p){case GT_BRIDGE_ROUTE_AUTH:return GT_EXT_STAGE_AUTH;case GT_BRIDGE_ROUTE_FORWARD:return GT_EXT_STAGE_FORWARD;case GT_BRIDGE_ROUTE_EXEC:return GT_EXT_STAGE_EXEC;default:return GT_EXT_STAGE_STREAM;}}
static gt_ssh_auth_phase_t channel_auth_phase(gt_ssh_connector_phase_t p){switch(p){
  case GT_SSH_CONNECT_TCP:return GT_SSH_AUTH_PHASE_TCP;case GT_SSH_CONNECT_NEGOTIATING:return GT_SSH_AUTH_PHASE_NEGOTIATING;case GT_SSH_CONNECT_HOSTKEY:return GT_SSH_AUTH_PHASE_HOSTKEY;
  case GT_SSH_CONNECT_AUTHENTICATING:return GT_SSH_AUTH_PHASE_AUTHENTICATING;case GT_SSH_CONNECT_READY:return GT_SSH_AUTH_PHASE_READY;case GT_SSH_CONNECT_TAKEN:return GT_SSH_AUTH_PHASE_TAKEN;
  case GT_SSH_CONNECT_FAILED:return GT_SSH_AUTH_PHASE_FAILED;case GT_SSH_CONNECT_CANCELLED:return GT_SSH_AUTH_PHASE_CANCELLED;case GT_SSH_CONNECT_CREDENTIALS:return GT_SSH_AUTH_PHASE_CREDENTIALS;
 }return GT_SSH_AUTH_PHASE_FAILED;
}
static gt_ssh_auth_host_state_t channel_host(gt_ssh_host_state_t h){switch(h){case GT_SSH_HOST_UNCHECKED:return GT_SSH_AUTH_HOST_UNCHECKED;case GT_SSH_HOST_KNOWN:return GT_SSH_AUTH_HOST_KNOWN;case GT_SSH_HOST_UNKNOWN:return GT_SSH_AUTH_HOST_UNKNOWN;case GT_SSH_HOST_MISMATCH:return GT_SSH_AUTH_HOST_MISMATCH;case GT_SSH_HOST_REVOKED:return GT_SSH_AUTH_HOST_REVOKED;}return GT_SSH_AUTH_HOST_UNCHECKED;}
static void channel_discard(gt_ext_channel_t *c){gt__secure_erase(c->write_buffer,sizeof(c->write_buffer));c->write_size=c->write_at=0;c->info.buffered=0;}
/* A WAIT provider may still borrow write_buffer. Stop/error alone does not
 * release that borrow; preserve bytes/address until accepted or CLOSED. */
static void channel_fail(gt_ext_channel_t *c,gt_status_t status){c->info.failure_stage=c->info.stage;c->info.phase=status==GT_ERR_CANCELLED?GT_EXT_STOPPED:GT_EXT_FAILED;c->info.error=status;c->info.challenge=0;c->waiting=false;}
static bool channel_refresh(gt_ext_channel_t *c){
  if(!c->ssh)return true;gt_bridge_route_info_t r;if(!gt__bridge_route_info(c->stream,&r) || !r.count || r.count>GT_EXT_MAX_HOPS || r.hop>=r.count)return false;
  c->info.hop=(uint32_t)r.hop;c->info.hops=(uint32_t)r.count;c->info.setup_expired=r.deadline;c->info.stage=channel_stage(r.phase);
  if(r.phase==GT_BRIDGE_ROUTE_FAILED || r.phase==GT_BRIDGE_ROUTE_STOPPED){channel_fail(c,r.error?r.error:GT_ERR_INTERNAL);c->info.failure_stage=channel_stage(r.failure_phase);}
  else if(r.phase==GT_BRIDGE_ROUTE_READY && c->info.phase==GT_EXT_CONNECTING)c->info.phase=GT_EXT_READY;
  for(size_t i=0;i<r.count;i++){gt_ssh_connector_info_t *a=&r.authentication[i];gt_ssh_auth_info_t *p=&c->info.authentication[i];p->phase=channel_auth_phase(a->phase);p->error=a->error;p->host=channel_host(a->host);p->deadline=a->deadline;p->auth_methods=a->auth_methods;memcpy(p->key_type,a->key_type,sizeof(p->key_type));memcpy(p->fingerprint,a->fingerprint,sizeof(p->fingerprint));c->info.decisions[i]=r.decisions[i];}
  c->info.stderr_bytes=r.channel.stderr_bytes;c->info.remote_eof=r.channel.remote_eof;c->info.exit_status_known=r.channel.exit_status_known;c->info.exit_status=r.channel.exit_status;c->info.diagnostic_truncated=r.channel.diagnostic_truncated;
  if(r.channel.diagnostic_size>sizeof(c->info.diagnostic))return false;c->info.diagnostic_size=(uint32_t)r.channel.diagnostic_size;memcpy(c->info.diagnostic,r.channel.diagnostic,r.channel.diagnostic_size);
  gt_ssh_auth_phase_t phase=c->info.authentication[r.hop].phase;bool wait=r.phase==GT_BRIDGE_ROUTE_AUTH && (phase==GT_SSH_AUTH_PHASE_HOSTKEY || phase==GT_SSH_AUTH_PHASE_CREDENTIALS);
  if(wait && (!c->waiting || c->wait_hop!=r.hop || c->wait_phase!=phase)){if(c->challenge_counter==UINT64_MAX){channel_fail(c,GT_ERR_INTERNAL);return false;}c->info.challenge=++c->challenge_counter;}
  if(!wait)c->info.challenge=0;c->waiting=wait;c->wait_hop=(uint32_t)r.hop;c->wait_phase=phase;return true;
}
gt_status_t gt_ext_channel_open_fd(int input,int output,gt_ext_channel_t **out){
  if(out)*out=NULL;if(!out || input<0 || output<0)return GT_ERR_INVALID_ARG;
#ifndef GT_HAVE_PLUGIN_CHANNEL
  return GT_ERR_UNSUPPORTED;
#else
  gt_ext_channel_t *c=channel_new();if(!c)return GT_ERR_IO;gt_status_t status=gt__bridge_fd_open(input,output,NULL,&c->stream);if(status!=GT_OK){channel_free(c);return status;}
  c->info.phase=GT_EXT_READY;c->info.stage=GT_EXT_STAGE_STREAM;*out=c;return GT_OK;
#endif
}
gt_status_t gt_ext_channel_open_ssh(gt_ssh_t **owned,const gt_ext_ssh_hop_t *hops,size_t count,const char *command,const gt_ext_ssh_options_t *options,gt_ext_channel_t **out){
  if(out)*out=NULL;if(!out || !owned || !hops || !count || count>GT_EXT_MAX_HOPS || !command)return GT_ERR_INVALID_ARG;
#ifndef GT_HAVE_PLUGIN_CHANNEL
  (void)options;return GT_ERR_UNSUPPORTED;
#else
  gt_bridge_route_hop_t private[GT_EXT_MAX_HOPS]={0};for(size_t i=0;i<count;i++){const gt_ssh_auth_options_t *a=&hops[i].authentication;private[i].connector=(gt_ssh_connector_config_t){.family=a->family,.scope=a->scope,.timeout_ms=a->timeout_ms,.clock_context=a->clock_context,.clock=a->clock?a->clock:channel_clock,.defer_credentials=a->defer_credentials};memcpy(private[i].connector.address,a->address,16);private[i].via_host=hops[i].via_host;}
  gt_bridge_route_options_t opts;if(options)opts=(gt_bridge_route_options_t){options->setup_ms,{options->admission_ms,options->stderr_limit}};
  gt_ext_channel_t *c=channel_new();if(!c)return GT_ERR_IO;gt_status_t status=gt__bridge_route_open(owned,private,count,command,options?&opts:NULL,NULL,&c->stream);if(status!=GT_OK){channel_free(c);return status;}
  c->ssh=true;c->info.phase=GT_EXT_CONNECTING;c->info.stage=GT_EXT_STAGE_AUTH;c->info.hops=(uint32_t)count;for(size_t i=0;i<count;i++)c->info.decisions[i]=GT_HOSTKEY_UNKNOWN;*out=c;return GT_OK;
#endif
}
gt_ext_phase_t gt_ext_channel_step(gt_ext_channel_t *c,int ms){
  if(!c || ms<0)return GT_EXT_FAILED;if(c->closed)return GT_EXT_CLOSED;if(c->info.phase==GT_EXT_FAILED || c->info.phase==GT_EXT_STOPPED)return c->info.phase;if(channel_stopped(c)){channel_fail(c,GT_ERR_CANCELLED);return c->info.phase;}
  if(c->info.phase==GT_EXT_CONNECTING){(void)gt__bridge_route_step(c->stream,ms);if(!channel_refresh(c))channel_fail(c,GT_ERR_INTERNAL);return c->info.phase;}
  if(c->write_at<c->write_size){gt_bridge_io_t r=gt__bridge_stream_write(c->stream,c->write_buffer+c->write_at,c->write_size-c->write_at,ms);
    if(r.state==GT_BRIDGE_DATA){c->write_at+=r.count;c->info.sent+=r.count;c->info.buffered=(uint32_t)(c->write_size-c->write_at);if(c->write_at==c->write_size)channel_discard(c);}
    else if(r.state!=GT_BRIDGE_WAIT)channel_fail(c,r.error?r.error:GT_ERR_IO);
    if(channel_stopped(c)){if(c->info.phase!=GT_EXT_FAILED)channel_fail(c,GT_ERR_CANCELLED);return c->info.phase;}
  }
  if(!channel_refresh(c))channel_fail(c,GT_ERR_INTERNAL);return c->info.phase;
}
bool gt_ext_channel_info(const gt_ext_channel_t *c,gt_ext_channel_info_t *out){if(out)memset(out,0,sizeof(*out));if(!c || !out)return false;*out=c->info;if(c->closed)out->phase=GT_EXT_CLOSED;else if(channel_stopped(c) && out->phase!=GT_EXT_FAILED){out->phase=GT_EXT_STOPPED;out->error=GT_ERR_CANCELLED;out->challenge=0;}return true;}
static gt_status_t channel_reply(gt_ext_channel_t *c,uint32_t hop,uint64_t challenge,gt_ssh_auth_phase_t phase){
  if(!c)return GT_ERR_INVALID_ARG;if(!c->ssh)return GT_ERR_UNSUPPORTED;if(c->closed || channel_stopped(c))return GT_ERR_CANCELLED;
  if(c->info.phase!=GT_EXT_CONNECTING || !c->waiting || !challenge || challenge!=c->info.challenge || hop!=c->info.hop || c->wait_phase!=phase)return GT_ERR_INVALID_ARG;return GT_OK;
}
gt_status_t gt_ext_channel_decide(gt_ext_channel_t *c,uint32_t hop,uint64_t challenge,gt_hostkey_decision_t decision){
  gt_status_t status=channel_reply(c,hop,challenge,GT_SSH_AUTH_PHASE_HOSTKEY);if(status!=GT_OK)return status;status=gt__bridge_route_decide(c->stream,decision);if(status==GT_OK && decision!=GT_HOSTKEY_UNKNOWN){c->waiting=false;c->info.challenge=0;}if(!channel_refresh(c))return GT_ERR_INTERNAL;return status;
}
gt_status_t gt_ext_channel_supply(gt_ext_channel_t *c,uint32_t hop,uint64_t challenge,gt_ssh_t **owned){
  if(!owned || !*owned)return GT_ERR_INVALID_ARG;gt_status_t status=channel_reply(c,hop,challenge,GT_SSH_AUTH_PHASE_CREDENTIALS);if(status!=GT_OK)return status;status=gt__bridge_route_supply(c->stream,owned);if(status==GT_OK){c->waiting=false;c->info.challenge=0;}if(!channel_refresh(c))return GT_ERR_INTERNAL;return status;
}
static gt_ext_io_t channel_io(gt_bridge_io_t r){gt_ext_io_state_t state;switch(r.state){case GT_BRIDGE_DATA:state=GT_EXT_DATA;break;case GT_BRIDGE_WAIT:state=GT_EXT_WAIT;break;case GT_BRIDGE_EOF:state=GT_EXT_EOF;break;case GT_BRIDGE_CLOSED:state=GT_EXT_IO_CLOSED;break;default:state=GT_EXT_IO_ERROR;break;}return (gt_ext_io_t){state,r.count,r.error};}
gt_ext_io_t gt_ext_channel_read(gt_ext_channel_t *c,void *p,size_t n,int ms){
  if(!c || !p || !n || ms<0)return (gt_ext_io_t){GT_EXT_IO_ERROR,0,GT_ERR_INVALID_ARG};if(c->closed)return (gt_ext_io_t){GT_EXT_IO_CLOSED,0,GT_ERR_CANCELLED};if(c->info.phase==GT_EXT_FAILED)return (gt_ext_io_t){GT_EXT_IO_ERROR,0,c->info.error};if(channel_stopped(c))return (gt_ext_io_t){GT_EXT_IO_CLOSED,0,GT_ERR_CANCELLED};if(c->info.phase!=GT_EXT_READY)return (gt_ext_io_t){GT_EXT_WAIT,0,GT_OK};
  gt_bridge_io_t r=gt__bridge_stream_read(c->stream,p,n,ms);
  if(r.state==GT_BRIDGE_DATA){if(UINT64_MAX-c->info.received<r.count){channel_fail(c,GT_ERR_INTERNAL);return (gt_ext_io_t){GT_EXT_IO_ERROR,0,GT_ERR_INTERNAL};}c->info.received+=r.count;}else if(r.state==GT_BRIDGE_ERROR)channel_fail(c,r.error);
  if(channel_stopped(c)){if(c->info.phase!=GT_EXT_FAILED)channel_fail(c,GT_ERR_CANCELLED);return (gt_ext_io_t){GT_EXT_IO_CLOSED,0,GT_ERR_CANCELLED};}
  if(!channel_refresh(c)){channel_fail(c,GT_ERR_INTERNAL);return (gt_ext_io_t){GT_EXT_IO_ERROR,0,GT_ERR_INTERNAL};}return channel_io(r);
}
gt_ext_io_t gt_ext_channel_write(gt_ext_channel_t *c,const void *p,size_t n){
  if(!c || !p || !n)return (gt_ext_io_t){GT_EXT_IO_ERROR,0,GT_ERR_INVALID_ARG};if(c->closed)return (gt_ext_io_t){GT_EXT_IO_CLOSED,0,GT_ERR_CANCELLED};if(c->info.phase==GT_EXT_FAILED)return (gt_ext_io_t){GT_EXT_IO_ERROR,0,c->info.error};if(channel_stopped(c))return (gt_ext_io_t){GT_EXT_IO_CLOSED,0,GT_ERR_CANCELLED};if(c->info.phase!=GT_EXT_READY || c->write_at<c->write_size)return (gt_ext_io_t){GT_EXT_WAIT,0,GT_OK};
  if(n>sizeof(c->write_buffer))n=sizeof(c->write_buffer);if(UINT64_MAX-c->info.accepted<n){channel_fail(c,GT_ERR_INTERNAL);return (gt_ext_io_t){GT_EXT_IO_ERROR,0,GT_ERR_INTERNAL};}
  memcpy(c->write_buffer,p,n);c->write_size=n;c->write_at=0;c->info.accepted+=n;c->info.buffered=(uint32_t)n;return (gt_ext_io_t){GT_EXT_DATA,n,GT_OK};
}
void gt_ext_channel_stop(gt_ext_channel_t *c){if(c)gt__bridge_stream_stop(c->stream);}
gt_ext_close_t gt_ext_channel_close(gt_ext_channel_t *c,int ms){
  if(!c || ms<0)return GT_EXT_CLOSE_ERROR;if(c->closed)return GT_EXT_RELEASED;gt__bridge_stream_stop(c->stream);c->info.challenge=0;c->waiting=false;
  gt_bridge_close_t r=gt__bridge_stream_close(c->stream,ms);if(r==GT_BRIDGE_RELEASED){channel_discard(c);c->closed=true;c->info.phase=GT_EXT_CLOSED;c->info.cleanup_error=GT_OK;return GT_EXT_RELEASED;}if(r==GT_BRIDGE_CLOSE_ERROR)c->info.cleanup_error=GT_ERR_IO;return r==GT_BRIDGE_DRAINING?GT_EXT_DRAINING:GT_EXT_CLOSE_ERROR;
}
gt_status_t gt_ext_channel_destroy(gt_ext_channel_t **owned){if(!owned || !*owned || !(*owned)->closed)return GT_ERR_INVALID_ARG;gt_ext_channel_t *c=*owned;gt_status_t status=gt__bridge_stream_destroy(c->stream);if(status!=GT_OK)return status;channel_free(c);*owned=NULL;return GT_OK;}
