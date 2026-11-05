#define _POSIX_C_SOURCE 200809L
#include "ghosterm/extension_session.h"
#include "ghosterm/extension_rpc.h"
#include "extension/session-owner.h"
#include "extension/session.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool ext_session_clock(void *ctx,uint64_t *out){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
/* Enum translations are exhaustive; public and private headers remain separate. */
static gt_ext_session_phase_t ext_phase(gt_bridge_session_phase_t p){switch(p){
 case GT_BRIDGE_SESSION_SENDING:return GT_EXT_SESSION_SENDING;case GT_BRIDGE_SESSION_WAIT_HELLO:return GT_EXT_SESSION_WAIT_HELLO;
 case GT_BRIDGE_SESSION_WAIT_OFFER:return GT_EXT_SESSION_WAIT_OFFER;case GT_BRIDGE_SESSION_CONSENT:return GT_EXT_SESSION_CONSENT;
 case GT_BRIDGE_SESSION_WAIT_DECISION:return GT_EXT_SESSION_WAIT_DECISION;case GT_BRIDGE_SESSION_WAIT_ACCEPT:return GT_EXT_SESSION_WAIT_ACCEPT;
 case GT_BRIDGE_SESSION_WAIT_READY:return GT_EXT_SESSION_WAIT_READY;case GT_BRIDGE_SESSION_WAIT_ACK:return GT_EXT_SESSION_WAIT_ACK;
 case GT_BRIDGE_SESSION_ACTIVE:return GT_EXT_SESSION_ACTIVE;case GT_BRIDGE_SESSION_ENDING:return GT_EXT_SESSION_ENDING;
 case GT_BRIDGE_SESSION_ENDED:return GT_EXT_SESSION_ENDED;case GT_BRIDGE_SESSION_CLOSED:return GT_EXT_SESSION_CLOSED;}return GT_EXT_SESSION_ENDED;}
static gt_ext_session_end_t ext_end(gt_bridge_session_end_t p){switch(p){
 case GT_BRIDGE_END_NONE:return GT_EXT_END_NONE;case GT_BRIDGE_END_DENIED:return GT_EXT_END_DENIED;case GT_BRIDGE_END_REVOKED:return GT_EXT_END_REVOKED;
 case GT_BRIDGE_END_EXPIRED:return GT_EXT_END_EXPIRED;case GT_BRIDGE_END_INCOMPATIBLE:return GT_EXT_END_INCOMPATIBLE;case GT_BRIDGE_END_PROTOCOL:return GT_EXT_END_PROTOCOL;
 case GT_BRIDGE_END_LOST:return GT_EXT_END_LOST;case GT_BRIDGE_END_PEER:return GT_EXT_END_PEER;case GT_BRIDGE_END_LOCAL:return GT_EXT_END_LOCAL;case GT_BRIDGE_END_INTERNAL:return GT_EXT_END_INTERNAL;}return GT_EXT_END_INTERNAL;}
/* Callbacks see already wire-validated JSON; only inspect its declared root.
 * Context is the facade's own copied method, never caller/platform storage. */
