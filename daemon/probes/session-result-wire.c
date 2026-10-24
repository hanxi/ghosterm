#include "session-result-wire.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(24-8*i));}
static uint32_t get32(const uint8_t *p){uint32_t n=0;for(unsigned i=0;i<4;i++)n=(n<<8)|p[i];return n;}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static bool valid(const gt_worker_result_t *r) {
  if(!r || r->kind<GT_WORKER_END_STOPPED || r->kind>GT_WORKER_END_ERROR ||
     (unsigned)r->stage>GT_WORKER_STAGE_CAPTURE || (unsigned)r->status>GT_ERR_INTERNAL ||
     (unsigned)r->termination_status>GT_ERR_INTERNAL)return false;
  if(r->kind==GT_WORKER_END_ERROR) {if(r->status==GT_OK || r->stage==GT_WORKER_STAGE_NONE || (r->stage==GT_WORKER_STAGE_STATE_OPEN && r->has_sequence))return false;}
  else if(r->status!=GT_OK || r->stage!=GT_WORKER_STAGE_NONE || !r->has_sequence)return false;
  if(!r->has_sequence && (r->sequence || (r->stage!=GT_WORKER_STAGE_STATE_OPEN && r->stage!=GT_WORKER_STAGE_REPLY)))return false;
  if(r->kind!=GT_WORKER_END_EOF && r->termination_status!=GT_ERR_NOT_FOUND)return false;
  if(r->termination_status!=GT_OK) {
    if(r->termination.kind!=GT_IO_EXIT_UNKNOWN || r->termination.code || r->has_exit_code || r->exit_code)return false;
  } else if(r->termination.kind==GT_IO_EXIT_CODE) {
    if(r->termination.code<0 || !r->has_exit_code || r->exit_code!=r->termination.code)return false;
  } else if(r->termination.kind==GT_IO_EXIT_SIGNAL) {
    if(r->termination.code<=0 || r->has_exit_code || r->exit_code)return false;
  } else return false;
  return true;
}
bool rsw_encode(uint64_t generation,const gt_worker_result_t *r,void *buffer,size_t capacity,size_t *length) {
  if(length)*length=0;
  if(!generation || !valid(r) || !buffer || !length || capacity<RSW_BYTES)return false;
  uint8_t *p=buffer;memcpy(p,"GWR1",4);put32(p+4,(r->has_sequence?1u:0u)|(r->has_exit_code?2u:0u));
  put64(p+8,generation);put64(p+16,r->sequence);put32(p+24,r->kind);put32(p+28,r->stage);
  put32(p+32,r->status);put32(p+36,(uint32_t)r->exit_code);put32(p+40,r->termination_status);
  put32(p+44,r->termination.kind);put32(p+48,(uint32_t)r->termination.code);put32(p+52,0);
  *length=RSW_BYTES;return true;
}
bool rsw_decode(const void *buffer,size_t n,uint64_t *generation,gt_worker_result_t *out) {
  if(generation)*generation=0;if(out)memset(out,0,sizeof(*out));
  if(!buffer || !generation || !out || n!=RSW_BYTES)return false;
  const uint8_t *p=buffer;uint32_t flags=get32(p+4);uint64_t g=get64(p+8);
  if(memcmp(p,"GWR1",4) || flags&~3u || !g || get32(p+52))return false;
  /* Negative values are invalid exit observations; reject before signed cast. */
  uint32_t code=get32(p+36),term=get32(p+48);if(code>INT32_MAX || term>INT32_MAX)return false;
  gt_worker_result_t r={.kind=(gt_worker_end_t)get32(p+24),.stage=(gt_worker_stage_t)get32(p+28),
    .status=(gt_status_t)get32(p+32),.has_sequence=(flags&1)!=0,.has_exit_code=(flags&2)!=0,
    .sequence=get64(p+16),.exit_code=(int32_t)code,.termination_status=(gt_status_t)get32(p+40),
    .termination={(gt_io_exit_kind_t)get32(p+44),(int32_t)term}};
  if(!valid(&r))return false;
  *generation=g;*out=r;return true;
}
