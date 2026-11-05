/* Independent implementation of the SHA-256 algorithm (RFC 6234 sections
 * 4.1, 5.1, 6.1/6.2); no sample source code is incorporated. */
#include "sha256.h"
#include "secure-random.h"
#include <string.h>
static uint32_t rotate(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void transform(gt_sha256_t *s) {
  static const uint32_t k[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t w[64];
  for(size_t i=0;i<16;i++){size_t j=4*i;w[i]=((uint32_t)s->block[j]<<24)|((uint32_t)s->block[j+1]<<16)|((uint32_t)s->block[j+2]<<8)|s->block[j+3];}
  for(size_t i=16;i<64;i++)w[i]=w[i-16]+(rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10));
  uint32_t a=s->state[0],b=s->state[1],c=s->state[2],d=s->state[3],e=s->state[4],f=s->state[5],g=s->state[6],h=s->state[7];
  for(size_t i=0;i<64;i++){
    uint32_t t=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
    uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
    h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
  }
  s->state[0]+=a;s->state[1]+=b;s->state[2]+=c;s->state[3]+=d;s->state[4]+=e;s->state[5]+=f;s->state[6]+=g;s->state[7]+=h;
  gt__secure_erase(w,sizeof(w));
}
void gt__sha256_init(gt_sha256_t *s){
  if(!s)return;memset(s,0,sizeof(*s));
  static const uint32_t initial[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(s->state,initial,sizeof(initial));
}
bool gt__sha256_update(gt_sha256_t *s,const void *data,size_t n){
  if(!s || s->ended || (!data && n) || n>UINT64_MAX/8-s->bytes)return false;
  const uint8_t *p=data;s->bytes+=n;
  while(n){size_t take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==64){transform(s);s->used=0;}}
  return true;
}
bool gt__sha256_final(gt_sha256_t *s,char hex[65]){
  if(hex)memset(hex,0,65);if(!s || !hex || s->ended)return false;
  uint64_t bits=s->bytes*8;s->block[s->used++]=0x80;
  if(s->used>56){memset(s->block+s->used,0,64-s->used);transform(s);s->used=0;}
  memset(s->block+s->used,0,56-s->used);
  for(size_t i=0;i<8;i++)s->block[63-i]=(uint8_t)(bits>>(8*i));transform(s);
  static const char digits[]="0123456789abcdef";
  for(size_t i=0;i<32;i++){uint8_t b=(uint8_t)(s->state[i/4]>>(24-8*(i%4)));hex[2*i]=digits[b>>4];hex[2*i+1]=digits[b&15];}
  gt__secure_erase(s,sizeof(*s));s->ended=true;return true;
}
