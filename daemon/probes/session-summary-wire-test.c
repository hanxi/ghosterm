#include "session-summary-wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
  assert(argc==2);
  static const uint8_t golden[]={ 'G','P','S','1',0,0,0,1,3,1,0,0,
    0,0,0,1,'a',0,0,0,1,'b',0,0,0,0,0,0,0,1,'c'};
  uint8_t bytes[128];memcpy(bytes,golden,sizeof(golden));size_t n=sizeof(golden);
  gt_program_status_store_t *store=NULL;
  if(!strcmp(argv[1],"valid")) {
    assert(psw_decode(golden,sizeof(golden),&store));gt_program_status_record_t r;
    assert(gt__program_status_count(store)==1 && gt__program_status_at(store,0,&r));
    assert(r.state==3 && r.kind==1 && r.progress==-1 && !strcmp(r.id,"a") && !strcmp(r.app,"b") && !strcmp(r.title,"") && !strcmp(r.message,"c"));
    size_t written=0;assert(psw_encode(store,bytes,sizeof(bytes),&written));
    assert(written==sizeof(golden) && !memcmp(bytes,golden,written));
    memset(bytes,0,sizeof(bytes));assert(!strcmp(r.message,"c"));
    gt__program_status_close(store);return 0;
  }
  if(!strcmp(argv[1],"capacity")) {
    assert(psw_decode(golden,sizeof(golden),&store));size_t written=99;
    memset(bytes,0xa5,sizeof(bytes));assert(!psw_encode(store,bytes,sizeof(golden)-1,&written) && !written);
    for(size_t i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    gt__program_status_close(store);return 0;
  }
  if(!strcmp(argv[1],"bounds")) {
    store=gt__program_status_open(33,PSW_STRINGS);assert(store);
    size_t written=99;assert(!psw_encode(store,bytes,7,&written) && !written);
    for(unsigned i=0;i<33;i++) {
      char id[16];snprintf(id,sizeof(id),"%u",i);
      gt_program_status_record_t r={id,"","","",0,0,-1};assert(gt__program_status_apply(store,&r)==GT_STATUS_OK);
    }
    uint8_t large[PSW_MAX];assert(!psw_encode(store,large,sizeof(large),&written) && !written);
    gt__program_status_close(store);return 0;
  }
  if(!strcmp(argv[1],"empty")) {
    memset(bytes+4,0,4);assert(psw_decode(bytes,8,&store) && !gt__program_status_count(store));
    gt__program_status_close(store);return 0;
  }
  if(!strcmp(argv[1],"magic"))bytes[0]^=1;
  else if(!strcmp(argv[1],"count"))bytes[7]=33;
  else if(!strcmp(argv[1],"state"))bytes[8]=5;
  else if(!strcmp(argv[1],"kind"))bytes[9]=4;
  else if(!strcmp(argv[1],"progress"))bytes[10]=102;
  else if(!strcmp(argv[1],"flags"))bytes[11]=1;
  else if(!strcmp(argv[1],"field-length"))bytes[12]=1;
  else if(!strcmp(argv[1],"nul"))bytes[16]=0;
  else if(!strcmp(argv[1],"truncated"))n--;
  else if(!strcmp(argv[1],"trailing"))bytes[n++]=0;
  else if(!strcmp(argv[1],"duplicate")) {
    bytes[7]=2;memcpy(bytes+n,bytes+8,sizeof(golden)-8);n+=sizeof(golden)-8;
  } else assert(false);
  store=(void *)1;assert(!psw_decode(bytes,n,&store) && !store);return 0;
}
