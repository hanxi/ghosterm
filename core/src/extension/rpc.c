#define _POSIX_C_SOURCE 200809L
#include "extension/rpc.h"
#include "extension/session.h"
#include "base/secure-random.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {RPC_REQUEST_FLOW,RPC_RESPONSE_FLOW,RPC_FLOWS};
typedef enum {RPC_TX_NONE,RPC_TX_CREDIT,RPC_TX_CANCEL,RPC_TX_REQUEST,RPC_TX_RESPONSE} rpc_tx_t;
typedef struct {
  bool used,confirmed,local_terminal,cancel_requested,cancel_sent,deadline_elapsed;
  uint64_t handle,deadline;char id[33];size_t method,input_size,outcome_size;
  gt_bridge_rpc_state_t state;
  char input[GT_BRIDGE_RPC_BODY_MAX+1],outcome[GT_BRIDGE_RPC_BODY_MAX+1];
} rpc_out_t;
typedef struct {
  bool used,taken,cancel_requested,done,needs_send;
  uint64_t ticket,ordinal,deadline;size_t method,input_size,outcome_size;
  uint32_t reserved;char id[33],method_name[129];
  char input[GT_BRIDGE_RPC_BODY_MAX+1],outcome[GT_BRIDGE_RPC_BODY_MAX+1];
} rpc_in_t;
struct gt_bridge_rpc {
  gt_allocator_t allocator;gt_bridge_session_t *session;gt_bridge_wire_t *wire;
  const gt_bridge_rpc_method_t *methods;size_t method_count;gt_bridge_role_t role;
  uint64_t handles,tickets,tx_ordinal,rx_ordinal,tx_seq[RPC_FLOWS],rx_seq[RPC_FLOWS];
  bool tx_exhausted[RPC_FLOWS],rx_exhausted[RPC_FLOWS],started,ended;
  uint32_t tx_credit[RPC_FLOWS],rx_credit[RPC_FLOWS],credit_pending[RPC_FLOWS];
  rpc_tx_t prepared,inflight;size_t selected,inflight_slot;unsigned grant_flow;
  uint32_t cost,grant;char candidate_id[33];
  rpc_out_t out[GT_BRIDGE_RPC_SLOTS];rpc_in_t in[GT_BRIDGE_RPC_SLOTS];
};
static const char *const rpc_streams[]={"rpc.request","rpc.response"};
static const char rpc_unknown[]="{\"kind\":\"unknown\",\"reason\":\"no complete business response\"}";
static const char rpc_cancelled[]="{\"kind\":\"cancelled\",\"effect\":\"none\"}";
static const char rpc_expired[]="{\"kind\":\"failed\",\"error\":{\"code\":\"EXPIRED\",\"message\":\"deadline passed before dispatch\",\"effect\":\"none\"}}";
static bool rpc_live(gt_bridge_rpc_t *r){gt_bridge_session_info_t i;return gt__bridge_session_info(r->session,&i) && i.phase==GT_BRIDGE_SESSION_ACTIVE && i.end==GT_BRIDGE_END_NONE;}
static size_t rpc_limit(gt_bridge_rpc_t *r){gt_bridge_session_info_t i;if(!gt__bridge_session_info(r->session,&i))return 0;return i.budgets.message_bytes<GT_BRIDGE_RPC_FRAME_MAX?i.budgets.message_bytes:GT_BRIDGE_RPC_FRAME_MAX;}
static bool rpc_string(gt_bridge_rpc_t *r,uint16_t object,const char *key,char *out,size_t capacity) {
  const uint8_t *p;size_t n;if(!gt__bridge_wire_string(r->wire,gt__bridge_wire_member(r->wire,object,key),&p,&n) || n>=capacity)return false;memcpy(out,p,n);out[n]=0;return true;
}
static bool rpc_raw(gt_bridge_rpc_t *r,uint16_t object,const char *key,const uint8_t **p,size_t *n){return gt__bridge_wire_raw(r->wire,gt__bridge_wire_member(r->wire,object,key),p,n);}
static void rpc_prefix(gt_bridge_rpc_t *r,bool own,char prefix[17]) {
  gt_bridge_session_info_t i;(void)gt__bridge_session_info(r->session,&i);gt_bridge_role_t role=own?r->role:r->role==GT_BRIDGE_HOST?GT_BRIDGE_EXECUTOR:GT_BRIDGE_HOST;
  prefix[0]=role==GT_BRIDGE_HOST?'0':'8';memcpy(prefix+1,role==GT_BRIDGE_HOST?i.connection:i.instance,15);prefix[16]=0;
}
static bool rpc_ordinal(gt_bridge_rpc_t *r,const char *id,bool own,uint64_t *out) {
  char prefix[17];rpc_prefix(r,own,prefix);if(memcmp(id,prefix,16))return false;uint64_t n=0;
  for(size_t i=16;i<32;i++){unsigned char c=(unsigned char)id[i];unsigned v=c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:16;if(v>15)return false;n=(n<<4)|v;}
  *out=n;return n!=0;
}
static void rpc_id(gt_bridge_rpc_t *r,uint64_t ordinal,char id[33]){rpc_prefix(r,true,id);(void)snprintf(id+16,17,"%016" PRIx64,ordinal);}
static size_t rpc_method(gt_bridge_rpc_t *r,const char *method){for(size_t i=0;i<r->method_count;i++)if(!strcmp(r->methods[i].method,method))return i;return SIZE_MAX;}
static rpc_out_t *rpc_handle(gt_bridge_rpc_t *r,uint64_t handle){for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(r->out[i].used && r->out[i].handle==handle)return &r->out[i];return NULL;}
static rpc_in_t *rpc_ticket(gt_bridge_rpc_t *r,uint64_t ticket){for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(r->in[i].used && r->in[i].ticket==ticket)return &r->in[i];return NULL;}
static rpc_in_t *rpc_in_id(gt_bridge_rpc_t *r,const char *id){for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(r->in[i].used && !strcmp(r->in[i].id,id))return &r->in[i];return NULL;}
static rpc_out_t *rpc_out_id(gt_bridge_rpc_t *r,const char *id){for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(r->out[i].used && r->out[i].id[0] && !strcmp(r->out[i].id,id))return &r->out[i];return NULL;}
static bool rpc_frame(gt_bridge_rpc_t *r,const char *stream,uint64_t seq,const char *kind,const char *payload,char *out,size_t capacity,size_t *n) {
  gt_bridge_session_info_t i;if(!gt__bridge_session_info(r->session,&i))return false;
  int length=snprintf(out,capacity,"{\"wireVersion\":1,\"connectionId\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64 "\",\"streamId\":\"%s\",\"seq\":\"%" PRIu64 "\",\"kind\":\"%s\",\"payload\":%s}",i.connection,i.instance,i.epoch,stream,seq,kind,payload);
  if(length<0 || (size_t)length>=capacity || (strcmp(stream,"control") && (size_t)length>rpc_limit(r)))return false;*n=(size_t)length;return true;
}
static bool rpc_json_copy(const void *p,size_t n,char out[GT_BRIDGE_RPC_BODY_MAX+1]){if(!p || !n || n>GT_BRIDGE_RPC_BODY_MAX || memchr(p,0,n))return false;memcpy(out,p,n);out[n]=0;return true;}
static bool rpc_validate_request(gt_bridge_rpc_t *r,size_t method,const char *input) {
  char payload[GT_BRIDGE_RPC_FRAME_MAX+1],frame[GT_BRIDGE_RPC_FRAME_MAX+1];size_t n;gt_bridge_message_t m;
  int count=snprintf(payload,sizeof(payload),"{\"requestId\":\"00000000000000000000000000000000\",\"method\":\"%s\",\"input\":%s,\"timeoutMs\":3600000}",r->methods[method].method,input);
  return count>0 && (size_t)count<sizeof(payload) && rpc_frame(r,rpc_streams[0],UINT64_MAX,"request",payload,frame,sizeof(frame),&n) && gt__bridge_wire_parse(r->wire,frame,n,&m)==GT_OK;
}
static bool rpc_validate_outcome(gt_bridge_rpc_t *r,const char *id,const char *outcome,size_t method,gt_bridge_rpc_state_t *state) {
  char payload[GT_BRIDGE_RPC_FRAME_MAX+1],frame[GT_BRIDGE_RPC_FRAME_MAX+1],kind[32];size_t n;gt_bridge_message_t m;
  int count=snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\",\"outcome\":%s}",id,outcome);
  if(count<0 || (size_t)count>=sizeof(payload) || !rpc_frame(r,rpc_streams[1],UINT64_MAX,"response",payload,frame,sizeof(frame),&n) || gt__bridge_wire_parse(r->wire,frame,n,&m)!=GT_OK)return false;
  uint16_t o=gt__bridge_wire_member(r->wire,m.payload,"outcome");if(!rpc_string(r,o,"kind",kind,sizeof(kind)))return false;
  if(!strcmp(kind,"success")){const uint8_t *p;size_t size;if(method==SIZE_MAX || !rpc_raw(r,o,"value",&p,&size) || !r->methods[method].output(r->methods[method].context,p,size))return false;*state=GT_BRIDGE_RPC_SUCCEEDED;}
  else *state=!strcmp(kind,"failed")?GT_BRIDGE_RPC_FAILED:!strcmp(kind,"cancelled")?GT_BRIDGE_RPC_CANCELLED:GT_BRIDGE_RPC_UNKNOWN;
  return true;
}
static void rpc_outcome(rpc_in_t *slot,const char *outcome){strcpy(slot->outcome,outcome);slot->outcome_size=strlen(outcome);slot->done=true;slot->needs_send=true;}
static void rpc_error(rpc_in_t *slot,const char *code){(void)snprintf(slot->outcome,sizeof(slot->outcome),"{\"kind\":\"failed\",\"error\":{\"code\":\"%s\",\"message\":\"request rejected before dispatch\",\"effect\":\"none\"}}",code);slot->outcome_size=strlen(slot->outcome);slot->done=true;slot->needs_send=true;}
static void rpc_local(rpc_out_t *slot,gt_bridge_rpc_state_t state,const char *outcome){slot->state=state;strcpy(slot->outcome,outcome);slot->outcome_size=strlen(outcome);slot->local_terminal=true;}
static void rpc_tick(gt_bridge_rpc_t *r,uint64_t now) {
  for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++) {
    rpc_out_t *o=&r->out[i];if(o->used && !o->confirmed && !o->local_terminal && now>=o->deadline){o->deadline_elapsed=true;if(!o->id[0])rpc_local(o,GT_BRIDGE_RPC_FAILED,rpc_expired);else{o->state=GT_BRIDGE_RPC_UNKNOWN;o->cancel_requested=true;strcpy(o->outcome,rpc_unknown);o->outcome_size=strlen(rpc_unknown);}}
    rpc_in_t *in=&r->in[i];if(in->used && !in->done && now>=in->deadline){in->cancel_requested=true;if(!in->taken)rpc_outcome(in,rpc_expired);}
  }
}
gt_status_t gt__bridge_rpc_new(gt_bridge_session_t *session,gt_bridge_wire_t *wire,const gt_allocator_t *allocator,gt_bridge_rpc_t **out) {
  if(out)*out=NULL;if(!session || !wire || !allocator || !out)return GT_ERR_INVALID_ARG;
  gt_bridge_rpc_t *r=allocator->allocate(allocator->context,sizeof(*r));if(!r)return GT_ERR_IO;memset(r,0,sizeof(*r));r->allocator=*allocator;r->session=session;r->wire=wire;
  r->methods=gt__bridge_session_methods(session,&r->method_count,&r->role);r->credit_pending[0]=r->credit_pending[1]=GT_BRIDGE_RPC_WINDOW;*out=r;return GT_OK;
}
void gt__bridge_rpc_free(gt_bridge_rpc_t *r){if(r){gt_allocator_t a=r->allocator;gt__secure_erase(r,sizeof(*r));a.release(a.context,r,sizeof(*r));}}
void gt__bridge_rpc_end(gt_bridge_rpc_t *r){if(!r || r->ended)return;r->ended=true;for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++){rpc_out_t *o=&r->out[i];if(o->used && !o->confirmed && !o->local_terminal){if(o->id[0])rpc_local(o,GT_BRIDGE_RPC_UNKNOWN,rpc_unknown);else rpc_local(o,GT_BRIDGE_RPC_CANCELLED,rpc_cancelled);}r->in[i].cancel_requested=true;}}
gt_bridge_rpc_status_t gt__bridge_rpc_start(gt_bridge_rpc_t *r,const void *lease,const char *method,const void *input,size_t input_size,uint32_t timeout,uint64_t now,uint64_t *handle) {
  if(handle)*handle=0;if(!r || !handle || !method || strnlen(method,129)>128 || !timeout || timeout>3600000 || timeout>UINT64_MAX-now)return GT_BRIDGE_RPC_INVALID;
  if(!rpc_live(r))return GT_BRIDGE_RPC_CLOSED;size_t m=rpc_method(r,method);
  if(m==SIZE_MAX || r->methods[m].receiver==(uint8_t)r->role)return GT_BRIDGE_RPC_UNSUPPORTED;
  if(!gt__bridge_session_has_capability(r->session,r->methods[m].capability))return GT_BRIDGE_RPC_UNSUPPORTED;
  if(gt__bridge_session_access(r->session,lease,r->methods[m].capability)!=GT_BRIDGE_ACCESS_ALLOWED)return GT_BRIDGE_RPC_DENIED;
  char copy[GT_BRIDGE_RPC_BODY_MAX+1];if(!rpc_json_copy(input,input_size,copy) || !rpc_validate_request(r,m,copy) || !r->methods[m].input(r->methods[m].context,(const uint8_t *)copy,input_size))return GT_BRIDGE_RPC_INVALID;
  rpc_out_t *slot=NULL;for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(!r->out[i].used){slot=&r->out[i];break;}
  if(!slot)return GT_BRIDGE_RPC_WAIT;
  if(r->handles==UINT64_MAX || r->tx_ordinal==UINT64_MAX)return GT_BRIDGE_RPC_UNSUPPORTED;
  memset(slot,0,sizeof(*slot));slot->used=true;slot->handle=++r->handles;slot->method=m;slot->state=GT_BRIDGE_RPC_QUEUED;slot->deadline=now+timeout;memcpy(slot->input,copy,input_size+1);slot->input_size=input_size;*handle=slot->handle;return GT_BRIDGE_RPC_OK;
}
gt_bridge_rpc_status_t gt__bridge_rpc_cancel(gt_bridge_rpc_t *r,const void *lease,uint64_t handle){if(!r || !gt__bridge_session_scope(r->session,lease))return GT_BRIDGE_RPC_DENIED;rpc_out_t *o=r?rpc_handle(r,handle):NULL;if(!o)return GT_BRIDGE_RPC_INVALID;if(o->confirmed || o->local_terminal)return GT_BRIDGE_RPC_OK;o->cancel_requested=true;if(!o->id[0])rpc_local(o,GT_BRIDGE_RPC_CANCELLED,rpc_cancelled);return GT_BRIDGE_RPC_OK;}
bool gt__bridge_rpc_result(gt_bridge_rpc_t *r,const void *lease,uint64_t handle,uint64_t now,gt_bridge_rpc_result_t *out) {
  if(out)memset(out,0,sizeof(*out));if(!r || !gt__bridge_session_scope(r->session,lease))return false;rpc_tick(r,now);rpc_out_t *o=rpc_handle(r,handle);if(!o || !out)return false;
  *out=(gt_bridge_rpc_result_t){.handle=handle,.state=o->state,.cancel_requested=o->cancel_requested,.deadline_elapsed=o->deadline_elapsed,.confirmed=o->confirmed,.outcome=(const uint8_t *)o->outcome,.outcome_size=o->outcome_size};memcpy(out->request_id,o->id,33);
  if(!rpc_live(r) && !o->confirmed && !o->local_terminal){out->state=o->id[0]?GT_BRIDGE_RPC_UNKNOWN:GT_BRIDGE_RPC_CANCELLED;out->outcome=(const uint8_t *)(o->id[0]?rpc_unknown:rpc_cancelled);out->outcome_size=strlen((const char *)out->outcome);}return true;
}
gt_bridge_rpc_status_t gt__bridge_rpc_forget(gt_bridge_rpc_t *r,const void *lease,uint64_t handle){if(!r || !gt__bridge_session_scope(r->session,lease))return GT_BRIDGE_RPC_DENIED;rpc_out_t *o=r?rpc_handle(r,handle):NULL;if(!o)return GT_BRIDGE_RPC_INVALID;if(!o->confirmed && !o->local_terminal && o->state!=GT_BRIDGE_RPC_UNKNOWN)return GT_BRIDGE_RPC_WAIT;gt__secure_erase(o,sizeof(*o));return GT_BRIDGE_RPC_OK;}
gt_bridge_rpc_status_t gt__bridge_rpc_check(gt_bridge_rpc_t *r,const void *lease,uint64_t ticket,uint64_t now){if(!r || !rpc_live(r))return GT_BRIDGE_RPC_CLOSED;rpc_tick(r,now);rpc_in_t *in=rpc_ticket(r,ticket);if(!in || in->method==SIZE_MAX)return GT_BRIDGE_RPC_INVALID;if(in->done || in->cancel_requested)return GT_BRIDGE_RPC_CLOSED;return gt__bridge_session_access(r->session,lease,r->methods[in->method].capability)==GT_BRIDGE_ACCESS_ALLOWED?GT_BRIDGE_RPC_OK:GT_BRIDGE_RPC_DENIED;}
gt_bridge_rpc_status_t gt__bridge_rpc_take(gt_bridge_rpc_t *r,const void *lease,uint64_t now,gt_bridge_rpc_request_t *out) {
  if(out)memset(out,0,sizeof(*out));if(!r || !out)return GT_BRIDGE_RPC_INVALID;if(!rpc_live(r))return GT_BRIDGE_RPC_CLOSED;rpc_tick(r,now);rpc_in_t *slot=NULL;
  for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++){rpc_in_t *p=&r->in[i];if(p->used && !p->done && !p->taken && (!slot || p->ticket<slot->ticket))slot=p;}
  if(!slot)return GT_BRIDGE_RPC_WAIT;if(gt__bridge_rpc_check(r,lease,slot->ticket,now)!=GT_BRIDGE_RPC_OK)return GT_BRIDGE_RPC_DENIED;
  slot->taken=true;out->ticket=slot->ticket;memcpy(out->request_id,slot->id,33);strcpy(out->method,slot->method_name);out->input=(const uint8_t *)slot->input;out->input_size=slot->input_size;return GT_BRIDGE_RPC_OK;
}
gt_bridge_rpc_status_t gt__bridge_rpc_respond(gt_bridge_rpc_t *r,const void *lease,uint64_t ticket,const void *outcome,size_t size,uint64_t now) {
  if(!r || !rpc_live(r))return GT_BRIDGE_RPC_CLOSED;if(!gt__bridge_session_scope(r->session,lease))return GT_BRIDGE_RPC_DENIED;rpc_tick(r,now);rpc_in_t *slot=rpc_ticket(r,ticket);if(!slot || !slot->taken || slot->done)return GT_BRIDGE_RPC_INVALID;
  char copy[GT_BRIDGE_RPC_BODY_MAX+1];gt_bridge_rpc_state_t state;if(!rpc_json_copy(outcome,size,copy) || !rpc_validate_outcome(r,slot->id,copy,slot->method,&state))return GT_BRIDGE_RPC_INVALID;
  rpc_outcome(slot,copy);return GT_BRIDGE_RPC_OK;
}
static gt_status_t rpc_request_received(gt_bridge_rpc_t *r,const gt_bridge_message_t *m,uint32_t cost,uint64_t now,bool old_seq) {
  char id[33],method[129];const uint8_t *p;size_t n;uint64_t ordinal,timeout;
  if(!rpc_string(r,m->payload,"requestId",id,33) || !rpc_string(r,m->payload,"method",method,129) || !rpc_ordinal(r,id,false,&ordinal) ||
     !rpc_raw(r,m->payload,"input",&p,&n) || n>GT_BRIDGE_RPC_BODY_MAX || !gt__bridge_wire_uint(r->wire,gt__bridge_wire_member(r->wire,m->payload,"timeoutMs"),&timeout) || timeout>UINT64_MAX-now)return GT_ERR_PROTOCOL;
  rpc_in_t *slot=rpc_in_id(r,id);
  if(slot){if(strcmp(slot->method_name,method) || slot->input_size!=n || memcmp(slot->input,p,n))return GT_ERR_PROTOCOL;slot->reserved+=cost;if(slot->done)slot->needs_send=true;return GT_OK;}
  if(old_seq && ordinal>r->rx_ordinal)return GT_ERR_PROTOCOL;
  if(ordinal>r->rx_ordinal && (r->rx_ordinal==UINT64_MAX || ordinal!=r->rx_ordinal+1))return GT_ERR_PROTOCOL;
  for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(!r->in[i].used){slot=&r->in[i];break;}
  if(!slot)for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++){rpc_in_t *candidate=&r->in[i];if(candidate->done && !candidate->needs_send && !candidate->reserved && (r->inflight!=RPC_TX_RESPONSE || r->inflight_slot!=i) && (!slot || candidate->ticket<slot->ticket))slot=candidate;}
  if(!slot || r->tickets==UINT64_MAX)return GT_ERR_PROTOCOL;
  memset(slot,0,sizeof(*slot));slot->used=true;slot->ticket=++r->tickets;slot->ordinal=ordinal;memcpy(slot->id,id,33);strcpy(slot->method_name,method);memcpy(slot->input,p,n);slot->input[n]=0;slot->input_size=n;slot->reserved=cost;slot->deadline=now+timeout;slot->method=rpc_method(r,method);
  if(ordinal<=r->rx_ordinal){rpc_outcome(slot,"{\"kind\":\"unknown\",\"reason\":\"request receipt retired; never dispatched again\"}");return GT_OK;}
  r->rx_ordinal=ordinal;
  if(slot->method==SIZE_MAX || r->methods[slot->method].receiver!=(uint8_t)r->role){rpc_error(slot,"UNSUPPORTED");return GT_OK;}
  if(!gt__bridge_session_has_capability(r->session,r->methods[slot->method].capability)){rpc_error(slot,"UNSUPPORTED");return GT_OK;}
  gt_bridge_lease_t lease;if(!gt__bridge_session_lease(r->session,&lease) || gt__bridge_session_access(r->session,&lease,r->methods[slot->method].capability)!=GT_BRIDGE_ACCESS_ALLOWED){rpc_error(slot,"DENIED");return GT_OK;}
  if(!r->methods[slot->method].input(r->methods[slot->method].context,(const uint8_t *)slot->input,n))rpc_error(slot,"INVALID_INPUT");return GT_OK;
}
static gt_status_t rpc_response_received(gt_bridge_rpc_t *r,const gt_bridge_message_t *m,uint32_t cost) {
  char id[33],copy[GT_BRIDGE_RPC_BODY_MAX+1];const uint8_t *p;size_t n;uint64_t ordinal;
  if(!rpc_string(r,m->payload,"requestId",id,33) || !rpc_ordinal(r,id,true,&ordinal) || ordinal>r->tx_ordinal || !rpc_raw(r,m->payload,"outcome",&p,&n) || !rpc_json_copy(p,n,copy))return GT_ERR_PROTOCOL;
  rpc_out_t *slot=rpc_out_id(r,id);
  if(slot){if(slot->confirmed){if(slot->outcome_size!=n || memcmp(slot->outcome,p,n))return GT_ERR_PROTOCOL;}else{gt_bridge_rpc_state_t state;if(!rpc_validate_outcome(r,id,copy,slot->method,&state))return GT_ERR_PROTOCOL;memcpy(slot->outcome,copy,n+1);slot->outcome_size=n;slot->state=state;slot->confirmed=true;}}
  r->credit_pending[1]+=cost;return GT_OK;
}
gt_status_t gt__bridge_rpc_receive(gt_bridge_rpc_t *r,const gt_bridge_message_t *m,size_t size,uint64_t now) {
  if(!r || !rpc_live(r))return GT_ERR_CANCELLED;rpc_tick(r,now);
  if(!strcmp(m->stream,"control")) {
    if(m->kind==GT_BRIDGE_CREDIT){char stream[129];uint64_t bytes;if(!rpc_string(r,m->payload,"streamId",stream,129) || !gt__bridge_wire_uint(r->wire,gt__bridge_wire_member(r->wire,m->payload,"bytes"),&bytes))return GT_ERR_PROTOCOL;for(unsigned i=0;i<RPC_FLOWS;i++)if(!strcmp(stream,rpc_streams[i])){if(bytes>GT_BRIDGE_RPC_WINDOW-r->tx_credit[i])return GT_ERR_PROTOCOL;r->tx_credit[i]+=(uint32_t)bytes;return GT_OK;}return GT_ERR_UNSUPPORTED;}
    if(m->kind==GT_BRIDGE_CANCEL){char id[33];uint64_t ordinal;if(!rpc_string(r,m->payload,"requestId",id,33) || !rpc_ordinal(r,id,false,&ordinal) || ordinal>r->rx_ordinal)return GT_ERR_PROTOCOL;rpc_in_t *slot=rpc_in_id(r,id);if(slot && !slot->done){slot->cancel_requested=true;if(!slot->taken)rpc_outcome(slot,rpc_cancelled);}return GT_OK;}
    return GT_ERR_UNSUPPORTED;
  }
  unsigned flow=!strcmp(m->stream,rpc_streams[0])?0:!strcmp(m->stream,rpc_streams[1])?1:RPC_FLOWS;
  if(flow==RPC_FLOWS || (flow==0?m->kind!=GT_BRIDGE_REQUEST:m->kind!=GT_BRIDGE_RESPONSE))return GT_ERR_UNSUPPORTED;
  if(size>rpc_limit(r) || size+4<128 || size+4>r->rx_credit[flow])return GT_ERR_PROTOCOL;
  bool old=m->sequence<r->rx_seq[flow] || (r->rx_exhausted[flow] && m->sequence==UINT64_MAX);
  if(!old && (r->rx_exhausted[flow] || m->sequence!=r->rx_seq[flow]))return GT_ERR_PROTOCOL;
  r->rx_credit[flow]-=(uint32_t)(size+4);
  gt_status_t status=flow==0?rpc_request_received(r,m,(uint32_t)(size+4),now,old):rpc_response_received(r,m,(uint32_t)(size+4));
  if(status==GT_OK && !old){if(r->rx_seq[flow]==UINT64_MAX)r->rx_exhausted[flow]=true;else r->rx_seq[flow]++;}return status;
}
gt_status_t gt__bridge_rpc_prepare(gt_bridge_rpc_t *r,uint64_t control_seq,uint64_t now,char *out,size_t capacity,size_t *size,bool *control) {
  *size=0;*control=false;if(!r || !rpc_live(r) || r->inflight!=RPC_TX_NONE)return GT_ERR_INVALID_ARG;rpc_tick(r,now);r->prepared=RPC_TX_NONE;
  char payload[GT_BRIDGE_RPC_FRAME_MAX+1];const char *kind=NULL;unsigned flow=0;
  for(unsigned i=0;i<RPC_FLOWS;i++)if(r->credit_pending[i]){r->prepared=RPC_TX_CREDIT;r->grant_flow=i;r->grant=r->credit_pending[i];(void)snprintf(payload,sizeof(payload),"{\"bytes\":%u,\"streamId\":\"%s\"}",r->grant,rpc_streams[i]);kind="credit";*control=true;break;}
  if(!kind)for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++){rpc_out_t *p=&r->out[i];if(p->used && p->id[0] && p->cancel_requested && !p->cancel_sent && !p->confirmed){r->prepared=RPC_TX_CANCEL;r->selected=i;(void)snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\"}",p->id);kind="cancel";*control=true;break;}}
  if(!kind)for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++){rpc_in_t *p=&r->in[i];if(p->used && p->done && p->needs_send){r->prepared=RPC_TX_RESPONSE;r->selected=i;(void)snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\",\"outcome\":%s}",p->id,p->outcome);kind="response";flow=1;break;}}
  if(!kind){rpc_out_t *slot=NULL;for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++){rpc_out_t *p=&r->out[i];if(p->used && p->state==GT_BRIDGE_RPC_QUEUED && !p->local_terminal && (!slot || p->handle<slot->handle)){slot=p;r->selected=i;}}if(slot){if(r->tx_ordinal==UINT64_MAX)return GT_ERR_UNSUPPORTED;rpc_id(r,r->tx_ordinal+1,r->candidate_id);r->prepared=RPC_TX_REQUEST;(void)snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\",\"method\":\"%s\",\"input\":%s,\"timeoutMs\":%" PRIu64 "}",r->candidate_id,r->methods[slot->method].method,slot->input,slot->deadline-now);kind="request";}}
  if(!kind)return GT_OK;
  if(!*control && r->tx_exhausted[flow])return GT_ERR_UNSUPPORTED;
  if(!rpc_frame(r,*control?"control":rpc_streams[flow],*control?control_seq:r->tx_seq[flow],kind,payload,out,capacity,size))return GT_ERR_PROTOCOL;
  r->cost=(uint32_t)(*size+4);
  if(!*control && r->cost>r->tx_credit[flow]){*size=0;r->prepared=RPC_TX_NONE;return GT_OK;}
  return GT_OK;
}
void gt__bridge_rpc_commit(gt_bridge_rpc_t *r) {
  r->inflight=r->prepared;r->inflight_slot=r->selected;
  if(r->prepared==RPC_TX_CREDIT)r->credit_pending[r->grant_flow]-=r->grant;
  else if(r->prepared==RPC_TX_CANCEL)r->out[r->selected].cancel_sent=true;
  else {unsigned flow=r->prepared==RPC_TX_REQUEST?0:1;r->tx_credit[flow]-=r->cost;if(r->tx_seq[flow]==UINT64_MAX)r->tx_exhausted[flow]=true;else r->tx_seq[flow]++;
    if(r->prepared==RPC_TX_REQUEST){rpc_out_t *slot=&r->out[r->selected];memcpy(slot->id,r->candidate_id,33);slot->state=GT_BRIDGE_RPC_PENDING;r->tx_ordinal++;}}
}
bool gt__bridge_rpc_current(gt_bridge_rpc_t *r,uint64_t now){rpc_tick(r,now);if(r->prepared==RPC_TX_REQUEST && r->out[r->selected].local_terminal){r->prepared=RPC_TX_NONE;return false;}return true;}
void gt__bridge_rpc_sent(gt_bridge_rpc_t *r) {
  if(r->inflight==RPC_TX_CREDIT)r->rx_credit[r->grant_flow]+=r->grant;
  if(r->inflight==RPC_TX_RESPONSE){rpc_in_t *p=&r->in[r->inflight_slot];r->credit_pending[0]+=p->reserved;p->reserved=0;p->needs_send=false;}
  r->inflight=RPC_TX_NONE;r->prepared=RPC_TX_NONE;
}

bool gt__bridge_rpc_has_owners(const gt_bridge_rpc_t *r){if(!r)return false;for(size_t i=0;i<GT_BRIDGE_RPC_SLOTS;i++)if(r->out[i].used || (r->in[i].used && r->in[i].taken && !r->in[i].done))return true;return false;}
