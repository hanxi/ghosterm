#include "session-result-wire.h"
#include <assert.h>
#include <string.h>
static gt_worker_result_t stopped(void){return (gt_worker_result_t){.kind=GT_WORKER_END_STOPPED,.has_sequence=true,.sequence=40,.termination_status=GT_ERR_NOT_FOUND};}
static void rejected(uint8_t *p,size_t n) {
  uint64_t g=99;gt_worker_result_t r;memset(&r,0xa5,sizeof(r));assert(!rsw_decode(p,n,&g,&r));
  assert(!g && r.kind==GT_WORKER_END_UNKNOWN && !r.has_sequence && !r.has_exit_code && !r.sequence);
}
static void put(uint8_t *p,unsigned n){p[0]=p[1]=p[2]=0;p[3]=(uint8_t)n;}
int main(int argc,char **argv) {
  assert(argc==2);uint8_t p[RSW_BYTES+1];size_t n=99;gt_worker_result_t r=stopped(),copy;uint64_t g=0;
  assert(rsw_encode(7,&r,p,sizeof(p),&n) && n==RSW_BYTES);
  if(!strcmp(argv[1],"valid")) {
    static const uint8_t expected[56]={'G','W','R','1',0,0,0,1,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,40,
      0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0};
    assert(!memcmp(p,expected,sizeof(expected)) && rsw_decode(p,n,&g,&copy));assert(g==7 && copy.sequence==40 && copy.kind==GT_WORKER_END_STOPPED);
    r=(gt_worker_result_t){.kind=GT_WORKER_END_ERROR,.stage=GT_WORKER_STAGE_APPLY,.status=GT_ERR_PROTOCOL,.has_sequence=true,.sequence=UINT64_MAX,.termination_status=GT_ERR_NOT_FOUND};
    assert(rsw_encode(UINT64_MAX,&r,p,sizeof(p),&n) && rsw_decode(p,n,&g,&copy) && g==UINT64_MAX && copy.sequence==UINT64_MAX && copy.stage==GT_WORKER_STAGE_APPLY);
    r=(gt_worker_result_t){.kind=GT_WORKER_END_ERROR,.stage=GT_WORKER_STAGE_STATE_OPEN,.status=GT_ERR_IO,.termination_status=GT_ERR_NOT_FOUND};
    assert(rsw_encode(1,&r,p,sizeof(p),&n) && rsw_decode(p,n,&g,&copy) && !copy.has_sequence);
    r=stopped();r.kind=GT_WORKER_END_EOF;
    const gt_status_t unknown[]={GT_ERR_NOT_FOUND,GT_ERR_UNSUPPORTED,GT_ERR_PROTOCOL};
    for(unsigned i=0;i<3;i++){r.termination_status=unknown[i];assert(rsw_encode(1,&r,p,sizeof(p),&n) && rsw_decode(p,n,&g,&copy) && copy.termination_status==unknown[i] && !copy.has_exit_code);}
    r.termination_status=GT_OK;r.termination=(gt_io_exit_t){GT_IO_EXIT_CODE,0};r.has_exit_code=true;
    assert(rsw_encode(1,&r,p,sizeof(p),&n) && rsw_decode(p,n,&g,&copy) && copy.has_exit_code && !copy.exit_code);
    r.termination=(gt_io_exit_t){GT_IO_EXIT_SIGNAL,15};r.has_exit_code=false;
    assert(rsw_encode(1,&r,p,sizeof(p),&n) && rsw_decode(p,n,&g,&copy) && copy.termination.kind==GT_IO_EXIT_SIGNAL && copy.termination.code==15 && !copy.has_exit_code);return 0;
  }
  if(!strcmp(argv[1],"capacity")) {memset(p,0xa5,sizeof(p));assert(!rsw_encode(7,&r,p,RSW_BYTES-1,&n) && !n && p[0]==0xa5);assert(!rsw_encode(7,&r,p,sizeof(p),NULL));return 0;}
  if(!strcmp(argv[1],"magic"))p[0]^=1;
  else if(!strcmp(argv[1],"generation")){memset(p+8,0,8);assert(!rsw_encode(0,&r,p,sizeof(p),&n) && !n);}
  else if(!strcmp(argv[1],"flags"))p[4]=128;
  else if(!strcmp(argv[1],"kind")){put(p+24,GT_WORKER_END_UNKNOWN);rejected(p,RSW_BYTES);put(p+24,9);}
  else if(!strcmp(argv[1],"status")){put(p+32,GT_ERR_IO);rejected(p,RSW_BYTES);put(p+32,0);put(p+28,GT_WORKER_STAGE_READ);rejected(p,RSW_BYTES);put(p+24,GT_WORKER_END_ERROR);put(p+28,0);put(p+32,GT_ERR_IO);}
  else if(!strcmp(argv[1],"sequence")){p[7]=0;rejected(p,RSW_BYTES);put(p+24,GT_WORKER_END_ERROR);put(p+28,GT_WORKER_STAGE_STATE_OPEN);put(p+32,GT_ERR_IO);rejected(p,RSW_BYTES);memset(p+16,0,8);p[7]=1;}
  else if(!strcmp(argv[1],"exit")){p[7]=3;rejected(p,RSW_BYTES);p[7]=1;put(p+24,GT_WORKER_END_EOF);put(p+40,0);put(p+44,GT_IO_EXIT_SIGNAL);put(p+48,0);rejected(p,RSW_BYTES);put(p+48,15);p[36]=128;}
  else if(!strcmp(argv[1],"reserved"))p[55]=1;
  else {assert(!strcmp(argv[1],"length"));for(size_t i=0;i<RSW_BYTES;i++)rejected(p,i);rejected(p,RSW_BYTES+1);return 0;}
  rejected(p,RSW_BYTES);return 0;
}
