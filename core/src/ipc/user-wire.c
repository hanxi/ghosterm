#include "user-wire.h"
#include <string.h>
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v=(v<<8)|p[i];return v;}
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=n;i;i--){p[i-1]=(uint8_t)v;v>>=8;}}
static uint16_t type(gt_worker_command_t k){return k==GT_WORKER_INPUT?GT_MSG_USER_INPUT:k==GT_WORKER_RESIZE?GT_MSG_USER_RESIZE:k==GT_WORKER_COLOR_SCHEME?GT_MSG_USER_COLOR:k==GT_WORKER_STYLE?GT_MSG_USER_STYLE:0;}
static bool kind(uint16_t t,gt_worker_command_t *k){if(t==GT_MSG_USER_INPUT)*k=GT_WORKER_INPUT;else if(t==GT_MSG_USER_RESIZE)*k=GT_WORKER_RESIZE;else if(t==GT_MSG_USER_COLOR)*k=GT_WORKER_COLOR_SCHEME;else if(t==GT_MSG_USER_STYLE)*k=GT_WORKER_STYLE;else return false;return true;}
static bool stamp_valid(gt_worker_stamp_t s){return s.generation && s.command_id;}
static gt_worker_stamp_t stamp_get(const uint8_t *p){return (gt_worker_stamp_t){get(p,8),get(p+8,8),get(p+16,8)};}
static void stamp_put(uint8_t *p,gt_worker_stamp_t s){put(p,s.generation,8);put(p+8,s.command_id,8);put(p+16,s.applied_sequence,8);}
static bool envelope(const gt_wire_frame_t *f){return f && !f->header.flags && !f->header.request_id && f->payload;}
bool gt__user_request_decode(const gt_wire_frame_t *f,gt_user_request_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!out || !envelope(f))return false;
  gt_user_request_t r={0};if(!kind(f->header.type,&r.kind) || f->header.payload_length<GT_USER_PREFIX_BYTES)return false;
  r.stamp=stamp_get(f->payload);if(!stamp_valid(r.stamp))return false;
  size_t n=f->header.payload_length;const uint8_t *p=f->payload+GT_USER_PREFIX_BYTES;
  if(r.kind==GT_WORKER_INPUT){if(n<=GT_USER_PREFIX_BYTES || n>GT_USER_PREFIX_BYTES+GT_USER_MAX_INPUT)return false;r.input=p;r.length=n-GT_USER_PREFIX_BYTES;}
  else if(r.kind==GT_WORKER_RESIZE){if(n!=36)return false;r.size=(gt_state_size_t){(uint16_t)get(p,2),(uint16_t)get(p+2,2),(uint32_t)get(p+4,4),(uint32_t)get(p+8,4)};if(!r.size.cols || !r.size.rows)return false;}
  else if(r.kind==GT_WORKER_STYLE){if(n!=805 || get(p,4)>1)return false;r.style.dark=(uint32_t)get(p,4);memcpy(r.style.background,p+4,3);memcpy(r.style.foreground,p+7,3);memcpy(r.style.cursor,p+10,3);memcpy(r.style.palette,p+13,768);}
  else {if(n!=28 || get(p,4)>1)return false;r.color.dark=(uint32_t)get(p,4);}
  *out=r;return true;
}
bool gt__user_request_encode(const gt_user_request_t *r,void *bytes,size_t cap,gt_wire_header_t *h) {
  if(h)memset(h,0,sizeof(*h));
  if(!h || !r || !bytes || !stamp_valid(r->stamp) || !type(r->kind))return false;
  size_t n=r->kind==GT_WORKER_INPUT?GT_USER_PREFIX_BYTES+r->length:r->kind==GT_WORKER_RESIZE?36:r->kind==GT_WORKER_STYLE?805:28;
  if((r->kind==GT_WORKER_INPUT && (!r->input || !r->length || r->length>GT_USER_MAX_INPUT)) ||
     (r->kind==GT_WORKER_RESIZE && (!r->size.cols || !r->size.rows)) ||
     (r->kind==GT_WORKER_COLOR_SCHEME && r->color.dark>1) ||
     (r->kind==GT_WORKER_STYLE && r->style.dark>1) || n>cap)return false;
  uint8_t *p=bytes;stamp_put(p,r->stamp);p+=GT_USER_PREFIX_BYTES;
  if(r->kind==GT_WORKER_INPUT)memcpy(p,r->input,r->length);
  else if(r->kind==GT_WORKER_RESIZE){put(p,r->size.cols,2);put(p+2,r->size.rows,2);put(p+4,r->size.px_width,4);put(p+8,r->size.px_height,4);}
  else if(r->kind==GT_WORKER_STYLE){put(p,r->style.dark,4);memcpy(p+4,r->style.background,3);memcpy(p+7,r->style.foreground,3);memcpy(p+10,r->style.cursor,3);memcpy(p+13,r->style.palette,768);}
  else put(p,r->color.dark,4);
  *h=(gt_wire_header_t){.type=type(r->kind),.payload_length=(uint32_t)n};return true;
}
static bool reply_valid(const gt_user_reply_t *r) {
  if(!r || !type(r->kind) || !stamp_valid(r->stamp))return false;
  if(r->rejected)return r->rejection>=GT_USER_REJECT_BACKPRESSURE && r->rejection<=GT_USER_REJECT_UNAVAILABLE && r->status==GT_OK && r->delivery==GT_USER_NOT_EXECUTED;
  return !r->rejection && (unsigned)r->status<=GT_ERR_INTERNAL && (unsigned)r->delivery<=GT_USER_OUTCOME_UNKNOWN &&
    ((r->status==GT_OK)==(r->delivery==GT_USER_OPERATION_OK));
}
bool gt__user_reply_encode(const gt_user_reply_t *r,void *bytes,size_t cap,gt_wire_header_t *h) {
  if(h)memset(h,0,sizeof(*h));
  if(!h || !bytes || !reply_valid(r))return false;
  size_t n=r->rejected?GT_USER_REJECT_BYTES:GT_USER_RESULT_BYTES;if(cap<n)return false;
  uint8_t *p=bytes;memset(p,0,n);stamp_put(p,r->stamp);put(p+24,type(r->kind),2);
  if(r->rejected)put(p+28,r->rejection,4);else {put(p+28,r->status,4);put(p+32,r->delivery,4);}
  *h=(gt_wire_header_t){.type=r->rejected?GT_MSG_USER_REJECT:GT_MSG_USER_RESULT,.payload_length=(uint32_t)n};return true;
}
bool gt__user_reply_decode(const gt_wire_frame_t *f,gt_user_reply_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!out || !envelope(f))return false;
  bool rejected=f->header.type==GT_MSG_USER_REJECT;
  if((!rejected && f->header.type!=GT_MSG_USER_RESULT) || f->header.payload_length!=(rejected?GT_USER_REJECT_BYTES:GT_USER_RESULT_BYTES))return false;
  const uint8_t *p=f->payload;gt_user_reply_t r={.stamp=stamp_get(p),.rejected=rejected};
  if(!kind((uint16_t)get(p+24,2),&r.kind) || get(p+26,2))return false;
  if(rejected)r.rejection=(gt_user_reject_t)get(p+28,4);else {r.status=(gt_status_t)get(p+28,4);r.delivery=(gt_user_delivery_t)get(p+32,4);if(get(p+36,4))return false;}
  if(!reply_valid(&r))return false;
  *out=r;return true;
}
