#include "ipc/tokens.h"
#include "base/secure-random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static const uint8_t instance[GT_INSTANCE_BYTES]={1};
static gt_token_scope_t scope(void){return (gt_token_scope_t){1,2,3,4};}
typedef struct {gt_status_t status;bool zero;unsigned calls,revoked;} random_source;
static gt_status_t fill(void *context,uint8_t *p,size_t n) {
  random_source *r=context;r->calls++;memset(p,r->zero?0:0x55,n);return r->status;
}
static void revoked(void *ctx,const gt_token_scope_t *scope) {
  random_source *r=ctx;CHECK(scope->principal && scope->issuer && scope->session && scope->generation);if(r)r->revoked++;
}
static gt_token_owner_t owner(random_source *r){return (gt_token_owner_t){r,revoked};}
static bool zero(const void *p,size_t n){const unsigned char *b=p;for(size_t i=0;i<n;i++)if(b[i])return false;return true;}
static gt_token_store_t *open_store(random_source *random,size_t capacity,uint64_t now) {
  gt_token_store_t *s=NULL;gt_token_random_t r={random,fill};
  CHECK(gt__tokens_open(instance,capacity,now,&r,owner(random),NULL,&s)==GT_TOKEN_OK && s);return s;
}
static gt_token_rc_t consume(gt_token_store_t *s,const gt_attachment_token_t *token,uint64_t now,gt_token_scope_t *out) {
  return gt__tokens_consume(s,token,instance,1,3,4,now,out);
}
static void lifecycle(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,0);gt_attachment_token_t token;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,10,&token)==GT_TOKEN_OK && r.calls==1);
  CHECK(token.bytes[7]==1 && token.bytes[8]==0x55);
  CHECK(consume(s,&token,9,&out)==GT_TOKEN_OK && out.principal==1 && out.issuer==2 && out.session==3 && out.generation==4);
  CHECK(consume(s,&token,9,&out)==GT_TOKEN_DENIED && zero(&out,sizeof(out)));
  gt__tokens_close(s);
}
static void binding(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,0);gt_attachment_token_t token;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,100,&token)==GT_TOKEN_OK);
  uint8_t other[GT_INSTANCE_BYTES]={2};
  CHECK(gt__tokens_consume(s,&token,other,1,3,4,1,&out)==GT_TOKEN_DENIED);
  CHECK(gt__tokens_consume(s,&token,instance,2,3,4,2,&out)==GT_TOKEN_DENIED);
  CHECK(gt__tokens_consume(s,&token,instance,1,5,4,3,&out)==GT_TOKEN_DENIED);
  CHECK(gt__tokens_consume(s,&token,instance,1,3,5,4,&out)==GT_TOKEN_DENIED);
  for(unsigned i=0;i<GT_TOKEN_BYTES;i++) {
    gt_attachment_token_t bad=token;bad.bytes[i]^=1;
    CHECK(consume(s,&bad,5,&out)==GT_TOKEN_DENIED && zero(&out,sizeof(out)));
  }
  CHECK(consume(s,&token,6,&out)==GT_TOKEN_OK && out.issuer==2);gt__tokens_close(s);
}
static void capacity(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,0);gt_attachment_token_t a,b;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,10,&a)==GT_TOKEN_OK);
  CHECK(gt__tokens_issue(s,scope(),0,10,&b)==GT_TOKEN_BUSY && r.calls==1);
  gt_token_scope_t next=scope();next.generation=5;
  CHECK(gt__tokens_issue(s,next,1,10,&b)==GT_TOKEN_FULL && zero(&b,sizeof(b)) && r.calls==1);
  CHECK(gt__tokens_issue(s,scope(),10,10,&b)==GT_TOKEN_OK);
  CHECK(r.revoked==1 && consume(s,&a,10,&out)==GT_TOKEN_DENIED);CHECK(consume(s,&b,19,&out)==GT_TOKEN_OK);
  CHECK(gt__tokens_issue(s,scope(),19,1,&b)==GT_TOKEN_OK);CHECK(consume(s,&b,20,&out)==GT_TOKEN_DENIED);
  gt__tokens_close(s);
}
static void replay(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,0);gt_attachment_token_t a,b;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,100,&a)==GT_TOKEN_OK && consume(s,&a,0,&out)==GT_TOKEN_OK);
  CHECK(gt__tokens_issue(s,scope(),0,100,&b)==GT_TOKEN_OK && memcmp(&a,&b,sizeof(a)));
  CHECK(consume(s,&a,0,&out)==GT_TOKEN_DENIED && consume(s,&b,0,&out)==GT_TOKEN_OK);
  gt__tokens_close(s);
}
static void idle_expiry(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,0);gt_attachment_token_t token;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,10,&token)==GT_TOKEN_OK);
  CHECK(gt__tokens_expire(s,9)==GT_TOKEN_OK && !r.revoked);
  CHECK(gt__tokens_expire(s,10)==GT_TOKEN_OK && r.revoked==1);
  CHECK(consume(s,&token,10,&out)==GT_TOKEN_DENIED);gt__tokens_close(s);CHECK(r.revoked==1);
}
static void revocation(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,4,0);gt_attachment_token_t a,b,c,d;gt_token_scope_t out;
  gt_token_scope_t one=scope(),two=scope(),three=scope(),four=scope();two.issuer=9;two.generation=5;three.generation=6;four.session=8;
  CHECK(gt__tokens_issue(s,one,0,100,&a)==GT_TOKEN_OK && gt__tokens_issue(s,two,0,100,&b)==GT_TOKEN_OK);
  CHECK(gt__tokens_issue(s,three,0,100,&c)==GT_TOKEN_OK && gt__tokens_issue(s,four,0,100,&d)==GT_TOKEN_OK);
  gt__tokens_revoke_generation(s,3,4);
  CHECK(consume(s,&a,1,&out)==GT_TOKEN_DENIED && gt__tokens_consume(s,&b,instance,1,3,5,1,&out)==GT_TOKEN_OK);
  CHECK(gt__tokens_consume(s,&c,instance,1,3,6,1,&out)==GT_TOKEN_OK);
  CHECK(gt__tokens_consume(s,&d,instance,1,8,4,1,&out)==GT_TOKEN_OK);
  CHECK(gt__tokens_issue(s,one,2,100,&a)==GT_TOKEN_OK && gt__tokens_issue(s,two,2,100,&b)==GT_TOKEN_OK);
  gt__tokens_revoke_issuer(s,2);CHECK(consume(s,&a,3,&out)==GT_TOKEN_DENIED && gt__tokens_consume(s,&b,instance,1,3,5,3,&out)==GT_TOKEN_OK);
  CHECK(r.revoked==2);gt__tokens_close(s);
}
static void entropy(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,2,0);gt_attachment_token_t a,b;gt_token_scope_t out;gt_token_scope_t next=scope();next.generation=5;
  CHECK(gt__tokens_issue(s,scope(),0,100,&a)==GT_TOKEN_OK);r.status=GT_ERR_IO;
  CHECK(gt__tokens_issue(s,next,1,100,&b)==GT_TOKEN_ENTROPY && zero(&b,sizeof(b)));
  r.status=GT_ERR_UNSUPPORTED;CHECK(gt__tokens_issue(s,next,1,100,&b)==GT_TOKEN_UNSUPPORTED && zero(&b,sizeof(b)));
  r.status=GT_OK;r.zero=true;CHECK(gt__tokens_issue(s,next,1,100,&b)==GT_TOKEN_ENTROPY && zero(&b,sizeof(b)));
  CHECK(consume(s,&a,2,&out)==GT_TOKEN_OK);r.zero=false;
  CHECK(gt__tokens_issue(s,next,3,100,&b)==GT_TOKEN_OK && gt__tokens_consume(s,&b,instance,1,3,5,3,&out)==GT_TOKEN_OK);gt__tokens_close(s);
}
static void clock_test(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,10);gt_attachment_token_t a;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),10,100,&a)==GT_TOKEN_OK);
  CHECK(consume(s,&a,9,&out)==GT_TOKEN_CLOCK && zero(&out,sizeof(out)));
  CHECK(gt__tokens_issue(s,scope(),11,100,&a)==GT_TOKEN_CLOCK && zero(&a,sizeof(a)));
  CHECK(r.revoked==1);gt__tokens_close(s);s=open_store(&r,1,UINT64_MAX-5);
  CHECK(gt__tokens_issue(s,scope(),UINT64_MAX-5,10,&a)==GT_TOKEN_OK);
  CHECK(consume(s,&a,UINT64_MAX,&out)==GT_TOKEN_OK);gt__tokens_close(s);
}
typedef struct {size_t bytes;bool reject,require_wipe;} allocation;
static void *admit(void *ctx,size_t n){allocation *a=ctx;if(a->reject)return NULL;CHECK(!a->bytes);a->bytes=n;return malloc(n);}
static void refund(void *ctx,void *p,size_t n){allocation *a=ctx;CHECK(p && a->bytes==n);if(a->require_wipe)CHECK(zero(p,n));a->bytes=0;free(p);}
static void allocation_test(void) {
  allocation a={.reject=true};gt_allocator_t domain={&a,admit,refund};gt_token_store_t *s=(gt_token_store_t *)1;
  CHECK(gt__tokens_open(instance,2,0,NULL,owner(NULL),&domain,&s)==GT_TOKEN_NO_MEMORY && !s && !a.bytes);
  random_source r={0};gt_token_random_t rng={&r,fill};a.reject=false;a.require_wipe=true;
  CHECK(gt__tokens_open(instance,2,0,&rng,owner(&r),&domain,&s)==GT_TOKEN_OK && a.bytes);
  gt_attachment_token_t token;CHECK(gt__tokens_issue(s,scope(),0,100,&token)==GT_TOKEN_OK);
  gt__tokens_close(s);CHECK(!a.bytes);
}
static void os_random(void) {
  uint8_t bytes[256];memset(bytes,0,sizeof(bytes));CHECK(gt__secure_random(bytes,sizeof(bytes))==GT_OK && !zero(bytes,sizeof(bytes)));
  gt__secure_erase(bytes,sizeof(bytes));CHECK(zero(bytes,sizeof(bytes)));
  CHECK(gt__secure_random(NULL,1)==GT_ERR_INVALID_ARG && gt__secure_random(bytes,0)==GT_ERR_INVALID_ARG);
  CHECK(gt__secure_random(bytes,257)==GT_ERR_INVALID_ARG);
  gt_token_store_t *s=NULL;gt_attachment_token_t token;gt_token_scope_t out;
  CHECK(gt__tokens_open(instance,1,0,NULL,owner(NULL),NULL,&s)==GT_TOKEN_OK && gt__tokens_issue(s,scope(),0,100,&token)==GT_TOKEN_OK);
  CHECK(consume(s,&token,0,&out)==GT_TOKEN_OK);gt__secure_erase(&token,sizeof(token));gt__tokens_close(s);
}
static void invalid(void) {
  random_source r={0};gt_token_random_t rng={&r,fill};gt_token_store_t *s=(gt_token_store_t *)1;uint8_t empty[GT_INSTANCE_BYTES]={0};
  CHECK(gt__tokens_open(empty,1,0,&rng,owner(&r),NULL,&s)==GT_TOKEN_INVALID && !s);
  CHECK(gt__tokens_open(instance,0,0,&rng,owner(&r),NULL,&s)==GT_TOKEN_INVALID);
  CHECK(gt__tokens_open(instance,257,0,&rng,owner(&r),NULL,&s)==GT_TOKEN_INVALID);
  CHECK(gt__tokens_open(instance,1,0,&rng,owner(&r),NULL,NULL)==GT_TOKEN_INVALID);
  s=open_store(&r,1,0);gt_attachment_token_t token;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,0,&token)==GT_TOKEN_INVALID && zero(&token,sizeof(token)));
  CHECK(gt__tokens_issue(s,scope(),0,60001,&token)==GT_TOKEN_INVALID);
  gt_token_scope_t bad=scope();bad.principal=0;CHECK(gt__tokens_issue(s,bad,0,1,&token)==GT_TOKEN_INVALID);
  CHECK(gt__tokens_consume(s,&token,instance,0,3,4,0,&out)==GT_TOKEN_INVALID);
  gt__tokens_revoke_issuer(NULL,1);gt__tokens_revoke_generation(NULL,1,1);gt__tokens_close(NULL);gt__tokens_close(s);
}
/* Invoked only by an isolated source-copy fixture initialized at UINT64_MAX. */
static void exhaustion(void) {
  random_source r={0};gt_token_store_t *s=open_store(&r,1,0);gt_attachment_token_t a,b;gt_token_scope_t out;
  CHECK(gt__tokens_issue(s,scope(),0,100,&a)==GT_TOKEN_OK);
  for(unsigned i=0;i<8;i++)CHECK(a.bytes[i]==0xff);
  CHECK(consume(s,&a,0,&out)==GT_TOKEN_OK);
  CHECK(gt__tokens_issue(s,scope(),0,100,&b)==GT_TOKEN_EXHAUSTED && zero(&b,sizeof(b)));gt__tokens_close(s);
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"lifecycle"))lifecycle();else if(!strcmp(argv[1],"binding"))binding();
  else if(!strcmp(argv[1],"capacity"))capacity();else if(!strcmp(argv[1],"replay"))replay();
  else if(!strcmp(argv[1],"revocation"))revocation();else if(!strcmp(argv[1],"entropy"))entropy();
  else if(!strcmp(argv[1],"clock"))clock_test();else if(!strcmp(argv[1],"allocation"))allocation_test();
  else if(!strcmp(argv[1],"os-random"))os_random();else if(!strcmp(argv[1],"invalid"))invalid();
  else if(!strcmp(argv[1],"exhaustion"))exhaustion();else if(!strcmp(argv[1],"idle-expiry"))idle_expiry();else CHECK(false);
  printf("PASS instance attachment token %s\n",argv[1]);return 0;
}
