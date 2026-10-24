#define _POSIX_C_SOURCE 200809L
#include "startup-wire.h"
#include "ipc/socket-stream.h"
#include "base/secure-random.h"
#include <poll.h>
#include <string.h>
#include <unistd.h>
static void put(uint8_t *p,uint64_t v,size_t n){for(size_t i=0;i<n;i++)p[n-1-i]=(uint8_t)(v>>(8*i));}
static uint64_t get(const uint8_t *p,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v=(v<<8)|p[i];return v;}
static bool nonzero(const void *p,size_t n){const uint8_t *b=p;uint8_t v=0;for(size_t i=0;i<n;i++)v|=b[i];return v!=0;}
bool gt__startup_ready_encode(gt_status_t status,const gt_protocol_identity_t *id,uint8_t out[72]) {
  if(!out)return false;
  memset(out,0,72);if((unsigned)status>GT_ERR_INTERNAL || (status==GT_OK && (!id || !nonzero(id->instance,16) || !nonzero(id->compatibility,32))))return false;
  memcpy(out,"GTSR",4);put(out+4,1,2);put(out+6,72,2);put(out+8,status,4);
  if(status==GT_OK){memcpy(out+16,id->compatibility,32);memcpy(out+48,id->instance,16);put(out+64,id->features,8);}return true;
}
bool gt__startup_ready_decode(const uint8_t in[72],gt_status_t *status,gt_protocol_identity_t *id) {
  if(status)*status=GT_ERR_PROTOCOL;
  if(id)memset(id,0,sizeof(*id));
  if(!in || !status || !id || memcmp(in,"GTSR",4) || get(in+4,2)!=1 || get(in+6,2)!=72 || get(in+8,4)>GT_ERR_INTERNAL || get(in+12,4))return false;
  gt_status_t s=(gt_status_t)get(in+8,4);
  if(s==GT_OK){if(!nonzero(in+16,32) || !nonzero(in+48,16))return false;memcpy(id->compatibility,in+16,32);memcpy(id->instance,in+48,16);id->features=get(in+64,8);}
  else if(nonzero(in+16,56))return false;
  *status=s;return true;
}
void gt__startup_ack_encode(const uint8_t instance[16],uint8_t out[24]) {
  memset(out,0,24);memcpy(out,"GTSA",4);put(out+4,1,2);put(out+6,24,2);memcpy(out+8,instance,16);
}
bool gt__startup_ack_valid(const uint8_t in[24],const uint8_t instance[16]) {
  return in && instance && !memcmp(in,"GTSA",4) && get(in+4,2)==1 && get(in+6,2)==24 && !memcmp(in+8,instance,16);
}
void gt__startup_commit_encode(const uint8_t instance[16],uint8_t out[24]){gt__startup_ack_encode(instance,out);memcpy(out,"GTSC",4);}
bool gt__startup_commit_valid(const uint8_t in[24],const uint8_t instance[16]) {
  return in && instance && !memcmp(in,"GTSC",4) && get(in+4,2)==1 && get(in+6,2)==24 && !memcmp(in+8,instance,16);
}
bool gt__startup_serve(int fd,gt_status_t status,const gt_protocol_identity_t *identity,uint32_t timeout,
  void *context,bool (*cancelled)(void *)) {
  gt_socket_stream_t stream;gt_stream_ops_t ops;
  if(!timeout || timeout>60000 || gt__socket_stream_adopt(fd,&stream,&ops)!=GT_OK){if(fd>=0)close(fd);return false;}
  uint64_t start=0,previous=0;uint8_t ready[72],ack[24]={0};size_t sent=0,received=0,confirmed=0;bool accepted=false,success=false;
  if(!gt__startup_ready_encode(status,identity,ready) || !ops.clock(ops.context,&start))goto done;
  previous=start;
  while(true) {
    uint64_t now;if((cancelled && cancelled(context)) || !ops.clock(ops.context,&now) || now<previous || now-start>=timeout)break;previous=now;
    if(sent<72) {
      gt_stream_result_t r=ops.write(ops.context,ready+sent,72-sent);
      if(r.kind==GT_STREAM_DATA)sent+=r.length;else if(r.kind!=GT_STREAM_WAIT)break;
    } else if(status!=GT_OK)break;
    else if(!accepted) {
      gt_stream_result_t r=ops.read(ops.context,ack+received,24-received);
      if(r.kind==GT_STREAM_DATA){received+=r.length;if(received==24){if(!gt__startup_ack_valid(ack,identity->instance))break;accepted=true;gt__startup_commit_encode(identity->instance,ack);}}
      else if(r.kind!=GT_STREAM_WAIT)break;
    } else {
      gt_stream_result_t r=ops.write(ops.context,ack+confirmed,24-confirmed);
      if(r.kind==GT_STREAM_DATA){confirmed+=r.length;if(confirmed==24){success=true;break;}}
      else if(r.kind!=GT_STREAM_WAIT)break;
    }
    struct pollfd p={.fd=fd,.events=(short)(sent<72 || accepted?POLLOUT:POLLIN)};(void)poll(&p,1,10);
  }
done:
  gt__secure_erase(ready,sizeof(ready));gt__secure_erase(ack,sizeof(ack));ops.close(ops.context);return success;
}
