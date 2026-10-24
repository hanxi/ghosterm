#include "handshake.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static bool equal(const uint8_t *a,const uint8_t *b,size_t n){volatile uint8_t d=0;for(size_t i=0;i<n;i++)d|=a[i]^b[i];return d==0;}
static gt_status_t random_fill(const gt_token_random_t *r,uint8_t *out,size_t n) {
  gt_status_t status=r && r->fill?r->fill(r->context,out,n):gt__secure_random(out,n);
  if(status!=GT_OK || !nonzero(out,n)){gt__secure_erase(out,n);return status==GT_ERR_UNSUPPORTED?status:GT_ERR_IO;}
  return GT_OK;
}
struct gt_handshake {
  gt_allocator_t allocator;gt_connection_t *connection;gt_handshake_config_t config;
  gt_handshake_state_t state;gt_handshake_error_t error,pending_error;
  gt_protocol_role_t role;gt_token_scope_t binding;bool bound;
  uint64_t started,now;uint8_t challenge[GT_CHALLENGE_BYTES];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static void drop_binding(gt_handshake_t *h) {
  if(h->bound){h->bound=false;h->config.binding_drop(h->config.binding_context,&h->binding);gt__secure_erase(&h->binding,sizeof(h->binding));}
}
static void fail(gt_handshake_t *h,gt_handshake_error_t error) {
  if(h->state==GT_HANDSHAKE_FAILED || h->state==GT_HANDSHAKE_TAKEN)return;
  h->error=error;h->state=GT_HANDSHAKE_FAILED;drop_binding(h);gt__connection_cancel(h->connection);
}
gt_status_t gt__handshake_open(gt_connection_t *c,const gt_handshake_config_t *config,const gt_allocator_t *allocator,gt_handshake_t **out) {
  if(out)*out=NULL;
  if(!out || !c || gt__connection_state(c)!=GT_CONNECTION_OPEN || !config || !config->principal ||
     !config->clock || !config->timeout_ms || config->timeout_ms>60000 ||
     !nonzero(config->identity.instance,GT_INSTANCE_BYTES) || !nonzero(config->identity.compatibility,GT_COMPAT_BYTES) ||
     (config->tokens && (!config->binding_current || !config->binding_drop)))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  gt_handshake_t *h=a.allocate(a.context,sizeof(*h));if(!h)return GT_ERR_INTERNAL;
  memset(h,0,sizeof(*h));h->allocator=a;h->connection=c;h->config=*config;
  if(!config->clock(config->clock_context,&h->now)){a.release(a.context,h,sizeof(*h));return GT_ERR_IO;}
  h->started=h->now;*out=h;return GT_OK;
}
static bool challenge(gt_handshake_t *h) {
  if(random_fill(&h->config.random,h->challenge,sizeof(h->challenge))!=GT_OK){fail(h,GT_HANDSHAKE_ENTROPY);return false;}
  return true;
}
static bool send_reply(gt_handshake_t *h,uint16_t type,uint64_t request,const uint8_t *bytes,uint32_t n) {
  gt_wire_header_t header={.type=type,.payload_length=n,.request_id=request};
  if(gt__connection_send(h->connection,&header,bytes)!=GT_SEND_ACCEPTED){fail(h,GT_HANDSHAKE_IO);return false;}
  return true;
}
static void hello(gt_handshake_t *h,const gt_wire_frame_t *frame) {
  gt_protocol_hello_t request;
  if(frame->header.type!=GT_MSG_HELLO || frame->header.flags || !gt__hello_decode(frame->payload,frame->header.payload_length,&request) ||
     (request.role==GT_ROLE_MANAGEMENT?!frame->header.request_id:frame->header.request_id!=0)) {fail(h,GT_HANDSHAKE_PROTOCOL);return;}
  h->role=request.role;gt_hello_status_t status=GT_HELLO_OK;
  if(!equal(request.compatibility,h->config.identity.compatibility,GT_COMPAT_BYTES)){status=GT_HELLO_INCOMPATIBLE;h->pending_error=GT_HANDSHAKE_COMPATIBILITY;}
  else if(request.required_features&~h->config.identity.features){status=GT_HELLO_UNSUPPORTED;h->pending_error=GT_HANDSHAKE_FEATURES;}
  else if(nonzero(request.expected_instance,GT_INSTANCE_BYTES) && !equal(request.expected_instance,h->config.identity.instance,GT_INSTANCE_BYTES)) {status=GT_HELLO_WRONG_INSTANCE;h->pending_error=GT_HANDSHAKE_INSTANCE;}
  else if(request.role==GT_ROLE_DATA && !h->config.tokens){status=GT_HELLO_UNSUPPORTED;h->pending_error=GT_HANDSHAKE_FEATURES;}
  if(!challenge(h))return;
  uint8_t bytes[GT_HELLO_REPLY_BYTES]={0};put32(bytes,status);bytes[5]=(uint8_t)h->role;
  memcpy(bytes+8,h->config.identity.compatibility,GT_COMPAT_BYTES);put64(bytes+40,h->config.identity.features);
  memcpy(bytes+48,h->config.identity.instance,GT_INSTANCE_BYTES);memcpy(bytes+64,h->challenge,GT_CHALLENGE_BYTES);
  if(send_reply(h,GT_MSG_HELLO_REPLY,frame->header.request_id,bytes,sizeof(bytes)))h->state=GT_HANDSHAKE_HELLO_ACK;
}
static void bind_request(gt_handshake_t *h,const gt_wire_frame_t *frame) {
  if(frame->header.type!=GT_MSG_BIND || frame->header.request_id || frame->header.flags || frame->header.payload_length!=GT_BIND_BYTES){fail(h,GT_HANDSHAKE_PROTOCOL);return;}
  const uint8_t *p=frame->payload;uint64_t session=get64(p+16),generation=get64(p+24);
  if(!session || !generation){fail(h,GT_HANDSHAKE_PROTOCOL);return;}
  gt_attachment_token_t token;memcpy(token.bytes,p+32,GT_TOKEN_BYTES);
  gt_token_rc_t result=gt__tokens_consume(h->config.tokens,&token,p,h->config.principal,session,generation,h->now,&h->binding);
  gt__secure_erase(&token,sizeof(token));
  if(result==GT_TOKEN_CLOCK){fail(h,GT_HANDSHAKE_CLOCK);return;}
  h->bound=result==GT_TOKEN_OK;
  if(h->bound && !h->config.binding_current(h->config.binding_context,&h->binding))drop_binding(h);
  gt_hello_status_t status=h->bound?GT_HELLO_OK:GT_HELLO_DENIED;
  if(!h->bound)h->pending_error=GT_HANDSHAKE_TOKEN;
  if(!challenge(h))return;
  uint8_t bytes[GT_BIND_REPLY_BYTES]={0};put32(bytes,status);put64(bytes+4,session);put64(bytes+12,generation);
  memcpy(bytes+20,h->challenge,GT_CHALLENGE_BYTES);
  if(send_reply(h,GT_MSG_BIND_REPLY,0,bytes,sizeof(bytes)))h->state=GT_HANDSHAKE_BIND_ACK;
}
gt_handshake_state_t gt__handshake_step(gt_handshake_t *h) {
  if(!h)return GT_HANDSHAKE_FAILED;
  if(h->state==GT_HANDSHAKE_FAILED || h->state==GT_HANDSHAKE_TAKEN)return h->state;
  uint64_t now;
  if(!h->config.clock(h->config.clock_context,&now) || now<h->now){fail(h,GT_HANDSHAKE_CLOCK);return h->state;}
  h->now=now;
  if(now-h->started>=h->config.timeout_ms){fail(h,GT_HANDSHAKE_TIMEOUT);return h->state;}
  if(h->config.tokens && gt__tokens_expire(h->config.tokens,now)!=GT_TOKEN_OK){fail(h,GT_HANDSHAKE_CLOCK);return h->state;}
  if(h->bound && !h->config.binding_current(h->config.binding_context,&h->binding)){fail(h,GT_HANDSHAKE_TOKEN);return h->state;}
  gt_connection_state_t state=gt__connection_step(h->connection);
  if(state!=GT_CONNECTION_OPEN) {
    gt_handshake_error_t reason=state==GT_CONNECTION_TIMEOUT?GT_HANDSHAKE_TIMEOUT:state==GT_CONNECTION_CLOCK_ERROR?GT_HANDSHAKE_CLOCK:state==GT_CONNECTION_PROTOCOL_ERROR?GT_HANDSHAKE_PROTOCOL:GT_HANDSHAKE_IO;
    fail(h,h->pending_error?h->pending_error:reason);return h->state;
  }
  if(h->state==GT_HANDSHAKE_READY)return h->state;
  if(gt__connection_wants_write(h->connection))return h->state;
  gt_wire_frame_t frame;if(!gt__connection_frame(h->connection,&frame))return h->state;
  if(h->state==GT_HANDSHAKE_HELLO)hello(h,&frame);
  else if(h->state==GT_HANDSHAKE_BIND)bind_request(h,&frame);
  else {
    uint16_t type=h->state==GT_HANDSHAKE_HELLO_ACK?GT_MSG_HELLO_ACK:GT_MSG_BIND_ACK;
    if(frame.header.type!=type || frame.header.flags || frame.header.request_id || frame.header.payload_length!=GT_CHALLENGE_BYTES || !equal(frame.payload,h->challenge,GT_CHALLENGE_BYTES))fail(h,GT_HANDSHAKE_PROTOCOL);
    else if(h->pending_error)fail(h,h->pending_error);
    else h->state=h->role==GT_ROLE_MANAGEMENT || h->state==GT_HANDSHAKE_BIND_ACK?GT_HANDSHAKE_READY:GT_HANDSHAKE_BIND;
  }
  if(h->state!=GT_HANDSHAKE_FAILED && !gt__connection_release_frame(h->connection))fail(h,GT_HANDSHAKE_PROTOCOL);
  return h->state;
}
gt_handshake_error_t gt__handshake_error(const gt_handshake_t *h){return h?h->error:GT_HANDSHAKE_PROTOCOL;}
bool gt__handshake_take(gt_handshake_t *h,gt_protocol_role_t *role,gt_token_scope_t *binding) {
  if(role)*role=0;
  if(binding)memset(binding,0,sizeof(*binding));
  if(!h || !role || !binding || gt__handshake_step(h)!=GT_HANDSHAKE_READY)return false;
  *role=h->role;if(h->bound)*binding=h->binding;
  h->bound=false;h->state=GT_HANDSHAKE_TAKEN;return true;
}
void gt__handshake_close(gt_handshake_t *h) {
  if(!h)return;
  if(h->state!=GT_HANDSHAKE_TAKEN)fail(h,GT_HANDSHAKE_IO);
  gt_allocator_t a=h->allocator;gt__secure_erase(h,sizeof(*h));a.release(a.context,h,sizeof(*h));
}
