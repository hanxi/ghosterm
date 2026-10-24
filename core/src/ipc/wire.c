#include "wire.h"
#include <string.h>

static uint16_t get16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]<<8 | p[1]);}
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}

bool gt__wire_header_encode(const gt_wire_header_t *h,uint8_t out[GT_WIRE_HEADER_BYTES]) {
  if(!out)return false;
  memset(out,0,GT_WIRE_HEADER_BYTES);
  if(!h || !h->type || h->flags || h->payload_length>GT_WIRE_MAX_PAYLOAD)return false;
  memcpy(out,"GTSD",4);put16(out+4,GT_WIRE_VERSION);put16(out+6,h->type);
  put32(out+8,h->flags);put32(out+12,h->payload_length);put64(out+16,h->request_id);
  return true;
}
bool gt__wire_header_decode(const uint8_t in[GT_WIRE_HEADER_BYTES],uint32_t limit,gt_wire_header_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!in || !out || limit>GT_WIRE_MAX_PAYLOAD)return false;
  if(memcmp(in,"GTSD",4) || get16(in+4)!=GT_WIRE_VERSION)return false;
  gt_wire_header_t h={.type=get16(in+6),.flags=get32(in+8),
    .payload_length=get32(in+12),.request_id=get64(in+16)};
  if(!h.type || h.flags || h.payload_length>limit)return false;
  *out=h;return true;
}
bool gt__wire_decoder_init(gt_wire_decoder_t *d,void *storage,size_t capacity,uint32_t limit) {
  if(!d)return false;
  memset(d,0,sizeof(*d));d->failed=true;
  if(limit>GT_WIRE_MAX_PAYLOAD || capacity<limit || (capacity && !storage))return false;
  d->storage=storage;d->capacity=capacity;d->limit=limit;d->failed=false;return true;
}
static gt_wire_result_t fail(gt_wire_decoder_t *d){if(d)d->failed=true;return GT_WIRE_ERROR;}
gt_wire_result_t gt__wire_decoder_feed(gt_wire_decoder_t *d,const void *bytes,size_t length,size_t *consumed) {
  if(consumed)*consumed=0;
  if(!d || !consumed || (length && !bytes))return fail(d);
  if(d->failed)return GT_WIRE_ERROR;
  if(d->ready)return GT_WIRE_FRAME;
  if(d->ended)return GT_WIRE_END;
  const uint8_t *p=bytes;
  if(d->envelope_used<GT_WIRE_HEADER_BYTES) {
    size_t n=GT_WIRE_HEADER_BYTES-d->envelope_used;if(n>length)n=length;
    if(n)memcpy(d->envelope+d->envelope_used,p,n);
    d->envelope_used+=n;*consumed+=n;length-=n;if(n)p+=n;
    if(d->envelope_used<GT_WIRE_HEADER_BYTES)return GT_WIRE_MORE;
    if(!gt__wire_header_decode(d->envelope,d->limit,&d->header))return fail(d);
  }
  size_t n=d->header.payload_length-d->payload_used;if(n>length)n=length;
  if(n)memcpy(d->storage+d->payload_used,p,n);
  d->payload_used+=n;*consumed+=n;
  if(d->payload_used<d->header.payload_length)return GT_WIRE_MORE;
  d->ready=true;return GT_WIRE_FRAME;
}
bool gt__wire_decoder_frame(const gt_wire_decoder_t *d,gt_wire_frame_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!d || !out || d->failed || !d->ready)return false;
  *out=(gt_wire_frame_t){.header=d->header,.payload=d->storage};return true;
}
bool gt__wire_decoder_next(gt_wire_decoder_t *d) {
  if(!d || d->failed || !d->ready)return false;
  d->ready=false;d->envelope_used=0;d->payload_used=0;memset(&d->header,0,sizeof(d->header));
  return true;
}
gt_wire_result_t gt__wire_decoder_finish(gt_wire_decoder_t *d) {
  if(!d || d->failed)return GT_WIRE_ERROR;
  d->ended=true;
  if(d->ready)return GT_WIRE_FRAME;
  if(d->envelope_used || d->payload_used)return fail(d);
  return GT_WIRE_END;
}
