#include "ipc/wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static const uint8_t golden[24]={
  0x47,0x54,0x53,0x44,0,1,0x12,0x34,0,0,0,0,0,0,0,3,
  1,2,3,4,5,6,7,8};
static gt_wire_header_t sample(void){return (gt_wire_header_t){.type=0x1234,.payload_length=3,.request_id=UINT64_C(0x0102030405060708)};}
static void gold(void) {
  gt_wire_header_t h=sample(),out;uint8_t bytes[24];
  CHECK(gt__wire_header_encode(&h,bytes));CHECK(!memcmp(bytes,golden,24));
  CHECK(gt__wire_header_decode(golden,3,&out));
  CHECK(out.type==0x1234 && !out.flags && out.payload_length==3 && out.request_id==UINT64_C(0x0102030405060708));
}
static void flow(void) {
  uint8_t stream[54],storage[8];memcpy(stream,golden,24);memcpy(stream+24,"abc",3);
  memcpy(stream+27,stream,27);
  for(size_t split=0;split<=27;split++) {
    gt_wire_decoder_t d;gt_wire_frame_t f;size_t used;
    CHECK(gt__wire_decoder_init(&d,storage,sizeof(storage),8));
    CHECK(gt__wire_decoder_feed(&d,stream,split,&used)==(split==27?GT_WIRE_FRAME:GT_WIRE_MORE));CHECK(used==split);
    CHECK(gt__wire_decoder_feed(&d,stream+split,54-split,&used)==GT_WIRE_FRAME);CHECK(used==27-split);
    CHECK(gt__wire_decoder_frame(&d,&f));CHECK(f.header.type==0x1234 && f.header.payload_length==3 && !memcmp(f.payload,"abc",3));
    CHECK(gt__wire_decoder_feed(&d,stream+27,27,&used)==GT_WIRE_FRAME && !used);
    CHECK(gt__wire_decoder_next(&d));CHECK(!gt__wire_decoder_frame(&d,&f));
    CHECK(gt__wire_decoder_feed(&d,stream+27,27,&used)==GT_WIRE_FRAME && used==27);
    CHECK(gt__wire_decoder_finish(&d)==GT_WIRE_FRAME);CHECK(gt__wire_decoder_next(&d));
    CHECK(gt__wire_decoder_feed(&d,NULL,0,&used)==GT_WIRE_END && !used);
    CHECK(gt__wire_decoder_finish(&d)==GT_WIRE_END);
  }
  gt_wire_decoder_t d;size_t used;CHECK(gt__wire_decoder_init(&d,storage,8,8));
  for(size_t i=0;i<27;i++)CHECK(gt__wire_decoder_feed(&d,stream+i,1,&used)==(i==26?GT_WIRE_FRAME:GT_WIRE_MORE) && used==1);
}
static void malformed(void) {
  const unsigned positions[]={0,4,5,6,7,8,11};
  for(size_t i=0;i<sizeof(positions)/sizeof(positions[0]);i++) {
    uint8_t bad[24],storage[8];memcpy(bad,golden,24);
    if(positions[i]==6 || positions[i]==7){bad[6]=0;bad[7]=0;}else bad[positions[i]]^=1;
    gt_wire_header_t h=sample();CHECK(!gt__wire_header_decode(bad,8,&h));CHECK(!h.type && !h.request_id && !h.payload_length);
    gt_wire_decoder_t d;size_t used=99;gt_wire_frame_t f;
    CHECK(gt__wire_decoder_init(&d,storage,8,8));
    CHECK(gt__wire_decoder_feed(&d,bad,24,&used)==GT_WIRE_ERROR && used==24);
    CHECK(gt__wire_decoder_feed(&d,golden,24,&used)==GT_WIRE_ERROR && !used);
    CHECK(!gt__wire_decoder_next(&d));CHECK(!gt__wire_decoder_frame(&d,&f));
    CHECK(gt__wire_decoder_finish(&d)==GT_WIRE_ERROR);
  }
}
static void limits(void) {
  uint8_t storage[8];memset(storage,0x5a,sizeof(storage));
  gt_wire_decoder_t d;size_t used;
  CHECK(gt__wire_decoder_init(&d,storage,8,2));
  uint8_t bytes[27];memcpy(bytes,golden,24);memcpy(bytes+24,"abc",3);
  CHECK(gt__wire_decoder_feed(&d,bytes,27,&used)==GT_WIRE_ERROR && used==24);
  for(size_t i=0;i<8;i++)CHECK(storage[i]==0x5a);
  gt_wire_header_t h=sample();uint8_t header[24];
  h.payload_length=GT_WIRE_MAX_PAYLOAD;CHECK(gt__wire_header_encode(&h,header));
  CHECK(gt__wire_header_decode(header,GT_WIRE_MAX_PAYLOAD,&h));
  h.payload_length=GT_WIRE_MAX_PAYLOAD+1;CHECK(!gt__wire_header_encode(&h,header));
  memcpy(header,golden,24);memset(header+12,0xff,4);
  CHECK(!gt__wire_header_decode(header,GT_WIRE_MAX_PAYLOAD,&h));
  CHECK(!gt__wire_header_decode(golden,GT_WIRE_MAX_PAYLOAD+1,&h));
}
static void eof(void) {
  uint8_t bytes[27],storage[8];memcpy(bytes,golden,24);memcpy(bytes+24,"abc",3);
  for(size_t n=0;n<27;n++) {
    gt_wire_decoder_t d;size_t used;CHECK(gt__wire_decoder_init(&d,storage,8,8));
    CHECK(gt__wire_decoder_feed(&d,bytes,n,&used)==GT_WIRE_MORE && used==n);
    gt_wire_result_t expected=n?GT_WIRE_ERROR:GT_WIRE_END;
    CHECK(gt__wire_decoder_finish(&d)==expected);CHECK(gt__wire_decoder_finish(&d)==expected);
    CHECK(gt__wire_decoder_feed(&d,bytes,27,&used)==expected && !used);
  }
}
static void invalid(void) {
  gt_wire_decoder_t d;gt_wire_frame_t f;uint8_t storage[8],header[24];gt_wire_header_t h=sample();size_t used=99;
  CHECK(!gt__wire_decoder_init(NULL,storage,8,8));
  CHECK(!gt__wire_decoder_init(&d,NULL,8,8));CHECK(d.failed);
  CHECK(!gt__wire_decoder_init(&d,storage,2,3));CHECK(d.failed);
  CHECK(!gt__wire_decoder_init(&d,storage,8,GT_WIRE_MAX_PAYLOAD+1));
  CHECK(!gt__wire_header_encode(&h,NULL));CHECK(!gt__wire_header_encode(NULL,header));
  for(size_t i=0;i<24;i++)CHECK(!header[i]);
  CHECK(!gt__wire_header_decode(NULL,8,&h));CHECK(!gt__wire_header_decode(golden,8,NULL));
  h=sample();h.flags=1;CHECK(!gt__wire_header_encode(&h,header));
  h=sample();h.type=0;CHECK(!gt__wire_header_encode(&h,header));
  CHECK(gt__wire_decoder_feed(NULL,NULL,0,&used)==GT_WIRE_ERROR && !used);
  CHECK(!gt__wire_decoder_frame(NULL,&f));CHECK(!gt__wire_decoder_next(NULL));
  CHECK(gt__wire_decoder_finish(NULL)==GT_WIRE_ERROR);
  CHECK(gt__wire_decoder_init(&d,storage,8,8));CHECK(!gt__wire_decoder_next(&d));
  CHECK(!gt__wire_decoder_frame(&d,&f));CHECK(!f.payload && !f.header.type);
  CHECK(gt__wire_decoder_feed(&d,NULL,1,&used)==GT_WIRE_ERROR && !used);
  CHECK(gt__wire_decoder_init(&d,storage,8,8));CHECK(gt__wire_decoder_feed(&d,NULL,0,NULL)==GT_WIRE_ERROR);
}
static void empty(void) {
  gt_wire_header_t h={.type=1};uint8_t header[24];gt_wire_decoder_t d;gt_wire_frame_t f;size_t used;
  CHECK(gt__wire_header_encode(&h,header));CHECK(gt__wire_decoder_init(&d,NULL,0,0));
  CHECK(gt__wire_decoder_feed(&d,header,24,&used)==GT_WIRE_FRAME && used==24);
  CHECK(gt__wire_decoder_frame(&d,&f));CHECK(!f.header.payload_length && !f.payload);
  CHECK(gt__wire_decoder_next(&d));CHECK(gt__wire_decoder_finish(&d)==GT_WIRE_END);
}
static void binary(void) {
  uint8_t bytes[27],storage[3]={0x55,0x55,0x55};memcpy(bytes,golden,24);bytes[24]=0;bytes[25]=0xff;bytes[26]=0xc0;
  gt_wire_decoder_t d;gt_wire_frame_t f;size_t used;CHECK(gt__wire_decoder_init(&d,storage,3,3));
  CHECK(gt__wire_decoder_feed(&d,bytes,27,&used)==GT_WIRE_FRAME && used==27);
  memset(bytes,0,sizeof(bytes));CHECK(gt__wire_decoder_frame(&d,&f));
  CHECK(f.payload[0]==0 && f.payload[1]==0xff && f.payload[2]==0xc0);
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"golden"))gold();else if(!strcmp(argv[1],"flow"))flow();
  else if(!strcmp(argv[1],"malformed"))malformed();else if(!strcmp(argv[1],"limits"))limits();
  else if(!strcmp(argv[1],"eof"))eof();else if(!strcmp(argv[1],"invalid"))invalid();
  else if(!strcmp(argv[1],"empty"))empty();else if(!strcmp(argv[1],"binary"))binary();else CHECK(false);
  printf("PASS shared local/SSH envelope %s\n",argv[1]);return 0;
}
