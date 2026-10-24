#include "server.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  uint64_t id,principal,last_request,last_activity;
  gt_connection_t *connection;gt_handshake_t *handshake;
  gt_protocol_role_t role;gt_token_scope_t binding;
} peer;
struct gt_server {
  gt_allocator_t allocator;gt_server_config_t config;gt_server_state_t state;
  uint64_t now,next_id;size_t count,cursor;
  uint8_t reply[GT_CONTROL_REPLY_BYTES];peer peers[];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static bool valid_ops(const gt_stream_ops_t *ops){return ops && ops->read && ops->write && ops->clock && ops->close;}
static void retire(gt_server_t *s,peer *p) {
  if(!p->id)return;
  uint64_t issuer=p->id;p->id=0;s->count--;
  gt__handshake_close(p->handshake);p->handshake=NULL;
  if(p->role==GT_ROLE_DATA) {
    s->config.handshake.binding_drop(s->config.handshake.binding_context,&p->binding);
    if(s->config.data_drop)s->config.data_drop(s->config.data_context,&p->binding);
  }
  gt__connection_destroy(p->connection);p->connection=NULL;
  if(s->config.handshake.tokens)gt__tokens_revoke_issuer(s->config.handshake.tokens,issuer);
  gt__secure_erase(p,sizeof(*p));
}
gt_status_t gt__server_open(const gt_server_config_t *config,const gt_allocator_t *allocator,gt_server_t **out) {
  if(out)*out=NULL;
  gt_server_config_t normalized;
  if(config && config->attachments) {
    if(config->handshake.tokens || config->handshake.binding_context || config->handshake.binding_current || config->handshake.binding_drop ||
       !gt__attachment_owner_matches(config->attachments,config->manager,config->creation,config->handshake.identity.instance))return GT_ERR_INVALID_ARG;
    normalized=*config;normalized.handshake.tokens=gt__attachment_owner_tokens(config->attachments);
    normalized.handshake.binding_context=config->attachments;normalized.handshake.binding_current=gt__attachment_owner_current;
    normalized.handshake.binding_drop=gt__attachment_owner_drop;config=&normalized;
  }
  if(!out || !config || !config->manager || !config->connections || config->connections>64 ||
     !config->handshake.clock || !config->handshake.timeout_ms || config->handshake.timeout_ms>60000 ||
     !nonzero(config->handshake.identity.instance,GT_INSTANCE_BYTES) ||
     !nonzero(config->handshake.identity.compatibility,GT_COMPAT_BYTES) ||
     !config->idle_timeout_ms || config->idle_timeout_ms>60000 ||
     config->limits.receive_limit<GT_HELLO_REPLY_BYTES || config->limits.receive_limit>GT_WIRE_MAX_PAYLOAD ||
     config->limits.send_limit<GT_CONTROL_REPLY_BYTES || config->limits.send_limit>GT_WIRE_MAX_PAYLOAD ||
     !config->limits.frame_timeout_ms || config->limits.frame_timeout_ms>60000 ||
     (config->creation && (config->limits.receive_limit<GT_CREATE_MAX_BYTES || !gt__creation_matches(config->creation,config->manager,config->handshake.identity.instance))) ||
     (config->data_drop && !config->data_step) ||
     (config->data_revoked && (!config->data_step || !config->data_drop)) ||
     (config->data_step && (!config->handshake.tokens || !config->handshake.binding_current || !config->handshake.binding_drop)))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  size_t bytes=sizeof(gt_server_t)+config->connections*sizeof(peer);
  gt_server_t *s=a.allocate(a.context,bytes);if(!s)return GT_ERR_INTERNAL;
  memset(s,0,bytes);s->allocator=a;s->config=*config;s->next_id=1;
  if(config->attachments && !gt__attachment_owner_claim(config->attachments,s)){gt__secure_erase(s,bytes);a.release(a.context,s,bytes);return GT_ERR_INVALID_ARG;}
  if(!config->handshake.clock(config->handshake.clock_context,&s->now)){
    gt__attachment_owner_unclaim(config->attachments,s);gt__secure_erase(s,bytes);a.release(a.context,s,bytes);return GT_ERR_IO;
  }
  *out=s;return GT_OK;
}
gt_server_admit_t gt__server_admit(gt_server_t *s,const gt_stream_ops_t *ops,uint64_t principal,uint64_t *id) {
  if(id)*id=0;
  if(!valid_ops(ops))return GT_SERVER_ADMIT_INVALID;
  gt_server_admit_t rejection=GT_SERVER_ADMITTED;
  if(!s || !id || !principal)rejection=GT_SERVER_ADMIT_INVALID;
  else if(s->state!=GT_SERVER_RUNNING)rejection=GT_SERVER_ADMIT_CLOSED;
  else if(!s->next_id)rejection=GT_SERVER_ADMIT_IDS_EXHAUSTED;
  else if(s->count==s->config.connections)rejection=GT_SERVER_ADMIT_FULL;
  if(rejection!=GT_SERVER_ADMITTED){ops->close(ops->context);return rejection;}
  peer *p=NULL;for(size_t i=0;i<s->config.connections;i++)if(!s->peers[i].id){p=&s->peers[i];break;}
  if(!p){ops->close(ops->context);return GT_SERVER_ADMIT_INTERNAL;}
  gt_connection_open_t opened=gt__connection_open(ops,s->config.limits,&s->allocator,&p->connection);
  if(opened!=GT_CONNECTION_CREATED) {
    ops->close(ops->context);return opened==GT_CONNECTION_NO_MEMORY?GT_SERVER_ADMIT_NO_MEMORY:
      opened==GT_CONNECTION_NO_CLOCK?GT_SERVER_ADMIT_CLOCK:GT_SERVER_ADMIT_INVALID;
  }
  gt_handshake_config_t handshake=s->config.handshake;handshake.principal=principal;
  if(!s->config.data_step)handshake.tokens=NULL;
  gt_status_t status=gt__handshake_open(p->connection,&handshake,&s->allocator,&p->handshake);
  if(status!=GT_OK) {
    gt__connection_destroy(p->connection);p->connection=NULL;
    return status==GT_ERR_INTERNAL?GT_SERVER_ADMIT_NO_MEMORY:status==GT_ERR_IO?GT_SERVER_ADMIT_CLOCK:GT_SERVER_ADMIT_INVALID;
  }
  p->id=s->next_id;s->next_id=s->next_id==UINT64_MAX?0:s->next_id+1;
  p->principal=principal;p->last_activity=s->now;s->count++;*id=p->id;return GT_SERVER_ADMITTED;
}
static void step_peer(gt_server_t *s,peer *p) {
  if(p->handshake) {
    gt_handshake_state_t state=gt__handshake_step(p->handshake);
    if(state==GT_HANDSHAKE_FAILED){retire(s,p);return;}
    if(state!=GT_HANDSHAKE_READY)return;
    if(!gt__handshake_take(p->handshake,&p->role,&p->binding)){retire(s,p);return;}
    gt__handshake_close(p->handshake);p->handshake=NULL;p->last_activity=s->now;return;
  }
  if(s->now-p->last_activity>=s->config.idle_timeout_ms || gt__connection_step(p->connection)!=GT_CONNECTION_OPEN) {
    retire(s,p);return;
  }
  if(p->role==GT_ROLE_DATA) {
    if(!s->config.handshake.binding_current(s->config.handshake.binding_context,&p->binding)) {
      if(!s->config.data_revoked || s->config.data_revoked(s->config.data_context,p->connection,&p->binding)!=GT_SERVER_DATA_IDLE)retire(s,p);
      return;
    }
    gt_wire_frame_t frame;
    if(gt__connection_frame(p->connection,&frame) && frame.header.type==GT_MSG_DETACH){retire(s,p);return;}
    gt_server_data_rc_t rc=s->config.data_step(s->config.data_context,p->connection,&p->binding);
    if(rc==GT_SERVER_DATA_PROGRESS)p->last_activity=s->now;
    else if(rc!=GT_SERVER_DATA_IDLE)retire(s,p);
    return;
  }
  if(gt__connection_wants_write(p->connection))return;
  gt_wire_frame_t frame;if(!gt__connection_frame(p->connection,&frame))return;
  gt_wire_header_t reply;
  gt_control_context_t context={.manager=s->config.manager,.creation=s->config.creation,
    .attachments=s->config.data_step?s->config.attachments:NULL,.principal=p->principal,.issuer=p->id,.now_ms=s->now};
  if(frame.header.request_id<=p->last_request ||
     gt__control_process(&context,&frame,s->reply,sizeof(s->reply),&reply)!=GT_CONTROL_REPLY) {
    retire(s,p);return;
  }
  p->last_request=frame.header.request_id;p->last_activity=s->now;
  gt_connection_send_t sent=gt__connection_send(p->connection,&reply,s->reply);
  gt__secure_erase(s->reply,sizeof(s->reply));
  if(sent!=GT_SEND_ACCEPTED || !gt__connection_release_frame(p->connection))retire(s,p);
}
gt_server_state_t gt__server_step(gt_server_t *s) {
  if(!s)return GT_SERVER_CLOSED;
  if(s->state!=GT_SERVER_RUNNING)return s->state;
  uint64_t now;
  if(!s->config.handshake.clock(s->config.handshake.clock_context,&now) || now<s->now ||
     (s->config.handshake.tokens && gt__tokens_expire(s->config.handshake.tokens,now)!=GT_TOKEN_OK) ||
     (s->config.creation && gt__creation_advance(s->config.creation,now)!=GT_OK)) {
    s->state=GT_SERVER_CLOCK_FAILED;
    for(size_t i=0;i<s->config.connections;i++)retire(s,&s->peers[i]);
    return s->state;
  }
  s->now=now;
  for(size_t i=0;i<s->config.connections;i++) {
    peer *p=&s->peers[(s->cursor+i)%s->config.connections];if(p->id)step_peer(s,p);
  }
  s->cursor=(s->cursor+1)%s->config.connections;return s->state;
}
static const peer *find(const gt_server_t *s,uint64_t id) {
  if(s && id)for(size_t i=0;i<s->config.connections;i++)if(s->peers[i].id==id)return &s->peers[i];
  return NULL;
}
bool gt__server_connected(const gt_server_t *s,uint64_t id){return find(s,id)!=NULL;}
bool gt__server_wants_write(const gt_server_t *s,uint64_t id){const peer *p=find(s,id);return p && gt__connection_wants_write(p->connection);}
size_t gt__server_connections(const gt_server_t *s){return s?s->count:0;}
void gt__server_close(gt_server_t *s) {
  if(!s)return;
  s->state=GT_SERVER_CLOSED;
  for(size_t i=0;i<s->config.connections;i++)retire(s,&s->peers[i]);
  gt__attachment_owner_unclaim(s->config.attachments,s);
  gt_allocator_t a=s->allocator;size_t bytes=sizeof(*s)+s->config.connections*sizeof(peer);
  gt__secure_erase(s,bytes);a.release(a.context,s,bytes);
}
