#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL public session:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static gt_session_sink_result_t snapshot(void *c,uint64_t g,uint64_t s,uint32_t f,const uint8_t *p,size_t n,bool ro,gt_status_t *e){(void)c;(void)g;(void)s;(void)f;(void)p;(void)n;(void)ro;*e=GT_ERR_UNSUPPORTED;return GT_SESSION_SINK_FAILED;}
static gt_session_sink_result_t event(void *c,uint64_t g,uint64_t s,const gt_session_event_t *v,gt_status_t *e){(void)c;(void)g;(void)s;(void)v;*e=GT_ERR_UNSUPPORTED;return GT_SESSION_SINK_FAILED;}
static gt_status_t applied(void *c,gt_session_applied_t *a){(void)c;memset(a,0,sizeof(*a));return GT_OK;}
static void invalidated(void *c,uint64_t g,gt_status_t e){(void)c;(void)g;(void)e;CHECK(false);}
static void receipt(void *c,const gt_session_receipt_t *r){(void)c;(void)r;CHECK(false);}
static bool clock_bad(void *c,uint64_t *out){(void)c;(void)out;return false;}
static gt_session_options_t options(gt_session_create_t *create){return (gt_session_options_t){.kind=GT_SESSION_LOCAL,.proxy="/ghosterm-fixture-absent-proxy",.daemon="/fixture/daemon",.runtime="/fixture/runtime",.compatibility={1},.create=create,.snapshot_format=7,.sink={NULL,snapshot,event,applied,invalidated,receipt}};}
int main(int argc,char **argv){
  CHECK(argc==2);gt_session_create_t create={.nonce={1},.process={.executable="/bin/sh",.cwd=argv[1],.cols=80,.rows=24}};gt_session_options_t good=options(&create),o=good;gt_session_t *s=(void *)1;
  CHECK(gt_session_open(NULL,&s)==GT_ERR_INVALID_ARG && !s);CHECK(gt_session_open(&good,NULL)==GT_ERR_INVALID_ARG);
#define BAD(field,value) do{o=good;o.field=value;s=(void *)1;CHECK(gt_session_open(&o,&s)==GT_ERR_INVALID_ARG && !s);}while(0)
  BAD(kind,(gt_session_kind_t)99);BAD(snapshot_format,0);BAD(snapshot_limit,64u*1024u*1024u+1);BAD(recovery_timeout_ms,600001);BAD(create,NULL);BAD(session,1);BAD(proxy,"relative");BAD(sink.snapshot,NULL);BAD(sink.event,NULL);BAD(sink.applied,NULL);BAD(sink.invalidated,NULL);BAD(sink.receipt,NULL);
#undef BAD
  o=good;o.clock=clock_bad;CHECK(gt_session_open(&o,&s)==GT_ERR_IO && !s);
  gt_session_info_t info;memset(&info,1,sizeof(info));CHECK(!gt_session_info(NULL,&info) && !info.session && !info.has_sequence && !info.has_exit_code);CHECK(gt_session_step(NULL)==GT_SESSION_STATE_FAILED && gt_session_close(NULL));gt_session_stamp_t stamp={1,1,1};CHECK(gt_session_input(NULL,1,0,"x",1,&stamp)==GT_SESSION_SUBMIT_INVALID && !stamp.command_id);
  CHECK(gt_session_open(&good,&s)==GT_OK && gt_session_info(s,&info) && info.state==GT_SESSION_STATE_STARTING && info.readonly);
  const uint8_t address[16]={127,0,0,1};gt_ssh_t *ssh=NULL;CHECK(gt_session_auth_supply(s,1,&ssh,4,address,0)==GT_ERR_UNSUPPORTED && !ssh);CHECK(gt_session_auth_reject(s,1,GT_ERR_AUTH)==GT_ERR_UNSUPPORTED && gt_session_auth_decide(s,1,GT_HOSTKEY_ACCEPT)==GT_ERR_UNSUPPORTED);
  CHECK(gt_session_input(s,1,0,"x",1,&stamp)==GT_SESSION_SUBMIT_LOCKED && !stamp.command_id);CHECK(gt_session_resize(s,1,0,(gt_session_size_t){80,24,0,0},&stamp)==GT_SESSION_SUBMIT_LOCKED);CHECK(gt_session_color(s,1,0,true,&stamp)==GT_SESSION_SUBMIT_LOCKED);
  for(unsigned i=0;i<8 && gt_session_step(s)!=GT_SESSION_STATE_FAILED;i++){}
  CHECK(gt_session_info(s,&info) && info.state==GT_SESSION_STATE_FAILED && info.error==GT_ERR_NOT_FOUND && !info.has_exit_code);CHECK(gt_session_close(s));
  CHECK(gt_session_open(&good,&s)==GT_OK);gt_session_cancel(s);gt_session_cancel(s);CHECK(gt_session_step(s)==GT_SESSION_STATE_CLOSED && gt_session_info(s,&info) && info.error==GT_ERR_CANCELLED);CHECK(gt_session_close(s));
  o=good;o.create=NULL;o.catalog_only=true;o.snapshot_format=0;o.sink=(gt_session_sink_t){0};CHECK(gt_session_open(&o,&s)==GT_OK);size_t count=99;CHECK(gt_session_catalog(s,NULL,0,&count)==GT_ERR_NOT_FOUND && !count);CHECK(gt_session_catalog_refresh(s)==GT_SESSION_SUBMIT_LOCKED && gt_session_terminate(s,0)==GT_SESSION_SUBMIT_INVALID && gt_session_terminate(s,5)==GT_SESSION_SUBMIT_LOCKED);gt_session_cancel(s);CHECK(gt_session_close(s));
  o=good;o.create=NULL;o.pending_nonce[0]=9;CHECK(gt_session_open(&o,&s)==GT_ERR_INVALID_ARG && !s);o.instance[0]=1;CHECK(gt_session_open(&o,&s)==GT_OK && gt_session_info(s,&info) && info.has_admitted_create && !info.session);CHECK(gt_session_catalog(s,NULL,0,&count)==GT_ERR_UNSUPPORTED);gt_session_cancel(s);CHECK(gt_session_close(s));o.catalog_only=true;CHECK(gt_session_open(&o,&s)==GT_ERR_INVALID_ARG && !s);
  const char *args[34];for(unsigned i=0;i<33;i++)args[i]="arg";args[33]=NULL;create.process.argv=args;CHECK(gt_session_open(&good,&s)==GT_ERR_INVALID_ARG && !s);
  puts("PASS public session validation, unknown results, unavailable capabilities, lifecycle and fresh creation limits");return 0;
}
