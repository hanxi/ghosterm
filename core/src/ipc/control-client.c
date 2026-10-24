#include "control-client.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
struct gt_control_client {
  gt_allocator_t allocator;gt_connection_t *connection;gt_control_client_config_t config;
  gt_control_client_state_t state;gt_status_t error;
  uint64_t now,last_activity,started,last_id;uint16_t pending;bool automatic;
  gt_control_result_t result;uint8_t tx[GT_CREATE_MAX_BYTES];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static uint64_t get(const uint8_t *p,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v=(v<<8)|p[i];return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(v>>(8*i));}
static bool nonzero(const void *memory,size_t n){const uint8_t *p=memory;uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static void fail(gt_control_client_t *c,gt_status_t error) {
  if(c->state==GT_CONTROL_CLIENT_FAILED)return;
  c->state=GT_CONTROL_CLIENT_FAILED;c->error=error;gt__connection_cancel(c->connection);
  if(c->pending && !c->automatic) {
    uint16_t type=c->pending;uint64_t id=c->result.request_id,session=c->result.session;uint8_t nonce[16];memcpy(nonce,c->result.nonce,16);
    gt__secure_erase(&c->result,sizeof(c->result));c->result=(gt_control_result_t){.delivery=GT_CONTROL_CLIENT_UNKNOWN,.error=error,.request_type=type,.request_id=id,.session=session};
    memcpy(c->result.nonce,nonce,16);gt__secure_erase(nonce,sizeof(nonce));c->pending=0;c->config.result(c->config.context,&c->result);
  }
  c->pending=0;gt__secure_erase(&c->result,sizeof(c->result));gt__secure_erase(c->tx,sizeof(c->tx));
}
static bool tick(gt_control_client_t *c) {
  uint64_t now;if(!c->config.clock(c->config.context,&now) || now<c->now){fail(c,GT_ERR_IO);return false;}
  c->now=now;if(c->pending && now-c->started>=c->config.request_timeout_ms){fail(c,GT_ERR_CANCELLED);return false;}
  return true;
}
static bool decode(gt_control_client_t *c,const gt_wire_frame_t *f) {
  if(!c->pending || f->header.flags || f->header.request_id!=c->result.request_id)return false;
  gt_control_result_t *r=&c->result;const uint8_t *p=f->payload;size_t n=f->header.payload_length;r->reply_type=f->header.type;
  if(f->header.type==GT_MSG_CONTROL_ERROR) {
    if(n!=4 || !get(p,4) || get(p,4)>GT_CONTROL_INTERNAL)return false;
    r->status=(gt_control_status_t)get(p,4);return !c->automatic;
  }
  if(c->pending==GT_MSG_CREATE || c->pending==GT_MSG_QUERY_CREATE) {
    uint8_t nonce[16];gt_creation_result_t creation;
    if(f->header.type!=(c->pending==GT_MSG_CREATE?GT_MSG_CREATE_REPLY:GT_MSG_QUERY_CREATE_REPLY) ||
      !gt__control_creation_decode(p,n,nonce,&creation) || memcmp(nonce,r->nonce,16))return false;
    r->creation=creation;return true;
  }
  if(c->pending==GT_MSG_ATTACH) {
    if(f->header.type!=GT_MSG_ATTACH_REPLY || !gt__control_attach_decode(p,n,&r->attachment) || r->attachment.session!=r->session)return false;
    return r->attachment.status!=GT_ATTACH_OK || !memcmp(r->attachment.instance,c->config.instance,GT_INSTANCE_BYTES);
  }
  if(c->pending==GT_MSG_PING)return f->header.type==GT_MSG_PING_REPLY && !n;
  uint16_t expected=c->pending==GT_MSG_LIST?GT_MSG_LIST_REPLY:c->pending==GT_MSG_INFO?GT_MSG_INFO_REPLY:GT_MSG_TERMINATE_REPLY;
  if(f->header.type!=expected || n<4 || get(p,4)>GT_CONTROL_INTERNAL)return false;
  r->status=(gt_control_status_t)get(p,4);
  if(c->pending==GT_MSG_LIST) {
    if(n<8)return false;
    uint64_t count=get(p+4,4);if(count>GT_CONTROL_MAX_ROWS || n!=8+count*GT_CONTROL_INFO_BYTES || (r->status!=GT_CONTROL_OK && count))return false;
    for(size_t i=0;i<count;i++) {
      if(!gt__control_info_decode(p+8+i*GT_CONTROL_INFO_BYTES,GT_CONTROL_INFO_BYTES,&r->rows[i]))return false;
      for(size_t j=0;j<i;j++)if(r->rows[j].id==r->rows[i].id)return false;
    }
    r->count=(size_t)count;return true;
  }
  if(c->pending==GT_MSG_INFO) {
    if(n!=4+GT_CONTROL_INFO_BYTES)return false;
    if(r->status!=GT_CONTROL_OK)return !nonzero(p+4,GT_CONTROL_INFO_BYTES);
    if(!gt__control_info_decode(p+4,GT_CONTROL_INFO_BYTES,&r->rows[0]) || r->rows[0].id!=r->session)return false;
    r->count=1;return true;
  }
  return n==12 && get(p+4,8)==r->session;
}
gt_status_t gt__control_client_open(gt_connection_t *connection,const gt_control_client_config_t *config,
  const gt_allocator_t *allocator,gt_control_client_t **out) {
  if(out)*out=NULL;
  gt_connection_limits_t limits={0};
  if(!out || !connection || !config || !config->clock || !config->result || !nonzero(config->instance,GT_INSTANCE_BYTES) ||
    !config->heartbeat_ms || config->heartbeat_ms>60000 || !config->request_timeout_ms || config->request_timeout_ms>60000 ||
    gt__connection_state(connection)!=GT_CONNECTION_OPEN || gt__connection_wants_write(connection) ||
    !gt__connection_limits(connection,&limits) || limits.receive_limit<GT_CONTROL_REPLY_BYTES || limits.send_limit<GT_CREATE_MAX_BYTES)return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  uint64_t now;if(!config->clock(config->context,&now))return GT_ERR_IO;
  gt_control_client_t *c=a.allocate(a.context,sizeof(*c));if(!c)return GT_ERR_IO;
  memset(c,0,sizeof(*c));c->allocator=a;c->connection=connection;c->config=*config;c->now=c->last_activity=now;*out=c;return GT_OK;
}
gt_control_submit_t gt__control_client_submit(gt_control_client_t *c,const gt_control_request_t *request,uint64_t *id,gt_status_t *error) {
  if(id)*id=0;
  if(error)*error=GT_OK;
  if(!c || !request || !id || !error){if(error)*error=GT_ERR_INVALID_ARG;return GT_CONTROL_CLIENT_INVALID;}
  if(c->state==GT_CONTROL_CLIENT_FAILED || !tick(c)){*error=c->error;return GT_CONTROL_CLIENT_UNAVAILABLE;}
  if(c->pending || gt__connection_wants_write(c->connection))return GT_CONTROL_CLIENT_BACKPRESSURE;
  size_t n=0;uint16_t type=request->type;uint8_t nonce[16]={0};
  if(type==GT_MSG_CREATE) {
    gt_status_t encoded=gt__create_spec_encode(request->create,c->tx,sizeof(c->tx),&n);
    if(encoded!=GT_OK){*error=encoded;gt__secure_erase(c->tx,sizeof(c->tx));return encoded==GT_ERR_UNSUPPORTED?GT_CONTROL_CLIENT_UNSUPPORTED:GT_CONTROL_CLIENT_INVALID;}
    memcpy(nonce,request->create->nonce,16);
  } else if(type==GT_MSG_QUERY_CREATE) {
    if(!nonzero(request->nonce,16)){*error=GT_ERR_INVALID_ARG;return GT_CONTROL_CLIENT_INVALID;}
    n=16;memcpy(c->tx,request->nonce,16);memcpy(nonce,request->nonce,16);
  } else if(type==GT_MSG_INFO || type==GT_MSG_TERMINATE || type==GT_MSG_ATTACH) {
    if(!request->session){*error=GT_ERR_INVALID_ARG;return GT_CONTROL_CLIENT_INVALID;}
    put64(c->tx,request->session);n=8;
  } else if(type!=GT_MSG_LIST && type!=GT_MSG_PING){*error=GT_ERR_INVALID_ARG;return GT_CONTROL_CLIENT_INVALID;}
  if(c->last_id==UINT64_MAX){gt__secure_erase(c->tx,sizeof(c->tx));fail(c,GT_ERR_PROTOCOL);*error=c->error;return GT_CONTROL_CLIENT_UNAVAILABLE;}
  gt_wire_header_t h={.type=type,.request_id=c->last_id+1,.payload_length=(uint32_t)n};
  gt_connection_send_t sent=gt__connection_send(c->connection,&h,c->tx);gt__secure_erase(c->tx,sizeof(c->tx));
  if(sent!=GT_SEND_ACCEPTED){if(sent==GT_SEND_BUSY)return GT_CONTROL_CLIENT_BACKPRESSURE;fail(c,GT_ERR_IO);*error=c->error;return GT_CONTROL_CLIENT_UNAVAILABLE;}
  c->last_id=h.request_id;c->pending=type;c->automatic=false;c->started=c->now;c->state=GT_CONTROL_CLIENT_BUSY;
  c->result=(gt_control_result_t){.delivery=GT_CONTROL_CLIENT_REPLY,.request_type=type,.request_id=h.request_id,.session=request->session};
  memcpy(c->result.nonce,nonce,16);gt__secure_erase(nonce,sizeof(nonce));*id=h.request_id;return GT_CONTROL_CLIENT_QUEUED;
}
gt_control_client_state_t gt__control_client_step(gt_control_client_t *c) {
  if(!c)return GT_CONTROL_CLIENT_FAILED;
  if(c->state==GT_CONTROL_CLIENT_FAILED || !tick(c))return c->state;
  gt_connection_state_t state=gt__connection_step(c->connection);
  if(state!=GT_CONNECTION_OPEN){fail(c,state==GT_CONNECTION_PROTOCOL_ERROR?GT_ERR_PROTOCOL:state==GT_CONNECTION_TIMEOUT?GT_ERR_CANCELLED:GT_ERR_IO);return c->state;}
  if(!tick(c))return c->state;
  gt_wire_frame_t frame;
  if(gt__connection_frame(c->connection,&frame)) {
    if(!decode(c,&frame)){fail(c,GT_ERR_PROTOCOL);return c->state;}
    if(!gt__connection_release_frame(c->connection)){fail(c,GT_ERR_PROTOCOL);return c->state;}
    bool automatic=c->automatic;c->pending=0;c->automatic=false;c->state=GT_CONTROL_CLIENT_READY;c->last_activity=c->now;
    if(!automatic)c->config.result(c->config.context,&c->result);
    gt__secure_erase(&c->result,sizeof(c->result));
  }
  if(!tick(c))return c->state;
  if(!c->pending && !gt__connection_wants_write(c->connection) && c->now-c->last_activity>=c->config.heartbeat_ms) {
    uint64_t id;gt_status_t error;gt_control_request_t ping={.type=GT_MSG_PING};
    if(gt__control_client_submit(c,&ping,&id,&error)==GT_CONTROL_CLIENT_QUEUED)c->automatic=true;
  }
  return c->state;
}
gt_status_t gt__control_client_error(const gt_control_client_t *c){return c?c->error:GT_ERR_INVALID_ARG;}
void gt__control_client_close(gt_control_client_t *c) {
  if(!c)return;
  fail(c,GT_ERR_CANCELLED);gt__connection_destroy(c->connection);gt_allocator_t a=c->allocator;
  gt__secure_erase(c,sizeof(*c));a.release(a.context,c,sizeof(*c));
}