static bool ext_shape(gt_ext_json_shape_t shape,const uint8_t *p,size_t n){
 if(!p || !n)return false;size_t i=0;while(i<n && (p[i]==' ' || p[i]=='\n' || p[i]=='\r' || p[i]=='\t'))i++;if(i==n)return false;
 switch(shape){case GT_EXT_JSON_ANY:return true;case GT_EXT_JSON_OBJECT:return p[i]=='{';case GT_EXT_JSON_ARRAY:return p[i]=='[';case GT_EXT_JSON_STRING:return p[i]=='"';case GT_EXT_JSON_BOOLEAN:return p[i]=='t' || p[i]=='f';case GT_EXT_JSON_NUMBER:return p[i]=='-' || (p[i]>='0' && p[i]<='9');case GT_EXT_JSON_NULL:return p[i]=='n';}return false;
}
static bool ext_input(void *ctx,const uint8_t *p,size_t n){return ext_shape(((gt_ext_rpc_method_t *)ctx)->input_shape,p,n);}
static bool ext_output(void *ctx,const uint8_t *p,size_t n){return ext_shape(((gt_ext_rpc_method_t *)ctx)->output_shape,p,n);}
static gt_status_t ext_session_open_owned(gt_ext_channel_t **owned,gt_ext_role_t role,const gt_ext_session_config_t *config,const gt_ext_rpc_method_t *methods,size_t count,bool requests,gt_ext_resource_store_t *store,uint64_t scope,gt_ext_session_t **out){
 gt_bridge_stream_t *stream=NULL;gt_status_t status=gt__ext_channel_admit(*owned,config->peer.kind==GT_EXT_PEER_SSH,&stream);if(status!=GT_OK)return status;
 gt_bridge_session_config_t c={0};
 memcpy(c.plugin_id,config->plugin_id,sizeof(c.plugin_id));memcpy(c.plugin_version,config->plugin_version,sizeof(c.plugin_version));
 memcpy(c.client_digest,config->client_digest,sizeof(c.client_digest));memcpy(c.executor_digest,config->executor_digest,sizeof(c.executor_digest));
 memcpy(c.host_version,config->host_version,sizeof(c.host_version));memcpy(c.sdk_version,config->sdk_version,sizeof(c.sdk_version));
 c.required_count=config->required_count;c.available_count=config->available_count;memcpy(c.required,config->required,sizeof(c.required));memcpy(c.available,config->available,sizeof(c.available));
 c.budgets=(gt_bridge_budgets_t){config->budgets.fragment_bytes,config->budgets.message_bytes,config->budgets.resource_bytes};
 c.handshake_ms=config->handshake_ms;c.consent_ms=config->consent_ms;c.peer.kind=config->peer.kind==GT_EXT_PEER_SSH?GT_BRIDGE_PEER_SSH:GT_BRIDGE_PEER_LOCAL;
 memcpy(c.peer.identity,config->peer.identity,sizeof(c.peer.identity));c.clock=config->clock?config->clock:ext_session_clock;c.clock_context=config->clock_context;
 gt_ext_session_t *s=calloc(1,sizeof(*s));if(!s)return GT_ERR_IO;
 s->requests=requests;c.rpc_enabled=requests;c.rpc_method_count=count;
 s->store=store;c.resource_store=store?store->inner:NULL;c.resource_scope_bytes=scope;
 for(size_t i=0;i<count;i++){
  s->methods[i]=methods[i];gt_bridge_rpc_method_t *m=&c.rpc_methods[i];memcpy(m->method,methods[i].method,sizeof(m->method));memcpy(m->capability,methods[i].capability,sizeof(m->capability));
  m->receiver=(uint8_t)methods[i].receiver;m->input=ext_input;m->output=ext_output;m->context=&s->methods[i];
 }
 status=gt__bridge_session_open(&stream,role==GT_EXT_HOST?GT_BRIDGE_HOST:GT_BRIDGE_EXECUTOR,&c,NULL,&s->inner);
 if(status!=GT_OK){gt__secure_erase(s,sizeof(*s));free(s);return status;}
 if(store)store->sessions++;s->channel=*owned;gt__ext_channel_commit(s->channel);*owned=NULL;*out=s;return GT_OK;
}
gt_status_t gt_ext_session_open(gt_ext_channel_t **owned,gt_ext_role_t role,const gt_ext_session_config_t *config,gt_ext_session_t **out){
 if(out)*out=NULL;if(!out || !owned || !*owned || !config || (role!=GT_EXT_HOST && role!=GT_EXT_EXECUTOR))return GT_ERR_INVALID_ARG;
 if(config->features || (config->peer.kind!=GT_EXT_PEER_LOCAL && config->peer.kind!=GT_EXT_PEER_SSH))return GT_ERR_UNSUPPORTED;
 return ext_session_open_owned(owned,role,config,NULL,0,false,NULL,0,out);
}
gt_status_t gt_ext_session_open_rpc(gt_ext_channel_t **owned,gt_ext_role_t role,const gt_ext_session_config_t *config,const gt_ext_rpc_method_t *methods,size_t count,gt_ext_session_t **out){
 if(out)*out=NULL;if(!out || !owned || !*owned || !config || (role!=GT_EXT_HOST && role!=GT_EXT_EXECUTOR) || count>GT_EXT_RPC_METHODS_MAX || (count && !methods))return GT_ERR_INVALID_ARG;
 if(config->features!=GT_EXT_FEATURE_REQUESTS || (config->peer.kind!=GT_EXT_PEER_LOCAL && config->peer.kind!=GT_EXT_PEER_SSH))return GT_ERR_UNSUPPORTED;
 for(size_t i=0;i<count;i++){
  if(methods[i].receiver!=GT_EXT_HOST && methods[i].receiver!=GT_EXT_EXECUTOR)return GT_ERR_INVALID_ARG;
  if(methods[i].input_shape<GT_EXT_JSON_ANY || methods[i].input_shape>GT_EXT_JSON_NULL || methods[i].output_shape<GT_EXT_JSON_ANY || methods[i].output_shape>GT_EXT_JSON_NULL)return GT_ERR_UNSUPPORTED;
 }
 return ext_session_open_owned(owned,role,config,methods,count,true,NULL,0,out);
}
gt_status_t gt_ext_session_open_resources(gt_ext_channel_t **owned,gt_ext_role_t role,const gt_ext_session_config_t *config,const gt_ext_rpc_method_t *methods,size_t count,gt_ext_resource_store_t *store,uint64_t scope,gt_ext_session_t **out){
 if(out)*out=NULL;if(!out || !owned || !*owned || !config || !store || !scope || scope>store->total_bytes || scope>GT_BRIDGE_RESOURCE_TOTAL_MAX || store->sessions==SIZE_MAX || (role!=GT_EXT_HOST && role!=GT_EXT_EXECUTOR) || count>GT_EXT_RPC_METHODS_MAX || (count && !methods))return GT_ERR_INVALID_ARG;
 if((config->features!=GT_EXT_FEATURE_RESOURCES && config->features!=(GT_EXT_FEATURE_RESOURCES|GT_EXT_FEATURE_REQUESTS)) || (config->peer.kind!=GT_EXT_PEER_LOCAL && config->peer.kind!=GT_EXT_PEER_SSH))return GT_ERR_UNSUPPORTED;
 bool requests=(config->features&GT_EXT_FEATURE_REQUESTS)!=0;if(!requests && count)return GT_ERR_INVALID_ARG;
 for(size_t i=0;i<count;i++){
  if(methods[i].receiver!=GT_EXT_HOST && methods[i].receiver!=GT_EXT_EXECUTOR)return GT_ERR_INVALID_ARG;
  if(methods[i].input_shape<GT_EXT_JSON_ANY || methods[i].input_shape>GT_EXT_JSON_NULL || methods[i].output_shape<GT_EXT_JSON_ANY || methods[i].output_shape>GT_EXT_JSON_NULL)return GT_ERR_UNSUPPORTED;
 }
 return ext_session_open_owned(owned,role,config,methods,count,requests,store,scope,out);
}
gt_ext_session_phase_t gt_ext_session_step(gt_ext_session_t *s,int ms){if(!s || ms<0)return GT_EXT_SESSION_ENDED;return ext_phase(gt__bridge_session_step(s->inner,ms));}
bool gt_ext_session_info(const gt_ext_session_t *s,gt_ext_session_info_t *out){
 if(out)memset(out,0,sizeof(*out));if(!s || !out)return false;gt_bridge_session_info_t i;if(!gt__bridge_session_info(s->inner,&i))return false;
 out->phase=ext_phase(i.phase);out->end=ext_end(i.end);out->error=i.error;out->cleanup_error=s->cleanup_error;
 out->decision=i.decision==GT_BRIDGE_DECISION_ALLOW?GT_EXT_DECISION_ALLOW:i.decision==GT_BRIDGE_DECISION_DENY?GT_EXT_DECISION_DENY:GT_EXT_DECISION_UNKNOWN;
 memcpy(out->connection,i.connection,33);memcpy(out->instance,i.instance,33);memcpy(out->consent_token,i.consent_token,33);memcpy(out->peer_host_version,i.peer_host_version,65);
 out->epoch=i.epoch;out->consent_deadline=i.consent_deadline;out->received=i.received;out->sent=i.sent;
 out->budgets=(gt_ext_budgets_t){i.budgets.fragment_bytes,i.budgets.message_bytes,i.budgets.resource_bytes};out->peer.kind=i.peer.kind==GT_BRIDGE_PEER_SSH?GT_EXT_PEER_SSH:GT_EXT_PEER_LOCAL;memcpy(out->peer.identity,i.peer.identity,sizeof(out->peer.identity));return true;
}
gt_status_t gt_ext_session_decide(gt_ext_session_t *s,const char *ticket,gt_ext_decision_t decision){
 if(!s || (decision!=GT_EXT_DECISION_UNKNOWN && decision!=GT_EXT_DECISION_ALLOW && decision!=GT_EXT_DECISION_DENY))return GT_ERR_INVALID_ARG;
 return gt__bridge_session_decide(s->inner,ticket,decision==GT_EXT_DECISION_ALLOW?GT_BRIDGE_DECISION_ALLOW:decision==GT_EXT_DECISION_DENY?GT_BRIDGE_DECISION_DENY:GT_BRIDGE_DECISION_UNKNOWN);
}
bool gt_ext_session_lease(const gt_ext_session_t *s,gt_ext_lease_t *out){
 if(out)memset(out,0,sizeof(*out));if(!s || !out)return false;gt_bridge_lease_t l;if(!gt__bridge_session_lease(s->inner,&l))return false;
 memcpy(out->connection,l.connection,33);memcpy(out->instance,l.instance,33);memcpy(out->token,l.token,33);out->epoch=l.epoch;return true;
}
gt_ext_access_t gt_ext_session_access(const gt_ext_session_t *s,const gt_ext_lease_t *lease,const char *capability){
 if(!s || !lease || !capability)return GT_EXT_ACCESS_DENIED;gt_bridge_lease_t l;memcpy(l.connection,lease->connection,33);memcpy(l.instance,lease->instance,33);memcpy(l.token,lease->token,33);l.epoch=lease->epoch;
 switch(gt__bridge_session_access(s->inner,&l,capability)){case GT_BRIDGE_ACCESS_UNKNOWN:return GT_EXT_ACCESS_UNKNOWN;case GT_BRIDGE_ACCESS_ALLOWED:return GT_EXT_ACCESS_ALLOWED;case GT_BRIDGE_ACCESS_DENIED:return GT_EXT_ACCESS_DENIED;case GT_BRIDGE_ACCESS_REVOKED:return GT_EXT_ACCESS_REVOKED;case GT_BRIDGE_ACCESS_UNSUPPORTED:return GT_EXT_ACCESS_UNSUPPORTED;}return GT_EXT_ACCESS_DENIED;
}
void gt_ext_session_revoke(gt_ext_session_t *s){if(s)gt__bridge_session_revoke(s->inner);}
void gt_ext_session_stop(gt_ext_session_t *s){if(s)gt__bridge_session_stop(s->inner);}
gt_ext_close_t gt_ext_session_close(gt_ext_session_t *s,int ms){
 if(!s || ms<0)return GT_EXT_CLOSE_ERROR;if(s->released)return s->sources?GT_EXT_DRAINING:GT_EXT_RELEASED;gt_bridge_close_t r=gt__bridge_session_close(s->inner,ms);
 if(r==GT_BRIDGE_RELEASED){s->released=true;s->cleanup_error=GT_OK;return s->sources?GT_EXT_DRAINING:GT_EXT_RELEASED;}
 if(r==GT_BRIDGE_CLOSE_ERROR)s->cleanup_error=GT_ERR_IO;return r==GT_BRIDGE_DRAINING?GT_EXT_DRAINING:GT_EXT_CLOSE_ERROR;
}
gt_status_t gt_ext_session_destroy(gt_ext_session_t **owned){
 if(!owned || !*owned || !(*owned)->released || (*owned)->sources)return GT_ERR_INVALID_ARG;gt_ext_session_t *s=*owned;gt_status_t status=gt__bridge_session_destroy(s->inner);
 if(status!=GT_OK){s->cleanup_error=status;return status;}if(s->store)s->store->sessions--;gt__ext_channel_retire(s->channel);gt__secure_erase(s,sizeof(*s));free(s);*owned=NULL;return GT_OK;
}
static gt_bridge_lease_t ext_lease(const gt_ext_lease_t *p){gt_bridge_lease_t l;memcpy(l.connection,p->connection,33);memcpy(l.instance,p->instance,33);memcpy(l.token,p->token,33);l.epoch=p->epoch;return l;}
static gt_ext_rpc_status_t ext_rpc_status(gt_bridge_rpc_status_t p){switch(p){case GT_BRIDGE_RPC_OK:return GT_EXT_RPC_OK;case GT_BRIDGE_RPC_WAIT:return GT_EXT_RPC_WAIT;case GT_BRIDGE_RPC_INVALID:return GT_EXT_RPC_INVALID;case GT_BRIDGE_RPC_DENIED:return GT_EXT_RPC_DENIED;case GT_BRIDGE_RPC_UNSUPPORTED:return GT_EXT_RPC_UNSUPPORTED;case GT_BRIDGE_RPC_CLOSED:return GT_EXT_RPC_CLOSED;}return GT_EXT_RPC_INVALID;}
static gt_ext_rpc_status_t ext_rpc_check(gt_ext_session_t *s,const gt_ext_lease_t *lease){if(!s || !lease)return GT_EXT_RPC_INVALID;if(!s->requests)return GT_EXT_RPC_UNSUPPORTED;gt_bridge_lease_t l=ext_lease(lease);return gt__bridge_session_scope(s->inner,&l)?GT_EXT_RPC_OK:GT_EXT_RPC_DENIED;}
gt_ext_rpc_status_t gt_ext_session_request(gt_ext_session_t *s,const gt_ext_lease_t *lease,const char *method,const void *json,size_t size,uint32_t timeout,uint64_t *handle){
 if(handle)*handle=0;if(!handle)return GT_EXT_RPC_INVALID;gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);return ext_rpc_status(gt__bridge_session_request(s->inner,&l,method,json,size,timeout,handle));
}
gt_ext_rpc_status_t gt_ext_session_cancel_request(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle){gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);return ext_rpc_status(gt__bridge_session_cancel_request(s->inner,&l,handle));}
gt_ext_rpc_status_t gt_ext_session_request_result(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle,gt_ext_rpc_result_t *out){
 if(out)memset(out,0,sizeof(*out));if(!out)return GT_EXT_RPC_INVALID;gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);
 if(!gt__bridge_session_scope(s->inner,&l))return GT_EXT_RPC_DENIED;gt_bridge_rpc_result_t r;if(!gt__bridge_session_request_result(s->inner,&l,handle,&r))return GT_EXT_RPC_INVALID;
 if(r.outcome_size>GT_EXT_RPC_BODY_MAX || (r.outcome_size && !r.outcome)){gt__bridge_session_stop(s->inner);return GT_EXT_RPC_INVALID;}
 out->handle=r.handle;memcpy(out->request_id,r.request_id,sizeof(out->request_id));out->cancel_requested=r.cancel_requested;out->deadline_elapsed=r.deadline_elapsed;out->confirmed=r.confirmed;
 switch(r.state){case GT_BRIDGE_RPC_QUEUED:out->state=GT_EXT_RPC_QUEUED;break;case GT_BRIDGE_RPC_PENDING:out->state=GT_EXT_RPC_PENDING;break;case GT_BRIDGE_RPC_SUCCEEDED:out->state=GT_EXT_RPC_SUCCEEDED;break;case GT_BRIDGE_RPC_FAILED:out->state=GT_EXT_RPC_FAILED;break;case GT_BRIDGE_RPC_CANCELLED:out->state=GT_EXT_RPC_CANCELLED;break;case GT_BRIDGE_RPC_UNKNOWN:out->state=GT_EXT_RPC_UNKNOWN;break;}
 out->outcome_size=(uint32_t)r.outcome_size;if(r.outcome_size)memcpy(out->outcome,r.outcome,r.outcome_size);return GT_EXT_RPC_OK;
}
gt_ext_rpc_status_t gt_ext_session_forget_request(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle){gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);return ext_rpc_status(gt__bridge_session_forget_request(s->inner,&l,handle));}
gt_ext_rpc_status_t gt_ext_session_take_request(gt_ext_session_t *s,const gt_ext_lease_t *lease,gt_ext_rpc_request_t *out){
 if(out)memset(out,0,sizeof(*out));if(!out)return GT_EXT_RPC_INVALID;gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);gt_bridge_rpc_request_t r;
 status=ext_rpc_status(gt__bridge_session_take_request(s->inner,&l,&r));if(status!=GT_EXT_RPC_OK)return status;
 if(!r.input || !r.input_size || r.input_size>GT_EXT_RPC_BODY_MAX){gt__bridge_session_stop(s->inner);return GT_EXT_RPC_INVALID;}
 out->ticket=r.ticket;memcpy(out->request_id,r.request_id,sizeof(out->request_id));memcpy(out->method,r.method,sizeof(out->method));out->input_size=(uint32_t)r.input_size;memcpy(out->input,r.input,r.input_size);return GT_EXT_RPC_OK;
}
gt_ext_rpc_status_t gt_ext_session_check_request(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t ticket){gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);return ext_rpc_status(gt__bridge_session_check_request(s->inner,&l,ticket));}
gt_ext_rpc_status_t gt_ext_session_respond(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t ticket,const void *outcome,size_t n){gt_ext_rpc_status_t status=ext_rpc_check(s,lease);if(status!=GT_EXT_RPC_OK)return status;gt_bridge_lease_t l=ext_lease(lease);return ext_rpc_status(gt__bridge_session_respond(s->inner,&l,ticket,outcome,n));}
