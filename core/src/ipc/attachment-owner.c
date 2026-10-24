#include "attachment-owner.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef struct {gt_token_scope_t scope;gt_runtime_session_t *lease;} reservation;
struct gt_attachment_owner {
  gt_allocator_t allocator;gt_attachment_owner_config_t config;
  gt_token_store_t *tokens;const void *server;reservation slots[];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool equal(gt_token_scope_t a,gt_token_scope_t b) {
  return a.principal==b.principal && a.issuer==b.issuer && a.session==b.session && a.generation==b.generation;
}
static reservation *find(gt_attachment_owner_t *o,const gt_token_scope_t *scope) {
  if(o && scope)for(size_t i=0;i<o->config.capacity;i++)if(o->slots[i].lease && equal(o->slots[i].scope,*scope))return &o->slots[i];
  return NULL;
}
bool gt__attachment_owner_current(void *ctx,const gt_token_scope_t *scope) {
  reservation *r=find(ctx,scope);return r && gt__attachment_current(gt__runtime_attachment(r->lease),scope->generation);
}
void gt__attachment_owner_drop(void *ctx,const gt_token_scope_t *scope) {
  reservation *r=find(ctx,scope);if(!r)return;
  (void)gt__attachment_detach(gt__runtime_attachment(r->lease),r->scope.generation);
  gt__runtime_release(&r->lease);gt__secure_erase(r,sizeof(*r));
}
gt_session_attachment_t *gt__attachment_owner_attachment(gt_attachment_owner_t *o,const gt_token_scope_t *scope) {
  reservation *r=find(o,scope);
  return r && gt__attachment_owner_current(o,scope)?gt__runtime_attachment(r->lease):NULL;
}
bool gt__attachment_owner_matches(const gt_attachment_owner_t *o,const gt_session_manager_t *manager,const gt_creation_t *creation,const uint8_t instance[16]) {
  return o && instance && manager==gt__runtime_manager(o->config.runtime) && creation==gt__runtime_creation(o->config.runtime) && !memcmp(instance,o->config.instance,16);
}
gt_token_store_t *gt__attachment_owner_tokens(gt_attachment_owner_t *o){return o?o->tokens:NULL;}
bool gt__attachment_owner_claim(gt_attachment_owner_t *o,const void *server) {
  if(!o || !server || o->server)return false;
  /* No reservation from a previous/direct issuer namespace may survive claim. */
  for(size_t i=0;i<o->config.capacity;i++)if(o->slots[i].lease)return false;
  o->server=server;return true;
}
void gt__attachment_owner_unclaim(gt_attachment_owner_t *o,const void *server){if(o && server && o->server==server)o->server=NULL;}
gt_status_t gt__attachment_owner_open(const gt_attachment_owner_config_t *config,uint64_t now,const gt_allocator_t *allocator,gt_attachment_owner_t **out) {
  if(out)*out=NULL;
  if(!out || !config || !config->runtime || !config->capacity || config->capacity>256 ||
     !config->token_ttl_ms || config->token_ttl_ms>60000 || (config->random.context && !config->random.fill) ||
     !gt__creation_matches(gt__runtime_creation(config->runtime),gt__runtime_manager(config->runtime),config->instance))return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  size_t bytes=sizeof(gt_attachment_owner_t)+config->capacity*sizeof(reservation);gt_attachment_owner_t *o=a.allocate(a.context,bytes);if(!o)return GT_ERR_INTERNAL;
  memset(o,0,bytes);o->allocator=a;o->config=*config;
  gt_token_rc_t rc=gt__tokens_open(config->instance,config->capacity,now,config->random.fill?&config->random:NULL,
    (gt_token_owner_t){o,gt__attachment_owner_drop},&a,&o->tokens);
  if(rc!=GT_TOKEN_OK){gt__secure_erase(o,bytes);a.release(a.context,o,bytes);return GT_ERR_INTERNAL;}
  *out=o;return GT_OK;
}
static gt_attach_result_t result(uint64_t id,gt_attach_status_t status,gt_status_t detail) {
  return (gt_attach_result_t){.session=id,.status=status,.detail=detail};
}
gt_attach_status_t gt__attachment_owner_issue(gt_attachment_owner_t *o,uint64_t principal,uint64_t issuer,uint64_t id,uint64_t now,gt_attach_result_t *out) {
  if(!out)return GT_ATTACH_INTERNAL;
  *out=result(id,GT_ATTACH_INTERNAL,GT_ERR_INVALID_ARG);
  if(!o || !principal || !issuer || !id)return out->status;
  if(gt__tokens_expire(o->tokens,now)!=GT_TOKEN_OK){*out=result(id,GT_ATTACH_CLOCK,GT_ERR_IO);return out->status;}
  gt_runtime_session_t *lease=NULL;gt_runtime_rc_t acquired=gt__runtime_acquire(o->config.runtime,principal,id,&lease);
  if(acquired!=GT_RUNTIME_OK){*out=result(id,acquired==GT_RUNTIME_NOT_FOUND?GT_ATTACH_NOT_FOUND:acquired==GT_RUNTIME_BUSY?GT_ATTACH_BUSY:GT_ATTACH_INTERNAL,
    acquired==GT_RUNTIME_NOT_FOUND || acquired==GT_RUNTIME_BUSY?GT_OK:GT_ERR_INTERNAL);return out->status;}
  reservation *slot=NULL;for(size_t i=0;i<o->config.capacity;i++)if(!o->slots[i].lease){slot=&o->slots[i];break;}
  if(!slot){gt__runtime_release(&lease);*out=result(id,GT_ATTACH_BUSY,GT_OK);return out->status;}
  uint64_t generation=0;gt_task_rc_t attached=gt__attachment_attach(gt__runtime_attachment(lease),&generation);
  if(attached!=GT_TASK_OK){gt__runtime_release(&lease);*out=result(id,attached==GT_TASK_BUSY?GT_ATTACH_BUSY:GT_ATTACH_INTERNAL,attached==GT_TASK_BUSY?GT_OK:GT_ERR_INTERNAL);return out->status;}
  *slot=(reservation){.scope={principal,issuer,id,generation},.lease=lease};
  *out=result(id,GT_ATTACH_OK,GT_OK);
  gt_token_rc_t issued=gt__tokens_issue(o->tokens,slot->scope,now,o->config.token_ttl_ms,&out->token);
  if(issued!=GT_TOKEN_OK) {
    gt__attachment_owner_drop(o,&slot->scope);
    *out=result(id,issued==GT_TOKEN_CLOCK?GT_ATTACH_CLOCK:issued==GT_TOKEN_UNSUPPORTED?GT_ATTACH_UNSUPPORTED:
      issued==GT_TOKEN_ENTROPY?GT_ATTACH_ENTROPY:issued==GT_TOKEN_FULL || issued==GT_TOKEN_BUSY?GT_ATTACH_BUSY:GT_ATTACH_INTERNAL,
      issued==GT_TOKEN_UNSUPPORTED?GT_ERR_UNSUPPORTED:issued==GT_TOKEN_FULL || issued==GT_TOKEN_BUSY?GT_OK:issued==GT_TOKEN_CLOCK || issued==GT_TOKEN_ENTROPY?GT_ERR_IO:GT_ERR_INTERNAL);return out->status;
  }
  memcpy(out->instance,o->config.instance,16);out->generation=generation;out->ttl_ms=o->config.token_ttl_ms;return out->status;
}
gt_runtime_rc_t gt__attachment_owner_close(gt_attachment_owner_t *o) {
  if(!o)return GT_RUNTIME_INVALID;
  if(o->server)return GT_RUNTIME_BUSY;
  gt__tokens_close(o->tokens);
  for(size_t i=0;i<o->config.capacity;i++)if(o->slots[i].lease)gt__attachment_owner_drop(o,&o->slots[i].scope);
  gt_allocator_t a=o->allocator;size_t bytes=sizeof(*o)+o->config.capacity*sizeof(reservation);
  gt__secure_erase(o,bytes);a.release(a.context,o,bytes);return GT_RUNTIME_OK;
}
