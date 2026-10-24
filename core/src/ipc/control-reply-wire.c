#include "control.h"
#include "base/secure-random.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
bool gt__control_creation_decode(const void *bytes,size_t n,uint8_t nonce[16],gt_creation_result_t *out) {
  if(nonce)memset(nonce,0,16);
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || n!=GT_CREATE_REPLY_BYTES || !nonce || !out)return false;
  const uint8_t *p=bytes;uint32_t state=get32(p+16),detail=get32(p+20);uint64_t id=get64(p+24);
  if(!nonzero(p,16) || state>GT_CREATION_INTERNAL || detail>GT_ERR_INTERNAL ||
     (state==GT_CREATION_ACCEPTED?(!id || detail!=GT_OK):id!=0))return false;
  memcpy(nonce,p,16);*out=(gt_creation_result_t){(gt_creation_status_t)state,(gt_status_t)detail,id};return true;
}
static bool attach_valid(const gt_attach_result_t *r) {
  if(!r->session || (unsigned)r->status>GT_ATTACH_INTERNAL || (unsigned)r->detail>GT_ERR_INTERNAL)return false;
  if(r->status==GT_ATTACH_OK)return r->detail==GT_OK && nonzero(r->instance,16) && r->generation && r->ttl_ms && r->ttl_ms<=60000 &&
    nonzero(r->token.bytes,8) && nonzero(r->token.bytes+8,32);
  return !nonzero(r->instance,16) && !r->generation && !r->ttl_ms && !nonzero(r->token.bytes,40);
}
bool gt__control_attach_decode(const void *bytes,size_t n,gt_attach_result_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || !out || n!=GT_ATTACH_REPLY_BYTES)return false;
  const uint8_t *p=bytes;gt_attach_result_t r={.status=(gt_attach_status_t)get32(p),.detail=(gt_status_t)get32(p+4),
    .session=get64(p+24),.generation=get64(p+32),.ttl_ms=get32(p+40)};
  if(get32(p+44))return false;
  memcpy(r.instance,p+8,16);memcpy(r.token.bytes,p+48,40);
  if(!attach_valid(&r)){gt__secure_erase(&r,sizeof(r));return false;}
  *out=r;gt__secure_erase(&r,sizeof(r));return true;
}
bool gt__control_attach_encode(const gt_attach_result_t *r,uint8_t out[GT_ATTACH_REPLY_BYTES]) {
  if(!out)return false;
  memset(out,0,GT_ATTACH_REPLY_BYTES);if(!r || !attach_valid(r))return false;
  put32(out,(uint32_t)r->status);put32(out+4,(uint32_t)r->detail);memcpy(out+8,r->instance,16);
  put64(out+24,r->session);put64(out+32,r->generation);put32(out+40,r->ttl_ms);memcpy(out+48,r->token.bytes,40);return true;
}
