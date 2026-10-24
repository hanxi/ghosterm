#include "client-bootstrap.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef enum {SEND_HELLO,FLUSH_HELLO,WAIT_HELLO,SEND_HELLO_ACK,FLUSH_HELLO_ACK,
  SEND_BIND,FLUSH_BIND,WAIT_BIND,SEND_BIND_ACK,FLUSH_BIND_ACK,REJECTION} stage;
struct gt_client_bootstrap {
  gt_allocator_t allocator;gt_connection_t *connection;gt_client_bootstrap_config_t config;
  gt_bootstrap_info_t info;gt_bootstrap_result_t result;stage stage;
  uint64_t now,started;uint8_t challenge[GT_CHALLENGE_BYTES];bool rejected;
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool nonzero(const void *memory,size_t n){const uint8_t *p=memory;uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static void fail(gt_client_bootstrap_t *c,gt_bootstrap_error_t error) {
  if(c->info.state==GT_BOOTSTRAP_FAILED || c->info.state==GT_BOOTSTRAP_TAKEN)return;
  c->info.state=GT_BOOTSTRAP_FAILED;c->info.error=error;gt__connection_cancel(c->connection);
  gt__secure_erase(&c->config.binding.token,sizeof(c->config.binding.token));gt__secure_erase(c->challenge,sizeof(c->challenge));
}
static bool tick(gt_client_bootstrap_t *c) {
  uint64_t now;if(!c->config.clock(c->config.clock_context,&now) || now<c->now){fail(c,GT_BOOTSTRAP_CLOCK);return false;}
  c->now=now;
  if(c->info.state!=GT_BOOTSTRAP_READY && now-c->started>=c->config.timeout_ms){fail(c,c->rejected?c->info.error:GT_BOOTSTRAP_TIMEOUT);return false;}
  return true;
}
static gt_bootstrap_error_t rejection(gt_hello_status_t status) {
  switch(status){case GT_HELLO_INCOMPATIBLE:return GT_BOOTSTRAP_COMPATIBILITY;
    case GT_HELLO_UNSUPPORTED:return GT_BOOTSTRAP_FEATURES;case GT_HELLO_WRONG_INSTANCE:return GT_BOOTSTRAP_INSTANCE;
    default:return GT_BOOTSTRAP_DENIED;}
}
static bool receive(gt_client_bootstrap_t *c) {
  gt_wire_frame_t f;if(!gt__connection_frame(c->connection,&f))return true;
  if(c->stage==REJECTION)return gt__connection_release_frame(c->connection);
  if(f.header.flags)return false;
  if(c->stage==WAIT_HELLO) {
    gt_protocol_hello_reply_t r;
    if(f.header.type!=GT_MSG_HELLO_REPLY || f.header.request_id!=(c->config.hello.role==GT_ROLE_MANAGEMENT?1u:0u) ||
      !gt__hello_reply_decode(f.payload,f.header.payload_length,&r) || r.role!=c->config.hello.role)return false;
    c->info.has_status=true;c->info.status=r.status;
    if(r.status==GT_HELLO_OK) {
      if(memcmp(r.identity.compatibility,c->config.hello.compatibility,GT_COMPAT_BYTES)){fail(c,GT_BOOTSTRAP_COMPATIBILITY);return true;}
      if(c->config.hello.required_features&~r.identity.features){fail(c,GT_BOOTSTRAP_FEATURES);return true;}
      if(nonzero(c->config.hello.expected_instance,GT_INSTANCE_BYTES) && memcmp(r.identity.instance,c->config.hello.expected_instance,GT_INSTANCE_BYTES)){fail(c,GT_BOOTSTRAP_INSTANCE);return true;}
      c->result.identity=r.identity;c->result.role=r.role;
    } else {c->rejected=true;c->info.error=rejection(r.status);c->info.state=GT_BOOTSTRAP_DRAINING;}
    memcpy(c->challenge,r.challenge,sizeof(c->challenge));c->stage=SEND_HELLO_ACK;
  } else if(c->stage==WAIT_BIND) {
    gt_protocol_bind_reply_t r;
    if(f.header.type!=GT_MSG_BIND_REPLY || f.header.request_id || !gt__bind_reply_decode(f.payload,f.header.payload_length,&r) ||
      r.session!=c->config.binding.session || r.generation!=c->config.binding.generation || (r.status!=GT_HELLO_OK && r.status!=GT_HELLO_DENIED))return false;
    c->info.has_status=true;c->info.status=r.status;
    if(r.status!=GT_HELLO_OK){c->rejected=true;c->info.error=GT_BOOTSTRAP_DENIED;c->info.state=GT_BOOTSTRAP_DRAINING;}
    else {c->result.session=r.session;c->result.generation=r.generation;}
    memcpy(c->challenge,r.challenge,sizeof(c->challenge));c->stage=SEND_BIND_ACK;
  } else return false;
  return gt__connection_release_frame(c->connection);
}
static bool send(gt_client_bootstrap_t *c,uint16_t type,uint64_t request,const void *bytes,size_t n) {
  gt_wire_header_t h={.type=type,.request_id=request,.payload_length=(uint32_t)n};
  if(gt__connection_send(c->connection,&h,bytes)!=GT_SEND_ACCEPTED){fail(c,GT_BOOTSTRAP_IO);return false;}
  return true;
}
static void output(gt_client_bootstrap_t *c) {
  if(gt__connection_wants_write(c->connection))return;
  uint8_t bytes[GT_BIND_BYTES]={0};
  if(c->stage==SEND_HELLO) {
    if(!gt__hello_encode(&c->config.hello,bytes)){fail(c,GT_BOOTSTRAP_PROTOCOL);return;}
    if(send(c,GT_MSG_HELLO,c->config.hello.role==GT_ROLE_MANAGEMENT?1:0,bytes,GT_HELLO_BYTES))c->stage=FLUSH_HELLO;
  } else if(c->stage==SEND_BIND) {
    if(!gt__bind_encode(&c->config.binding,bytes)){fail(c,GT_BOOTSTRAP_PROTOCOL);return;}
    if(send(c,GT_MSG_BIND,0,bytes,GT_BIND_BYTES))c->stage=FLUSH_BIND;
    gt__secure_erase(&c->config.binding.token,sizeof(c->config.binding.token));
  } else if(c->stage==SEND_HELLO_ACK || c->stage==SEND_BIND_ACK) {
    bool hello=c->stage==SEND_HELLO_ACK;
    if(send(c,hello?GT_MSG_HELLO_ACK:GT_MSG_BIND_ACK,0,c->challenge,sizeof(c->challenge)))c->stage=hello?FLUSH_HELLO_ACK:FLUSH_BIND_ACK;
    gt__secure_erase(c->challenge,sizeof(c->challenge));
  }
  gt__secure_erase(bytes,sizeof(bytes));
}
gt_status_t gt__client_bootstrap_open(gt_connection_t *connection,const gt_client_bootstrap_config_t *config,
  const gt_allocator_t *allocator,gt_client_bootstrap_t **out) {
  if(out)*out=NULL;
  gt_connection_limits_t limits={0};uint8_t bytes[GT_BIND_BYTES]={0};
  if(!out || !connection || !config || !config->clock || !config->timeout_ms || config->timeout_ms>60000 ||
    gt__connection_state(connection)!=GT_CONNECTION_OPEN || gt__connection_wants_write(connection) ||
    !gt__connection_limits(connection,&limits) || limits.receive_limit<80 || limits.send_limit<80 || !gt__hello_encode(&config->hello,bytes))return GT_ERR_INVALID_ARG;
  bool valid=config->hello.role==GT_ROLE_DATA?
    (!memcmp(config->hello.expected_instance,config->binding.instance,GT_INSTANCE_BYTES) && gt__bind_encode(&config->binding,bytes)):
    (!config->binding.session && !config->binding.generation && !nonzero(config->binding.instance,GT_INSTANCE_BYTES) && !nonzero(config->binding.token.bytes,GT_TOKEN_BYTES));
  gt__secure_erase(bytes,sizeof(bytes));if(!valid)return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  uint64_t now;if(!config->clock(config->clock_context,&now))return GT_ERR_IO;
  gt_client_bootstrap_t *c=a.allocate(a.context,sizeof(*c));if(!c)return GT_ERR_IO;
  memset(c,0,sizeof(*c));c->allocator=a;c->connection=connection;c->config=*config;c->now=c->started=now;*out=c;return GT_OK;
}
gt_bootstrap_state_t gt__client_bootstrap_step(gt_client_bootstrap_t *c) {
  if(!c)return GT_BOOTSTRAP_FAILED;
  if(c->info.state==GT_BOOTSTRAP_FAILED || c->info.state==GT_BOOTSTRAP_TAKEN || !tick(c))return c->info.state;
  gt_connection_state_t state=gt__connection_step(c->connection);
  if(state!=GT_CONNECTION_OPEN){fail(c,c->rejected?c->info.error:state==GT_CONNECTION_CLOCK_ERROR?GT_BOOTSTRAP_CLOCK:state==GT_CONNECTION_PROTOCOL_ERROR?GT_BOOTSTRAP_PROTOCOL:state==GT_CONNECTION_TIMEOUT?GT_BOOTSTRAP_TIMEOUT:GT_BOOTSTRAP_IO);return c->info.state;}
  if(!tick(c))return c->info.state;
  if(c->info.state==GT_BOOTSTRAP_READY)return c->info.state;
  if(!gt__connection_wants_write(c->connection)) {
    if(c->stage==FLUSH_HELLO)c->stage=WAIT_HELLO;
    else if(c->stage==FLUSH_BIND)c->stage=WAIT_BIND;
    else if(c->stage==FLUSH_HELLO_ACK || c->stage==FLUSH_BIND_ACK) {
      if(c->rejected)c->stage=REJECTION;
      else if(c->stage==FLUSH_BIND_ACK || c->config.hello.role==GT_ROLE_MANAGEMENT){c->info.state=GT_BOOTSTRAP_READY;return c->info.state;}
      else c->stage=SEND_BIND;
    }
  }
  if(!receive(c)){fail(c,GT_BOOTSTRAP_PROTOCOL);return c->info.state;}
  if(c->info.state!=GT_BOOTSTRAP_FAILED && tick(c))output(c);
  return c->info.state;
}
bool gt__client_bootstrap_info(const gt_client_bootstrap_t *c,gt_bootstrap_info_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!c || !out)return false;
  *out=c->info;return true;
}
bool gt__client_bootstrap_take(gt_client_bootstrap_t *c,gt_connection_t **connection,gt_bootstrap_result_t *out) {
  if(connection)*connection=NULL;
  if(out)memset(out,0,sizeof(*out));
  if(!c || !connection || !out || gt__client_bootstrap_step(c)!=GT_BOOTSTRAP_READY)return false;
  *out=c->result;*connection=c->connection;c->connection=NULL;c->info.state=GT_BOOTSTRAP_TAKEN;return true;
}
void gt__client_bootstrap_cancel(gt_client_bootstrap_t *c){if(c)fail(c,GT_BOOTSTRAP_CANCELLED);}
void gt__client_bootstrap_close(gt_client_bootstrap_t *c) {
  if(!c)return;
  if(c->info.state!=GT_BOOTSTRAP_TAKEN){gt__client_bootstrap_cancel(c);gt__connection_destroy(c->connection);}
  gt_allocator_t a=c->allocator;gt__secure_erase(c,sizeof(*c));a.release(a.context,c,sizeof(*c));
}
