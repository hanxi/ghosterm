#include "user-dispatcher.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef enum {EMPTY,PENDING,READY} phase;
typedef struct peer peer;
typedef struct command {struct gt_user_dispatcher *owner;peer *peer;phase phase;gt_user_reply_t reply;} command;
struct peer {
  bool active;gt_token_scope_t scope;gt_runtime_session_t *lease;
  size_t head,count,pending;command commands[16];
};
struct gt_user_dispatcher {
  gt_allocator_t allocator;gt_user_dispatcher_config_t config;pthread_mutex_t lock;peer peers[];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool same(gt_token_scope_t a,gt_token_scope_t b){return a.principal==b.principal && a.issuer==b.issuer && a.session==b.session && a.generation==b.generation;}
static bool valid_scope(const gt_token_scope_t *s){return s && s->principal && s->issuer && s->session && s->generation;}
static peer *find(gt_user_dispatcher_t *d,const gt_token_scope_t *scope) {
  for(size_t i=0;i<d->config.connections;i++)if(d->peers[i].lease && same(d->peers[i].scope,*scope))return &d->peers[i];
  return NULL;
}
static void completed(void *ctx,const gt_worker_user_result_t *result) {
  command *c=ctx;gt_user_dispatcher_t *d=c->owner;
  pthread_mutex_lock(&d->lock);
  c->reply.status=result->status;c->reply.delivery=result->delivery;c->reply.stamp=result->stamp;c->phase=READY;
  c->peer->pending--;
  pthread_mutex_unlock(&d->lock); /* Last mailbox access: IO owner may now reap. */
}
gt_status_t gt__user_dispatcher_open(const gt_user_dispatcher_config_t *config,const gt_allocator_t *allocator,gt_user_dispatcher_t **out) {
  if(out)*out=NULL;
  if(!out || !config || !config->runtime || !config->attachments || !config->connections || config->connections>64 || !config->commands || config->commands>16 ||
    !gt__attachment_owner_matches(config->attachments,gt__runtime_manager(config->runtime),gt__runtime_creation(config->runtime),config->instance))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  size_t n=sizeof(gt_user_dispatcher_t)+config->connections*sizeof(peer);gt_user_dispatcher_t *d=a.allocate(a.context,n);if(!d)return GT_ERR_INTERNAL;
  memset(d,0,n);d->config=*config;d->allocator=a;if(pthread_mutex_init(&d->lock,NULL)){gt__secure_erase(d,n);a.release(a.context,d,n);return GT_ERR_INTERNAL;}
  *out=d;return GT_OK;
}
void gt__user_dispatcher_reap(gt_user_dispatcher_t *d) {
  if(!d)return;
  for(size_t i=0;i<d->config.connections;i++) {
    pthread_mutex_lock(&d->lock);peer *p=&d->peers[i];gt_runtime_session_t *lease=NULL;
    if(p->lease && !p->active && !p->pending){lease=p->lease;gt__secure_erase(p,sizeof(*p));}
    pthread_mutex_unlock(&d->lock);if(lease)gt__runtime_release(&lease);
  }
}
void gt__user_dispatcher_drop(void *context,const gt_token_scope_t *scope) {
  gt_user_dispatcher_t *d=context;if(!d || !valid_scope(scope))return;
  pthread_mutex_lock(&d->lock);peer *p=find(d,scope);if(p)p->active=false;pthread_mutex_unlock(&d->lock);
  gt__user_dispatcher_reap(d);
}
static peer *adopt(gt_user_dispatcher_t *d,const gt_token_scope_t *scope) {
  peer *p=find(d,scope);if(p)return p->active?p:NULL;
  for(size_t i=0;i<d->config.connections;i++)if(!d->peers[i].lease) {
    gt_runtime_session_t *lease=NULL;if(gt__runtime_acquire(d->config.runtime,scope->principal,scope->session,&lease)!=GT_RUNTIME_OK)return NULL;
    p=&d->peers[i];p->active=true;p->scope=*scope;p->lease=lease;return p;
  }
  return NULL;
}
static gt_user_reject_t rejection(gt_worker_submit_t s) {
  switch(s){case GT_WORKER_BACKPRESSURE:return GT_USER_REJECT_BACKPRESSURE;case GT_WORKER_STOPPED:return GT_USER_REJECT_STOPPED;
  case GT_WORKER_INVALID:return GT_USER_REJECT_INVALID;case GT_WORKER_NO_MEMORY:return GT_USER_REJECT_NO_MEMORY;
  case GT_WORKER_UNSUPPORTED:return GT_USER_REJECT_UNSUPPORTED;default:return GT_USER_REJECT_UNAVAILABLE;}
}
gt_server_data_rc_t gt__user_dispatcher_step(void *context,gt_connection_t *connection,const gt_token_scope_t *scope) {
  gt_user_dispatcher_t *d=context;
  if(!d || !connection || !valid_scope(scope) || !gt__attachment_owner_current(d->config.attachments,scope))return GT_SERVER_DATA_CLOSE;
  gt__user_dispatcher_reap(d);peer *p=adopt(d,scope);if(!p)return GT_SERVER_DATA_CLOSE;
  gt_user_reply_t reply;bool ready=false;
  pthread_mutex_lock(&d->lock);if(p->count && p->commands[p->head].phase==READY){reply=p->commands[p->head].reply;ready=true;}pthread_mutex_unlock(&d->lock);
  if(ready && !gt__connection_wants_write(connection)) {
    uint8_t bytes[GT_USER_RESULT_BYTES];gt_wire_header_t h;
    if(!gt__user_reply_encode(&reply,bytes,sizeof(bytes),&h))return GT_SERVER_DATA_CLOSE;
    gt_connection_send_t sent=gt__connection_send(connection,&h,bytes);gt__secure_erase(bytes,sizeof(bytes));
    if(sent!=GT_SEND_ACCEPTED)return GT_SERVER_DATA_CLOSE;
    pthread_mutex_lock(&d->lock);gt__secure_erase(&p->commands[p->head],sizeof(command));p->head=(p->head+1)%d->config.commands;p->count--;pthread_mutex_unlock(&d->lock);
  }
  gt_wire_frame_t frame;if(!gt__connection_frame(connection,&frame))return GT_SERVER_DATA_IDLE;
  gt_user_request_t request;
  if(!gt__user_request_decode(&frame,&request) || request.stamp.generation!=scope->generation)return GT_SERVER_DATA_CLOSE;
  pthread_mutex_lock(&d->lock);
  if(p->count==d->config.commands){pthread_mutex_unlock(&d->lock);return GT_SERVER_DATA_CLOSE;}
  command *c=&p->commands[(p->head+p->count)%d->config.commands];
  *c=(command){.owner=d,.peer=p,.phase=PENDING,.reply={.kind=request.kind,.stamp=request.stamp}};p->count++;p->pending++;
  pthread_mutex_unlock(&d->lock);
  const void *bytes=request.kind==GT_WORKER_INPUT?(const void *)request.input:request.kind==GT_WORKER_RESIZE?(const void *)&request.size:request.kind==GT_WORKER_STYLE?(const void *)&request.style:(const void *)&request.color;
  size_t n=request.kind==GT_WORKER_INPUT?request.length:request.kind==GT_WORKER_RESIZE?sizeof(request.size):request.kind==GT_WORKER_STYLE?sizeof(request.style):sizeof(request.color);
  gt_worker_submit_t admission=GT_WORKER_INVALID;
  gt_manager_rc_t rc=gt__runtime_submit_user(p->lease,request.kind,&request.stamp,bytes,n,completed,c,&admission);
  if(rc!=GT_MANAGER_OK || admission!=GT_WORKER_ACCEPTED) {
    pthread_mutex_lock(&d->lock);c->reply.rejected=true;c->reply.rejection=rc==GT_MANAGER_OK?rejection(admission):GT_USER_REJECT_UNAVAILABLE;c->phase=READY;p->pending--;pthread_mutex_unlock(&d->lock);
  }
  if(!gt__connection_release_frame(connection))return GT_SERVER_DATA_CLOSE;
  return GT_SERVER_DATA_PROGRESS;
}
gt_runtime_rc_t gt__user_dispatcher_close(gt_user_dispatcher_t *d) {
  if(!d)return GT_RUNTIME_OK;
  gt__user_dispatcher_reap(d);pthread_mutex_lock(&d->lock);bool busy=false;
  for(size_t i=0;i<d->config.connections;i++)if(d->peers[i].lease)busy=true;
  pthread_mutex_unlock(&d->lock);if(busy)return GT_RUNTIME_BUSY;
  pthread_mutex_destroy(&d->lock);gt_allocator_t a=d->allocator;size_t n=sizeof(*d)+d->config.connections*sizeof(peer);
  gt__secure_erase(d,n);a.release(a.context,d,n);return GT_RUNTIME_OK;
}
