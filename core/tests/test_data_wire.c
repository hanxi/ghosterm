#define _POSIX_C_SOURCE 200809L
#include "ipc/data-wire.h"
#include "session/attachment.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static uint64_t gen(void){return 0x0102030405060708ULL;}
static void vectors(void) {
  uint8_t b[128];gt_wire_frame_t f={.payload=b};gt_data_message_t m={.type=40,.generation=gen(),.sequence=0,.total=0x010203,.format=0xa1a2a3a4,.flags=1},out;
  const uint8_t expected[]={1,2,3,4,5,6,7,8,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,0xa1,0xa2,0xa3,0xa4,0,0,0,1,0,0,0,0,0,0,0,0};
  CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.type==40 && f.header.payload_length==40 && !f.header.flags && !f.header.request_id && !memcmp(b,expected,40));CHECK(gt__data_decode(&f,&out) && out.generation==gen() && !out.sequence && out.total==0x010203 && out.format==0xa1a2a3a4 && out.flags==1);
  const uint8_t input[]={0,0xff,'x'};m=(gt_data_message_t){.type=41,.generation=gen(),.offset=65536,.bytes=input,.length=3};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.payload_length==19 && b[13]==1 && !b[14] && !b[15]);CHECK(gt__data_decode(&f,&out) && out.offset==65536 && out.bytes==b+16 && out.length==3 && !memcmp(out.bytes,input,3));
  for(uint16_t type=43;type<=46;type++){if(type==44)continue;m=(gt_data_message_t){.type=type,.generation=gen(),.sequence=UINT64_MAX};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.payload_length==16 && gt__data_decode(&f,&out) && out.sequence==UINT64_MAX);}
  for(uint16_t type=49;type<=50;type++){m=(gt_data_message_t){.type=type,.generation=gen(),.nonce=0x1112131415161718ULL};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && b[8]==0x11 && b[15]==0x18 && gt__data_decode(&f,&out) && out.nonce==m.nonce);}
  for(uint16_t type=51;type<=52;type++){m=(gt_data_message_t){.type=type,.generation=gen(),.error=GT_ERR_UNSUPPORTED,.stage=GT_DATA_ERROR_ENCODE};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.payload_length==16 && b[11]==7 && b[15]==2 && gt__data_decode(&f,&out) && out.error==GT_ERR_UNSUPPORTED && out.stage==GT_DATA_ERROR_ENCODE);}
}
static void events(void) {
  uint8_t b[64];gt_wire_frame_t f={.payload=b};gt_data_message_t m={.type=44,.generation=gen(),.sequence=0x1112131415161718ULL,.kind=GT_STATE_RESIZE,.size={0x3132,0x3334,0x41424344,0x51525354}},out;
  const uint8_t expected[]={1,2,3,4,5,6,7,8,17,18,19,20,21,22,23,24,0,0,0,2,0,0,0,12,49,50,51,52,65,66,67,68,81,82,83,84};
  CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.type==44 && f.header.payload_length==36 && !memcmp(b,expected,36));CHECK(gt__data_decode(&f,&out) && out.sequence==m.sequence && out.kind==GT_STATE_RESIZE && out.size.cols==0x3132 && out.size.rows==0x3334 && out.size.px_width==0x41424344 && out.size.px_height==0x51525354);
  const uint8_t input[]={0,0xff,'x'};m.kind=GT_STATE_OUTPUT;m.bytes=input;m.length=3;CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && b[19]==1 && b[23]==3 && gt__data_decode(&f,&out) && out.kind==GT_STATE_OUTPUT && out.length==3 && !memcmp(out.bytes,input,3));
  m.kind=GT_STATE_COLOR_SCHEME;m.color.dark=1;CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.payload_length==28 && b[19]==4 && b[23]==4 && b[27]==1 && gt__data_decode(&f,&out) && out.color.dark==1);
}
static void bad(gt_wire_frame_t *f){gt_data_message_t out;memset(&out,0xab,sizeof(out));CHECK(!gt__data_decode(f,&out));const uint8_t *p=(const uint8_t *)&out;for(size_t i=0;i<sizeof(out);i++)CHECK(!p[i]);}
static void malformed(void) {
  uint8_t b[128];gt_wire_frame_t f={.payload=b};const uint16_t types[]={40,42,43,45,46,49,50,51,52};
  for(size_t i=0;i<sizeof(types)/sizeof(types[0]);i++){gt_data_message_t m={.type=types[i],.generation=gen(),.total=10,.format=1,.nonce=1,.error=GT_ERR_IO,.stage=GT_DATA_ERROR_CAPTURE};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header));uint32_t n=f.header.payload_length;for(uint32_t j=0;j<n;j++){f.header.payload_length=j;bad(&f);}f.header.payload_length=n+1;bad(&f);f.header.payload_length=n;f.header.flags=1;bad(&f);f.header.flags=0;f.header.request_id=1;bad(&f);f.header.request_id=0;memset(b,0,8);bad(&f);}
  gt_data_message_t m={.type=40,.generation=gen(),.total=1,.format=1};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header));b[32]=1;bad(&f);b[32]=0;b[31]=2;bad(&f);b[31]=0;memset(b+24,0,4);bad(&f);
  m=(gt_data_message_t){.type=44,.generation=gen(),.sequence=1,.kind=GT_STATE_RESIZE,.size={80,24,0,0}};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header));b[23]=11;bad(&f);b[23]=12;b[19]=99;bad(&f);b[19]=2;b[24]=b[25]=0;bad(&f);CHECK(gt__data_encode(&m,b,sizeof(b),&f.header));memset(b+8,0,8);bad(&f);m.kind=GT_STATE_COLOR_SCHEME;CHECK(gt__data_encode(&m,b,sizeof(b),&f.header));b[27]=2;bad(&f);
  m=(gt_data_message_t){.type=51,.generation=gen(),.error=GT_ERR_IO,.stage=GT_DATA_ERROR_CAPTURE};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header));b[11]=0;bad(&f);b[11]=99;bad(&f);b[11]=3;b[15]=0;bad(&f);b[15]=7;bad(&f);f.header.type=48;bad(&f);f.header.type=65535;bad(&f);
}
static gt_managed_info_t info(void){return (gt_managed_info_t){.id=9,.phase=GT_MANAGED_RETAINED,.has_result=true,.result={.kind=GT_WORKER_END_EOF,.has_sequence=true,.sequence=0,.termination_status=GT_ERR_NOT_FOUND}};}
static void terminal(void) {
  uint8_t b[81];gt_wire_frame_t f={.payload=b};gt_data_message_t m={.type=47,.generation=gen(),.terminal=info()},out;CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && f.header.payload_length==80 && gt__data_decode(&f,&out) && !out.flags && !out.error && out.terminal.result.has_sequence && !out.terminal.result.sequence && out.terminal.id==9);
  for(unsigned n=0;n<80;n++){f.header.payload_length=n;bad(&f);}f.header.payload_length=81;bad(&f);f.header.payload_length=80;b[11]=1;bad(&f);b[15]=7;CHECK(gt__data_decode(&f,&out) && out.flags==1 && out.error==GT_ERR_UNSUPPORTED);b[11]=0;bad(&f);b[11]=1;b[15]=99;bad(&f);
  m.flags=1;m.error=GT_ERR_UNSUPPORTED;m.terminal.result=(gt_worker_result_t){.kind=GT_WORKER_END_ERROR,.stage=GT_WORKER_STAGE_STATE_OPEN,.status=GT_ERR_UNSUPPORTED,.termination_status=GT_ERR_NOT_FOUND};CHECK(gt__data_encode(&m,b,sizeof(b),&f.header) && gt__data_decode(&f,&out) && !out.terminal.result.has_sequence && !out.terminal.result.sequence && out.error==GT_ERR_UNSUPPORTED);b[11]=0;b[15]=0;bad(&f);
}
static void limits(void) {
  uint8_t *b=malloc(65553),*input=malloc(65537);CHECK(b && input);memset(input,0xad,65537);gt_wire_frame_t f={.payload=b};gt_data_message_t out,m={.type=41,.generation=gen(),.offset=(64u*1024u*1024u)-65536,.bytes=input,.length=65536};CHECK(gt__data_encode(&m,b,65552,&f.header) && f.header.payload_length==65552 && gt__data_decode(&f,&out) && out.length==65536 && out.bytes[65535]==0xad);CHECK(!gt__data_encode(&m,b,65551,&f.header) && !f.header.type);m.offset=0;m.length=65537;CHECK(!gt__data_encode(&m,b,65553,&f.header));m.length=1;m.offset=UINT64_MAX;CHECK(!gt__data_encode(&m,b,65553,&f.header));m.offset=(64u*1024u*1024u);CHECK(!gt__data_encode(&m,b,65553,&f.header));
  m=(gt_data_message_t){.type=40,.generation=gen(),.format=1,.total=(64u*1024u*1024u)};CHECK(gt__data_encode(&m,b,40,&f.header) && gt__data_decode(&f,&out));m.total++;CHECK(!gt__data_encode(&m,b,40,&f.header));m.total=0;CHECK(!gt__data_encode(&m,b,40,&f.header));m.type=49;m.nonce=0;CHECK(!gt__data_encode(&m,b,40,&f.header));m.type=44;m.sequence=1;m.kind=GT_STATE_OUTPUT;m.bytes=input;m.length=16384;CHECK(gt__data_encode(&m,b,65553,&f.header) && gt__data_decode(&f,&out));m.length++;CHECK(!gt__data_encode(&m,b,65553,&f.header));free(input);free(b);
}
static gt_status_t task_encode(void *ctx,void **out){(void)ctx;*out=malloc(1);CHECK(*out);return GT_OK;}
static void task_input(void *ctx){(void)ctx;}
static void task_result(void *ctx,void *p){(void)ctx;free(p);}
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static gt_attachment_checkpoint_t take(gt_session_attachment_t *a,uint64_t g){gt_attachment_checkpoint_t c;for(unsigned i=0;;i++){CHECK(i<5000);gt_task_rc_t rc=gt__attachment_take(a,g,&c);if(rc==GT_TASK_OK)return c;CHECK(rc==GT_TASK_BUSY);pause_one();}}
static void exact(void) {
  gt_checkpoint_tasks_t *pool=gt__tasks_open(2);CHECK(pool);gt_session_attachment_t *a=gt__attachment_open(pool,64,4);CHECK(a);uint64_t old,next,id1,id2;gt_task_spec_t s={.encode=task_encode,.release_input=task_input,.release_result=task_result};CHECK(gt__attachment_attach(a,&old)==GT_TASK_OK && gt__attachment_begin(a,old,0)==GT_RESTORE_OK && gt__attachment_submit(a,old,&s,&id1)==GT_TASK_OK);gt_attachment_checkpoint_t first=take(a,old);CHECK(gt__attachment_detach(a,old)==GT_TASK_OK && gt__attachment_attach(a,&next)==GT_TASK_OK && gt__attachment_begin(a,next,0)==GT_RESTORE_OK && gt__attachment_submit(a,next,&s,&id2)==GT_TASK_OK);gt_attachment_checkpoint_t second=take(a,next);CHECK(gt__attachment_release(a,&second)==GT_TASK_OK);gt_task_info_t info;for(unsigned i=0;;i++){CHECK(i<5000 && gt__tasks_info(pool,id2,&info)==GT_TASK_OK);if(info.state==GT_TASK_FINISHED)break;pause_one();}
  gt_attachment_retired_t retired;CHECK(gt__attachment_reap_task(a,id1,&retired)==GT_TASK_BUSY && !retired.task);CHECK(gt__attachment_reap_task(a,0,&retired)==GT_TASK_INVALID);CHECK(gt__attachment_reap_task(a,id2,&retired)==GT_TASK_OK && retired.task==id2 && retired.generation==next);CHECK(gt__attachment_release(a,&first)==GT_TASK_OK);for(unsigned i=0;;i++){CHECK(i<5000);gt_task_rc_t rc=gt__attachment_reap_task(a,id1,&retired);if(rc==GT_TASK_OK)break;CHECK(rc==GT_TASK_BUSY);pause_one();}CHECK(retired.task==id1 && retired.generation==old && gt__attachment_detach(a,next)==GT_TASK_OK && gt__attachment_close(a)==GT_TASK_OK && gt__tasks_close(pool)==GT_TASK_OK);
}
int main(int argc,char **argv){CHECK(argc==2);if(!strcmp(argv[1],"vectors"))vectors();else if(!strcmp(argv[1],"events"))events();else if(!strcmp(argv[1],"malformed"))malformed();else if(!strcmp(argv[1],"terminal"))terminal();else if(!strcmp(argv[1],"limits"))limits();else if(!strcmp(argv[1],"exact"))exact();else CHECK(false);printf("PASS DATA schema/lifecycle %s\n",argv[1]);return 0;}
