#include "ipc/user-wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static gt_worker_stamp_t stamp(void){return (gt_worker_stamp_t){0x0102030405060708ULL,0x1112131415161718ULL,0x2122232425262728ULL};}
static void requests(void) {
  uint8_t b[64];gt_wire_header_t h;gt_user_request_t r={.stamp=stamp(),.kind=GT_WORKER_RESIZE,.size={0x3132,0x3334,0x41424344,0x51525354}},out;
  const uint8_t expected[]={1,2,3,4,5,6,7,8,17,18,19,20,21,22,23,24,33,34,35,36,37,38,39,40,49,50,51,52,65,66,67,68,81,82,83,84};
  CHECK(gt__user_request_encode(&r,b,sizeof(b),&h) && h.type==65 && h.payload_length==36 && !h.flags && !h.request_id && !memcmp(b,expected,36));
  gt_wire_frame_t f={h,b};CHECK(gt__user_request_decode(&f,&out) && out.size.px_width==0x41424344 && out.size.px_height==0x51525354 && out.size.cols==0x3132 && out.size.rows==0x3334 && out.stamp.command_id==r.stamp.command_id && out.stamp.applied_sequence==r.stamp.applied_sequence && out.stamp.generation==r.stamp.generation);
  const uint8_t input[]={0,0xff,0xc0,'\n'};r.kind=GT_WORKER_INPUT;r.input=input;r.length=sizeof(input);CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header));CHECK(gt__user_request_decode(&f,&out) && out.length==4 && out.input==b+24 && !memcmp(out.input,input,4));
  r.kind=GT_WORKER_COLOR_SCHEME;r.color.dark=1;CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header) && f.header.type==66 && f.header.payload_length==28 && b[24]==0 && b[27]==1);CHECK(gt__user_request_decode(&f,&out) && out.kind==GT_WORKER_COLOR_SCHEME && out.color.dark==1);
}
static void bad(const gt_wire_frame_t *f) {gt_user_request_t out;memset(&out,0xab,sizeof(out));CHECK(!gt__user_request_decode(f,&out));const uint8_t *p=(const uint8_t *)&out;for(size_t i=0;i<sizeof(out);i++)CHECK(!p[i]);}
static void malformed(void) {
  uint8_t b[64]={0};gt_user_request_t r={.stamp=stamp(),.kind=GT_WORKER_RESIZE,.size={80,24,0,UINT32_MAX}};gt_wire_frame_t f={.payload=b};CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header));
  for(unsigned n=0;n<36;n++){f.header.payload_length=n;bad(&f);}f.header.payload_length=37;bad(&f);f.header.payload_length=36;
  f.header.flags=1;bad(&f);f.header.flags=0;f.header.request_id=1;bad(&f);f.header.request_id=0;
  f.header.type=GT_MSG_USER_RESULT;bad(&f);f.header.type=0xffff;bad(&f);f.header.type=GT_MSG_USER_RESIZE;
  memset(b,0,8);bad(&f);CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header));memset(b+8,0,8);bad(&f);
  CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header));b[24]=b[25]=0;bad(&f);CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header));b[26]=b[27]=0;bad(&f);
  r.kind=GT_WORKER_COLOR_SCHEME;CHECK(gt__user_request_encode(&r,b,sizeof(b),&f.header));b[27]=2;bad(&f);b[27]=0;f.header.payload_length=29;bad(&f);f.header.payload_length=27;bad(&f);
  f.header.type=GT_MSG_USER_INPUT;f.header.payload_length=24;bad(&f);f.header.payload_length=24+65537;bad(&f);bad(NULL);
}
static void replies(void) {
  uint8_t b[41];gt_wire_frame_t f={.payload=b};gt_user_reply_t out,r={.kind=GT_WORKER_INPUT,.stamp=stamp(),.status=GT_OK,.delivery=GT_USER_OPERATION_OK};
  CHECK(gt__user_reply_encode(&r,b,sizeof(b),&f.header) && f.header.type==68 && f.header.payload_length==40 && b[24]==0 && b[25]==64 && !b[26] && !b[27] && b[35]==1 && !b[36]);
  CHECK(gt__user_reply_decode(&f,&out) && !out.rejected && out.status==GT_OK && out.delivery==GT_USER_OPERATION_OK && out.stamp.command_id==r.stamp.command_id);
  for(unsigned delivery=0;delivery<=2;delivery+=2){r.status=GT_ERR_AUTH;r.delivery=(gt_user_delivery_t)delivery;CHECK(gt__user_reply_encode(&r,b,sizeof(b),&f.header) && gt__user_reply_decode(&f,&out) && out.status==GT_ERR_AUTH && out.delivery==delivery);}
  for(unsigned n=0;n<40;n++){f.header.payload_length=n;CHECK(!gt__user_reply_decode(&f,&out) && !out.stamp.command_id);}f.header.payload_length=41;CHECK(!gt__user_reply_decode(&f,&out));f.header.payload_length=40;
  b[26]=1;CHECK(!gt__user_reply_decode(&f,&out));b[26]=0;b[36]=1;CHECK(!gt__user_reply_decode(&f,&out));b[36]=0;b[31]=99;CHECK(!gt__user_reply_decode(&f,&out));b[31]=GT_ERR_AUTH;b[35]=99;CHECK(!gt__user_reply_decode(&f,&out));b[35]=1;CHECK(!gt__user_reply_decode(&f,&out));b[35]=0;b[31]=0;CHECK(!gt__user_reply_decode(&f,&out));
  r.rejected=true;r.status=GT_OK;r.delivery=GT_USER_NOT_EXECUTED;
  for(unsigned code=1;code<=6;code++){r.rejection=(gt_user_reject_t)code;CHECK(gt__user_reply_encode(&r,b,sizeof(b),&f.header) && f.header.type==67 && f.header.payload_length==32 && gt__user_reply_decode(&f,&out) && out.rejected && out.rejection==code && !out.status && !out.delivery);}
  b[31]=0;CHECK(!gt__user_reply_decode(&f,&out));b[31]=7;CHECK(!gt__user_reply_decode(&f,&out));b[31]=1;f.header.request_id=1;CHECK(!gt__user_reply_decode(&f,&out));
}
static void limits(void) {
  uint8_t *b=malloc(24+65537),*input=malloc(65537);CHECK(b && input);memset(input,0xad,65537);
  gt_wire_header_t h;gt_user_request_t r={.stamp={UINT64_MAX,UINT64_MAX,0},.kind=GT_WORKER_INPUT,.input=input,.length=65536};
  CHECK(gt__user_request_encode(&r,b,24+65536,&h) && h.payload_length==24+65536);gt_wire_frame_t f={h,b};gt_user_request_t out;CHECK(gt__user_request_decode(&f,&out) && out.length==65536 && out.stamp.command_id==UINT64_MAX && !out.stamp.applied_sequence && out.input[65535]==0xad);
  CHECK(!gt__user_request_encode(&r,b,24+65535,&h) && !h.type);r.length=65537;CHECK(!gt__user_request_encode(&r,b,24+65537,&h));r.length=0;CHECK(!gt__user_request_encode(&r,b,24+65537,&h));
  r.kind=GT_WORKER_CAPTURE;CHECK(!gt__user_request_encode(&r,b,24+65537,&h));r.kind=GT_WORKER_COLOR_SCHEME;r.color.dark=2;CHECK(!gt__user_request_encode(&r,b,24+65537,&h));r.color.dark=0;r.stamp.generation=0;CHECK(!gt__user_request_encode(&r,b,24+65537,&h));
  gt_user_reply_t reply={.kind=GT_WORKER_INPUT,.stamp=stamp(),.status=GT_OK,.delivery=GT_USER_OPERATION_OK};CHECK(!gt__user_reply_encode(&reply,b,39,&h) && !h.type);reply.delivery=GT_USER_NOT_EXECUTED;CHECK(!gt__user_reply_encode(&reply,b,40,&h));reply.rejected=true;reply.rejection=GT_USER_REJECT_UNSUPPORTED;CHECK(!gt__user_reply_encode(&reply,b,31,&h));reply.delivery=GT_USER_OUTCOME_UNKNOWN;CHECK(!gt__user_reply_encode(&reply,b,40,&h));
  free(input);free(b);
}
int main(int argc,char **argv){CHECK(argc==2);if(!strcmp(argv[1],"requests"))requests();else if(!strcmp(argv[1],"malformed"))malformed();else if(!strcmp(argv[1],"replies"))replies();else if(!strcmp(argv[1],"limits"))limits();else CHECK(false);printf("PASS USER wire %s\n",argv[1]);return 0;}
