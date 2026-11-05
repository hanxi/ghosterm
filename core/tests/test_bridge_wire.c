#define _POSIX_C_SOURCE 200809L
#include "extension/wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct {size_t calls,live,size;bool fail;} budget_t;
static void *allocate(void *ctx,size_t n){budget_t *b=ctx;b->calls++;if(b->fail)return NULL;CHECK(!b->live);b->size=n;b->live=n;return malloc(n);}
static void release(void *ctx,void *p,size_t n) {
  budget_t *b=ctx;CHECK(n==b->size && b->live==n);
  const unsigned char *bytes=p;for(size_t i=0;i<n;i++)CHECK(bytes[i]==0);
  b->live=0;free(p);
}
static bool exact(void *p,size_t n) {
  size_t at=0;while(at<n){ssize_t got=read(0,(char *)p+at,n-at);if(got<0 && errno==EINTR)continue;if(got<=0)return false;at+=(size_t)got;}return true;
}
static uint64_t uint64be(void){uint8_t bytes[8];CHECK(exact(bytes,8));uint64_t n=0;for(size_t i=0;i<8;i++)n=(n<<8)|bytes[i];return n;}
int main(int argc,char **argv) {
  CHECK(argc==2 && !strcmp(argv[1],"cases"));budget_t budget={0};gt_allocator_t allocator={&budget,allocate,release};gt_bridge_wire_t *wire=NULL;
  budget.fail=true;CHECK(gt__bridge_wire_new(&allocator,&wire)==GT_ERR_IO && !wire && !budget.live);
  budget.fail=false;CHECK(gt__bridge_wire_new(&allocator,&wire)==GT_OK && wire && budget.live);
  size_t calls=budget.calls,count=0;uint8_t bytes[70000];
  for(;;) {
    uint8_t name_size;ssize_t first=read(0,&name_size,1);if(first<0 && errno==EINTR)continue;if(!first)break;CHECK(first==1 && name_size);
    char name[256]={0};CHECK(exact(name,name_size));printf("CASE %s\n",name);fflush(stdout);
    uint8_t expected[2],length[4];CHECK(exact(expected,2) && exact(length,4));
    size_t size=((size_t)length[0]<<24)|((size_t)length[1]<<16)|((size_t)length[2]<<8)|length[3];CHECK(size<=sizeof(bytes));
    uint64_t epoch=0,sequence=0;uint8_t kind=0;char connection[33]={0},instance[33]={0},stream[129]={0};
    if(expected[0]) {
      epoch=uint64be();sequence=uint64be();CHECK(exact(&kind,1) && exact(connection,32) && exact(instance,32) && exact(stream,129));
    }
    CHECK(exact(bytes,size));gt_bridge_message_t message;memset(&message,0xa5,sizeof(message));
    gt_status_t status=gt__bridge_wire_parse(wire,bytes,size,&message);
    CHECK((status==GT_OK)==(expected[0]!=0) && gt__bridge_wire_error(wire)==expected[1] && budget.calls==calls);
    const uint8_t *view=(const uint8_t *)1;size_t view_size=1;
    if(expected[0]) {
      CHECK(message.epoch==epoch && message.sequence==sequence && message.kind==(gt_bridge_kind_t)kind);
      CHECK(!strcmp(message.connection,connection) && !strcmp(message.instance,instance) && !strcmp(message.stream,stream));
      CHECK(gt__bridge_wire_raw(wire,message.payload,&view,&view_size) && view>=bytes && view_size && view+view_size<=bytes+size);
      CHECK(!gt__bridge_wire_member(wire,message.payload,"missing-member"));
    } else {
      uint8_t zero[sizeof(message)];memset(zero,0,sizeof(zero));CHECK(status==GT_ERR_PROTOCOL && !memcmp(&message,zero,sizeof(message)));
      CHECK(!gt__bridge_wire_raw(wire,1,&view,&view_size) && !view && !view_size && !gt__bridge_wire_member(wire,1,"payload"));
      CHECK(!gt__bridge_wire_string(wire,1,&view,&view_size) && !view && !view_size);
    }
    count++;
  }
  CHECK(count && gt__bridge_wire_parse(wire,NULL,1,NULL)==GT_ERR_INVALID_ARG);
  gt__bridge_wire_free(wire);CHECK(!budget.live);gt__bridge_wire_free(NULL);
  printf("PASS bridge_wire %zu cases; fixed workspace %zu bytes; no parse allocations; wiped release\n",count,budget.size);return 0;
}
