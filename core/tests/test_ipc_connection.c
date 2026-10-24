#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ipc/connection.h"
#include "ipc/socket-stream.h"
#include "ipc/unix-endpoint.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static const uint8_t golden[27]={0x47,0x54,0x53,0x44,0,1,0x12,0x34,0,0,0,0,0,0,0,3,1,2,3,4,5,6,7,8,'a','b','c'};
static gt_wire_header_t header(void){return (gt_wire_header_t){.type=0x1234,.payload_length=3,.request_id=UINT64_C(0x0102030405060708)};}
typedef struct {
  uint8_t input[128],output[128];size_t length,position,sent,chunk;
  uint64_t now,read_delay,write_delay;
  bool clock_ok,blocked,eof;
  unsigned reads,writes,closed;
  int bad_read,bad_write;
} fake;
static gt_stream_result_t read_fake(void *context,void *bytes,size_t n) {
  fake *f=context;f->reads++;f->now+=f->read_delay;
  if(f->bad_read) {
    switch(f->bad_read) {
      case 1:return (gt_stream_result_t){GT_STREAM_DATA,0,0};
      case 2:return (gt_stream_result_t){GT_STREAM_DATA,n+1,0};
      case 3:return (gt_stream_result_t){GT_STREAM_WAIT,1,0};
      case 4:return (gt_stream_result_t){GT_STREAM_END,1,0};
      case 5:return (gt_stream_result_t){GT_STREAM_ERROR,1,0};
      case 6:return (gt_stream_result_t){(gt_stream_kind_t)99,0,0};
      default:return (gt_stream_result_t){GT_STREAM_ERROR,0,123};
    }
  }
  size_t count=f->length-f->position;if(count>n)count=n;if(count>f->chunk)count=f->chunk;
  if(count){memcpy(bytes,f->input+f->position,count);f->position+=count;return (gt_stream_result_t){GT_STREAM_DATA,count,0};}
  return (gt_stream_result_t){f->eof?GT_STREAM_END:GT_STREAM_WAIT,0,0};
}
static gt_stream_result_t write_fake(void *context,const void *bytes,size_t n) {
  fake *f=context;f->writes++;f->now+=f->write_delay;
  if(f->bad_write)return (gt_stream_result_t){GT_STREAM_DATA,n+1,0};
  if(f->blocked)return (gt_stream_result_t){GT_STREAM_WAIT,0,0};
  size_t count=n<f->chunk?n:f->chunk;CHECK(f->sent+count<=sizeof(f->output));
  memcpy(f->output+f->sent,bytes,count);f->sent+=count;return (gt_stream_result_t){GT_STREAM_DATA,count,0};
}
static bool clock_fake(void *context,uint64_t *out){fake *f=context;*out=f->now;return f->clock_ok;}
static void close_fake(void *context){fake *f=context;f->closed++;}
static fake initial(void){return (fake){.chunk=128,.clock_ok=true};}
static gt_stream_ops_t ops(fake *f){return (gt_stream_ops_t){f,read_fake,write_fake,clock_fake,close_fake};}
static gt_connection_t *open_fake(fake *f) {
  gt_connection_t *c=NULL;gt_stream_ops_t o=ops(f);
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){64,64,10},NULL,&c)==GT_CONNECTION_CREATED && c);return c;
}
static void fake_flow(void) {
  fake f=initial();memcpy(f.input,golden,27);memcpy(f.input+27,golden,27);f.length=54;f.chunk=1;
  gt_connection_t *c=open_fake(&f);gt_wire_header_t h=header();char payload[]="abc";
  CHECK(gt__connection_send(c,&h,payload)==GT_SEND_ACCEPTED);memset(payload,'x',3);
  unsigned frames=0;
  for(unsigned i=0;i<100;i++) {
    CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);gt_wire_frame_t frame;
    if(gt__connection_frame(c,&frame)) {
      CHECK(frame.header.request_id==UINT64_C(0x0102030405060708) && frame.header.payload_length==3 && !memcmp(frame.payload,"abc",3));
      frames++;CHECK(gt__connection_release_frame(c));
    }
    if(frames==2 && !gt__connection_wants_write(c))break;
  }
  CHECK(frames==2 && f.sent==27 && !memcmp(f.output,golden,27));
  gt__connection_cancel(c);CHECK(f.closed==1 && gt__connection_state(c)==GT_CONNECTION_CANCELLED);
  gt__connection_destroy(c);CHECK(f.closed==1);
}
static void receive_deadline(void) {
  fake f=initial();memcpy(f.input,golden,27);f.length=25;f.chunk=24;gt_connection_t *c=open_fake(&f);
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && f.position==24);f.now=4;
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && f.position==25);f.now=10;
  CHECK(gt__connection_step(c)==GT_CONNECTION_TIMEOUT && f.closed==1);gt__connection_destroy(c);CHECK(f.closed==1);
  f=initial();memcpy(f.input,golden,24);f.input[15]=0;f.length=24;f.read_delay=11;c=open_fake(&f);
  CHECK(gt__connection_step(c)==GT_CONNECTION_TIMEOUT);gt_wire_frame_t frame;CHECK(!gt__connection_frame(c,&frame));gt__connection_destroy(c);
}
static void send_deadline(void) {
  fake f=initial();f.chunk=1;gt_connection_t *c=open_fake(&f);gt_wire_header_t h=header();
  CHECK(gt__connection_send(c,&h,"abc")==GT_SEND_ACCEPTED);
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && f.sent==1);f.now=4;
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && f.sent==2);f.now=10;
  CHECK(gt__connection_step(c)==GT_CONNECTION_TIMEOUT && f.writes==2 && f.closed==1);gt__connection_destroy(c);
  f=initial();f.write_delay=11;c=open_fake(&f);h.payload_length=0;
  CHECK(gt__connection_send(c,&h,NULL)==GT_SEND_ACCEPTED);
  CHECK(gt__connection_step(c)==GT_CONNECTION_TIMEOUT && f.sent==24 && f.closed==1);gt__connection_destroy(c);
}
static void fairness(void) {
  fake f=initial();f.blocked=true;memcpy(f.input,golden,27);memcpy(f.input+27,golden,27);f.length=54;
  gt_connection_t *c=open_fake(&f);gt_wire_header_t h=header();CHECK(gt__connection_send(c,&h,"abc")==GT_SEND_ACCEPTED);
  CHECK(gt__connection_send(c,&h,"xyz")==GT_SEND_BUSY);
  for(unsigned i=0;i<2;i++)CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
  gt_wire_frame_t frame;CHECK(gt__connection_frame(c,&frame));unsigned reads=f.reads,writes=f.writes;
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && f.reads==reads && f.writes==writes+1);
  CHECK(gt__connection_release_frame(c));
  for(unsigned i=0;i<2;i++)CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(gt__connection_frame(c,&frame) && f.reads==reads+2 && !memcmp(frame.payload,"abc",3));
  CHECK(gt__connection_release_frame(c));f.blocked=false;
  for(unsigned i=0;i<3 && gt__connection_wants_write(c);i++)CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(f.sent==27 && !memcmp(f.output,golden,27));gt__connection_destroy(c);CHECK(f.closed==1);
}
static void buffers(void) {
  fake f=initial();f.blocked=true;memcpy(f.input,golden,27);memcpy(f.input+27,golden,27);
  memcpy(f.input+51,"xyz",3);f.length=54;gt_connection_t *c=open_fake(&f);
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && gt__connection_step(c)==GT_CONNECTION_OPEN);
  gt_wire_frame_t frame;CHECK(gt__connection_frame(c,&frame));
  CHECK(gt__connection_send(c,&frame.header,frame.payload)==GT_SEND_ACCEPTED && gt__connection_release_frame(c));
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(gt__connection_frame(c,&frame) && !memcmp(frame.payload,"xyz",3));
  CHECK(gt__connection_release_frame(c));f.blocked=false;
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(f.sent==27 && !memcmp(f.output,golden,27));gt__connection_destroy(c);
}
static void invalid_io(void) {
  for(int bad=1;bad<=7;bad++) {
    fake f=initial();f.bad_read=bad;gt_connection_t *c=open_fake(&f);
    CHECK(gt__connection_step(c)==GT_CONNECTION_IO_ERROR && f.closed==1);
    if(bad==7)CHECK(gt__connection_system_error(c)==123);
    CHECK(gt__connection_step(c)==GT_CONNECTION_IO_ERROR && f.closed==1);gt__connection_destroy(c);CHECK(f.closed==1);
  }
  fake f=initial();f.bad_write=1;gt_connection_t *c=open_fake(&f);gt_wire_header_t h=header();
  CHECK(gt__connection_send(c,&h,"abc")==GT_SEND_ACCEPTED);
  CHECK(gt__connection_step(c)==GT_CONNECTION_IO_ERROR && f.closed==1);gt__connection_destroy(c);
}
static void cancel_test(void) {
  fake f=initial();f.chunk=1;gt_connection_t *c=open_fake(&f);gt_wire_header_t h=header();
  CHECK(gt__connection_send(c,&h,"abc")==GT_SEND_ACCEPTED);CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
  gt__connection_cancel(c);gt__connection_cancel(c);CHECK(f.closed==1 && !gt__connection_wants_write(c));
  CHECK(gt__connection_send(c,&h,"abc")==GT_SEND_CLOSED);CHECK(gt__connection_step(c)==GT_CONNECTION_CANCELLED);
  gt_wire_frame_t frame;CHECK(!gt__connection_frame(c,&frame) && !gt__connection_release_frame(c));
  gt__connection_destroy(c);CHECK(f.closed==1);
}
static void clock_test(void) {
  fake f=initial();gt_connection_t *c=open_fake(&f);f.now=5;CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
  f.now=4;CHECK(gt__connection_step(c)==GT_CONNECTION_CLOCK_ERROR && f.closed==1);gt__connection_destroy(c);
  f=initial();c=open_fake(&f);f.clock_ok=false;CHECK(gt__connection_step(c)==GT_CONNECTION_CLOCK_ERROR && f.closed==1);gt__connection_destroy(c);
  f=initial();f.clock_ok=false;gt_stream_ops_t o=ops(&f);c=(gt_connection_t *)1;
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){1,1,10},NULL,&c)==GT_CONNECTION_NO_CLOCK && !c && !f.closed);
}
typedef struct {size_t live;bool reject;} allocation;
static void *admit(void *context,size_t n){allocation *a=context;if(a->reject)return NULL;CHECK(!a->live);a->live=n;return malloc(n);}
static void refund(void *context,void *p,size_t n){allocation *a=context;CHECK(p && a->live==n);a->live=0;free(p);}
static void allocation_test(void) {
  fake f=initial();allocation a={.reject=true};gt_allocator_t domain={&a,admit,refund};gt_stream_ops_t o=ops(&f);gt_connection_t *c=(gt_connection_t *)1;
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){64,64,10},&domain,&c)==GT_CONNECTION_NO_MEMORY && !c && !a.live && !f.closed);
  a.reject=false;CHECK(gt__connection_open(&o,(gt_connection_limits_t){64,64,10},&domain,&c)==GT_CONNECTION_CREATED && a.live>=128);
  gt__connection_destroy(c);CHECK(!a.live && f.closed==1);
  f.clock_ok=false;CHECK(gt__connection_open(&o,(gt_connection_limits_t){64,64,10},&domain,&c)==GT_CONNECTION_NO_CLOCK && !c && !a.live && f.closed==1);
}
typedef struct {uint8_t *memory;size_t size;} erased_allocation;
static void *erased_admit(void *ctx,size_t n) {
  erased_allocation *a=ctx;a->memory=malloc(n);a->size=n;return a->memory;
}
static void erased_refund(void *ctx,void *p,size_t n) {
  erased_allocation *a=ctx;CHECK(p==a->memory && n==a->size);
  for(size_t i=0;i<n;i++)CHECK(a->memory[i]==0);
  free(p);a->memory=NULL;a->size=0;
}
static bool secret_present(const erased_allocation *a) {
  for(size_t i=0;i+16<=a->size;i++) {
    unsigned count=0;while(count<16 && a->memory[i+count]==0xd7)count++;
    if(count==16)return true;
  }
  return false;
}
static void erasure(void) {
  fake f=initial();erased_allocation a={0};gt_allocator_t allocator={&a,erased_admit,erased_refund};
  gt_stream_ops_t o=ops(&f);gt_connection_t *c;gt_wire_header_t h={.type=1,.payload_length=16};
  CHECK(gt__wire_header_encode(&h,f.input));memset(f.input+24,0xd7,16);f.length=40;
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){64,64,10},&allocator,&c)==GT_CONNECTION_CREATED);
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(secret_present(&a));CHECK(gt__connection_release_frame(c));CHECK(!secret_present(&a));
  CHECK(gt__connection_send(c,&h,f.input+24)==GT_SEND_ACCEPTED && secret_present(&a));
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN && gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(!gt__connection_wants_write(c) && !secret_present(&a));
  CHECK(gt__connection_send(c,&h,f.input+24)==GT_SEND_ACCEPTED && secret_present(&a));
  gt__connection_cancel(c);CHECK(!secret_present(&a));gt__connection_destroy(c);CHECK(!a.memory);
}
static void protocol(void) {
  fake f=initial();memcpy(f.input,golden,27);f.input[0]='X';f.length=27;gt_connection_t *c=open_fake(&f);
  CHECK(gt__connection_step(c)==GT_CONNECTION_PROTOCOL_ERROR && f.closed==1);gt__connection_destroy(c);
  f=initial();f.eof=true;c=open_fake(&f);CHECK(gt__connection_step(c)==GT_CONNECTION_END && f.closed==1);gt__connection_destroy(c);
  f=initial();f.eof=true;memcpy(f.input,golden,27);f.length=25;c=open_fake(&f);
  CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
  CHECK(gt__connection_step(c)==GT_CONNECTION_PROTOCOL_ERROR && f.closed==1);gt__connection_destroy(c);
}
static void invalid(void) {
  fake f=initial();gt_stream_ops_t o=ops(&f);gt_connection_t *c=(gt_connection_t *)1;
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){0,0,0},NULL,&c)==GT_CONNECTION_INVALID && !c && !f.closed);
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){GT_WIRE_MAX_PAYLOAD+1,0,1},NULL,&c)==GT_CONNECTION_INVALID);
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){0,GT_WIRE_MAX_PAYLOAD+1,1},NULL,&c)==GT_CONNECTION_INVALID);
  CHECK(gt__connection_open(NULL,(gt_connection_limits_t){0,0,1},NULL,&c)==GT_CONNECTION_INVALID);
  CHECK(gt__connection_open(&o,(gt_connection_limits_t){0,0,1},NULL,NULL)==GT_CONNECTION_INVALID);
  c=open_fake(&f);gt_wire_header_t h=header();h.payload_length=65;
  CHECK(gt__connection_send(c,&h,f.input)==GT_SEND_REJECTED);h=header();h.flags=1;
  CHECK(gt__connection_send(c,&h,"abc")==GT_SEND_REJECTED);h=header();CHECK(gt__connection_send(c,&h,NULL)==GT_SEND_REJECTED);
  CHECK(gt__connection_state(c)==GT_CONNECTION_OPEN && !gt__connection_wants_write(c));gt__connection_destroy(c);
  gt__connection_destroy(NULL);gt__connection_cancel(NULL);CHECK(gt__connection_step(NULL)==GT_CONNECTION_IO_ERROR);
}
static gt_connection_t *adopt(int fd,gt_socket_stream_t *stream,uint32_t send_limit,unsigned timeout) {
  gt_stream_ops_t o;CHECK(gt__socket_stream_adopt(fd,stream,&o)==GT_OK);
  CHECK((fcntl(fd,F_GETFL)&O_NONBLOCK) && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
  gt_connection_t *c=NULL;CHECK(gt__connection_open(&o,(gt_connection_limits_t){64,send_limit,timeout},NULL,&c)==GT_CONNECTION_CREATED);return c;
}
static void real_flow(const char *temporary) {
  char root[256],directory[256],name[512];CHECK(snprintf(root,sizeof(root),"%s/cn-XXXXXX",temporary)>0 && mkdtemp(root));
  CHECK(snprintf(directory,sizeof(directory),"%s/runtime",root)>0);gt_unix_endpoint_t *server=NULL,*client=NULL;
  CHECK(gt__unix_endpoint_open(directory,true,NULL,&server)==GT_ENDPOINT_OK && gt__unix_endpoint_listen(server,8)==GT_ENDPOINT_OK);
  CHECK(gt__unix_endpoint_open(directory,false,NULL,&client)==GT_ENDPOINT_OK);int a=-1,b=-1;
  CHECK(gt__unix_endpoint_connect(client,100,&a)==GT_ENDPOINT_OK && gt__unix_endpoint_accept(server,&b)==GT_ENDPOINT_OK);
  gt_socket_stream_t sa,sb;gt_connection_t *ca=adopt(a,&sa,64,1000),*cb=adopt(b,&sb,64,1000);gt_wire_header_t h=header();
  CHECK(gt__connection_send(ca,&h,"abc")==GT_SEND_ACCEPTED);bool received=false;
  for(unsigned i=0;i<1000;i++) {
    CHECK(gt__connection_step(ca)==GT_CONNECTION_OPEN && gt__connection_step(cb)==GT_CONNECTION_OPEN);
    gt_wire_frame_t frame;if(gt__connection_frame(cb,&frame)) {
      CHECK(!memcmp(frame.payload,"abc",3));CHECK(gt__connection_send(cb,&frame.header,frame.payload)==GT_SEND_ACCEPTED);
      CHECK(gt__connection_release_frame(cb));
    }
    if(gt__connection_frame(ca,&frame)){CHECK(!memcmp(frame.payload,"abc",3));received=true;break;}
  }
  CHECK(received);gt__connection_destroy(ca);gt__connection_destroy(cb);CHECK(sa.fd==-1 && sb.fd==-1);
  CHECK(gt__unix_endpoint_close(client)==GT_ENDPOINT_OK && gt__unix_endpoint_close(server)==GT_ENDPOINT_OK);
  CHECK(snprintf(name,sizeof(name),"%s/daemon.lock",directory)>0 && !unlink(name));CHECK(!rmdir(directory) && !rmdir(root));
}
static void real_pressure(void) {
  int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));int size=4096;CHECK(!setsockopt(fds[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)));
  gt_socket_stream_t stream;gt_connection_t *c=adopt(fds[0],&stream,GT_WIRE_MAX_PAYLOAD,100);
  uint8_t *payload=malloc(GT_WIRE_MAX_PAYLOAD);CHECK(payload);memset(payload,'p',GT_WIRE_MAX_PAYLOAD);
  gt_wire_header_t h=header();h.payload_length=GT_WIRE_MAX_PAYLOAD;CHECK(gt__connection_send(c,&h,payload)==GT_SEND_ACCEPTED);free(payload);
  CHECK(send(fds[1],golden,sizeof(golden),0)==sizeof(golden));bool received=false;unsigned steps=0;
  while(gt__connection_step(c)==GT_CONNECTION_OPEN) {
    gt_wire_frame_t frame;if(gt__connection_frame(c,&frame)){CHECK(!memcmp(frame.payload,"abc",3));received=true;CHECK(gt__connection_release_frame(c));}
    CHECK(++steps<1000);const struct timespec delay={0,1000000};nanosleep(&delay,NULL);
  }
  CHECK(received && gt__connection_state(c)==GT_CONNECTION_TIMEOUT && stream.fd==-1);close(fds[1]);gt__connection_destroy(c);
}
static void real_eof(void) {
  for(unsigned n=0;n<3;n++) {
    int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));gt_socket_stream_t stream;gt_connection_t *c=adopt(fds[0],&stream,64,1000);
    if(n)CHECK(send(fds[1],golden,n,0)==(ssize_t)n);
    if(n)CHECK(gt__connection_step(c)==GT_CONNECTION_OPEN);
    close(fds[1]);CHECK(gt__connection_step(c)==(n?GT_CONNECTION_PROTOCOL_ERROR:GT_CONNECTION_END));
    CHECK(stream.fd==-1);gt__connection_destroy(c);
  }
  gt_socket_stream_t s;gt_stream_ops_t o;int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,fds));
  CHECK(gt__socket_stream_adopt(fds[0],&s,&o)==GT_ERR_INVALID_ARG && s.fd==-1 && !o.read);CHECK(fcntl(fds[0],F_GETFD)>=0);close(fds[0]);close(fds[1]);
}
static void hup(void) {
  int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));gt_socket_stream_t stream;
  gt_connection_t *c=adopt(fds[0],&stream,64,1000);
  CHECK(send(fds[1],golden,sizeof(golden),0)==sizeof(golden));close(fds[1]);
  CHECK(gt__connection_step(c)==GT_CONNECTION_END && stream.fd==-1);
  gt_wire_frame_t frame;CHECK(!gt__connection_frame(c,&frame));gt__connection_destroy(c);
}
static volatile sig_atomic_t pipe_signals;
static void pipe_signal(int sig){(void)sig;pipe_signals++;}
static void sigpipe_test(void) {
  int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));gt_socket_stream_t stream;gt_stream_ops_t o;
  CHECK(gt__socket_stream_adopt(fds[0],&stream,&o)==GT_OK);close(fds[1]);
  struct sigaction action={0},saved;action.sa_handler=pipe_signal;sigemptyset(&action.sa_mask);
  CHECK(!sigaction(SIGPIPE,&action,&saved));pipe_signals=0;
  gt_stream_result_t result=o.write(o.context,"x",1);
  CHECK(!sigaction(SIGPIPE,&saved,NULL));CHECK(!pipe_signals && result.kind==GT_STREAM_ERROR && !result.length && result.system_error);
  o.close(o.context);CHECK(stream.fd==-1);gt__socket_stream_close(&stream);
}
int main(int argc,char **argv) {
  CHECK(argc==3);
  if(!strcmp(argv[1],"flow"))fake_flow();else if(!strcmp(argv[1],"receive-deadline"))receive_deadline();
  else if(!strcmp(argv[1],"send-deadline"))send_deadline();else if(!strcmp(argv[1],"fairness"))fairness();
  else if(!strcmp(argv[1],"buffers"))buffers();
  else if(!strcmp(argv[1],"invalid-io"))invalid_io();else if(!strcmp(argv[1],"cancel"))cancel_test();
  else if(!strcmp(argv[1],"clock"))clock_test();else if(!strcmp(argv[1],"allocation"))allocation_test();
  else if(!strcmp(argv[1],"protocol"))protocol();else if(!strcmp(argv[1],"invalid"))invalid();
  else if(!strcmp(argv[1],"real-flow"))real_flow(argv[2]);else if(!strcmp(argv[1],"real-pressure"))real_pressure();
  else if(!strcmp(argv[1],"real-eof"))real_eof();else if(!strcmp(argv[1],"sigpipe"))sigpipe_test();
  else if(!strcmp(argv[1],"hup"))hup();else if(!strcmp(argv[1],"erasure"))erasure();else CHECK(false);
  printf("PASS framed connection %s\n",argv[1]);return 0;
}
