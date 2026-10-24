#include "session-command-wire.h"
#include <assert.h>
#include <string.h>
int main(int argc,char **argv) {
  assert(argc==2);
  const uint8_t golden[]={'G','C','S','1',6,0,0,0,255,255,255,254,
    1,2,3,4,5,6,7,8,0,0,0,1,0,0,0,1,'a','b'};
  uint8_t bytes[CSW_MAX+1];memcpy(bytes,golden,sizeof(golden));size_t n=sizeof(golden);
  gt_command_status_t out;memset(&out,0xa5,sizeof(out));gt_command_status_t before=out;
  if(!strcmp(argv[1],"valid")) {
    assert(csw_decode(bytes,n,&out));assert(!out.running && out.has_exit_code && out.exit_code==-2);
    assert(out.has_duration && out.duration_ns==UINT64_C(0x0102030405060708) && !out.has_start_time && !out.start_ns);
    assert(!strcmp(out.command,"a") && !strcmp(out.error,"b"));
    size_t written=0;assert(csw_encode(&out,bytes,sizeof(bytes),&written) && written==sizeof(golden) && !memcmp(bytes,golden,written));return 0;
  }
  if(!strcmp(argv[1],"unknown")) {
    gt_command_status_t s={0};size_t written=0;assert(csw_encode(&s,bytes,sizeof(bytes),&written));
    assert(csw_decode(bytes,written,&out) && !out.running && !out.has_exit_code && !out.has_duration);
    s.running=true;s.has_start_time=true;s.start_ns=123;
    assert(csw_encode(&s,bytes,sizeof(bytes),&written) && csw_decode(bytes,written,&out));
    assert(out.running && !out.has_start_time && !out.has_exit_code && !out.has_duration);
    s.running=false;s.has_duration=true;s.duration_ns=0;s.has_exit_code=true;s.exit_code=0;
    assert(csw_encode(&s,bytes,sizeof(bytes),&written) && csw_decode(bytes,written,&out));
    assert(out.has_duration && !out.duration_ns && out.has_exit_code && !out.exit_code);return 0;
  }
  if(!strcmp(argv[1],"capacity")) {
    gt_command_status_t s={0};memset(s.command,'x',4096);memset(s.error,'y',4096);
    size_t written=0;assert(csw_encode(&s,bytes,sizeof(bytes),&written) && written==CSW_MAX && csw_decode(bytes,written,&out));
    assert(strlen(out.command)==4096 && strlen(out.error)==4096);
    memset(bytes,0xa5,sizeof(bytes));assert(!csw_encode(&s,bytes,CSW_MAX-1,&written) && !written);
    for(size_t i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    s.command[4096]='x';assert(!csw_encode(&s,bytes,sizeof(bytes),&written) && !written);return 0;
  }
  if(!strcmp(argv[1],"magic"))bytes[0]^=1;
  else if(!strcmp(argv[1],"flags"))bytes[4]|=8;
  else if(!strcmp(argv[1],"reserved"))bytes[7]=1;
  else if(!strcmp(argv[1],"running-result"))bytes[4]|=1;
  else if(!strcmp(argv[1],"hidden-exit"))bytes[4]&=(uint8_t)~2u;
  else if(!strcmp(argv[1],"hidden-time"))bytes[4]&=(uint8_t)~4u;
  else if(!strcmp(argv[1],"length"))bytes[20]=1;
  else if(!strcmp(argv[1],"nul"))bytes[28]=0;
  else if(!strcmp(argv[1],"truncated"))n--;
  else if(!strcmp(argv[1],"trailing"))bytes[n++]=0;
  else assert(false);
  assert(!csw_decode(bytes,n,&out) && !memcmp(&out,&before,sizeof(out)));return 0;
}
