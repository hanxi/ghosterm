#include "control.h"
#include <limits.h>
#include <string.h>
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static int32_t signed32(uint32_t v){return v>INT32_MAX?-1-(int32_t)(UINT32_MAX-v):(int32_t)v;}
static bool valid_info(const gt_managed_info_t *r) {
  if(!r->id || (unsigned)r->phase>GT_MANAGED_RETAINED)return false;
  if(!r->has_result)return r->phase==GT_MANAGED_WORKER;
  const gt_worker_result_t *v=&r->result;
  if(v->has_exit_code) {
    if(v->termination_status!=GT_OK || v->termination.kind!=GT_IO_EXIT_CODE || v->exit_code<0 || v->exit_code!=v->termination.code)return false;
  } else if(v->termination_status==GT_OK && v->termination.kind==GT_IO_EXIT_CODE)return false;
  return (unsigned)v->kind<=GT_WORKER_END_ERROR && (unsigned)v->stage<=GT_WORKER_STAGE_CAPTURE &&
    (unsigned)v->status<=GT_ERR_INTERNAL && (unsigned)v->termination_status<=GT_ERR_INTERNAL &&
    (unsigned)v->termination.kind<=GT_IO_EXIT_SIGNAL &&
    (v->termination_status==GT_OK?
      ((v->termination.kind==GT_IO_EXIT_CODE && v->termination.code>=0) ||
       (v->termination.kind==GT_IO_EXIT_SIGNAL && v->termination.code>0)):
      v->termination.kind==GT_IO_EXIT_UNKNOWN);
}
bool gt__control_info_encode(const gt_managed_info_t *r,uint8_t out[GT_CONTROL_INFO_BYTES]) {
  if(!out)return false;
  memset(out,0,GT_CONTROL_INFO_BYTES);if(!r || !valid_info(r))return false;
  put64(out,r->id);put32(out+8,(uint32_t)r->phase+1);
  uint32_t flags=r->termination_requested?1u:0u;
  if(r->has_result) {
    const gt_worker_result_t *v=&r->result;flags|=2u;
    if(v->has_sequence){flags|=4u;put64(out+32,v->sequence);}
    if(v->has_exit_code){flags|=8u;put32(out+40,(uint32_t)v->exit_code);}
    put32(out+16,(uint32_t)v->kind);put32(out+20,(uint32_t)v->stage);
    put32(out+24,(uint32_t)v->status);put32(out+28,(uint32_t)v->termination_status);
    put32(out+44,(uint32_t)v->termination.kind);
    if(v->termination_status==GT_OK)put32(out+48,(uint32_t)v->termination.code);
  }
  put32(out+12,flags);return true;
}
bool gt__control_info_decode(const void *bytes,size_t n,gt_managed_info_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || !out || n!=GT_CONTROL_INFO_BYTES)return false;
  const uint8_t *p=bytes;uint32_t phase=get32(p+8),flags=get32(p+12);
  if(!phase || phase>3 || flags&~15u)return false;
  for(unsigned i=52;i<64;i++)if(p[i])return false;
  gt_managed_info_t r={.id=get64(p),.phase=(gt_managed_phase_t)(phase-1),
    .termination_requested=(flags&1u)!=0,.has_result=(flags&2u)!=0};
  if(r.has_result) {
    r.result=(gt_worker_result_t){.kind=(gt_worker_end_t)get32(p+16),.stage=(gt_worker_stage_t)get32(p+20),
      .status=(gt_status_t)get32(p+24),.termination_status=(gt_status_t)get32(p+28),
      .has_sequence=(flags&4u)!=0,.has_exit_code=(flags&8u)!=0,.sequence=get64(p+32),
      .exit_code=signed32(get32(p+40)),.termination={(gt_io_exit_kind_t)get32(p+44),signed32(get32(p+48))}};
    if((!r.result.has_sequence && r.result.sequence) || (!r.result.has_exit_code && r.result.exit_code) ||
       (r.result.termination_status!=GT_OK && r.result.termination.code))return false;
  } else {
    if(flags&12u)return false;
    for(unsigned i=16;i<52;i++)if(p[i])return false;
  }
  if(!valid_info(&r))return false;
  *out=r;return true;
}
