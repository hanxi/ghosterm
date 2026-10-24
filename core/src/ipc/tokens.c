#include "tokens.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  bool active;gt_attachment_token_t token;gt_token_scope_t scope;
  uint64_t issued;uint32_t ttl;
} entry;
struct gt_token_store {
  gt_allocator_t allocator;gt_token_random_t random;gt_token_owner_t owner;
  uint8_t instance[GT_INSTANCE_BYTES];size_t capacity;
  uint64_t now,next;bool failed;entry entries[];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static gt_status_t random_bytes(void *ctx,uint8_t *p,size_t n){(void)ctx;return gt__secure_random(p,n);}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static bool equal(const uint8_t *a,const uint8_t *b,size_t n){volatile uint8_t difference=0;for(size_t i=0;i<n;i++)difference|=a[i]^b[i];return difference==0;}
static void revoke(gt_token_store_t *s,entry *e) {
  gt_token_scope_t scope=e->scope;gt__secure_erase(e,sizeof(*e));
  s->owner.revoke(s->owner.context,&scope);gt__secure_erase(&scope,sizeof(scope));
}
static gt_token_rc_t advance(gt_token_store_t *s,uint64_t now) {
  if(s->failed)return GT_TOKEN_CLOCK;
  if(now<s->now) {
    s->failed=true;
    for(size_t i=0;i<s->capacity;i++)if(s->entries[i].active)revoke(s,&s->entries[i]);
    return GT_TOKEN_CLOCK;
  }
  s->now=now;
  for(size_t i=0;i<s->capacity;i++)if(s->entries[i].active && now-s->entries[i].issued>=s->entries[i].ttl)
    revoke(s,&s->entries[i]);
  return GT_TOKEN_OK;
}
gt_token_rc_t gt__tokens_open(const uint8_t instance[GT_INSTANCE_BYTES],size_t capacity,uint64_t now,
  const gt_token_random_t *random,gt_token_owner_t owner,const gt_allocator_t *allocator,gt_token_store_t **out) {
  if(out)*out=NULL;
  if(!out || !instance || !nonzero(instance,GT_INSTANCE_BYTES) || !capacity || capacity>256 || !owner.revoke || (random && !random->fill))return GT_TOKEN_INVALID;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_TOKEN_INVALID;
  size_t bytes=sizeof(gt_token_store_t)+capacity*sizeof(entry);
  gt_token_store_t *s=a.allocate(a.context,bytes);if(!s)return GT_TOKEN_NO_MEMORY;
  memset(s,0,bytes);s->allocator=a;s->owner=owner;s->capacity=capacity;s->now=now;s->next=1;
  s->random=random?*random:(gt_token_random_t){NULL,random_bytes};memcpy(s->instance,instance,GT_INSTANCE_BYTES);
  *out=s;return GT_TOKEN_OK;
}
gt_token_rc_t gt__tokens_issue(gt_token_store_t *s,gt_token_scope_t scope,uint64_t now,uint32_t ttl,gt_attachment_token_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!s || !out || !scope.principal || !scope.issuer || !scope.session || !scope.generation || !ttl || ttl>60000)return GT_TOKEN_INVALID;
  gt_token_rc_t rc=advance(s,now);if(rc!=GT_TOKEN_OK)return rc;
  if(!s->next)return GT_TOKEN_EXHAUSTED;
  for(size_t i=0;i<s->capacity;i++)if(s->entries[i].active && s->entries[i].scope.session==scope.session && s->entries[i].scope.generation==scope.generation)return GT_TOKEN_BUSY;
  entry *e=NULL;for(size_t i=0;i<s->capacity;i++)if(!s->entries[i].active){e=&s->entries[i];break;}
  if(!e)return GT_TOKEN_FULL;
  gt_attachment_token_t token={0};
  gt_status_t entropy=s->random.fill(s->random.context,token.bytes+8,32);
  if(entropy!=GT_OK || !nonzero(token.bytes+8,32)) {
    gt__secure_erase(&token,sizeof(token));return entropy==GT_ERR_UNSUPPORTED?GT_TOKEN_UNSUPPORTED:GT_TOKEN_ENTROPY;
  }
  for(unsigned i=0;i<8;i++)token.bytes[i]=(uint8_t)(s->next>>(56-8*i));
  s->next=s->next==UINT64_MAX?0:s->next+1;
  *e=(entry){.active=true,.token=token,.scope=scope,.issued=now,.ttl=ttl};*out=token;
  gt__secure_erase(&token,sizeof(token));return GT_TOKEN_OK;
}
gt_token_rc_t gt__tokens_consume(gt_token_store_t *s,const gt_attachment_token_t *token,const uint8_t instance[GT_INSTANCE_BYTES],
  uint64_t principal,uint64_t session,uint64_t generation,uint64_t now,gt_token_scope_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!s || !token || !instance || !out || !principal || !session || !generation)return GT_TOKEN_INVALID;
  gt_token_rc_t rc=advance(s,now);if(rc!=GT_TOKEN_OK)return rc;
  if(!equal(instance,s->instance,GT_INSTANCE_BYTES))return GT_TOKEN_DENIED;
  for(size_t i=0;i<s->capacity;i++) {
    entry *e=&s->entries[i];
    if(e->active && equal(token->bytes,e->token.bytes,GT_TOKEN_BYTES)) {
      if(e->scope.principal!=principal || e->scope.session!=session || e->scope.generation!=generation)return GT_TOKEN_DENIED;
      *out=e->scope;gt__secure_erase(e,sizeof(*e));return GT_TOKEN_OK;
    }
  }
  return GT_TOKEN_DENIED;
}
void gt__tokens_revoke_issuer(gt_token_store_t *s,uint64_t issuer) {
  if(s && issuer)for(size_t i=0;i<s->capacity;i++)if(s->entries[i].active && s->entries[i].scope.issuer==issuer)revoke(s,&s->entries[i]);
}
void gt__tokens_revoke_generation(gt_token_store_t *s,uint64_t session,uint64_t generation) {
  if(s && session && generation)for(size_t i=0;i<s->capacity;i++)if(s->entries[i].active && s->entries[i].scope.session==session && s->entries[i].scope.generation==generation)revoke(s,&s->entries[i]);
}
gt_token_rc_t gt__tokens_expire(gt_token_store_t *s,uint64_t now){return s?advance(s,now):GT_TOKEN_INVALID;}
void gt__tokens_close(gt_token_store_t *s) {
  if(!s)return;
  for(size_t i=0;i<s->capacity;i++)if(s->entries[i].active)revoke(s,&s->entries[i]);
  gt_allocator_t a=s->allocator;size_t bytes=sizeof(*s)+s->capacity*sizeof(entry);
  gt__secure_erase(s,bytes);a.release(a.context,s,bytes);
}
