#define _POSIX_C_SOURCE 200809L
#include "session-process-wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {uint8_t bytes[2*PW_CHUNK+128];size_t used,read,chunk,calls;} memory;
static ssize_t transfer(void *context,void *bytes,size_t n,bool sending) {
  memory *m=context;if(m->calls++%3==0)return 0;
  if(n>m->chunk)n=m->chunk;
  if(sending) {assert(n<=sizeof(m->bytes)-m->used);memcpy(m->bytes+m->used,bytes,n);m->used+=n;}
  else {if(m->read==m->used)return -1;if(n>m->used-m->read)n=m->used-m->read;memcpy(bytes,m->bytes+m->read,n);m->read+=n;}
  return (ssize_t)n;
}
typedef struct {memory memory;bool sending;unsigned bodies;double release;} slow_memory;
static double seconds(void) {
  struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));return ts.tv_sec+ts.tv_nsec/1e9;
}
static ssize_t slow_transfer(void *context,void *bytes,size_t n,bool sending) {
  slow_memory *s=context;
  /* Pause before each body, then return WAIT. Each frame costs 2.7s (less
   * than 5s), but two frames cannot both finish within one blob deadline. */
  size_t offset=sending ? s->memory.used : s->memory.read;
  if(sending==s->sending && s->bodies<2 && offset==(s->bodies ? PW_CHUNK+48u : 24u)) {
    s->bodies++;s->release=seconds()+2.7;
  }
  if(sending==s->sending && seconds()<s->release) {
    const struct timespec pause={0,30000000};assert(!nanosleep(&pause,NULL));return 0;
  }
  return transfer(&s->memory,bytes,n,sending);
}
int main(int argc,char **argv) {
  assert(argc==2);memory m={.chunk=3};pw_io io={&m,transfer};pw_frame frame;
  /* Independent literal wire oracle. Changing encoder and decoder together
   * cannot make this pass when bytes/endianness/header size have drifted. */
  const uint8_t expected[]={0x47,0x54,0x50,0x30,0,1,0,5,0,0,0,0,0,0,0,3,
    1,2,3,4,5,6,7,8,0,255,'X'};
  if(!strcmp(argv[1],"slow-send") || !strcmp(argv[1],"slow-receive")) {
    slow_memory slow={.memory={.chunk=PW_CHUNK},.sending=!strcmp(argv[1],"slow-send")};
    uint8_t *bytes=calloc(PW_CHUNK+17,1);assert(bytes);
    pw_io slow_io={&slow,slow_transfer};bool ok;
    if(!slow.sending) {
      pw_io writer={&slow.memory,transfer};assert(pw_send_blob(&writer,PW_RESOURCES,bytes,PW_CHUNK+17));
    }
    double start=seconds();
    ok=slow.sending ? pw_send_blob(&slow_io,PW_RESOURCES,bytes,PW_CHUNK+17) :
                     pw_receive_blob(&slow_io,PW_RESOURCES,bytes,PW_CHUNK+17);
    double elapsed=seconds()-start;
    assert(!ok && slow.bodies==2 && elapsed>=5 && elapsed<6);
    free(bytes);return 0;
  } else if(!strcmp(argv[1],"valid")) {
    assert(pw_send(&io,PW_OUTPUT,UINT64_C(0x0102030405060708),"\0\xffX",3));
    assert(m.used==sizeof(expected) && !memcmp(m.bytes,expected,sizeof(expected)));
    assert(pw_receive(&io,&frame) && frame.kind==PW_OUTPUT && frame.sequence==UINT64_C(0x0102030405060708));
    assert(frame.length==3 && !memcmp(frame.bytes,expected+24,3));
    assert(!pw_receive(&io,&frame) && !frame.kind && !frame.length);
    if(!strcmp(argv[1],"length"))assert(m.read==24);
  } else if(!strcmp(argv[1],"blobs")) {
    uint8_t *bytes=malloc(PW_CHUNK+17),*copy=malloc(PW_CHUNK+17);assert(bytes && copy);
    for(size_t i=0;i<PW_CHUNK+17;i++)bytes[i]=(uint8_t)(i*37);
    m.chunk=7;assert(pw_send_blob(&io,PW_RESOURCES,bytes,PW_CHUNK+17));
    assert(pw_receive_blob(&io,PW_RESOURCES,copy,PW_CHUNK+17) && !memcmp(bytes,copy,PW_CHUNK+17));
    free(bytes);free(copy);
  } else if(!strcmp(argv[1],"invalid-send")) {
    uint8_t byte=0;
    assert(!pw_send(&io,PW_OUTPUT,0,&byte,PW_CHUNK+1) && !m.used);
    assert(!pw_send(&io,0,0,&byte,1) && !m.used);
    assert(!pw_send(&io,PW_HISTORY+1,0,&byte,1) && !m.used);
    assert(!pw_send(&io,PW_OUTPUT,0,NULL,1) && !m.used);
    assert(!pw_send_blob(&io,PW_RESOURCES,&byte,PW_LIMIT+1) && !m.used);
  } else {
    memcpy(m.bytes,expected,sizeof(expected));m.used=sizeof(expected);
    if(!strcmp(argv[1],"magic"))m.bytes[0]^=1;
    else if(!strcmp(argv[1],"version"))m.bytes[5]=2;
    else if(!strcmp(argv[1],"flags"))m.bytes[11]=1;
    else if(!strcmp(argv[1],"kind"))m.bytes[7]=PW_HISTORY+1;
    else if(!strcmp(argv[1],"length")) {m.bytes[12]=1;m.bytes[13]=0;m.bytes[14]=0;m.bytes[15]=0;}
    else if(!strcmp(argv[1],"truncated"))m.used--;
    else if(!strcmp(argv[1],"blob-kind")) {
      memset(m.bytes+16,0,8);uint8_t bytes[3];assert(!pw_receive_blob(&io,PW_CANONICAL,bytes,3));return 0;
    } else if(!strcmp(argv[1],"blob-length")) {
      memset(m.bytes+16,0,8);uint8_t guarded[3]={0,0,42};
      assert(!pw_receive_blob(&io,PW_OUTPUT,guarded,2));
      assert(!guarded[0] && !guarded[1] && guarded[2]==42);return 0;
    } else assert(false);
    assert(!pw_receive(&io,&frame) && !frame.kind && !frame.length);
    if(!strcmp(argv[1],"length"))assert(m.read==24);
  }
  return 0;
}
