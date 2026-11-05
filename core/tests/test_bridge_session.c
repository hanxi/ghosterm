#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <sys/socket.h>
#ifdef GT_HAVE_LIBSSH2
int session_fixture_connect(int,const struct sockaddr *,socklen_t);
#define connect session_fixture_connect
#include "../src/ssh.c"
#undef connect
#endif
#define main session_stream_base_main
#include "test_bridge_ssh.c"
#undef main
#include "extension/session.h"
#include "extension/route.h"
#include <dirent.h>
static bool entropy_fails;
static gt_status_t session_entropy(uint8_t *p,size_t n){if(entropy_fails)return GT_ERR_IO;return gt__secure_random(p,n);}
/* Production implementation; only the OS entropy failure boundary is replaced. */
#define gt__secure_random session_entropy
#include "../src/extension/session.c"
#undef gt__secure_random
static uint64_t session_now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
typedef struct {uint64_t now;bool fail;} session_clock_t;
static bool session_clock(void *ctx,uint64_t *out){session_clock_t *c=ctx;if(c){*out=c->now;return !c->fail;}*out=session_now();return true;}
static gt_bridge_session_config_t session_config(session_clock_t *clock) {
  gt_bridge_session_config_t c={.required_count=2,.available_count=3,.budgets={768,65536,16777216},.handshake_ms=5000,.consent_ms=60000,.clock_context=clock,.clock=session_clock,.peer={GT_BRIDGE_PEER_LOCAL,"fixture-local-evidence"}};
  strcpy(c.plugin_id,"org.ghosterm.preview");strcpy(c.plugin_version,"0.1.0");memset(c.client_digest,'d',64);memset(c.executor_digest,'e',64);
  strcpy(c.host_version,"0.1.0");strcpy(c.sdk_version,"1.0.0");strcpy(c.required[0],"resource.receive");strcpy(c.required[1],"ui.form");
  memcpy(c.available,c.required,sizeof(c.required));strcpy(c.available[2],"ui.chat");return c;
}
typedef struct {size_t calls,live,fail;} session_budget_t;
typedef union {max_align_t align;size_t size;} session_block_t;
static void *session_allocate(void *ctx,size_t n){session_budget_t *b=ctx;if(++b->calls==b->fail)return NULL;session_block_t *p=malloc(sizeof(*p)+n);CHECK(p);p->size=n;b->live+=n;return p+1;}
static void session_release(void *ctx,void *p,size_t n){session_budget_t *b=ctx;session_block_t *block=(session_block_t *)p-1;CHECK(block->size==n && b->live>=n);b->live-=n;free(block);}
static size_t session_fds(void){DIR *d=opendir("/dev/fd");CHECK(d);size_t n=0;struct dirent *e;while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,".."))n++;CHECK(!closedir(d));return n;}
static gt_bridge_session_info_t session_info(gt_bridge_session_t *s){gt_bridge_session_info_t i;CHECK(gt__bridge_session_info(s,&i));return i;}
static void session_dispose(gt_bridge_session_t *s){uint64_t end=session_now()+1000;for(;;){gt_bridge_close_t r=gt__bridge_session_close(s,0);if(r==GT_BRIDGE_RELEASED)break;CHECK(r==GT_BRIDGE_DRAINING && session_now()<end);}CHECK(gt__bridge_session_close(s,0)==GT_BRIDGE_RELEASED && gt__bridge_session_destroy(s)==GT_OK);}
typedef struct {gt_bridge_session_t *h,*e;session_clock_t clock;session_budget_t budget;} session_pair_t;
typedef struct {gt_bridge_stream_t base;gt_bridge_stream_t *inner;session_clock_t *clock;unsigned reads,writes,late_read,late_write;} session_timed_t;
static gt_bridge_io_t session_timed_read(gt_bridge_stream_t *base,void *p,size_t n,int ms){session_timed_t *t=(session_timed_t *)base;gt_bridge_io_t r=gt__bridge_stream_read(t->inner,p,n,ms);if(r.state==GT_BRIDGE_DATA && ++t->reads==t->late_read)t->clock->now+=5001;return r;}
static gt_bridge_io_t session_timed_write(gt_bridge_stream_t *base,const void *p,size_t n,int ms){session_timed_t *t=(session_timed_t *)base;gt_bridge_io_t r=gt__bridge_stream_write(t->inner,p,n,ms);if(r.state==GT_BRIDGE_DATA && ++t->writes==t->late_write)t->clock->now+=5001;return r;}
static void session_timed_wake(gt_bridge_stream_t *base){gt__bridge_stream_stop(((session_timed_t *)base)->inner);}
static gt_bridge_close_t session_timed_close(gt_bridge_stream_t *base,int ms){return gt__bridge_stream_close(((session_timed_t *)base)->inner,ms);}
static void session_timed_destroy(gt_bridge_stream_t *base){session_timed_t *t=(session_timed_t *)base;CHECK(gt__bridge_stream_destroy(t->inner)==GT_OK);free(t);}
static const gt_bridge_stream_ops_t session_timed_ops={session_timed_read,session_timed_write,session_timed_wake,session_timed_close,session_timed_destroy};
static void session_wrap(gt_bridge_stream_t **stream,session_clock_t *clock,bool reading){session_timed_t *t=calloc(1,sizeof(*t));CHECK(t && gt__bridge_stream_init(&t->base,&session_timed_ops)==GT_OK);t->inner=*stream;t->clock=clock;if(reading)t->late_read=3;else t->late_write=3;*stream=&t->base;}
static void session_pair_timed(session_pair_t *p,const char *mode) {
  memset(p,0,sizeof(*p));p->clock.now=1000;int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));gt_bridge_stream_t *a=NULL,*b=NULL;
  CHECK(gt__bridge_fd_open(fds[0],fds[0],NULL,&a)==GT_OK && gt__bridge_fd_open(fds[1],fds[1],NULL,&b)==GT_OK);
  if(mode && !strcmp(mode,"late-read"))session_wrap(&a,&p->clock,true);
  if(mode && !strcmp(mode,"late-write"))session_wrap(&b,&p->clock,false);
  gt_bridge_session_config_t h=session_config(&p->clock),e=h;e.budgets=(gt_bridge_budgets_t){512,16384,262144};strcpy(e.host_version,"0.2.0");
  gt_allocator_t allocator={&p->budget,session_allocate,session_release};
  CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&h,&allocator,&p->h)==GT_OK && !a);
  CHECK(gt__bridge_session_open(&b,GT_BRIDGE_EXECUTOR,&e,&allocator,&p->e)==GT_OK && !b);
  memset(h.plugin_id,'x',sizeof(h.plugin_id));memset(e.required,0,sizeof(e.required));
}
static void session_pair_open(session_pair_t *p){session_pair_timed(p,NULL);}
static void session_pair_close(session_pair_t *p){session_dispose(p->h);session_dispose(p->e);CHECK(!p->budget.live);}
static void session_consent(session_pair_t *p) {
  for(unsigned i=0;i<1000;i++) {
    gt_bridge_session_phase_t h=gt__bridge_session_step(p->h,0),e=gt__bridge_session_step(p->e,0);
    CHECK(session_info(p->h).end==GT_BRIDGE_END_NONE && session_info(p->e).end==GT_BRIDGE_END_NONE);
    if(h==GT_BRIDGE_SESSION_CONSENT && e==GT_BRIDGE_SESSION_WAIT_DECISION)return;
  }
  CHECK(false);
}
static void session_active(session_pair_t *p) {
  for(unsigned i=0;i<1000;i++) {
    gt_bridge_session_phase_t h=gt__bridge_session_step(p->h,0),e=gt__bridge_session_step(p->e,0);
    CHECK(session_info(p->h).end==GT_BRIDGE_END_NONE && session_info(p->e).end==GT_BRIDGE_END_NONE);
    if(h==GT_BRIDGE_SESSION_ACTIVE && e==GT_BRIDGE_SESSION_ACTIVE)return;
  }
  CHECK(false);
}
static void session_ended(session_pair_t *p) {
  for(unsigned i=0;i<1000;i++){gt__bridge_session_step(p->h,0);gt__bridge_session_step(p->e,0);if(session_info(p->h).phase==GT_BRIDGE_SESSION_ENDED && session_info(p->e).phase==GT_BRIDGE_SESSION_ENDED)return;}CHECK(false);
}
static void session_allow(session_pair_t *p){gt_bridge_session_info_t i=session_info(p->h);CHECK(gt__bridge_session_decide(p->h,i.consent_token,GT_BRIDGE_DECISION_ALLOW)==GT_OK);}
static void session_late(const char *mode) {
  session_pair_t p;session_pair_timed(&p,mode);session_consent(&p);session_allow(&p);
  gt_bridge_session_t *target=!strcmp(mode,"late-read")?p.h:p.e;
  for(unsigned i=0;i<1000;i++){gt__bridge_session_step(p.h,0);gt__bridge_session_step(p.e,0);CHECK(session_info(target).phase!=GT_BRIDGE_SESSION_ACTIVE);if(session_info(target).end!=GT_BRIDGE_END_NONE)break;}
  gt_bridge_lease_t lease;CHECK(session_info(target).end==GT_BRIDGE_END_EXPIRED && !gt__bridge_session_lease(target,&lease));session_pair_close(&p);
}
static void *session_stop_later(void *arg){struct timespec t={0,30000000};while(nanosleep(&t,&t))CHECK(errno==EINTR);gt__bridge_session_stop(arg);return NULL;}
static void session_basic(const char *mode) {
  session_pair_t p;session_pair_open(&p);session_consent(&p);gt_bridge_lease_t empty={0};
  CHECK(session_info(p.h).decision==GT_BRIDGE_DECISION_UNKNOWN && !gt__bridge_session_lease(p.h,&empty));
  CHECK(gt__bridge_session_access(p.h,&empty,"resource.receive")==GT_BRIDGE_ACCESS_UNKNOWN);
  gt_bridge_session_info_t initial=session_info(p.h);
  CHECK(gt__bridge_session_decide(p.h,initial.consent_token,GT_BRIDGE_DECISION_UNKNOWN)==GT_OK && session_info(p.h).phase==GT_BRIDGE_SESSION_CONSENT);
  if(!strcmp(mode,"deny") || !strcmp(mode,"late-deny")) {
    if(!strcmp(mode,"late-deny"))p.clock.now+=10000;
    CHECK(gt__bridge_session_decide(p.h,initial.consent_token,GT_BRIDGE_DECISION_DENY)==GT_OK);
    CHECK(gt__bridge_session_access(p.h,&empty,"resource.receive")==GT_BRIDGE_ACCESS_DENIED);session_ended(&p);
    CHECK(session_info(p.h).end==GT_BRIDGE_END_DENIED && session_info(p.e).decision==GT_BRIDGE_DECISION_DENY && session_info(p.e).end==GT_BRIDGE_END_DENIED);
  } else if(!strcmp(mode,"expired")) {
    p.clock.now+=60001;gt__bridge_session_step(p.h,0);CHECK(session_info(p.h).end==GT_BRIDGE_END_EXPIRED);
    CHECK(gt__bridge_session_decide(p.h,initial.consent_token,GT_BRIDGE_DECISION_ALLOW)!=GT_OK);session_ended(&p);
    CHECK(session_info(p.h).decision==GT_BRIDGE_DECISION_UNKNOWN && !gt__bridge_session_lease(p.h,&empty));
  } else if(!strcmp(mode,"clock")) {
    p.clock.now--;gt__bridge_session_step(p.h,0);CHECK(session_info(p.h).end==GT_BRIDGE_END_INTERNAL && session_info(p.h).decision==GT_BRIDGE_DECISION_UNKNOWN);
  } else {
    char bad[33];memcpy(bad,initial.consent_token,33);bad[0]=bad[0]=='0'?'1':'0';
    CHECK(gt__bridge_session_decide(p.h,bad,GT_BRIDGE_DECISION_ALLOW)==GT_ERR_INVALID_ARG && session_info(p.h).decision==GT_BRIDGE_DECISION_UNKNOWN);
    if(!strcmp(mode,"consent-time")){p.clock.now+=10000;gt__bridge_session_step(p.h,0);gt__bridge_session_step(p.e,0);CHECK(session_info(p.h).phase==GT_BRIDGE_SESSION_CONSENT && session_info(p.e).phase==GT_BRIDGE_SESSION_WAIT_DECISION);}
    session_allow(&p);CHECK(!gt__bridge_session_lease(p.h,&empty) && gt__bridge_session_access(p.h,&empty,"resource.receive")==GT_BRIDGE_ACCESS_UNKNOWN);
    session_active(&p);gt_bridge_lease_t h,e;CHECK(gt__bridge_session_lease(p.h,&h) && gt__bridge_session_lease(p.e,&e));
    CHECK(gt__bridge_session_access(p.h,&h,"resource.receive")==GT_BRIDGE_ACCESS_ALLOWED && gt__bridge_session_access(p.e,&e,"ui.form")==GT_BRIDGE_ACCESS_ALLOWED);
    CHECK(gt__bridge_session_access(p.h,&h,"ui.chat")==GT_BRIDGE_ACCESS_UNSUPPORTED && gt__bridge_session_access(p.e,&h,"ui.form")==GT_BRIDGE_ACCESS_DENIED);
    gt_bridge_lease_t wrong=h;wrong.epoch++;CHECK(gt__bridge_session_access(p.h,&wrong,"ui.form")==GT_BRIDGE_ACCESS_DENIED);
    CHECK(!strcmp(session_info(p.h).connection,session_info(p.e).connection) && !strcmp(session_info(p.h).instance,session_info(p.e).instance));
    CHECK(session_info(p.h).sent==3 && session_info(p.h).received==3 && session_info(p.e).sent==3 && session_info(p.e).received==3);
    CHECK(session_info(p.h).budgets.message_bytes==16384 && !strcmp(session_info(p.h).peer.identity,"fixture-local-evidence") && !strcmp(session_info(p.h).peer_host_version,"0.2.0"));
    CHECK(gt__bridge_session_decide(p.h,initial.consent_token,GT_BRIDGE_DECISION_ALLOW)==GT_ERR_INVALID_ARG);
    if(!strcmp(mode,"revoke")){gt__bridge_session_revoke(p.h);CHECK(gt__bridge_session_access(p.h,&h,"ui.form")==GT_BRIDGE_ACCESS_REVOKED);session_ended(&p);CHECK(session_info(p.h).end==GT_BRIDGE_END_REVOKED && session_info(p.e).end==GT_BRIDGE_END_PEER);}
    else if(!strcmp(mode,"lost")){CHECK(gt__bridge_session_close(p.e,0)==GT_BRIDGE_RELEASED);gt__bridge_session_step(p.h,0);CHECK(session_info(p.h).end==GT_BRIDGE_END_LOST && gt__bridge_session_access(p.h,&h,"ui.form")==GT_BRIDGE_ACCESS_REVOKED);}
    else if(!strcmp(mode,"stop")){pthread_t thread;CHECK(!pthread_create(&thread,NULL,session_stop_later,p.h));uint64_t start=session_now();CHECK(gt__bridge_session_step(p.h,5000)==GT_BRIDGE_SESSION_ENDED && session_now()-start<1000);CHECK(!pthread_join(thread,NULL));CHECK(gt__bridge_session_access(p.h,&h,"ui.form")==GT_BRIDGE_ACCESS_REVOKED);}
    else if(!strcmp(mode,"stop-before-step")){gt__bridge_session_stop(p.h);CHECK(gt__bridge_session_access(p.h,&h,"ui.form")==GT_BRIDGE_ACCESS_REVOKED && !gt__bridge_session_lease(p.h,&h));}
    else if(!strcmp(mode,"consent-time")){p.clock.now+=1000000;CHECK(gt__bridge_session_step(p.h,0)==GT_BRIDGE_SESSION_ACTIVE);}
  }
  session_pair_close(&p);
}
static void session_allocation(void) {
  int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));gt_bridge_stream_t *a=NULL,*b=NULL;CHECK(gt__bridge_fd_open(fds[0],fds[0],NULL,&a)==GT_OK && gt__bridge_fd_open(fds[1],fds[1],NULL,&b)==GT_OK);
  session_clock_t clock={1000,false};gt_bridge_session_config_t config=session_config(&clock);session_budget_t budget={0};gt_allocator_t allocator={&budget,session_allocate,session_release};gt_bridge_session_t *s=NULL;
  for(size_t i=1;i<=6;i++){budget.calls=0;budget.fail=i;CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&config,&allocator,&s)==GT_ERR_IO && !s && a && !budget.live && !atomic_load(&a->stopping));}
  budget.fail=0;entropy_fails=true;CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&config,&allocator,&s)==GT_ERR_IO && !s && a && !budget.live);entropy_fails=false;
  config.peer.kind=(gt_bridge_peer_kind_t)2;CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&config,&allocator,&s)==GT_ERR_UNSUPPORTED && a && !s);config.peer.kind=GT_BRIDGE_PEER_LOCAL;
  strcpy(config.sdk_version,"2.0.0");CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&config,&allocator,&s)==GT_ERR_UNSUPPORTED && a);strcpy(config.sdk_version,"1.0.0");
  clock.now=UINT64_MAX-100;CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&config,&allocator,&s)==GT_ERR_IO && a);clock.now=1000;
  CHECK(gt__bridge_stream_write(b,"x",1,0).state==GT_BRIDGE_DATA);uint8_t byte;CHECK(gt__bridge_stream_read(a,&byte,1,0).state==GT_BRIDGE_DATA && byte=='x');
  CHECK(gt__bridge_stream_close(a,0)==GT_BRIDGE_RELEASED && gt__bridge_stream_destroy(a)==GT_OK && gt__bridge_stream_close(b,0)==GT_BRIDGE_RELEASED && gt__bridge_stream_destroy(b)==GT_OK);
}
static void session_receive(gt_bridge_stream_t *peer,gt_bridge_message_t *message,char *payload,size_t capacity) {
  gt_bridge_decoder_t *d=NULL;gt_bridge_wire_t *w=NULL;CHECK(gt__bridge_decoder_new(4096,NULL,&d)==GT_OK && gt__bridge_wire_new(NULL,&w)==GT_OK);
  for(unsigned i=0;i<1000;i++) {
    uint8_t byte;gt_bridge_io_t r=gt__bridge_stream_read(peer,&byte,1,0);CHECK(r.state==GT_BRIDGE_DATA || r.state==GT_BRIDGE_WAIT);if(r.state==GT_BRIDGE_WAIT)continue;
    size_t consumed;CHECK(gt__bridge_decoder_feed(d,&byte,1,&consumed)==GT_OK && consumed==1);const uint8_t *bytes;size_t n;
    if(gt__bridge_decoder_view(d,&bytes,&n)){CHECK(gt__bridge_wire_parse(w,bytes,n,message)==GT_OK && gt__bridge_wire_raw(w,message->payload,&bytes,&n) && n<capacity);memcpy(payload,bytes,n);payload[n]=0;gt__bridge_wire_free(w);gt__bridge_decoder_free(d);return;}
  }
  CHECK(false);
}
static void session_send(gt_bridge_stream_t *peer,const gt_bridge_message_t *header,uint64_t seq,const char *kind,const char *payload,bool truncate) {
  char bytes[4097];int n=snprintf(bytes,sizeof(bytes),"{\"wireVersion\":1,\"connectionId\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64 "\",\"streamId\":\"control\",\"seq\":\"%" PRIu64 "\",\"kind\":\"%s\",\"payload\":%s}",header->connection,header->instance,header->epoch,seq,kind,payload);CHECK(n>0 && (size_t)n<sizeof(bytes));
  uint8_t encoded[4100];encoded[0]=(uint8_t)((unsigned)n>>24);encoded[1]=(uint8_t)((unsigned)n>>16);encoded[2]=(uint8_t)((unsigned)n>>8);encoded[3]=(uint8_t)n;memcpy(encoded+4,bytes,(size_t)n);
  size_t total=(size_t)n+4-(truncate?1:0),at=0;while(at<total){gt_bridge_io_t r=gt__bridge_stream_write(peer,encoded+at,total-at,0);CHECK(r.state==GT_BRIDGE_DATA);at+=r.count;}
}
static void session_raw(const char *mode) {
  int fds[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));gt_bridge_stream_t *a=NULL,*peer=NULL;CHECK(gt__bridge_fd_open(fds[0],fds[0],NULL,&a)==GT_OK && gt__bridge_fd_open(fds[1],fds[1],NULL,&peer)==GT_OK);
  session_clock_t clock={1000,false};gt_bridge_session_config_t config=session_config(&clock);if(!strcmp(mode,"budget-upgrade"))config.budgets.message_bytes=8192;gt_bridge_session_t *host=NULL;CHECK(gt__bridge_session_open(&a,GT_BRIDGE_HOST,&config,NULL,&host)==GT_OK && !a);
  gt__bridge_session_step(host,0);gt_bridge_message_t hello;char body[4097];session_receive(peer,&hello,body,sizeof(body));
  gt_bridge_wire_t *wire=NULL;CHECK(gt__bridge_wire_new(NULL,&wire)==GT_OK);char nonce[33];
  const char *tag="\"nonce\":\"";const char *found=strstr(body,tag);CHECK(found);memcpy(nonce,found+strlen(tag),32);nonce[32]=0;
  if(!strcmp(mode,"bad-nonce"))nonce[0]=nonce[0]=='0'?'1':'0';
  char payload[4097];char digest[65];memset(digest,'e',64);digest[64]=0;if(!strcmp(mode,"wrong-digest"))digest[0]='0';
  int n=snprintf(payload,sizeof(payload),"{\"hostVersion\":\"0.2.0\",\"sdkVersion\":\"1.0.0\",\"hostNonce\":\"%s\",\"nonce\":\"22222222222222222222222222222222\",\"pluginId\":\"org.ghosterm.preview\",\"pluginVersion\":\"0.1.0\",\"clientDigest\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\",\"executorDigest\":\"%s\",\"features\":[],\"route\":\"direct-duplex\",\"requires\":[\"resource.receive\",\"%s\"],\"consentMs\":60000,\"budgets\":{\"fragmentBytes\":512,\"messageBytes\":16384,\"resourceBytes\":262144}}",nonce,digest,!strcmp(mode,"wrong-cap")?"process.spawn":"ui.form");CHECK(n>0 && (size_t)n<sizeof(payload));
  if(!strcmp(mode,"bad-scope"))hello.instance[0]=hello.instance[0]=='0'?'1':'0';if(!strcmp(mode,"old-epoch"))hello.epoch++;
  const char *kind="offer";if(!strcmp(mode,"early-message")){kind="request";strcpy(payload,"{\"requestId\":\"ffffffffffffffffffffffffffffffff\",\"method\":\"form.submit\",\"input\":{},\"timeoutMs\":30000}");}
  session_send(peer,&hello,!strcmp(mode,"gap")?1:0,kind,payload,!strcmp(mode,"truncated"));
  if(!strcmp(mode,"truncated"))CHECK(gt__bridge_stream_close(peer,0)==GT_BRIDGE_RELEASED);
  for(unsigned i=0;i<1000 && session_info(host).phase!=GT_BRIDGE_SESSION_CONSENT && session_info(host).end==GT_BRIDGE_END_NONE;i++)gt__bridge_session_step(host,0);
  if(!strcmp(mode,"replay") || !strcmp(mode,"bad-accept") || !strcmp(mode,"bad-ready") || !strcmp(mode,"active-message") || !strcmp(mode,"expired-after-allow")) {
    CHECK(session_info(host).phase==GT_BRIDGE_SESSION_CONSENT);gt_bridge_session_info_t offered=session_info(host);
    if(!strcmp(mode,"replay")){session_send(peer,&hello,0,"offer",payload,false);gt__bridge_session_step(host,0);CHECK(gt__bridge_session_decide(host,offered.consent_token,GT_BRIDGE_DECISION_ALLOW)!=GT_OK);}
    else {
      CHECK(gt__bridge_session_decide(host,offered.consent_token,GT_BRIDGE_DECISION_ALLOW)==GT_OK);gt__bridge_session_step(host,0);
      gt_bridge_message_t probe;session_receive(peer,&probe,payload,sizeof(payload));CHECK(probe.kind==GT_BRIDGE_PROBE);
      if(!strcmp(mode,"expired-after-allow")) {
        clock.now+=5001;gt__bridge_session_step(host,0);gt__bridge_session_step(host,0);gt_bridge_message_t close_message;session_receive(peer,&close_message,payload,sizeof(payload));
        CHECK(close_message.kind==GT_BRIDGE_CLOSE && strstr(payload,"\"kind\":\"unknown\"") && !strstr(payload,"\"effect\":\"none\""));
      } else {
      if(!strcmp(mode,"bad-accept")){char *challenge=strstr(payload,"\"challenge\":\"");CHECK(challenge);challenge+=strlen("\"challenge\":\"");challenge[0]=challenge[0]=='0'?'1':'0';}
      session_send(peer,&hello,1,"accept",payload,false);gt__bridge_session_step(host,0);
      if(strcmp(mode,"bad-accept")) {
        gt__bridge_session_step(host,0);gt_bridge_message_t ready_message;session_receive(peer,&ready_message,payload,sizeof(payload));CHECK(ready_message.kind==GT_BRIDGE_READY);
        gt_bridge_lease_t lease;CHECK(!gt__bridge_session_lease(host,&lease));
        if(!strcmp(mode,"bad-ready")){char *challenge=strstr(payload,"\"challenge\":\"");CHECK(challenge);challenge+=strlen("\"challenge\":\"");challenge[0]=challenge[0]=='0'?'1':'0';}
        session_send(peer,&hello,2,"ready",payload,false);gt__bridge_session_step(host,0);
        if(!strcmp(mode,"active-message")){CHECK(gt__bridge_session_lease(host,&lease));session_send(peer,&hello,3,"event","{\"topic\":\"task.updated\",\"value\":{}}",false);gt__bridge_session_step(host,0);CHECK(session_info(host).error==GT_ERR_UNSUPPORTED && gt__bridge_session_access(host,&lease,"ui.form")==GT_BRIDGE_ACCESS_REVOKED);}
      }
      }
    }
  }
  if(!strcmp(mode,"truncated"))gt__bridge_session_step(host,0);
  CHECK(session_info(host).end!=GT_BRIDGE_END_NONE);
  if(!strcmp(mode,"wrong-digest") || !strcmp(mode,"wrong-cap") || !strcmp(mode,"budget-upgrade"))CHECK(session_info(host).end==GT_BRIDGE_END_INCOMPATIBLE && session_info(host).decision==GT_BRIDGE_DECISION_UNKNOWN);
  else if(!strcmp(mode,"expired-after-allow"))CHECK(session_info(host).end==GT_BRIDGE_END_EXPIRED);
  else if(strcmp(mode,"active-message"))CHECK(session_info(host).end==GT_BRIDGE_END_PROTOCOL);
  gt_bridge_lease_t lease;CHECK(!gt__bridge_session_lease(host,&lease));gt__bridge_wire_free(wire);session_dispose(host);
  CHECK(gt__bridge_stream_close(peer,0)==GT_BRIDGE_RELEASED && gt__bridge_stream_destroy(peer)==GT_OK);
}
static void session_executor(void) {
  CHECK(!isatty(0) && !isatty(1));gt_bridge_stream_t *stream=NULL;CHECK(gt__bridge_fd_open(0,1,NULL,&stream)==GT_OK);
  gt_bridge_session_config_t config=session_config(NULL);strcpy(config.peer.identity,"fixture-SSH-stdio");gt_bridge_session_t *s=NULL;CHECK(gt__bridge_session_open(&stream,GT_BRIDGE_EXECUTOR,&config,NULL,&s)==GT_OK);
  uint64_t end=session_now()+15000;for(;;){gt_bridge_session_phase_t phase=gt__bridge_session_step(s,25);if(phase>=GT_BRIDGE_SESSION_ENDED)break;CHECK(session_now()<end);}
  CHECK(session_info(s).end==GT_BRIDGE_END_PEER || session_info(s).end==GT_BRIDGE_END_DENIED || session_info(s).end==GT_BRIDGE_END_LOST);session_dispose(s);
}
#ifdef GT_HAVE_LIBSSH2
static unsigned session_dials;static uint16_t session_port;
int session_fixture_connect(int fd,const struct sockaddr *address,socklen_t size){session_dials++;if(address->sa_family!=AF_INET || ntohs(((const struct sockaddr_in *)address)->sin_port)!=session_port){errno=EACCES;return -1;}return connect(fd,address,size);}
static void session_ssh(int argc,char **argv) {
  CHECK(argc>=8);size_t count=(size_t)atoi(argv[2]);CHECK(count>=2 && count<=3 && argc==(int)(6+count));session_port=(uint16_t)atoi(argv[6]);session_dials=0;
  uint8_t prefix[4];CHECK(exact_read(0,prefix,4));size_t n=((size_t)prefix[0]<<24)|((size_t)prefix[1]<<16)|((size_t)prefix[2]<<8)|prefix[3];CHECK(n && n<8192);private_key=calloc(1,n+1);CHECK(private_key && exact_read(0,private_key,n));
  gt_ssh_t *material[3]={0};gt_bridge_route_hop_t hops[3]={0};
  for(size_t i=0;i<count;i++){char identity[64];CHECK(snprintf(identity,sizeof(identity),"route-hop-%zu.invalid",i)>0);gt_ssh_opts_t o={.host=identity,.port=(uint16_t)atoi(argv[6+i]),.username=argv[3],.known_hosts_path=argv[4]};material[i]=gt_ssh_new(&o);CHECK(material[i] && gt_ssh_auth_private_key(material[i],private_key,NULL)==GT_OK);hops[i].connector=(gt_ssh_connector_config_t){.family=i?0:4,.address={127,0,0,1},.timeout_ms=5000,.clock=session_clock};if(i)hops[i].via_host="127.0.0.1";}
  gt_bridge_stream_t *route=NULL;CHECK(gt__bridge_route_open(material,hops,count,argv[5],NULL,NULL,&route)==GT_OK);uint64_t end=session_now()+15000;
  for(;;){gt_bridge_route_phase_t phase=gt__bridge_route_step(route,25);if(phase==GT_BRIDGE_ROUTE_READY)break;CHECK(phase<GT_BRIDGE_ROUTE_FAILED && session_now()<end);}
  gt_bridge_route_info_t ri;CHECK(gt__bridge_route_info(route,&ri));gt_bridge_session_config_t config=session_config(NULL);config.peer.kind=GT_BRIDGE_PEER_SSH;strcpy(config.peer.identity,ri.authentication[count-1].fingerprint);
  gt_bridge_session_t *host=NULL;CHECK(gt__bridge_session_open(&route,GT_BRIDGE_HOST,&config,NULL,&host)==GT_OK && !route);bool decided=false;
  for(;;) {
    gt_bridge_session_phase_t phase=gt__bridge_session_step(host,25);gt_bridge_session_info_t i=session_info(host);
    if(phase==GT_BRIDGE_SESSION_CONSENT && !decided){CHECK(gt__bridge_session_decide(host,i.consent_token,!strcmp(argv[1],"ssh-deny")?GT_BRIDGE_DECISION_DENY:GT_BRIDGE_DECISION_ALLOW)==GT_OK);decided=true;}
    if(phase==GT_BRIDGE_SESSION_ACTIVE){gt_bridge_lease_t lease;CHECK(gt__bridge_session_lease(host,&lease) && gt__bridge_session_access(host,&lease,"resource.receive")==GT_BRIDGE_ACCESS_ALLOWED);CHECK(i.peer.kind==GT_BRIDGE_PEER_SSH && !strcmp(i.peer.identity,ri.authentication[count-1].fingerprint));gt__bridge_session_revoke(host);CHECK(gt__bridge_session_access(host,&lease,"resource.receive")==GT_BRIDGE_ACCESS_REVOKED);}
    if(phase==GT_BRIDGE_SESSION_ENDED)break;CHECK(i.end==GT_BRIDGE_END_NONE || i.end==GT_BRIDGE_END_DENIED || i.end==GT_BRIDGE_END_REVOKED);CHECK(session_now()<end);
  }
  CHECK(decided && session_dials==1 && session_info(host).end==(!strcmp(argv[1],"ssh-deny")?GT_BRIDGE_END_DENIED:GT_BRIDGE_END_REVOKED));session_dispose(host);gt__secure_erase(private_key,n);free(private_key);private_key=NULL;
  printf("PASS bridge_session_%s_%zu\n",argv[1],count);
}
#endif
#ifndef GT_BRIDGE_SESSION_TEST_MAIN
#define GT_BRIDGE_SESSION_TEST_MAIN main
#endif
int GT_BRIDGE_SESSION_TEST_MAIN(int argc,char **argv) {
  if(argc==2 && !strcmp(argv[1],"--executor")){session_executor();return 0;}
#ifdef GT_HAVE_LIBSSH2
  if(argc>2){session_ssh(argc,argv);return 0;}
#endif
  CHECK(argc==2);size_t fds=session_fds();const char *mode=argv[1];
  if(!strcmp(mode,"allocation"))session_allocation();
  else if(!strcmp(mode,"late-read") || !strcmp(mode,"late-write"))session_late(mode);
  else if(!strcmp(mode,"allow") || !strcmp(mode,"deny") || !strcmp(mode,"late-deny") || !strcmp(mode,"consent-time") || !strcmp(mode,"expired") || !strcmp(mode,"clock") || !strcmp(mode,"lost") || !strcmp(mode,"revoke") || !strcmp(mode,"stop") || !strcmp(mode,"stop-before-step"))session_basic(mode);
  else session_raw(mode);
  CHECK(session_fds()==fds);printf("PASS bridge_session_%s\n",mode);return 0;
}
