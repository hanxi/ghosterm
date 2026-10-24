#include "bootstrap-wire.h"
#include "base/secure-random.h"
#include <string.h>
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static bool role_valid(gt_protocol_role_t role){return role==GT_ROLE_MANAGEMENT || role==GT_ROLE_DATA;}
static gt_status_t random_fill(const gt_token_random_t *r,uint8_t *out,size_t n) {
  gt_status_t status=r && r->fill?r->fill(r->context,out,n):gt__secure_random(out,n);
  if(status!=GT_OK || !nonzero(out,n)){gt__secure_erase(out,n);return status==GT_ERR_UNSUPPORTED?status:GT_ERR_IO;}
  return GT_OK;
}
gt_status_t gt__protocol_identity_new(const uint8_t compatibility[GT_COMPAT_BYTES],uint64_t features,
  const gt_token_random_t *random,gt_protocol_identity_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!out || !compatibility || !nonzero(compatibility,GT_COMPAT_BYTES) || (random && !random->fill))return GT_ERR_INVALID_ARG;
  gt_protocol_identity_t identity={.features=features};
  gt_status_t status=random_fill(random,identity.instance,GT_INSTANCE_BYTES);if(status!=GT_OK)return status;
  memcpy(identity.compatibility,compatibility,GT_COMPAT_BYTES);*out=identity;return GT_OK;
}
bool gt__hello_encode(const gt_protocol_hello_t *h,uint8_t out[GT_HELLO_BYTES]) {
  if(!out)return false;
  memset(out,0,GT_HELLO_BYTES);
  if(!h || !role_valid(h->role) || !nonzero(h->compatibility,GT_COMPAT_BYTES) ||
     (h->role==GT_ROLE_DATA && !nonzero(h->expected_instance,GT_INSTANCE_BYTES)))return false;
  out[1]=(uint8_t)h->role;memcpy(out+4,h->compatibility,GT_COMPAT_BYTES);
  put64(out+36,h->required_features);memcpy(out+44,h->expected_instance,GT_INSTANCE_BYTES);return true;
}
bool gt__hello_decode(const void *bytes,size_t n,gt_protocol_hello_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || !out || n!=GT_HELLO_BYTES)return false;
  const uint8_t *p=bytes;
  if(p[0] || p[2] || p[3] || !role_valid((gt_protocol_role_t)p[1]) || !nonzero(p+4,GT_COMPAT_BYTES) ||
     (p[1]==GT_ROLE_DATA && !nonzero(p+44,GT_INSTANCE_BYTES)))return false;
  out->role=(gt_protocol_role_t)p[1];out->required_features=get64(p+36);
  memcpy(out->compatibility,p+4,GT_COMPAT_BYTES);memcpy(out->expected_instance,p+44,GT_INSTANCE_BYTES);return true;
}
bool gt__hello_reply_decode(const void *bytes,size_t n,gt_protocol_hello_reply_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || !out || n!=GT_HELLO_REPLY_BYTES)return false;
  const uint8_t *p=bytes;uint32_t status=get32(p);
  if(status>GT_HELLO_DENIED || p[4] || !role_valid((gt_protocol_role_t)p[5]) || p[6] || p[7] ||
     !nonzero(p+8,GT_COMPAT_BYTES) || !nonzero(p+48,GT_INSTANCE_BYTES) || !nonzero(p+64,GT_CHALLENGE_BYTES))return false;
  out->status=(gt_hello_status_t)status;out->role=(gt_protocol_role_t)p[5];
  memcpy(out->identity.compatibility,p+8,GT_COMPAT_BYTES);out->identity.features=get64(p+40);
  memcpy(out->identity.instance,p+48,GT_INSTANCE_BYTES);memcpy(out->challenge,p+64,GT_CHALLENGE_BYTES);return true;
}
bool gt__bind_encode(const gt_protocol_bind_t *b,uint8_t out[GT_BIND_BYTES]) {
  if(!out)return false;
  memset(out,0,GT_BIND_BYTES);
  if(!b || !b->session || !b->generation || !nonzero(b->instance,GT_INSTANCE_BYTES) || !nonzero(b->token.bytes,GT_TOKEN_BYTES))return false;
  memcpy(out,b->instance,GT_INSTANCE_BYTES);put64(out+16,b->session);put64(out+24,b->generation);
  memcpy(out+32,b->token.bytes,GT_TOKEN_BYTES);return true;
}
bool gt__bind_reply_decode(const void *bytes,size_t n,gt_protocol_bind_reply_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || !out || n!=GT_BIND_REPLY_BYTES)return false;
  const uint8_t *p=bytes;uint32_t status=get32(p);
  if(status>GT_HELLO_DENIED || !get64(p+4) || !get64(p+12) || !nonzero(p+20,GT_CHALLENGE_BYTES))return false;
  out->status=(gt_hello_status_t)status;out->session=get64(p+4);out->generation=get64(p+12);
  memcpy(out->challenge,p+20,GT_CHALLENGE_BYTES);return true;
}
