#include "session-summary-wire.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(24-i*8));}
static uint32_t get32(const uint8_t *p){uint32_t n=0;for(unsigned i=0;i<4;i++)n=(n<<8)|p[i];return n;}
bool psw_encode(const gt_program_status_store_t *s,void *buffer,size_t capacity,size_t *length) {
  if(length)*length=0;
  if(!s || !buffer || !length || capacity<8)return false;
  size_t count=gt__program_status_count(s),need=8;
  if(count>PSW_RECORDS || gt__program_status_bytes(s)>PSW_STRINGS)return false;
  for(size_t i=0;i<count;i++) {
    gt_program_status_record_t r;if(!gt__program_status_at(s,i,&r))return false;
    need+=20+strlen(r.id)+strlen(r.app)+strlen(r.title)+strlen(r.message);
  }
  if(need>capacity || need>PSW_MAX)return false;
  uint8_t *p=buffer;memcpy(p,"GPS1",4);put32(p+4,(uint32_t)count);size_t offset=8;
  for(size_t i=0;i<count;i++) {
    gt_program_status_record_t r;if(!gt__program_status_at(s,i,&r))return false;
    p[offset++]=(uint8_t)r.state;p[offset++]=(uint8_t)r.kind;p[offset++]=(uint8_t)(r.progress+1);p[offset++]=0;
    const char *fields[]={r.id,r.app,r.title,r.message};
    for(unsigned j=0;j<4;j++) {size_t n=strlen(fields[j]);put32(p+offset,(uint32_t)n);offset+=4;memcpy(p+offset,fields[j],n);offset+=n;}
  }
  *length=offset;return true;
}
bool psw_decode(const void *buffer,size_t n,gt_program_status_store_t **out) {
  return psw_decode_with_allocator(buffer,n,NULL,out);
}
bool psw_decode_with_allocator(const void *buffer,size_t n,const gt_status_allocator_t *allocator,gt_program_status_store_t **out) {
  if(out)*out=NULL;
  if(!out || !buffer || n<8 || n>PSW_MAX)return false;
  const uint8_t *p=buffer;uint32_t count=get32(p+4);
  if(memcmp(p,"GPS1",4) || count>PSW_RECORDS)return false;
  gt_program_status_store_t *s=gt__program_status_open_with_allocator(PSW_RECORDS,PSW_STRINGS,allocator);if(!s)return false;
  size_t offset=8;
  for(uint32_t i=0;i<count;i++) {
    if(n-offset<4)goto fail;
    gt_program_status_record_t r={.state=p[offset],.kind=p[offset+1],.progress=(int)p[offset+2]-1};
    if(r.state>4 || p[offset+3])goto fail;
    offset+=4;char fields[4][4097];
    for(unsigned j=0;j<4;j++) {
      if(n-offset<4)goto fail;
      size_t length=get32(p+offset);offset+=4;
      if(length>4096 || length>n-offset || memchr(p+offset,0,length))goto fail;
      memcpy(fields[j],p+offset,length);fields[j][length]=0;offset+=length;
    }
    r.id=fields[0];r.app=fields[1];r.title=fields[2];r.message=fields[3];
    if(gt__program_status_apply(s,&r)!=GT_STATUS_OK || gt__program_status_count(s)!=(size_t)i+1)goto fail;
  }
  if(offset!=n)goto fail;
  *out=s;return true;
fail:
  gt__program_status_close(s);return false;
}
