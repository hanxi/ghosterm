/* Actual Session + real socket/SSH fixtures. Private class fields are visible
 * only to drive counter/window boundary cases and to script hostile peers. */
#define GT_BRIDGE_SESSION_TEST_MAIN rpc_session_fixture_main
#include "test_bridge_session.c"
#include "../src/extension/rpc.c"
typedef struct {unsigned input,output,effects;} rpc_fixture_work_t;
typedef struct {session_pair_t pair;rpc_fixture_work_t host,executor;gt_bridge_lease_t hlease,elease;} rpc_fixture_t;
typedef struct {rpc_fixture_t *fixture;bool jumped;} rpc_selection_clock_t;
static bool rpc_selection_clock(void *ctx,uint64_t *out){rpc_selection_clock_t *c=ctx;gt_bridge_rpc_t *r=c->fixture->pair.h->rpc;if(!c->jumped && r->prepared==RPC_TX_REQUEST && r->inflight==RPC_TX_NONE){c->fixture->pair.clock.now=r->out[r->selected].deadline;c->jumped=true;}*out=c->fixture->pair.clock.now;return true;}
static bool rpc_fixture_input(void *ctx,const uint8_t *p,size_t n){rpc_fixture_work_t *w=ctx;w->input++;return n && !(n==4 && !memcmp(p,"null",4));}
static bool rpc_fixture_output(void *ctx,const uint8_t *p,size_t n){rpc_fixture_work_t *w=ctx;w->output++;return n && !(n==4 && !memcmp(p,"null",4));}
static bool rpc_fixture_reject(void *ctx,const uint8_t *p,size_t n){(void)p;(void)n;((rpc_fixture_work_t *)ctx)->input++;return false;}
static void rpc_fixture_config(gt_bridge_session_config_t *c,rpc_fixture_work_t *work) {
  c->rpc_enabled=true;c->rpc_method_count=3;
  c->available_count=4;strcpy(c->available[3],"process.spawn");
  const char *names[]={"fixture.echo","fixture.ask","fixture.denied"};
  for(size_t i=0;i<3;i++){gt_bridge_rpc_method_t *m=&c->rpc_methods[i];strcpy(m->method,names[i]);strcpy(m->capability,i==2?"process.spawn":"ui.form");m->receiver=i==1?GT_BRIDGE_HOST:GT_BRIDGE_EXECUTOR;m->input=rpc_fixture_input;m->output=rpc_fixture_output;m->context=work;}
}
static void rpc_fixture_open(rpc_fixture_t *f,const char *mode) {
  memset(f,0,sizeof(*f));f->pair.clock.now=1000;int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));gt_bridge_stream_t *a=NULL,*b=NULL;
  CHECK(gt__bridge_fd_open(fd[0],fd[0],NULL,&a)==GT_OK && gt__bridge_fd_open(fd[1],fd[1],NULL,&b)==GT_OK);
  gt_bridge_session_config_t h=session_config(&f->pair.clock),e=h;rpc_fixture_config(&h,&f->host);rpc_fixture_config(&e,&f->executor);
  if(mode && !strcmp(mode,"unknown"))e.rpc_method_count=0;
  if(mode && !strcmp(mode,"denied"))strcpy(e.rpc_methods[0].capability,"process.spawn");
  if(mode && !strcmp(mode,"missing-cap"))strcpy(e.rpc_methods[0].capability,"network.connect");
  if(mode && !strcmp(mode,"remote-invalid"))e.rpc_methods[0].input=rpc_fixture_reject;
  if(mode && !strcmp(mode,"feature")){e.rpc_enabled=false;e.rpc_method_count=0;}
  gt_allocator_t allocator={&f->pair.budget,session_allocate,session_release};
  CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&h,&allocator,&f->pair.h)==GT_OK && !a);
  CHECK(gt__bridge_session_open(&b,GT_BRIDGE_EXECUTOR,&e,&allocator,&f->pair.e)==GT_OK && !b);
  if(mode && !strcmp(mode,"feature"))return;
  session_consent(&f->pair);uint64_t handle=99;gt_bridge_lease_t none={0};
  CHECK(gt__bridge_session_request(f->pair.h,&none,"fixture.echo","true",4,1000,&handle)==GT_BRIDGE_RPC_CLOSED && !handle);
  session_allow(&f->pair);session_active(&f->pair);
  CHECK(gt__bridge_session_lease(f->pair.h,&f->hlease) && gt__bridge_session_lease(f->pair.e,&f->elease));
}
static void rpc_fixture_pump(rpc_fixture_t *f,unsigned n){for(unsigned i=0;i<n;i++){gt__bridge_session_step(f->pair.h,0);gt__bridge_session_step(f->pair.e,0);CHECK(session_info(f->pair.h).end==GT_BRIDGE_END_NONE && session_info(f->pair.e).end==GT_BRIDGE_END_NONE);}}
static gt_bridge_rpc_result_t rpc_fixture_result(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,uint64_t handle){gt_bridge_rpc_result_t out;CHECK(gt__bridge_session_request_result(s,lease,handle,&out));return out;}
static uint64_t rpc_fixture_start(rpc_fixture_t *f,bool reverse,const char *input,uint32_t timeout) {
  uint64_t handle=0;CHECK(gt__bridge_session_request(reverse?f->pair.e:f->pair.h,reverse?&f->elease:&f->hlease,reverse?"fixture.ask":"fixture.echo",input,strlen(input),timeout,&handle)==GT_BRIDGE_RPC_OK && handle);return handle;
}
static gt_bridge_rpc_request_t rpc_fixture_take(rpc_fixture_t *f,bool reverse) {
  gt_bridge_rpc_request_t q;
  for(unsigned i=0;i<1000;i++){rpc_fixture_pump(f,1);gt_bridge_rpc_status_t status=gt__bridge_session_take_request(reverse?f->pair.h:f->pair.e,reverse?&f->hlease:&f->elease,&q);CHECK(status==GT_BRIDGE_RPC_OK || status==GT_BRIDGE_RPC_WAIT);if(status==GT_BRIDGE_RPC_OK)return q;}
  CHECK(false);return q;
}
static void rpc_fixture_reply(rpc_fixture_t *f,bool reverse,const gt_bridge_rpc_request_t *q) {
  gt_bridge_session_t *s=reverse?f->pair.h:f->pair.e;const gt_bridge_lease_t *lease=reverse?&f->hlease:&f->elease;
  CHECK(gt__bridge_session_check_request(s,lease,q->ticket)==GT_BRIDGE_RPC_OK);
  (reverse?&f->host:&f->executor)->effects++;
  char outcome[GT_BRIDGE_RPC_BODY_MAX+1];int n=snprintf(outcome,sizeof(outcome),"{\"kind\":\"success\",\"value\":%.*s}",(int)q->input_size,q->input);CHECK(n>0 && (size_t)n<sizeof(outcome));
  CHECK(gt__bridge_session_respond(s,lease,q->ticket,outcome,(size_t)n)==GT_BRIDGE_RPC_OK);
}
static gt_bridge_rpc_result_t rpc_fixture_complete(rpc_fixture_t *f,bool reverse,uint64_t handle) {
  for(unsigned i=0;i<1000;i++){rpc_fixture_pump(f,1);gt_bridge_rpc_result_t out=rpc_fixture_result(reverse?f->pair.e:f->pair.h,reverse?&f->elease:&f->hlease,handle);if(out.confirmed)return out;}CHECK(false);return (gt_bridge_rpc_result_t){0};
}
static void rpc_fixture_inject(gt_bridge_session_t *sender,unsigned flow,uint64_t seq,const char *kind,const char *payload,bool debit) {
  gt_bridge_rpc_t *r=sender->rpc;char frame[4097];size_t n;CHECK(rpc_frame(r,rpc_streams[flow],seq,kind,payload,frame,sizeof(frame),&n));
  CHECK(!gt__bridge_writer_pending(sender->writer));if(debit){CHECK(r->tx_credit[flow]>=n+4);r->tx_credit[flow]-=(uint32_t)(n+4);}
  uint8_t bytes[4100];bytes[0]=(uint8_t)(n>>24);bytes[1]=(uint8_t)(n>>16);bytes[2]=(uint8_t)(n>>8);bytes[3]=(uint8_t)n;memcpy(bytes+4,frame,n);
  size_t at=0;while(at<n+4){gt_bridge_io_t io=gt__bridge_stream_write(sender->stream,bytes+at,n+4-at,0);CHECK(io.state==GT_BRIDGE_DATA);at+=io.count;}
}
static void rpc_fixture_request_packet(rpc_fixture_t *f,const char *id,uint64_t seq,const char *input,bool debit) {
  char payload[4097];CHECK(snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\",\"method\":\"fixture.echo\",\"input\":%s,\"timeoutMs\":60000}",id,input)>0);rpc_fixture_inject(f->pair.h,0,seq,"request",payload,debit);
}
static void rpc_fixture_normal(const char *mode) {
  rpc_fixture_t f;rpc_fixture_open(&f,mode);rpc_fixture_pump(&f,20);
  if(!strcmp(mode,"feature"))CHECK(false);
  if(!strcmp(mode,"full")) {
    uint64_t handles[64];for(size_t i=0;i<64;i++)handles[i]=rpc_fixture_start(&f,false,"true",60000);
    uint64_t extra=1;CHECK(gt__bridge_session_request(f.pair.h,&f.hlease,"fixture.echo","true",4,60000,&extra)==GT_BRIDGE_RPC_WAIT && !extra);
    for(size_t i=0;i<64;i++){CHECK(gt__bridge_session_cancel_request(f.pair.h,&f.hlease,handles[i])==GT_BRIDGE_RPC_OK);gt_bridge_rpc_result_t o=rpc_fixture_result(f.pair.h,&f.hlease,handles[i]);CHECK(o.state==GT_BRIDGE_RPC_CANCELLED && !o.request_id[0] && !o.confirmed);CHECK(gt__bridge_session_forget_request(f.pair.h,&f.hlease,handles[i])==GT_BRIDGE_RPC_OK);}
    uint64_t handle=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,handle).state==GT_BRIDGE_RPC_SUCCEEDED && f.executor.effects==1);
  } else if(!strcmp(mode,"unknown") || !strcmp(mode,"denied") || !strcmp(mode,"missing-cap") || !strcmp(mode,"remote-invalid")) {
    uint64_t handle=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_result_t o=rpc_fixture_complete(&f,false,handle);CHECK(o.state==GT_BRIDGE_RPC_FAILED && strstr((const char *)o.outcome,!strcmp(mode,"remote-invalid")?"INVALID_INPUT":!strcmp(mode,"denied")?"DENIED":"UNSUPPORTED") && !f.executor.effects);
    gt_bridge_rpc_request_t q;CHECK(gt__bridge_session_take_request(f.pair.e,&f.elease,&q)==GT_BRIDGE_RPC_WAIT);
  } else if(!strcmp(mode,"queued-cancel") || !strcmp(mode,"queued-timeout")) {
    uint64_t handle=rpc_fixture_start(&f,false,"true",20);
    if(!strcmp(mode,"queued-cancel"))CHECK(gt__bridge_session_cancel_request(f.pair.h,&f.hlease,handle)==GT_BRIDGE_RPC_OK);else f.pair.clock.now+=21;
    gt_bridge_rpc_result_t o=rpc_fixture_result(f.pair.h,&f.hlease,handle);CHECK(o.state==(!strcmp(mode,"queued-cancel")?GT_BRIDGE_RPC_CANCELLED:GT_BRIDGE_RPC_FAILED) && !o.request_id[0] && !o.confirmed);rpc_fixture_pump(&f,10);CHECK(f.pair.e->rpc->rx_ordinal==0);
  } else if(!strcmp(mode,"validation")) {
    uint64_t handle=0;CHECK(gt__bridge_session_request(f.pair.h,&f.hlease,"fixture.echo","null",4,1000,&handle)==GT_BRIDGE_RPC_INVALID && !handle);
    CHECK(gt__bridge_session_request(f.pair.h,&f.hlease,"fixture.echo","true\0false",10,1000,&handle)==GT_BRIDGE_RPC_INVALID && !handle);
    CHECK(gt__bridge_session_request(f.pair.h,&f.hlease,"fixture.denied","true",4,1000,&handle)==GT_BRIDGE_RPC_DENIED && !handle);
    handle=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);
    CHECK(gt__bridge_session_respond(f.pair.e,&f.elease,q.ticket,"{\"kind\":\"success\",\"value\":null}",31)==GT_BRIDGE_RPC_INVALID);
    rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,handle).state==GT_BRIDGE_RPC_SUCCEEDED && f.executor.effects==1);
  } else if(!strcmp(mode,"scope")) {
    uint64_t handle=rpc_fixture_start(&f,false,"true",60000);gt_bridge_lease_t wrong=f.hlease;wrong.epoch++;gt_bridge_rpc_result_t o;
    CHECK(gt__bridge_session_cancel_request(f.pair.h,&wrong,handle)==GT_BRIDGE_RPC_DENIED && gt__bridge_session_forget_request(f.pair.h,&wrong,handle)==GT_BRIDGE_RPC_DENIED && !gt__bridge_session_request_result(f.pair.h,&wrong,handle,&o));
    gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);CHECK(gt__bridge_session_respond(f.pair.e,&f.hlease,q.ticket,"{\"kind\":\"success\",\"value\":true}",31)==GT_BRIDGE_RPC_DENIED);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,handle).confirmed);
  } else if(!strcmp(mode,"cancel") || !strcmp(mode,"late-result")) {
    uint64_t handle=rpc_fixture_start(&f,false,"true",20);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);
    CHECK(gt__bridge_session_check_request(f.pair.e,&f.elease,q.ticket)==GT_BRIDGE_RPC_OK);f.executor.effects++;
    if(!strcmp(mode,"cancel")){CHECK(gt__bridge_session_cancel_request(f.pair.h,&f.hlease,handle)==GT_BRIDGE_RPC_OK);gt_bridge_rpc_result_t before=rpc_fixture_result(f.pair.h,&f.hlease,handle);CHECK(before.state==GT_BRIDGE_RPC_PENDING && !before.confirmed);}else{f.pair.clock.now+=21;CHECK(rpc_fixture_result(f.pair.h,&f.hlease,handle).state==GT_BRIDGE_RPC_UNKNOWN);}
    rpc_fixture_pump(&f,10);CHECK(gt__bridge_session_check_request(f.pair.e,&f.elease,q.ticket)==GT_BRIDGE_RPC_CLOSED);
    const char *answer=!strcmp(mode,"cancel")?"{\"kind\":\"cancelled\",\"effect\":\"possible\"}":"{\"kind\":\"success\",\"value\":true}";
    CHECK(gt__bridge_session_respond(f.pair.e,&f.elease,q.ticket,answer,strlen(answer))==GT_BRIDGE_RPC_OK);gt_bridge_rpc_result_t o=rpc_fixture_complete(&f,false,handle);CHECK(o.state==(!strcmp(mode,"cancel")?GT_BRIDGE_RPC_CANCELLED:GT_BRIDGE_RPC_SUCCEEDED) && f.executor.effects==1);
    CHECK(gt__bridge_session_cancel_request(f.pair.h,&f.hlease,handle)==GT_BRIDGE_RPC_OK && rpc_fixture_result(f.pair.h,&f.hlease,handle).state==o.state);
  } else if(!strcmp(mode,"lost") || !strcmp(mode,"stop")) {
    uint64_t known=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,known).confirmed);
    uint64_t pending=rpc_fixture_start(&f,false,"true",60000);q=rpc_fixture_take(&f,false);uint64_t queued=0;
    if(!strcmp(mode,"stop")){queued=rpc_fixture_start(&f,false,"true",60000);gt__bridge_session_stop(f.pair.h);}else{CHECK(gt__bridge_session_close(f.pair.e,0)==GT_BRIDGE_RELEASED);for(unsigned i=0;i<10 && session_info(f.pair.h).end==GT_BRIDGE_END_NONE;i++)gt__bridge_session_step(f.pair.h,0);CHECK(session_info(f.pair.h).end==GT_BRIDGE_END_LOST);}
    CHECK(rpc_fixture_result(f.pair.h,&f.hlease,known).state==GT_BRIDGE_RPC_SUCCEEDED && rpc_fixture_result(f.pair.h,&f.hlease,pending).state==GT_BRIDGE_RPC_UNKNOWN);
    if(queued)CHECK(rpc_fixture_result(f.pair.h,&f.hlease,queued).state==GT_BRIDGE_RPC_CANCELLED);
    CHECK(gt__bridge_session_check_request(f.pair.e,&f.elease,q.ticket)!=GT_BRIDGE_RPC_OK || !strcmp(mode,"stop"));
  } else if(!strcmp(mode,"duplicate") || !strcmp(mode,"conflict") || !strcmp(mode,"retired")) {
    char first[33]={0};unsigned rounds=!strcmp(mode,"retired")?70:1;
    for(unsigned i=0;i<rounds;i++){uint64_t h=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);if(!i)memcpy(first,q.request_id,33);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,h).confirmed);CHECK(gt__bridge_session_forget_request(f.pair.h,&f.hlease,h)==GT_BRIDGE_RPC_OK);rpc_fixture_pump(&f,5);}
    unsigned validated=f.executor.input;
    rpc_fixture_request_packet(&f,first,0,!strcmp(mode,"conflict")?"false":"true",true);
    if(!strcmp(mode,"conflict")){for(unsigned i=0;i<10 && session_info(f.pair.e).end==GT_BRIDGE_END_NONE;i++)gt__bridge_session_step(f.pair.e,0);CHECK(session_info(f.pair.e).end==GT_BRIDGE_END_PROTOCOL);}
    else{rpc_fixture_pump(&f,20);gt_bridge_rpc_request_t q;CHECK(gt__bridge_session_take_request(f.pair.e,&f.elease,&q)==GT_BRIDGE_RPC_WAIT && f.executor.input==validated && f.executor.effects==rounds);uint64_t h=rpc_fixture_start(&f,false,"true",60000);q=rpc_fixture_take(&f,false);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,h).confirmed && f.executor.effects==rounds+1);}
  } else {
    uint64_t h=rpc_fixture_start(&f,false,"{\"value\":1}",60000),e=rpc_fixture_start(&f,true,"{\"value\":2}",60000);
    gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);rpc_fixture_reply(&f,false,&q);q=rpc_fixture_take(&f,true);rpc_fixture_reply(&f,true,&q);
    CHECK(rpc_fixture_complete(&f,false,h).state==GT_BRIDGE_RPC_SUCCEEDED && rpc_fixture_complete(&f,true,e).state==GT_BRIDGE_RPC_SUCCEEDED && f.host.effects==1 && f.executor.effects==1);
  }
  session_pair_close(&f.pair);
}
static void rpc_fixture_feature(void){rpc_fixture_t f;rpc_fixture_open(&f,"feature");for(unsigned i=0;i<20 && session_info(f.pair.e).end==GT_BRIDGE_END_NONE;i++){gt__bridge_session_step(f.pair.h,0);gt__bridge_session_step(f.pair.e,0);}CHECK(session_info(f.pair.e).end==GT_BRIDGE_END_INCOMPATIBLE && !f.executor.effects);session_pair_close(&f.pair);}
static void rpc_fixture_flow(const char *mode) {
  rpc_fixture_t f;rpc_fixture_open(&f,NULL);rpc_fixture_pump(&f,20);
  if(!strcmp(mode,"allocation")) {
    session_pair_close(&f.pair);int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));gt_bridge_stream_t *a=NULL,*b=NULL;CHECK(gt__bridge_fd_open(fd[0],fd[0],NULL,&a)==GT_OK && gt__bridge_fd_open(fd[1],fd[1],NULL,&b)==GT_OK);
    session_clock_t clock={1000,false};gt_bridge_session_config_t cfg=session_config(&clock);rpc_fixture_config(&cfg,&f.host);session_budget_t budget={0};gt_allocator_t alloc={&budget,session_allocate,session_release};gt_bridge_session_t *s=NULL;
    for(size_t i=1;i<=7;i++){budget.calls=0;budget.fail=i;CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&cfg,&alloc,&s)==GT_ERR_IO && a && !s && !budget.live && !atomic_load(&a->stopping));}
    CHECK(gt__bridge_stream_close(a,0)==GT_BRIDGE_RELEASED && gt__bridge_stream_destroy(a)==GT_OK && gt__bridge_stream_close(b,0)==GT_BRIDGE_RELEASED && gt__bridge_stream_destroy(b)==GT_OK);return;
  }
  if(!strcmp(mode,"held-credit")) {
    uint64_t h[64];for(size_t i=0;i<64;i++)h[i]=rpc_fixture_start(&f,false,"true",60000);rpc_fixture_pump(&f,200);unsigned committed=0;for(size_t i=0;i<64;i++)committed+=rpc_fixture_result(f.pair.h,&f.hlease,h[i]).request_id[0]!=0;CHECK(committed && committed<64 && !f.executor.effects);
  } else if(!strcmp(mode,"incoming-timeout")) {
    uint64_t h=rpc_fixture_start(&f,false,"true",20);rpc_fixture_pump(&f,10);f.pair.clock.now+=21;gt_bridge_rpc_request_t q;CHECK(gt__bridge_session_take_request(f.pair.e,&f.elease,&q)==GT_BRIDGE_RPC_WAIT && !f.executor.effects);gt_bridge_rpc_result_t out=rpc_fixture_complete(&f,false,h);CHECK(out.state==GT_BRIDGE_RPC_FAILED && strstr((const char *)out.outcome,"EXPIRED"));
  } else if(!strcmp(mode,"credit")) {
    char large[2600];large[0]='"';memset(large+1,'x',2500);large[2501]='"';large[2502]=0;uint64_t h[16],e[16];
    for(size_t i=0;i<16;i++){h[i]=rpc_fixture_start(&f,false,large,60000);e[i]=rpc_fixture_start(&f,true,large,60000);}
    for(unsigned i=0;i<60;i++)gt__bridge_session_step(f.pair.h,0);
    size_t sent=0;for(size_t i=0;i<16;i++)if(rpc_fixture_result(f.pair.h,&f.hlease,h[i]).request_id[0])sent++;
    CHECK(sent && sent<16 && f.pair.h->rpc->tx_credit[0]<3000);
    unsigned done=0;
    for(unsigned i=0;i<5000 && done<32;i++) {
      rpc_fixture_pump(&f,1);gt_bridge_rpc_request_t q;
      if(gt__bridge_session_take_request(f.pair.e,&f.elease,&q)==GT_BRIDGE_RPC_OK)rpc_fixture_reply(&f,false,&q);
      if(gt__bridge_session_take_request(f.pair.h,&f.hlease,&q)==GT_BRIDGE_RPC_OK)rpc_fixture_reply(&f,true,&q);
      done=0;for(size_t n=0;n<16;n++){done+=rpc_fixture_result(f.pair.h,&f.hlease,h[n]).confirmed;done+=rpc_fixture_result(f.pair.e,&f.elease,e[n]).confirmed;}
    }
    CHECK(done==32 && f.host.effects==16 && f.executor.effects==16);
  } else if(!strcmp(mode,"pending-duplicate")) {
    uint64_t h=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);char id[33];memcpy(id,q.request_id,33);unsigned count=f.executor.input;
    rpc_fixture_request_packet(&f,id,0,"true",true);rpc_fixture_pump(&f,10);gt_bridge_rpc_request_t duplicate;CHECK(gt__bridge_session_take_request(f.pair.e,&f.elease,&duplicate)==GT_BRIDGE_RPC_WAIT && f.executor.input==count);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,h).confirmed && f.executor.effects==1);
  } else if(!strcmp(mode,"cancel-before-take")) {
    uint64_t h=rpc_fixture_start(&f,false,"true",60000);for(unsigned i=0;i<20;i++)rpc_fixture_pump(&f,1);CHECK(gt__bridge_session_cancel_request(f.pair.h,&f.hlease,h)==GT_BRIDGE_RPC_OK);gt_bridge_rpc_result_t out=rpc_fixture_complete(&f,false,h);CHECK(out.state==GT_BRIDGE_RPC_CANCELLED && strstr((const char *)out.outcome,"none") && !f.executor.effects);
  } else if(!strcmp(mode,"cancel-no-credit")) {
    uint64_t h=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);uint32_t credit=f.pair.e->rpc->tx_credit[1];f.pair.e->rpc->tx_credit[1]=0;f.pair.h->rpc->tx_credit[0]=0;
    CHECK(gt__bridge_session_cancel_request(f.pair.h,&f.hlease,h)==GT_BRIDGE_RPC_OK);rpc_fixture_pump(&f,10);CHECK(gt__bridge_session_check_request(f.pair.e,&f.elease,q.ticket)==GT_BRIDGE_RPC_CLOSED);
    const char *outcome="{\"kind\":\"cancelled\",\"effect\":\"possible\"}";CHECK(gt__bridge_session_respond(f.pair.e,&f.elease,q.ticket,outcome,strlen(outcome))==GT_BRIDGE_RPC_OK);f.pair.e->rpc->tx_credit[1]=credit;CHECK(rpc_fixture_complete(&f,false,h).state==GT_BRIDGE_RPC_CANCELLED);
  } else if(!strcmp(mode,"credit-overflow")) {
    gt_bridge_message_t header={.epoch=1};gt_bridge_session_info_t i=session_info(f.pair.h);memcpy(header.connection,i.connection,33);memcpy(header.instance,i.instance,33);
    session_send(f.pair.h->stream,&header,f.pair.h->tx_sequence,"credit","{\"bytes\":8193,\"streamId\":\"rpc.request\"}",false);
    for(unsigned n=0;n<10 && session_info(f.pair.e).end==GT_BRIDGE_END_NONE;n++)gt__bridge_session_step(f.pair.e,0);CHECK(session_info(f.pair.e).end==GT_BRIDGE_END_PROTOCOL && !f.executor.effects);
  } else if(!strcmp(mode,"overspend") || !strcmp(mode,"gap")) {
    char id[33];rpc_id(f.pair.h->rpc,1,id);if(!strcmp(mode,"overspend"))f.pair.e->rpc->rx_credit[0]=0;
    rpc_fixture_request_packet(&f,id,!strcmp(mode,"gap")?1:0,"true",false);
    for(unsigned n=0;n<10 && session_info(f.pair.e).end==GT_BRIDGE_END_NONE;n++)gt__bridge_session_step(f.pair.e,0);CHECK(session_info(f.pair.e).end==GT_BRIDGE_END_PROTOCOL && !f.executor.effects);
  } else if(!strcmp(mode,"response-conflict")) {
    uint64_t h=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);char id[33];memcpy(id,q.request_id,33);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,h).state==GT_BRIDGE_RPC_SUCCEEDED);rpc_fixture_pump(&f,10);
    char payload[400];CHECK(snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\",\"outcome\":{\"kind\":\"unknown\",\"reason\":\"contradiction\"}}",id)>0);rpc_fixture_inject(f.pair.e,1,0,"response",payload,true);
    for(unsigned n=0;n<10 && session_info(f.pair.h).end==GT_BRIDGE_END_NONE;n++)gt__bridge_session_step(f.pair.h,0);CHECK(session_info(f.pair.h).end==GT_BRIDGE_END_PROTOCOL && rpc_fixture_result(f.pair.h,&f.hlease,h).state==GT_BRIDGE_RPC_SUCCEEDED);
  } else if(!strcmp(mode,"select-expiry")) {
    rpc_selection_clock_t clock={&f,false};f.pair.h->config.clock=rpc_selection_clock;f.pair.h->config.clock_context=&clock;uint64_t h=rpc_fixture_start(&f,false,"true",20);gt__bridge_session_step(f.pair.h,0);
    gt_bridge_rpc_result_t out=rpc_fixture_result(f.pair.h,&f.hlease,h);CHECK(clock.jumped && out.state==GT_BRIDGE_RPC_FAILED && !out.request_id[0]);rpc_fixture_pump(&f,10);CHECK(f.pair.e->rpc->rx_ordinal==0 && !f.executor.effects);
    session_pair_close(&f.pair);return;
  } else if(!strcmp(mode,"counter")) {
    f.pair.h->rpc->tx_ordinal=UINT64_MAX;uint64_t h=123;CHECK(gt__bridge_session_request(f.pair.h,&f.hlease,"fixture.echo","true",4,1000,&h)==GT_BRIDGE_RPC_UNSUPPORTED && !h);f.pair.h->rpc->tx_ordinal=0;
    f.pair.h->rpc->tx_seq[0]=UINT64_MAX;f.pair.e->rpc->rx_seq[0]=UINT64_MAX;
    h=rpc_fixture_start(&f,false,"true",60000);gt_bridge_rpc_request_t q=rpc_fixture_take(&f,false);rpc_fixture_reply(&f,false,&q);CHECK(rpc_fixture_complete(&f,false,h).confirmed && f.pair.h->rpc->tx_exhausted[0] && f.pair.e->rpc->rx_exhausted[0]);
    rpc_fixture_pump(&f,10);h=rpc_fixture_start(&f,false,"true",60000);for(unsigned i=0;i<10 && session_info(f.pair.h).end==GT_BRIDGE_END_NONE;i++)gt__bridge_session_step(f.pair.h,0);CHECK(session_info(f.pair.h).end!=GT_BRIDGE_END_NONE && f.executor.effects==1);
  }
  session_pair_close(&f.pair);
}
static void rpc_fixture_executor(void) {
  CHECK(!isatty(0) && !isatty(1));gt_bridge_stream_t *stream=NULL;CHECK(gt__bridge_fd_open(0,1,NULL,&stream)==GT_OK);rpc_fixture_work_t work={0};gt_bridge_session_config_t cfg=session_config(NULL);rpc_fixture_config(&cfg,&work);strcpy(cfg.peer.identity,"fixture-SSH-RPC");gt_bridge_session_t *s=NULL;CHECK(gt__bridge_session_open(&stream,GT_BRIDGE_EXECUTOR,&cfg,NULL,&s)==GT_OK);
  uint64_t end=session_now()+20000;
  while(gt__bridge_session_step(s,25)<GT_BRIDGE_SESSION_ENDED) {
    gt_bridge_lease_t lease;if(gt__bridge_session_lease(s,&lease)){gt_bridge_rpc_request_t q;if(gt__bridge_session_take_request(s,&lease,&q)==GT_BRIDGE_RPC_OK){CHECK(gt__bridge_session_check_request(s,&lease,q.ticket)==GT_BRIDGE_RPC_OK);work.effects++;char answer[GT_BRIDGE_RPC_BODY_MAX+1];int n=snprintf(answer,sizeof(answer),"{\"kind\":\"success\",\"value\":%.*s}",(int)q.input_size,q.input);CHECK(n>0 && (size_t)n<sizeof(answer) && gt__bridge_session_respond(s,&lease,q.ticket,answer,(size_t)n)==GT_BRIDGE_RPC_OK);}}
    CHECK(session_now()<end);
  }
  CHECK(work.effects==0 || work.effects==80);session_dispose(s);
}
#ifdef GT_HAVE_LIBSSH2
static void rpc_fixture_ssh(int argc,char **argv) {
  CHECK(argc>=8);size_t count=(size_t)atoi(argv[2]);CHECK(count>=2 && count<=3 && argc==(int)(6+count));session_port=(uint16_t)atoi(argv[6]);session_dials=0;uint8_t prefix[4];CHECK(exact_read(0,prefix,4));size_t n=((size_t)prefix[0]<<24)|((size_t)prefix[1]<<16)|((size_t)prefix[2]<<8)|prefix[3];CHECK(n && n<8192);private_key=calloc(1,n+1);CHECK(private_key && exact_read(0,private_key,n));
  gt_ssh_t *material[3]={0};gt_bridge_route_hop_t hops[3]={0};
  for(size_t i=0;i<count;i++){char identity[64];CHECK(snprintf(identity,sizeof(identity),"route-hop-%zu.invalid",i)>0);gt_ssh_opts_t o={.host=identity,.port=(uint16_t)atoi(argv[6+i]),.username=argv[3],.known_hosts_path=argv[4]};material[i]=gt_ssh_new(&o);CHECK(material[i] && gt_ssh_auth_private_key(material[i],private_key,NULL)==GT_OK);hops[i].connector=(gt_ssh_connector_config_t){.family=i?0:4,.address={127,0,0,1},.timeout_ms=5000,.clock=session_clock};if(i)hops[i].via_host="127.0.0.1";}
  gt_bridge_stream_t *route=NULL;CHECK(gt__bridge_route_open(material,hops,count,argv[5],NULL,NULL,&route)==GT_OK);uint64_t end=session_now()+20000;
  while(gt__bridge_route_step(route,25)!=GT_BRIDGE_ROUTE_READY)CHECK(session_now()<end);
  gt_bridge_route_info_t ri;CHECK(gt__bridge_route_info(route,&ri));rpc_fixture_work_t work={0};gt_bridge_session_config_t cfg=session_config(NULL);rpc_fixture_config(&cfg,&work);cfg.peer.kind=GT_BRIDGE_PEER_SSH;strcpy(cfg.peer.identity,ri.authentication[count-1].fingerprint);gt_bridge_session_t *s=NULL;CHECK(gt__bridge_session_open(&route,GT_BRIDGE_HOST,&cfg,NULL,&s)==GT_OK && !route);bool decided=false;unsigned completed=0;uint64_t pending=0;
  for(;;) {
    gt_bridge_session_phase_t phase=gt__bridge_session_step(s,25);gt_bridge_session_info_t info=session_info(s);
    if(phase==GT_BRIDGE_SESSION_CONSENT && !decided){CHECK(gt__bridge_session_decide(s,info.consent_token,!strcmp(argv[1],"ssh-deny")?GT_BRIDGE_DECISION_DENY:GT_BRIDGE_DECISION_ALLOW)==GT_OK);decided=true;}
    if(phase==GT_BRIDGE_SESSION_ACTIVE){gt_bridge_lease_t lease;CHECK(gt__bridge_session_lease(s,&lease));if(pending){gt_bridge_rpc_result_t out;CHECK(gt__bridge_session_request_result(s,&lease,pending,&out));if(out.confirmed){char expected[64];CHECK(snprintf(expected,sizeof(expected),"{\"n\":%u}",completed)>0);CHECK(out.state==GT_BRIDGE_RPC_SUCCEEDED && strstr((const char *)out.outcome,expected));CHECK(gt__bridge_session_forget_request(s,&lease,pending)==GT_BRIDGE_RPC_OK);pending=0;completed++;}}
      if(completed==80)gt__bridge_session_revoke(s);else if(!pending){char input[64];int length=snprintf(input,sizeof(input),"{\"n\":%u}",completed);CHECK(gt__bridge_session_request(s,&lease,"fixture.echo",input,(size_t)length,60000,&pending)==GT_BRIDGE_RPC_OK);}}
    if(phase==GT_BRIDGE_SESSION_ENDED)break;CHECK(info.end==GT_BRIDGE_END_NONE || info.end==GT_BRIDGE_END_DENIED || info.end==GT_BRIDGE_END_REVOKED);CHECK(session_now()<end);
  }
  CHECK(decided && session_dials==1 && completed==(!strcmp(argv[1],"ssh-deny")?0:80));session_dispose(s);gt__secure_erase(private_key,n);free(private_key);private_key=NULL;printf("PASS bridge_rpc_%s_%zu\n",argv[1],count);
}
#endif
int main(int argc,char **argv) {
  if(argc==2 && !strcmp(argv[1],"--executor")){rpc_fixture_executor();return 0;}
#ifdef GT_HAVE_LIBSSH2
  if(argc>2){rpc_fixture_ssh(argc,argv);return 0;}
#endif
  CHECK(argc==2);size_t fds=session_fds();if(!strcmp(argv[1],"feature"))rpc_fixture_feature();
  else if(!strcmp(argv[1],"allocation") || !strcmp(argv[1],"credit") || !strcmp(argv[1],"pending-duplicate") || !strcmp(argv[1],"cancel-before-take") || !strcmp(argv[1],"cancel-no-credit") || !strcmp(argv[1],"credit-overflow") || !strcmp(argv[1],"overspend") || !strcmp(argv[1],"gap") || !strcmp(argv[1],"response-conflict") || !strcmp(argv[1],"counter") || !strcmp(argv[1],"select-expiry") || !strcmp(argv[1],"held-credit") || !strcmp(argv[1],"incoming-timeout"))rpc_fixture_flow(argv[1]);
  else rpc_fixture_normal(argv[1]);CHECK(session_fds()==fds);printf("PASS bridge_rpc_%s\n",argv[1]);return 0;
}
