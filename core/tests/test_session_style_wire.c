#include "ipc/user-wire.h"
#include "ipc/data-wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static gt_session_style_t style(void){gt_session_style_t s={.dark=1,.background={1,2,3},.foreground={4,5,6},.cursor={7,8,9}};for(size_t i=0;i<256;i++)for(size_t j=0;j<3;j++)s.palette[i][j]=(uint8_t)(i*3+j);return s;}
static void user(void){
  uint8_t bytes[806];gt_user_request_t r={.kind=GT_WORKER_STYLE,.stamp={1,2,0},.style=style()},out;
  gt_wire_frame_t f={.payload=bytes};CHECK(gt__user_request_encode(&r,bytes,sizeof(bytes),&f.header));
  CHECK(f.header.type==69 && f.header.payload_length==805 && !f.header.flags && !f.header.request_id);
  CHECK(bytes[27]==1 && !memcmp(bytes+28,"\1\2\3\4\5\6\7\10\11",9));
  for(size_t i=0;i<768;i++)CHECK(bytes[37+i]==(uint8_t)i);
  CHECK(gt__user_request_decode(&f,&out) && out.kind==GT_WORKER_STYLE && out.style.dark==r.style.dark && !memcmp(out.style.background,r.style.background,3) && !memcmp(out.style.foreground,r.style.foreground,3) && !memcmp(out.style.cursor,r.style.cursor,3) && !memcmp(out.style.palette,r.style.palette,768));
  for(unsigned n=0;n<805;n++){f.header.payload_length=n;CHECK(!gt__user_request_decode(&f,&out) && !out.stamp.generation);}f.header.payload_length=806;CHECK(!gt__user_request_decode(&f,&out));f.header.payload_length=805;
  bytes[27]=2;CHECK(!gt__user_request_decode(&f,&out));bytes[27]=1;f.header.request_id=1;CHECK(!gt__user_request_decode(&f,&out));
  CHECK(!gt__user_request_encode(&r,bytes,804,&f.header));r.style.dark=2;CHECK(!gt__user_request_encode(&r,bytes,sizeof(bytes),&f.header));
  gt_user_reply_t receipt={.kind=GT_WORKER_STYLE,.stamp={1,2,0},.delivery=GT_USER_OPERATION_OK};gt_user_reply_t result;
  CHECK(gt__user_reply_encode(&receipt,bytes,sizeof(bytes),&f.header) && gt__user_reply_decode(&f,&result) && result.kind==GT_WORKER_STYLE);
}
static void data(void){
  uint8_t bytes[806];gt_data_message_t r={.type=GT_MSG_STATE_EVENT,.kind=GT_STATE_STYLE,.generation=1,.sequence=2,.style=style()},out;
  gt_wire_frame_t f={.payload=bytes};CHECK(gt__data_encode(&r,bytes,sizeof(bytes),&f.header));
  CHECK(f.header.payload_length==805 && bytes[19]==8 && bytes[22]==3 && bytes[23]==13 && bytes[27]==1);
  CHECK(!memcmp(bytes+28,"\1\2\3\4\5\6\7\10\11",9));for(size_t i=0;i<768;i++)CHECK(bytes[37+i]==(uint8_t)i);
  CHECK(gt__data_decode(&f,&out) && out.kind==GT_STATE_STYLE && out.style.dark==r.style.dark && !memcmp(out.style.background,r.style.background,3) && !memcmp(out.style.foreground,r.style.foreground,3) && !memcmp(out.style.cursor,r.style.cursor,3) && !memcmp(out.style.palette,r.style.palette,768));
  for(unsigned n=0;n<805;n++){f.header.payload_length=n;CHECK(!gt__data_decode(&f,&out) && !out.generation);}f.header.payload_length=806;CHECK(!gt__data_decode(&f,&out));f.header.payload_length=805;
  bytes[27]=2;CHECK(!gt__data_decode(&f,&out));bytes[27]=1;bytes[23]=12;CHECK(!gt__data_decode(&f,&out));
  CHECK(!gt__data_encode(&r,bytes,804,&f.header));r.style.dark=2;CHECK(!gt__data_encode(&r,bytes,sizeof(bytes),&f.header));
}
int main(int argc,char **argv){CHECK(argc==2);if(!strcmp(argv[1],"user"))user();else if(!strcmp(argv[1],"data"))data();else CHECK(false);printf("PASS ordered style wire %s\n",argv[1]);return 0;}
