#include "ipc/writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static const uint8_t golden[27]={0x47,0x54,0x53,0x44,0,1,0x12,0x34,0,0,0,0,0,0,0,3,1,2,3,4,5,6,7,8,0,0xff,0xc0};
static gt_wire_header_t header(void){return (gt_wire_header_t){.type=0x1234,.payload_length=3,.request_id=UINT64_C(0x0102030405060708)};}
static void short_writes(void) {
  for(size_t chunk=1;chunk<=27;chunk++) {
    gt_wire_writer_t w;gt_wire_header_t h=header();gt__wire_writer_init(&w);
    uint8_t output[27];size_t offset=0;CHECK(gt__wire_writer_begin(&w,&h,golden+24));
    while(w.state==GT_WRITER_PENDING) {
      const uint8_t *bytes,*again;size_t length,available;CHECK(gt__wire_writer_view(&w,&bytes,&length));
      CHECK(length && gt__wire_writer_advance(&w,0));
      CHECK(gt__wire_writer_view(&w,&again,&available) && bytes==again && available==length);
      size_t n=length<chunk?length:chunk;CHECK(offset+n<=sizeof(output));
      memcpy(output+offset,bytes,n);offset+=n;CHECK(gt__wire_writer_advance(&w,n));
    }
    CHECK(w.state==GT_WRITER_DONE && !w.payload && offset==27 && !memcmp(output,golden,27));
  }
}
static void replacement(void) {
  gt_wire_writer_t w;gt_wire_header_t h=header();gt__wire_writer_init(&w);
  CHECK(gt__wire_writer_begin(&w,&h,golden+24));CHECK(gt__wire_writer_advance(&w,5));
  gt_wire_header_t other={.type=9};CHECK(!gt__wire_writer_begin(&w,&other,NULL));
  const uint8_t *p;size_t n;CHECK(gt__wire_writer_view(&w,&p,&n) && n==19 && !memcmp(p,golden+5,19));
  CHECK(w.payload==golden+24 && w.payload_length==3);
  CHECK(gt__wire_writer_advance(&w,19));CHECK(gt__wire_writer_view(&w,&p,&n) && n==3 && !memcmp(p,golden+24,3));
  CHECK(gt__wire_writer_advance(&w,3));CHECK(gt__wire_writer_begin(&w,&other,NULL));
  CHECK(gt__wire_writer_view(&w,&p,&n) && n==24 && p[7]==9 && p[15]==0);
}
static void invalid(void) {
  gt_wire_writer_t w;gt_wire_header_t h=header();gt__wire_writer_init(&w);
  const uint8_t *p=(const uint8_t *)1;size_t n=99;
  CHECK(!gt__wire_writer_view(&w,&p,&n) && !p && !n);
  CHECK(!gt__wire_writer_begin(NULL,&h,golden+24));CHECK(!gt__wire_writer_begin(&w,NULL,NULL));
  CHECK(!gt__wire_writer_begin(&w,&h,NULL));h.flags=1;CHECK(!gt__wire_writer_begin(&w,&h,golden+24));
  CHECK(w.state==GT_WRITER_IDLE);h=header();CHECK(gt__wire_writer_begin(&w,&h,golden+24));
  CHECK(!gt__wire_writer_view(&w,NULL,&n) && !n);CHECK(!gt__wire_writer_view(&w,&p,NULL) && !p);
  CHECK(!gt__wire_writer_advance(&w,25));CHECK(w.state==GT_WRITER_FAILED && !w.payload);
  CHECK(!gt__wire_writer_begin(&w,&h,golden+24));CHECK(!gt__wire_writer_view(&w,&p,&n));
  CHECK(!gt__wire_writer_advance(&w,0));CHECK(!gt__wire_writer_advance(NULL,1));
  gt__wire_writer_init(NULL);gt__wire_writer_cancel(NULL);
}
static void cancel(void) {
  for(size_t n=0;n<=26;n++) {
    gt_wire_writer_t w;gt_wire_header_t h=header();gt__wire_writer_init(&w);CHECK(gt__wire_writer_begin(&w,&h,golden+24));
    if(n<=24)CHECK(gt__wire_writer_advance(&w,n));else{CHECK(gt__wire_writer_advance(&w,24));CHECK(gt__wire_writer_advance(&w,n-24));}
    gt__wire_writer_cancel(&w);CHECK(w.state==GT_WRITER_FAILED && !w.payload);
    const uint8_t *bytes;size_t length;CHECK(!gt__wire_writer_view(&w,&bytes,&length));
    CHECK(!gt__wire_writer_begin(&w,&h,golden+24));CHECK(!gt__wire_writer_advance(&w,1));
  }
}
static void empty(void) {
  gt_wire_writer_t w;gt_wire_header_t h={.type=1};gt__wire_writer_init(&w);
  for(unsigned i=0;i<2;i++) {
    CHECK(gt__wire_writer_begin(&w,&h,NULL));CHECK(gt__wire_writer_advance(&w,23));
    CHECK(w.state==GT_WRITER_PENDING);CHECK(gt__wire_writer_advance(&w,1));
    CHECK(w.state==GT_WRITER_DONE && !w.payload);
    const uint8_t *bytes;size_t length;CHECK(!gt__wire_writer_view(&w,&bytes,&length) && !bytes && !length);
  }
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"short"))short_writes();else if(!strcmp(argv[1],"replacement"))replacement();
  else if(!strcmp(argv[1],"invalid"))invalid();else if(!strcmp(argv[1],"cancel"))cancel();
  else if(!strcmp(argv[1],"empty"))empty();else CHECK(false);
  printf("PASS shared local/SSH writer %s\n",argv[1]);return 0;
}
