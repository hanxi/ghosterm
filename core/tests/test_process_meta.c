#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include "ipc/data-wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef struct {pthread_mutex_t lock;pthread_cond_t changed;unsigned mode,calls,queries;gt_session_process_t last;pthread_t main;bool closed;} fixture;
typedef struct {gt_io_backend_t base;fixture *f;} backend;
static int read_io(gt_io_backend_t *b,uint8_t *p,size_t n,int ms){(void)b;(void)p;(void)n;struct timespec t={0,ms*1000000L};nanosleep(&t,NULL);return GT_TRANSPORT_READ_TIMEOUT;}
static int write_io(gt_io_backend_t *b,const uint8_t *p,size_t n){(void)b;(void)p;(void)n;return GT_OK;}
static int resize_io(gt_io_backend_t *b,uint16_t c,uint16_t r,uint32_t x,uint32_t y){(void)b;(void)c;(void)r;(void)x;(void)y;return GT_OK;}
static unsigned mode(const gt_io_backend_t *b){fixture *f=((const backend *)b)->f;pthread_mutex_lock(&f->lock);unsigned m=f->mode;pthread_mutex_unlock(&f->lock);return m;}
static gt_transport_child_t child(const gt_io_backend_t *b){fixture *f=((const backend *)b)->f;pthread_mutex_lock(&f->lock);f->queries++;pthread_mutex_unlock(&f->lock);unsigned m=mode(b);return m==1?GT_TRANSPORT_CHILD_IDLE:m==2?GT_TRANSPORT_CHILD_JOB:m==6?(gt_transport_child_t)99:GT_TRANSPORT_CHILD_UNKNOWN;}
static bool cwd(const gt_io_backend_t *b,char *p,size_t n){unsigned m=mode(b);CHECK(n==1025);if(!m)return false;if(m==3){strcpy(p,"/\xc0\xaf");return true;}if(m==4){memset(p,'a',n);return true;}if(m==5){strcpy(p,"/partial");return false;}if(m==7){memset(p,'x',n-1);p[0]='/';p[n-1]=0;return true;}strcpy(p,m==2?"/changed":"/目录");return true;}
static void close_io(gt_io_backend_t *b){((backend *)b)->f->closed=true;free(b);}
static const gt_io_ops_t ops={.read=read_io,.write=write_io,.resize=resize_io,.child_state=child,.child_cwd=cwd,.close=close_io};
static gt_io_backend_t *open_io(void *ctx){backend *b=calloc(1,sizeof(*b));CHECK(b);b->f=ctx;b->base.ops=&ops;return &b->base;}
static gt_status_t open_state(void *ctx,gt_worker_reply_sink_t sink,void **out){(void)sink;*out=ctx;return GT_OK;}
static void close_state(void *ctx){(void)ctx;}
static gt_status_t apply(void *ctx,const gt_state_event_t *e){(void)ctx;(void)e;CHECK(false);return GT_ERR_INTERNAL;}
static gt_status_t freeze(void *ctx,void **out){(void)ctx;*out=malloc(1);return *out?GT_OK:GT_ERR_INTERNAL;}
static void process(void *ctx,const gt_session_process_t *m){fixture *f=ctx;CHECK(!pthread_equal(f->main,pthread_self()));pthread_mutex_lock(&f->lock);f->last=*m;f->calls++;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);}
static void wait_next(fixture *f,unsigned previous){struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=3;pthread_mutex_lock(&f->lock);while(f->calls==previous)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&end));pthread_mutex_unlock(&f->lock);}
static void owner(void){
 fixture f={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.main=pthread_self()};
 gt_worker_config_t c={.context=&f,.open_backend=open_io,.open_state=open_state,.close_state=close_state,.state_ops={apply,freeze},.process=process};
 gt_session_worker_t *w=gt__worker_start(&c);CHECK(w);wait_next(&f,0);
 pthread_mutex_lock(&f.lock);CHECK(f.calls==1 && f.last.child==GT_TRANSPORT_CHILD_UNKNOWN && !f.last.has_cwd && !f.last.cwd[0]);pthread_mutex_unlock(&f.lock);
 struct timespec before,after;CHECK(!clock_gettime(CLOCK_MONOTONIC,&before));struct timespec delay={0,650000000};nanosleep(&delay,NULL);CHECK(!clock_gettime(CLOCK_MONOTONIC,&after));
 uint64_t elapsed=(uint64_t)((after.tv_sec-before.tv_sec)*1000+(after.tv_nsec-before.tv_nsec)/1000000);
 pthread_mutex_lock(&f.lock);CHECK(f.calls==1 && f.queries<=2+elapsed/200);f.mode=1;unsigned count=f.calls;pthread_mutex_unlock(&f.lock);wait_next(&f,count);
 pthread_mutex_lock(&f.lock);CHECK(f.last.child==GT_TRANSPORT_CHILD_IDLE && f.last.has_cwd && !strcmp(f.last.cwd,"/目录"));f.mode=2;count=f.calls;pthread_mutex_unlock(&f.lock);wait_next(&f,count);
 pthread_mutex_lock(&f.lock);CHECK(f.last.child==GT_TRANSPORT_CHILD_JOB && f.last.has_cwd && !strcmp(f.last.cwd,"/changed"));pthread_mutex_unlock(&f.lock);
 for(unsigned m=3;m<=5;m++){
  pthread_mutex_lock(&f.lock);f.mode=m;count=f.calls;pthread_mutex_unlock(&f.lock);
  if(m==3)wait_next(&f,count);else {struct timespec settle={0,350000000};nanosleep(&settle,NULL);}
  pthread_mutex_lock(&f.lock);CHECK(f.last.child==GT_TRANSPORT_CHILD_UNKNOWN && !f.last.has_cwd && !f.last.cwd[0]);pthread_mutex_unlock(&f.lock);
 }
 pthread_mutex_lock(&f.lock);f.mode=7;pthread_mutex_unlock(&f.lock);struct timespec overflow_wait={0,350000000};nanosleep(&overflow_wait,NULL);
 pthread_mutex_lock(&f.lock);CHECK(!f.last.has_cwd && !f.last.cwd[0]);pthread_mutex_unlock(&f.lock);
 pthread_mutex_lock(&f.lock);f.mode=6;count=f.calls;pthread_mutex_unlock(&f.lock);wait_next(&f,count);
 pthread_mutex_lock(&f.lock);CHECK(f.last.child==GT_TRANSPORT_CHILD_UNKNOWN && f.last.has_cwd);pthread_mutex_unlock(&f.lock);
 gt__worker_stop(w);gt__worker_join(w);CHECK(f.closed);pthread_cond_destroy(&f.changed);pthread_mutex_destroy(&f.lock);
}
static void bad(gt_wire_frame_t f){gt_data_message_t out;memset(&out,0xad,sizeof(out));CHECK(!gt__data_decode(&f,&out));gt_data_message_t zero={0};CHECK(!memcmp(&out,&zero,sizeof(out)));}
static void codec(void){
 uint8_t bytes[1100];gt_wire_frame_t f={.payload=bytes};gt_data_message_t out,m={.type=GT_MSG_PROCESS_META,.generation=0x0102030405060708ULL,.process={.child=GT_TRANSPORT_CHILD_UNKNOWN}};
 CHECK(gt__data_encode(&m,bytes,sizeof(bytes),&f.header) && f.header.payload_length==20 && bytes[0]==1 && bytes[7]==8 && !bytes[11] && !bytes[15] && !bytes[19]);
 CHECK(gt__data_decode(&f,&out) && out.process.child==GT_TRANSPORT_CHILD_UNKNOWN && !out.process.has_cwd && !out.process.cwd[0]);
 m.process.child=GT_TRANSPORT_CHILD_JOB;m.process.has_cwd=true;strcpy(m.process.cwd,"/目录");
 CHECK(gt__data_encode(&m,bytes,sizeof(bytes),&f.header) && f.header.payload_length==27 && bytes[11]==1 && bytes[15]==3 && bytes[19]==7);
 CHECK(gt__data_decode(&f,&out) && out.process.has_cwd && !strcmp(out.process.cwd,"/目录") && !out.sequence);
 for(unsigned n=0;n<27;n++){gt_wire_frame_t cut=f;cut.header.payload_length=n;bad(cut);}gt_wire_frame_t extra=f;extra.header.payload_length++;bad(extra);
 bytes[11]=2;bad(f);bytes[11]=0;bad(f);bytes[11]=1;bytes[15]=99;bad(f);bytes[15]=3;bytes[20]=0;bad(f);bytes[20]='/';bytes[21]=0xc0;bad(f);
 memset(m.process.cwd,'a',sizeof(m.process.cwd));m.process.cwd[0]='/';m.process.cwd[1023]=0;
 CHECK(gt__data_encode(&m,bytes,sizeof(bytes),&f.header) && f.header.payload_length==1043 && gt__data_decode(&f,&out) && strlen(out.process.cwd)==1023);
 m.process.cwd[1023]='a';CHECK(!gt__data_encode(&m,bytes,sizeof(bytes),&f.header));
 strcpy(m.process.cwd,"relative");CHECK(!gt__data_encode(&m,bytes,sizeof(bytes),&f.header));strcpy(m.process.cwd,"/\xed\xa0\x80");CHECK(!gt__data_encode(&m,bytes,sizeof(bytes),&f.header));
}
int main(int argc,char **argv){CHECK(argc==2);if(!strcmp(argv[1],"codec"))codec();else {CHECK(!strcmp(argv[1],"owner"));owner();}printf("PASS process metadata %s\n",argv[1]);return 0;}
