#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ipc/ssh-session-provider.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static gt_ssh_session_provider_t *active;static gt_ssh_t *owned;
static void cleanup(void){gt_ssh_session_provider_t *p=active;gt_ssh_t *s=owned;active=NULL;owned=NULL;gt__ssh_session_provider_close(p);gt_ssh_close(s);}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);cleanup();exit(1);}} while(0)
typedef union {max_align_t align;size_t n;} block;
typedef struct {gt_session_link_t link;uint64_t now,request;unsigned requested,cancelled,hostkey_calls;size_t live;bool refuse,reenter,allow_hostkey;uint64_t host_request;} fixture;
static void *admit(void *ctx,size_t n){fixture *f=ctx;if(f->refuse)return NULL;block *p=malloc(sizeof(*p)+n);CHECK(p);p->n=n;f->live++;return p+1;}
static void refund(void *ctx,void *p,size_t n){fixture *f=ctx;block *b=(block *)p-1;CHECK(f->live && b->n==n);for(size_t i=0;i<n;i++)CHECK(!((uint8_t *)p)[i]);f->live--;free(b);}
static bool clock_now(void *ctx,uint64_t *now){*now=((fixture *)ctx)->now;return true;}
static gt_ssh_connector_config_t resolved(void){return (gt_ssh_connector_config_t){.family=4,.address={127,0,0,1},.timeout_ms=60000};}
static void requested(void *ctx,uint64_t request,uint32_t attempt){fixture *f=ctx;f->request=request;f->requested++;CHECK(attempt==f->requested);if(f->reenter){gt_ssh_connector_config_t c=resolved();CHECK(gt__ssh_session_provider_supply(active,request,&owned,&c)==GT_ERR_INVALID_ARG && owned);CHECK(gt__ssh_session_provider_reject(active,request,GT_ERR_IO)==GT_ERR_INVALID_ARG);}}
static void cancelled(void *ctx,uint64_t request){fixture *f=ctx;CHECK(request==f->request);f->cancelled++;}
static void hostkey(void *ctx,uint64_t request,const gt_ssh_connector_info_t *info){fixture *f=ctx;CHECK(f->allow_hostkey && info->host==GT_SSH_HOST_UNKNOWN);f->host_request=request;f->hostkey_calls++;}
static gt_ssh_session_provider_config_t config(fixture *f){return (gt_ssh_session_provider_config_t){.proxy="/fixture/proxy",.daemon="/fixture/daemon",.runtime="/fixture/runtime",.authentication_timeout_ms=100,.clock_context=f,.clock=clock_now,.context=f,.requested=requested,.cancelled=cancelled,.hostkey=hostkey};}
static void ssh(uint16_t port){gt_ssh_opts_t o={.host="provider-identity.invalid",.port=port,.username="fixture",.known_hosts_path="/dev/null"};owned=gt_ssh_new(&o);CHECK(owned && gt_ssh_auth_password(owned,"provider-fixture-password")==GT_OK);}
static void open_provider(fixture *f){gt_allocator_t a={f,admit,refund};gt_ssh_session_provider_config_t c=config(f);CHECK(gt__ssh_session_provider_open(&c,&a,&active)==GT_OK && gt__ssh_session_provider_link(active,&f->link));}
static gt_ssh_provider_info_t info(void){gt_ssh_provider_info_t i;CHECK(gt__ssh_session_provider_info(active,&i));return i;}
static gt_session_link_state_t step(fixture *f,bool start,uint32_t attempt,gt_status_t *error){return f->link.step(f->link.context,start,attempt,error);}
static void finish(fixture *f){cleanup();CHECK(!f->live);}
static int listener(uint16_t *port){int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(UINT32_C(0x7f000001))};CHECK(!bind(fd,(struct sockaddr *)&a,sizeof(a)) && !listen(fd,4));socklen_t n=sizeof(a);CHECK(!getsockname(fd,(struct sockaddr *)&a,&n));*port=ntohs(a.sin_port);CHECK(!fcntl(fd,F_SETFL,O_NONBLOCK));return fd;}
static void requests(void){fixture f={0};open_provider(&f);gt_status_t e;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT && e==GT_OK && f.request==1 && f.requested==1);for(unsigned i=0;i<4;i++)CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_WAIT && f.requested==1);f.link.reset(f.link.context);CHECK(f.cancelled==1 && info().phase==GT_SSH_PROVIDER_IDLE);CHECK(step(&f,true,2,&e)==GT_SESSION_LINK_WAIT && f.request==2 && f.requested==2);finish(&f);CHECK(f.cancelled==2);}
static void stale(void){fixture f={0};open_provider(&f);gt_status_t e;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT);uint64_t old=f.request;f.link.reset(f.link.context);CHECK(step(&f,true,2,&e)==GT_SESSION_LINK_WAIT);ssh(1);gt_ssh_connector_config_t c=resolved();CHECK(gt__ssh_session_provider_supply(active,old,&owned,&c)==GT_ERR_NOT_FOUND && owned);CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_OK && !owned);ssh(1);CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_ERR_NOT_FOUND && owned);finish(&f);CHECK(f.cancelled==2);}
static void deadline(void){for(unsigned variant=0;variant<2;variant++){fixture f={0};open_provider(&f);gt_status_t e;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT);f.now=100;ssh(1);gt_ssh_connector_config_t c=resolved();
  if(variant){CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_ERR_IO && owned && info().deadline);}
  CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_RETRY && e==GT_ERR_IO && info().deadline && f.cancelled==1);CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_ERR_NOT_FOUND && owned);f.link.reset(f.link.context);finish(&f);CHECK(f.cancelled==1);
}}
static void budget(bool cancel) {
  fixture f={0};open_provider(&f);gt_status_t e;uint16_t port;int server=listener(&port),accepted=-1;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT);f.now=60;ssh(port);gt_ssh_connector_config_t c=resolved();CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_OK && !owned);
  for(unsigned i=0;i<100;i++){CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_WAIT && e==GT_OK);if(accepted<0)accepted=accept(server,NULL,NULL);if(accepted>=0 && info().authentication.phase==GT_SSH_CONNECT_NEGOTIATING)break;}
  CHECK(accepted>=0 && info().authentication.phase==GT_SSH_CONNECT_NEGOTIATING);
  if(cancel){f.link.reset(f.link.context);CHECK(info().phase==GT_SSH_PROVIDER_IDLE && f.cancelled==1);}
  else{f.now=99;CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_WAIT);f.now=100;CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_RETRY && e==GT_ERR_IO && info().deadline && f.cancelled==1);}
  finish(&f);close(accepted);close(server);
}
static void rejection(void){fixture f={0};open_provider(&f);gt_status_t e;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT);CHECK(gt__ssh_session_provider_reject(active,f.request,GT_OK)==GT_ERR_INVALID_ARG);CHECK(gt__ssh_session_provider_reject(active,f.request,GT_ERR_AUTH)==GT_OK);CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_FATAL && e==GT_ERR_AUTH && f.cancelled==1);f.link.reset(f.link.context);CHECK(step(&f,true,2,&e)==GT_SESSION_LINK_WAIT);CHECK(gt__ssh_session_provider_reject(active,f.request,GT_ERR_IO)==GT_OK);CHECK(step(&f,false,2,&e)==GT_SESSION_LINK_RETRY && e==GT_ERR_IO && f.cancelled==2);finish(&f);}
static void admission(void){fixture f={.refuse=true};gt_allocator_t a={&f,admit,refund};gt_ssh_session_provider_config_t c=config(&f);CHECK(gt__ssh_session_provider_open(&c,&a,&active)==GT_ERR_IO && !active && !f.requested && !f.live);f.refuse=false;c.proxy="relative";CHECK(gt__ssh_session_provider_open(&c,&a,&active)==GT_ERR_INVALID_ARG && !active && !f.live);}
static void clock_error(void){fixture f={0};open_provider(&f);gt_status_t e;f.now=10;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT);f.now=9;CHECK(step(&f,false,1,&e)==GT_SESSION_LINK_FATAL && e==GT_ERR_INTERNAL && f.cancelled==1);f.link.reset(f.link.context);CHECK(step(&f,true,2,&e)==GT_SESSION_LINK_FATAL && e==GT_ERR_INTERNAL && f.requested==1);finish(&f);}
static void reentry(void){fixture f={.reenter=true};ssh(1);open_provider(&f);gt_status_t e;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT && owned);gt_ssh_connector_config_t c=resolved();CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_OK && !owned);finish(&f);CHECK(f.cancelled==1);}
static bool clock_real(void *ctx,uint64_t *out){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static uint64_t real_now(void){uint64_t n;CHECK(clock_real(NULL,&n));return n;}
static void real_flow(int argc,char **argv) {
  CHECK(argc==6);char directory[1024],known[1100],proxy[1100],executable[64]="/fixture/daemon",runtime[64]="/fixture/runtime";
  CHECK(snprintf(directory,sizeof(directory),"%s/provider-XXXXXX",argv[5])>0 && mkdtemp(directory));CHECK(snprintf(known,sizeof(known),"%s/known_hosts",directory)>0 && snprintf(proxy,sizeof(proxy),"%s/proxy 'fixture.sh",directory)>0);
  FILE *f=fopen(proxy,"wb");CHECK(f && fputs("#!/bin/sh\n[ \"$1\" = --runtime-dir ] && [ \"$2\" = /fixture/runtime ] && [ \"$3\" = --role ] && [ \"$4\" = management ] && [ \"$5\" = --daemon ] && [ \"$6\" = /fixture/daemon ] || exit 77\nprintf provider-ready\nexec cat\n",f)>=0 && !fclose(f) && !chmod(proxy,0700));
  fixture state={.allow_hostkey=true};gt_ssh_session_provider_config_t c=config(&state);c.proxy=proxy;c.daemon=executable;c.runtime=runtime;c.clock=clock_real;c.authentication_timeout_ms=3000;
  CHECK(gt__ssh_session_provider_open(&c,NULL,&active)==GT_OK && gt__ssh_session_provider_link(active,&state.link));char script[1100];strcpy(script,proxy);proxy[0]=executable[0]=runtime[0]=0;
  gt_status_t error;CHECK(step(&state,true,1,&error)==GT_SESSION_LINK_WAIT);f=fopen(argv[4],"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));char *key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,f)==(size_t)n);fclose(f);
  gt_ssh_opts_t options={.host="provider-identity.invalid",.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=known};owned=gt_ssh_new(&options);CHECK(owned && gt_ssh_auth_private_key(owned,key,NULL)==GT_OK);memset(key,0,(size_t)n);free(key);
  gt_ssh_connector_config_t address=resolved();CHECK(inet_pton(AF_INET,argv[1],address.address)==1 && gt__ssh_session_provider_supply(active,state.request,&owned,&address)==GT_OK && !owned);
  uint64_t deadline=real_now()+5000;
  while(info().phase!=GT_SSH_PROVIDER_ACTIVE) {
    gt_session_link_state_t link=step(&state,false,1,&error);CHECK((link==GT_SESSION_LINK_WAIT || link==GT_SESSION_LINK_READY) && error==GT_OK && real_now()<deadline);
    if(state.host_request){for(unsigned j=0;j<4;j++)CHECK(step(&state,false,1,&error)==GT_SESSION_LINK_WAIT && error==GT_OK);CHECK(state.hostkey_calls==1);CHECK(gt__ssh_session_provider_decide(active,state.host_request,GT_HOSTKEY_ACCEPT)==GT_OK);state.host_request=0;}
    struct timespec t={0,1000000};nanosleep(&t,NULL);
  }
  gt_stream_ops_t stream;CHECK(state.link.stream(state.link.context,0,&stream)==GT_OK);const uint8_t payload[]={0,'X',255};uint8_t bytes[64]={0};size_t sent=0,received=0;const size_t marker=sizeof("provider-ready")-1;
  while(received<marker+sizeof(payload)) {
    CHECK(step(&state,false,1,&error)==GT_SESSION_LINK_READY && error==GT_OK && real_now()<deadline);
    if(sent<sizeof(payload)){gt_stream_result_t r=stream.write(stream.context,payload+sent,sizeof(payload)-sent);CHECK(r.kind==GT_STREAM_WAIT || r.kind==GT_STREAM_DATA);if(r.kind==GT_STREAM_DATA)sent+=r.length;}
    gt_stream_result_t r=stream.read(stream.context,bytes+received,sizeof(bytes)-received);CHECK(r.kind==GT_STREAM_WAIT || r.kind==GT_STREAM_DATA);if(r.kind==GT_STREAM_DATA)received+=r.length;
    struct timespec t={0,1000000};nanosleep(&t,NULL);
  }
  CHECK(received==marker+sizeof(payload) && !memcmp(bytes,"provider-ready",marker) && !memcmp(bytes+marker,payload,sizeof(payload)));
  stream.close(stream.context);cleanup();CHECK(state.requested==1 && !state.cancelled);CHECK(!unlink(script) && !unlink(known) && !rmdir(directory));
}
static void invalid_supply(void){fixture f={0};open_provider(&f);gt_status_t e;CHECK(step(&f,true,1,&e)==GT_SESSION_LINK_WAIT);ssh(1);gt_ssh_connector_config_t c=resolved();c.family=3;CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_ERR_INVALID_ARG && owned && info().phase==GT_SSH_PROVIDER_REQUESTED);c.family=4;CHECK(gt__ssh_session_provider_supply(active,f.request,&owned,&c)==GT_OK && !owned);finish(&f);}
int main(int argc,char **argv){if(argc==6){real_flow(argc,argv);puts("PASS real SSH provider path copies, callbacks and binary stream");return 0;}CHECK(argc==2);if(!strcmp(argv[1],"unsupported")){CHECK(gt__ssh_session_provider_open(NULL,NULL,&active)==GT_ERR_UNSUPPORTED && !active);}
  else if(!strcmp(argv[1],"requests"))requests();else if(!strcmp(argv[1],"stale"))stale();else if(!strcmp(argv[1],"deadline"))deadline();else if(!strcmp(argv[1],"budget"))budget(false);else if(!strcmp(argv[1],"cancel"))budget(true);else if(!strcmp(argv[1],"rejection"))rejection();else if(!strcmp(argv[1],"admission"))admission();else if(!strcmp(argv[1],"clock"))clock_error();else if(!strcmp(argv[1],"reentry"))reentry();else{CHECK(!strcmp(argv[1],"invalid"));invalid_supply();}
  cleanup();printf("PASS SSH session provider %s\n",argv[1]);return 0;}
