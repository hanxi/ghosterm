#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/session.h"
#include "extension/transfer.h"
#include "base/secure-random.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gt_bridge_session {
  gt_allocator_t allocator;
  gt_bridge_session_config_t config;
  gt_bridge_session_info_t info;
  gt_bridge_role_t role;
  gt_bridge_stream_t *stream;
  gt_bridge_decoder_t *decoder;
  gt_bridge_writer_t *writer;
  gt_bridge_wire_t *wire;
  gt_bridge_rpc_t *rpc;
  gt_bridge_transfer_t *resources;
  bool rpc_sending,resource_sending,prefer_resource;
  atomic_bool stopping;
  gt_bridge_session_phase_t after_send;
  bool released,close_queued,tx_exhausted,rx_exhausted;
  uint64_t now,network_end,wait_started,wait_end,ending_end,tx_sequence,rx_sequence;
  uint32_t consent_ms;
  char host_nonce[33],executor_nonce[33],challenge[33],ticket[33],lease[33];
  uint8_t rx[GT_BRIDGE_IO_QUANTUM];size_t rx_used,rx_size;
};
static const char zero_id[]="00000000000000000000000000000000";
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool stopped(const gt_bridge_session_t *s){return atomic_load_explicit(&s->stopping,memory_order_acquire);}
static bool add_time(uint64_t now,uint64_t delta,uint64_t *out){if(delta>UINT64_MAX-now)return false;*out=now+delta;return true;}
static gt_status_t random_id(char out[33]) {
  uint8_t bytes[16];gt_status_t status=gt__secure_random(bytes,sizeof(bytes));
  if(status!=GT_OK)return status;
  static const char hex[]="0123456789abcdef";
  for(size_t i=0;i<16;i++){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15];}
  out[32]=0;gt__secure_erase(bytes,sizeof(bytes));return GT_OK;
}
static void end_session(gt_bridge_session_t *s,gt_bridge_session_end_t reason,gt_status_t error,bool notify) {
  if(s->info.end!=GT_BRIDGE_END_NONE)return;
  s->info.end=reason;s->info.error=error;memset(s->info.consent_token,0,sizeof(s->info.consent_token));
  gt__bridge_rpc_end(s->rpc);
  gt__bridge_transfer_end(s->resources);
  if(notify && s->info.connection[0] && add_time(s->now,1000,&s->ending_end))s->info.phase=GT_BRIDGE_SESSION_ENDING;
  else{s->info.phase=GT_BRIDGE_SESSION_ENDED;gt__bridge_stream_stop(s->stream);}
}
static bool tick(gt_bridge_session_t *s) {
  if(stopped(s)){end_session(s,GT_BRIDGE_END_REVOKED,GT_ERR_CANCELLED,false);s->info.phase=GT_BRIDGE_SESSION_ENDED;return false;}
  uint64_t now;
  if(!s->config.clock(s->config.clock_context,&now) || now<s->now){end_session(s,GT_BRIDGE_END_INTERNAL,GT_ERR_INTERNAL,false);return false;}
  s->now=now;
  if(s->info.phase==GT_BRIDGE_SESSION_ACTIVE || s->info.phase>=GT_BRIDGE_SESSION_ENDED)return true;
  if(s->info.end!=GT_BRIDGE_END_NONE) {
    if(now>=s->ending_end){s->info.phase=GT_BRIDGE_SESSION_ENDED;gt__bridge_stream_stop(s->stream);return false;}
    return true;
  }
  bool waiting=s->info.phase==GT_BRIDGE_SESSION_CONSENT || s->info.phase==GT_BRIDGE_SESSION_WAIT_DECISION;
  if(now>=(waiting?s->wait_end:s->network_end)){end_session(s,GT_BRIDGE_END_EXPIRED,GT_ERR_IO,true);return false;}
  return true;
}
static bool transition(gt_bridge_session_t *s,gt_bridge_session_phase_t next) {
  s->info.phase=next;
  if(next==GT_BRIDGE_SESSION_CONSENT || next==GT_BRIDGE_SESSION_WAIT_DECISION) {
    s->wait_started=s->now;uint64_t grace=next==GT_BRIDGE_SESSION_WAIT_DECISION && s->network_end>s->now?s->network_end-s->now:0;
    if(!add_time(s->now,(uint64_t)s->consent_ms+grace,&s->wait_end)){end_session(s,GT_BRIDGE_END_INTERNAL,GT_ERR_INTERNAL,false);return false;}
    s->info.consent_deadline=s->wait_end;
    if(next==GT_BRIDGE_SESSION_CONSENT)memcpy(s->info.consent_token,s->ticket,33);
  }
  return true;
}
static bool resume_network(gt_bridge_session_t *s) {
  if(!add_time(s->network_end,s->now-s->wait_started,&s->network_end)){end_session(s,GT_BRIDGE_END_INTERNAL,GT_ERR_INTERNAL,false);return false;}
  return true;
}
static bool text_safe(const char *p,size_t capacity) {
  size_t n=strnlen(p,capacity);if(!n || n==capacity)return false;
  for(size_t i=0;i<n;i++)if((unsigned char)p[i]<33 || (unsigned char)p[i]>126 || p[i]=='"' || p[i]=='\\')return false;
  return true;
}
static bool name_safe(const char *p) {
  if(!text_safe(p,129) || p[0]<'a' || p[0]>'z')return false;
  bool separator=false;
  for(size_t i=0;p[i];i++) {
    unsigned char c=(unsigned char)p[i];
    if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9')){separator=false;continue;}
    if((c!='.' && c!='-' && c!='_') || separator)return false;separator=true;
  }
  return !separator;
}
static bool names_valid(const char names[][129],size_t count) {
  for(size_t i=0;i<count;i++){if(!name_safe(names[i]))return false;for(size_t j=0;j<i;j++)if(!strcmp(names[i],names[j]))return false;}return true;
}
static bool contains(const char names[][129],size_t count,const char *name) {
  for(size_t i=0;i<count;i++)if(!strcmp(names[i],name))return true;return false;
}
static bool local_budgets(gt_bridge_budgets_t b) {
  return b.fragment_bytes>=32 && b.fragment_bytes<=768 && b.message_bytes>=1024 && b.message_bytes<=65536 && b.resource_bytes && b.resource_bytes<=16777216;
}
static bool same_budgets(gt_bridge_budgets_t a,gt_bridge_budgets_t b){return a.fragment_bytes==b.fragment_bytes && a.message_bytes==b.message_bytes && a.resource_bytes==b.resource_bytes;}
static gt_bridge_budgets_t minimum_budgets(gt_bridge_budgets_t a,gt_bridge_budgets_t b) {
  return (gt_bridge_budgets_t){a.fragment_bytes<b.fragment_bytes?a.fragment_bytes:b.fragment_bytes,a.message_bytes<b.message_bytes?a.message_bytes:b.message_bytes,a.resource_bytes<b.resource_bytes?a.resource_bytes:b.resource_bytes};
}
static bool string_member(gt_bridge_session_t *s,uint16_t object,const char *key,char *out,size_t capacity) {
  const uint8_t *p;size_t n;
  if(!gt__bridge_wire_string(s->wire,gt__bridge_wire_member(s->wire,object,key),&p,&n) || n>=capacity)return false;
  memcpy(out,p,n);out[n]=0;return true;
}
static bool string_equal(gt_bridge_session_t *s,uint16_t object,const char *key,const char *expected) {
  const uint8_t *p;size_t n;return gt__bridge_wire_string(s->wire,gt__bridge_wire_member(s->wire,object,key),&p,&n) && n==strlen(expected) && !memcmp(p,expected,n);
}
static bool number_member(gt_bridge_session_t *s,uint16_t object,const char *key,uint64_t *out){return gt__bridge_wire_uint(s->wire,gt__bridge_wire_member(s->wire,object,key),out);}
static bool read_budgets(gt_bridge_session_t *s,uint16_t payload,gt_bridge_budgets_t *out) {
  uint16_t id=gt__bridge_wire_member(s->wire,payload,"budgets");uint64_t a,b,c;
  if(!number_member(s,id,"fragmentBytes",&a) || !number_member(s,id,"messageBytes",&b) || !number_member(s,id,"resourceBytes",&c) || a>UINT32_MAX || b>UINT32_MAX || c>UINT32_MAX)return false;
  *out=(gt_bridge_budgets_t){(uint32_t)a,(uint32_t)b,(uint32_t)c};return local_budgets(*out);
}
static bool metadata_equal(gt_bridge_session_t *s,uint16_t payload) {
  return string_equal(s,payload,"pluginId",s->config.plugin_id) && string_equal(s,payload,"pluginVersion",s->config.plugin_version) &&
    string_equal(s,payload,"clientDigest",s->config.client_digest) && string_equal(s,payload,"executorDigest",s->config.executor_digest) &&
    string_equal(s,payload,"sdkVersion",s->config.sdk_version);
}
static bool caps_equal(gt_bridge_session_t *s,uint16_t payload,const char *key) {
  uint16_t array=gt__bridge_wire_member(s->wire,payload,key);
  if(gt__bridge_wire_count(s->wire,array)!=s->config.required_count)return false;
  for(size_t i=0;i<s->config.required_count;i++) {
    const uint8_t *p;size_t n;char name[129];
    if(!gt__bridge_wire_string(s->wire,gt__bridge_wire_at(s->wire,array,i),&p,&n) || n>128)return false;
    memcpy(name,p,n);name[n]=0;
    if(!contains(s->config.required,s->config.required_count,name) || !contains(s->config.available,s->config.available_count,name))return false;
  }
  return true;
}
static bool nonce_equal(gt_bridge_session_t *s,uint16_t payload,bool challenge) {
  return string_equal(s,payload,"hostNonce",s->host_nonce) && string_equal(s,payload,"executorNonce",s->executor_nonce) && (!challenge || string_equal(s,payload,"challenge",s->challenge));
}
static bool features_equal(gt_bridge_session_t *s,uint16_t payload) {
  uint16_t id=gt__bridge_wire_member(s->wire,payload,"features");size_t count=gt__bridge_wire_count(s->wire,id);
  if(count!=(size_t)s->config.rpc_enabled+(s->config.resource_store?1u:0u))return false;
  for(size_t i=0;i<count;i++){const uint8_t *p;size_t n;if(!gt__bridge_wire_string(s->wire,gt__bridge_wire_at(s->wire,id,i),&p,&n))return false;
    if(n==8 && !memcmp(p,"requests",8)){if(!s->config.rpc_enabled)return false;}
    else if(n==9 && !memcmp(p,"resources",9)){if(!s->config.resource_store)return false;}else return false;}
  return true;
}
static bool capabilities_json(gt_bridge_session_t *s,char *out,size_t capacity) {
  size_t at=0;out[at++]='[';
  for(size_t i=0;i<s->config.required_count;i++) {
    int n=snprintf(out+at,capacity-at,"%s\"%s\"",i?",":"",s->config.required[i]);
    if(n<0 || (size_t)n>=capacity-at)return false;at+=(size_t)n;
  }
  if(capacity-at<2)return false;out[at++]=']';out[at]=0;return true;
}
static bool payload_json(gt_bridge_session_t *s,gt_bridge_kind_t kind,char *out,size_t capacity) {
  char budgets[160],caps[2200];gt_bridge_budgets_t b=s->info.budgets;
  (void)snprintf(budgets,sizeof(budgets),"{\"fragmentBytes\":%u,\"messageBytes\":%u,\"resourceBytes\":%u}",b.fragment_bytes,b.message_bytes,b.resource_bytes);
  if(!capabilities_json(s,caps,sizeof(caps)))return false;
  const char *hn=s->host_nonce[0]?s->host_nonce:zero_id,*en=s->executor_nonce[0]?s->executor_nonce:zero_id;
  const char *features=s->config.resource_store?(s->config.rpc_enabled?"[\"requests\",\"resources\"]":"[\"resources\"]"):(s->config.rpc_enabled?"[\"requests\"]":"[]");int n=-1;
  if(kind==GT_BRIDGE_HELLO)n=snprintf(out,capacity,
    "{\"hostVersion\":\"%s\",\"sdkVersion\":\"%s\",\"nonce\":\"%s\",\"pluginId\":\"%s\",\"pluginVersion\":\"%s\",\"clientDigest\":\"%s\",\"executorDigest\":\"%s\",\"consentMs\":%u,\"budgets\":%s,\"features\":%s}",
    s->config.host_version,s->config.sdk_version,hn,s->config.plugin_id,s->config.plugin_version,s->config.client_digest,s->config.executor_digest,s->consent_ms,budgets,features);
  else if(kind==GT_BRIDGE_OFFER)n=snprintf(out,capacity,
    "{\"hostVersion\":\"%s\",\"sdkVersion\":\"%s\",\"hostNonce\":\"%s\",\"nonce\":\"%s\",\"pluginId\":\"%s\",\"pluginVersion\":\"%s\",\"clientDigest\":\"%s\",\"executorDigest\":\"%s\",\"route\":\"direct-duplex\",\"requires\":%s,\"consentMs\":%u,\"budgets\":%s,\"features\":%s}",
    s->config.host_version,s->config.sdk_version,hn,en,s->config.plugin_id,s->config.plugin_version,s->config.client_digest,s->config.executor_digest,caps,s->consent_ms,budgets,features);
  else if(kind==GT_BRIDGE_PROBE || kind==GT_BRIDGE_ACCEPT)n=snprintf(out,capacity,
    "{\"hostNonce\":\"%s\",\"executorNonce\":\"%s\",\"challenge\":\"%s\",\"capabilities\":%s,\"budgets\":%s,\"features\":%s}",hn,en,s->challenge,caps,budgets,features);
  else if(kind==GT_BRIDGE_READY)n=snprintf(out,capacity,"{\"hostNonce\":\"%s\",\"executorNonce\":\"%s\",\"challenge\":\"%s\"}",hn,en,s->challenge);
  else if(kind==GT_BRIDGE_CLOSE) {
    const char *outcome=s->info.end==GT_BRIDGE_END_DENIED?"{\"kind\":\"failed\",\"error\":{\"code\":\"DENIED\",\"message\":\"declined\",\"effect\":\"none\"}}":
      s->info.end==GT_BRIDGE_END_EXPIRED && s->info.decision!=GT_BRIDGE_DECISION_ALLOW?"{\"kind\":\"failed\",\"error\":{\"code\":\"EXPIRED\",\"message\":\"expired\",\"effect\":\"none\"}}":
      "{\"kind\":\"unknown\",\"reason\":\"bridge ended without a business completion receipt\"}";
    n=snprintf(out,capacity,"{\"outcome\":%s}",outcome);
  }
  return n>=0 && (size_t)n<capacity;
}
static bool frame_json(gt_bridge_session_t *s,gt_bridge_kind_t kind,char *out,size_t capacity,size_t *length) {
  static const char *names[]={"offer","request","response","event","cancel","credit","resource.begin","resource.chunk","resource.end","close","hello","probe","accept","ready","resource.result","resource.cancel"};
  if((unsigned)kind>=sizeof(names)/sizeof(*names))return false;
  char payload[GT_BRIDGE_SESSION_CONTROL_MAX+1];if(!payload_json(s,kind,payload,sizeof(payload)))return false;
  int n=snprintf(out,capacity,"{\"wireVersion\":1,\"connectionId\":\"%s\",\"instanceId\":\"%s\",\"epoch\":\"1\",\"streamId\":\"control\",\"seq\":\"%" PRIu64 "\",\"kind\":\"%s\",\"payload\":%s}",
    s->info.connection[0]?s->info.connection:zero_id,s->info.instance[0]?s->info.instance:zero_id,s->tx_sequence,names[kind],payload);
  if(n<0 || (size_t)n>=capacity)return false;*length=(size_t)n;return true;
}
static bool queue(gt_bridge_session_t *s,gt_bridge_kind_t kind,gt_bridge_session_phase_t after) {
  char frame[GT_BRIDGE_SESSION_CONTROL_MAX+1];size_t size;gt_bridge_message_t message;
  if(s->tx_exhausted || gt__bridge_writer_pending(s->writer) || !frame_json(s,kind,frame,sizeof(frame),&size) ||
     gt__bridge_wire_parse(s->wire,frame,size,&message)!=GT_OK || gt__bridge_writer_queue(s->writer,frame,size)!=GT_OK){end_session(s,GT_BRIDGE_END_INTERNAL,GT_ERR_INTERNAL,false);return false;}
  if(s->tx_sequence==UINT64_MAX)s->tx_exhausted=true;else s->tx_sequence++;
  s->after_send=after;s->info.phase=GT_BRIDGE_SESSION_SENDING;return true;
}
static void incompatible(gt_bridge_session_t *s){end_session(s,GT_BRIDGE_END_INCOMPATIBLE,GT_ERR_UNSUPPORTED,false);}
static void protocol(gt_bridge_session_t *s){end_session(s,GT_BRIDGE_END_PROTOCOL,GT_ERR_PROTOCOL,false);}
static gt_status_t application_received(gt_bridge_session_t *s,const gt_bridge_message_t *m,size_t size){
  bool resource=!strcmp(m->stream,"resource") || m->kind==GT_BRIDGE_RESOURCE_RESULT || m->kind==GT_BRIDGE_RESOURCE_CANCEL;
  if(m->kind==GT_BRIDGE_CREDIT && string_equal(s,m->payload,"streamId","resource"))resource=true;
  return resource?(s->resources?gt__bridge_transfer_receive(s->resources,m,size):GT_ERR_UNSUPPORTED):(s->rpc?gt__bridge_rpc_receive(s->rpc,m,size,s->now):GT_ERR_UNSUPPORTED);
}
static bool incoming(gt_bridge_session_t *s,const uint8_t *bytes,size_t size) {
  gt_bridge_message_t m;
  if(size>GT_BRIDGE_SESSION_CONTROL_MAX || gt__bridge_wire_parse(s->wire,bytes,size,&m)!=GT_OK){protocol(s);return false;}
  if(m.epoch!=1){protocol(s);return false;}
  if(s->info.connection[0] && (strcmp(m.connection,s->info.connection) || strcmp(m.instance,s->info.instance))){protocol(s);return false;}
  if(strcmp(m.stream,"control")) {
    if(s->info.phase!=GT_BRIDGE_SESSION_ACTIVE){protocol(s);return false;}
    gt_status_t status=application_received(s,&m,size);s->info.received++;
    if(status!=GT_OK){end_session(s,status==GT_ERR_UNSUPPORTED?GT_BRIDGE_END_INCOMPATIBLE:GT_BRIDGE_END_PROTOCOL,status,false);return false;}return true;
  }
  if(s->rx_exhausted || m.sequence!=s->rx_sequence){protocol(s);return false;}
  if(s->rx_sequence==UINT64_MAX)s->rx_exhausted=true;else s->rx_sequence++;
  s->info.received++;
  if(m.kind==GT_BRIDGE_CLOSE) {
    uint16_t outcome=gt__bridge_wire_member(s->wire,m.payload,"outcome"),error=gt__bridge_wire_member(s->wire,outcome,"error");
    bool denied=s->role==GT_BRIDGE_EXECUTOR && s->info.phase==GT_BRIDGE_SESSION_WAIT_DECISION && string_equal(s,outcome,"kind","failed") && string_equal(s,error,"code","DENIED");
    if(denied)s->info.decision=GT_BRIDGE_DECISION_DENY;
    end_session(s,denied?GT_BRIDGE_END_DENIED:GT_BRIDGE_END_PEER,GT_OK,false);return true;
  }
  gt_bridge_budgets_t budgets;uint64_t consent=0;
  if(s->info.phase==GT_BRIDGE_SESSION_WAIT_HELLO && s->role==GT_BRIDGE_EXECUTOR && m.kind==GT_BRIDGE_HELLO) {
    if(!metadata_equal(s,m.payload) || !features_equal(s,m.payload) || !read_budgets(s,m.payload,&budgets)){incompatible(s);return false;}
    s->info.budgets=minimum_budgets(s->config.budgets,budgets);
    if(!string_member(s,m.payload,"nonce",s->host_nonce,33) || !string_member(s,m.payload,"hostVersion",s->info.peer_host_version,65) || !number_member(s,m.payload,"consentMs",&consent)){protocol(s);return false;}
    s->consent_ms=(uint32_t)(consent<s->config.consent_ms?consent:s->config.consent_ms);
    memcpy(s->info.connection,m.connection,33);memcpy(s->info.instance,m.instance,33);s->info.epoch=1;
    return queue(s,GT_BRIDGE_OFFER,GT_BRIDGE_SESSION_WAIT_DECISION);
  }
  if(s->info.phase==GT_BRIDGE_SESSION_WAIT_OFFER && s->role==GT_BRIDGE_HOST && m.kind==GT_BRIDGE_OFFER) {
    if(!metadata_equal(s,m.payload) || !features_equal(s,m.payload) || !string_equal(s,m.payload,"route","direct-duplex") || !caps_equal(s,m.payload,"requires") ||
       !read_budgets(s,m.payload,&budgets) || !same_budgets(budgets,minimum_budgets(s->config.budgets,budgets))){incompatible(s);return false;}
    if(!string_equal(s,m.payload,"hostNonce",s->host_nonce) || !string_member(s,m.payload,"nonce",s->executor_nonce,33) || !number_member(s,m.payload,"consentMs",&consent) || consent>s->config.consent_ms){protocol(s);return false;}
    if(!string_member(s,m.payload,"hostVersion",s->info.peer_host_version,65)){protocol(s);return false;}
    s->info.budgets=budgets;s->consent_ms=(uint32_t)consent;return transition(s,GT_BRIDGE_SESSION_CONSENT);
  }
  if(s->info.phase==GT_BRIDGE_SESSION_WAIT_DECISION && s->role==GT_BRIDGE_EXECUTOR && m.kind==GT_BRIDGE_PROBE) {
    if(!nonce_equal(s,m.payload,false) || !features_equal(s,m.payload) || !caps_equal(s,m.payload,"capabilities") || !read_budgets(s,m.payload,&budgets) || !same_budgets(budgets,s->info.budgets) ||
       !string_member(s,m.payload,"challenge",s->challenge,33)){protocol(s);return false;}
    if(!resume_network(s))return false;s->info.decision=GT_BRIDGE_DECISION_ALLOW;return queue(s,GT_BRIDGE_ACCEPT,GT_BRIDGE_SESSION_WAIT_READY);
  }
  if(s->info.phase==GT_BRIDGE_SESSION_WAIT_ACCEPT && s->role==GT_BRIDGE_HOST && m.kind==GT_BRIDGE_ACCEPT) {
    if(!nonce_equal(s,m.payload,true) || !features_equal(s,m.payload) || !caps_equal(s,m.payload,"capabilities") || !read_budgets(s,m.payload,&budgets) || !same_budgets(budgets,s->info.budgets)){protocol(s);return false;}
    return queue(s,GT_BRIDGE_READY,GT_BRIDGE_SESSION_WAIT_ACK);
  }
  if((s->info.phase==GT_BRIDGE_SESSION_WAIT_READY || s->info.phase==GT_BRIDGE_SESSION_WAIT_ACK) && m.kind==GT_BRIDGE_READY) {
    if(!nonce_equal(s,m.payload,true)){protocol(s);return false;}
    if(s->role==GT_BRIDGE_EXECUTOR && s->info.phase==GT_BRIDGE_SESSION_WAIT_READY)return queue(s,GT_BRIDGE_READY,GT_BRIDGE_SESSION_ACTIVE);
    if(s->role==GT_BRIDGE_HOST && s->info.phase==GT_BRIDGE_SESSION_WAIT_ACK)return transition(s,GT_BRIDGE_SESSION_ACTIVE);
    protocol(s);return false;
  }
  /* Each explicitly negotiated class owns its traffic and credit separately. */
  if(s->info.phase==GT_BRIDGE_SESSION_ACTIVE) {
    gt_status_t status=application_received(s,&m,size);
    if(status!=GT_OK){end_session(s,status==GT_ERR_UNSUPPORTED?GT_BRIDGE_END_INCOMPATIBLE:GT_BRIDGE_END_PROTOCOL,status,false);return false;}return true;
  }
  protocol(s);return false;
}
static int budget(gt_bridge_session_t *s,uint64_t start,int ms) {
  if(!tick(s))return 0;
  uint64_t elapsed=s->now-start,remaining=elapsed>=(uint64_t)ms?0:(uint64_t)ms-elapsed;
  uint64_t end=s->info.end!=GT_BRIDGE_END_NONE?s->ending_end:
    s->info.phase==GT_BRIDGE_SESSION_CONSENT || s->info.phase==GT_BRIDGE_SESSION_WAIT_DECISION?s->wait_end:
    s->info.phase==GT_BRIDGE_SESSION_ACTIVE?UINT64_MAX:s->network_end;
  if(end<=s->now)return 0;if(remaining>end-s->now)remaining=end-s->now;return (int)remaining;
}
gt_bridge_session_phase_t gt__bridge_session_step(gt_bridge_session_t *s,int ms) {
  if(!s || ms<0)return GT_BRIDGE_SESSION_ENDED;
  if(s->released || s->info.phase==GT_BRIDGE_SESSION_CLOSED)return GT_BRIDGE_SESSION_CLOSED;
  if(!tick(s) || s->info.phase>=GT_BRIDGE_SESSION_ENDED)return s->info.phase;
  uint64_t start=s->now;
  if(s->info.phase==GT_BRIDGE_SESSION_ACTIVE && (s->rpc || s->resources) && !gt__bridge_writer_pending(s->writer)) {
    char rpc_frame[GT_BRIDGE_RPC_FRAME_MAX+1],resource_frame[4097];size_t rpc_n=0,resource_n=0;bool rpc_control=false,resource_control=false;
    gt_status_t status=s->rpc?gt__bridge_rpc_prepare(s->rpc,s->tx_sequence,s->now,rpc_frame,sizeof(rpc_frame),&rpc_n,&rpc_control):GT_OK;
    if(status==GT_OK && s->resources)status=gt__bridge_transfer_prepare(s->resources,s->tx_sequence,resource_frame,sizeof(resource_frame),&resource_n,&resource_control);
    if(status!=GT_OK){end_session(s,status==GT_ERR_CANCELLED?GT_BRIDGE_END_REVOKED:status==GT_ERR_UNSUPPORTED?GT_BRIDGE_END_INCOMPATIBLE:status==GT_ERR_IO?GT_BRIDGE_END_INTERNAL:GT_BRIDGE_END_PROTOCOL,status,false);return s->info.phase;}
    bool resource=resource_n && (!rpc_n || (resource_control && !rpc_control) || (resource_control==rpc_control && s->prefer_resource));
    char *frame=resource?resource_frame:rpc_frame;size_t n=resource?resource_n:rpc_n;bool control=resource?resource_control:rpc_control;
    if(n) {
      gt_bridge_message_t m;
      if((control && s->tx_exhausted) || gt__bridge_wire_parse(s->wire,frame,n,&m)!=GT_OK){protocol(s);return s->info.phase;}
      if(!tick(s))return s->info.phase;
      if(resource || gt__bridge_rpc_current(s->rpc,s->now)) {
        if(gt__bridge_writer_queue(s->writer,frame,n)!=GT_OK){protocol(s);return s->info.phase;}
        if(resource){gt__bridge_transfer_commit(s->resources);s->resource_sending=true;}else{gt__bridge_rpc_commit(s->rpc);s->rpc_sending=true;}s->prefer_resource=!resource;
        if(control){if(s->tx_sequence==UINT64_MAX)s->tx_exhausted=true;else s->tx_sequence++;}
      }
    }
  }
  if(s->info.phase==GT_BRIDGE_SESSION_ENDING && !gt__bridge_writer_pending(s->writer)) {
    if(!queue(s,GT_BRIDGE_CLOSE,GT_BRIDGE_SESSION_ENDED))return s->info.phase;s->close_queued=true;
  }
  if(gt__bridge_writer_pending(s->writer)) {
    gt_bridge_io_t result=gt__bridge_writer_step(s->writer,s->stream,budget(s,start,ms));
    if(stopped(s)){end_session(s,GT_BRIDGE_END_REVOKED,GT_ERR_CANCELLED,false);s->info.phase=GT_BRIDGE_SESSION_ENDED;return s->info.phase;}
    if(result.state==GT_BRIDGE_ERROR || result.state==GT_BRIDGE_CLOSED || result.state==GT_BRIDGE_EOF){end_session(s,GT_BRIDGE_END_LOST,GT_ERR_IO,false);return s->info.phase;}
    bool complete=gt__bridge_writer_pending(s->writer)==0;
    if(complete)s->info.sent++;
    if(!tick(s))return s->info.phase; /* time may advance during the write */
    if(!complete)return s->info.phase;
    bool application=s->rpc_sending || s->resource_sending;
    if(s->rpc_sending){gt__bridge_rpc_sent(s->rpc);s->rpc_sending=false;}
    if(s->resource_sending){gt__bridge_transfer_sent(s->resources);s->resource_sending=false;}
    if(s->info.end!=GT_BRIDGE_END_NONE)s->info.phase=s->close_queued?GT_BRIDGE_SESSION_ENDED:GT_BRIDGE_SESSION_ENDING;
    else if(!application && !transition(s,s->after_send))return s->info.phase;
    if(s->info.phase>=GT_BRIDGE_SESSION_ENDING)return s->info.phase;
  }
  if(!tick(s) || s->info.phase>=GT_BRIDGE_SESSION_ENDING)return s->info.phase;
  if(s->rx_used==s->rx_size) {
    gt_bridge_io_t result=gt__bridge_stream_read(s->stream,s->rx,sizeof(s->rx),budget(s,start,ms));
    if(stopped(s)){end_session(s,GT_BRIDGE_END_REVOKED,GT_ERR_CANCELLED,false);s->info.phase=GT_BRIDGE_SESSION_ENDED;return s->info.phase;}
    if(result.state==GT_BRIDGE_WAIT)return s->info.phase;
    if(result.state==GT_BRIDGE_EOF) {
      if(gt__bridge_decoder_finish(s->decoder)!=GT_OK)protocol(s);else end_session(s,GT_BRIDGE_END_LOST,GT_ERR_IO,false);
      return s->info.phase;
    }
    if(result.state!=GT_BRIDGE_DATA){end_session(s,GT_BRIDGE_END_LOST,result.error==GT_OK?GT_ERR_IO:result.error,false);return s->info.phase;}
    s->rx_used=0;s->rx_size=result.count;
    if(!tick(s))return s->info.phase; /* no activation from late read bytes */
  }
  size_t consumed=0;
  if(gt__bridge_decoder_feed(s->decoder,s->rx+s->rx_used,s->rx_size-s->rx_used,&consumed)!=GT_OK){protocol(s);return s->info.phase;}
  s->rx_used+=consumed;
  const uint8_t *bytes;size_t size;
  if(gt__bridge_decoder_view(s->decoder,&bytes,&size)) {
    (void)incoming(s,bytes,size);(void)gt__bridge_decoder_consume(s->decoder);
  }
  return s->info.phase;
}
bool gt__bridge_session_info(const gt_bridge_session_t *s,gt_bridge_session_info_t *out) {
  if(out)memset(out,0,sizeof(*out));if(!s || !out)return false;*out=s->info;
  if(stopped(s) && out->end==GT_BRIDGE_END_NONE){out->phase=GT_BRIDGE_SESSION_ENDED;out->end=GT_BRIDGE_END_REVOKED;out->error=GT_ERR_CANCELLED;memset(out->consent_token,0,33);}
  return true;
}
gt_status_t gt__bridge_session_decide(gt_bridge_session_t *s,const char *token,gt_bridge_decision_t decision) {
  if(!s || !token || strnlen(token,33)!=32 || s->role!=GT_BRIDGE_HOST)return GT_ERR_INVALID_ARG;
  if(!tick(s))return s->info.error;
  if(s->info.phase!=GT_BRIDGE_SESSION_CONSENT || strcmp(token,s->ticket))return GT_ERR_INVALID_ARG;
  if(decision==GT_BRIDGE_DECISION_UNKNOWN)return GT_OK;
  if(decision!=GT_BRIDGE_DECISION_ALLOW && decision!=GT_BRIDGE_DECISION_DENY)return GT_ERR_INVALID_ARG;
  s->info.decision=decision;memset(s->info.consent_token,0,33);
  if(decision==GT_BRIDGE_DECISION_DENY){end_session(s,GT_BRIDGE_END_DENIED,GT_OK,true);return GT_OK;}
  gt_status_t status=random_id(s->challenge);
  if(status!=GT_OK){end_session(s,GT_BRIDGE_END_INTERNAL,status,false);return status;}
  if(!resume_network(s) || !queue(s,GT_BRIDGE_PROBE,GT_BRIDGE_SESSION_WAIT_ACCEPT))return s->info.error;
  return GT_OK;
}
bool gt__bridge_session_lease(const gt_bridge_session_t *s,gt_bridge_lease_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!s || !out || stopped(s) || s->info.end!=GT_BRIDGE_END_NONE || s->info.phase!=GT_BRIDGE_SESSION_ACTIVE)return false;
  memcpy(out->connection,s->info.connection,33);memcpy(out->instance,s->info.instance,33);memcpy(out->token,s->lease,33);out->epoch=s->info.epoch;return true;
}
gt_bridge_access_t gt__bridge_session_access(const gt_bridge_session_t *s,const gt_bridge_lease_t *lease,const char *capability) {
  if(!s || !lease || !capability)return GT_BRIDGE_ACCESS_DENIED;
  if(s->info.decision==GT_BRIDGE_DECISION_DENY)return GT_BRIDGE_ACCESS_DENIED;
  if(stopped(s) || s->info.end!=GT_BRIDGE_END_NONE)return GT_BRIDGE_ACCESS_REVOKED;
  if(s->info.phase!=GT_BRIDGE_SESSION_ACTIVE)return GT_BRIDGE_ACCESS_UNKNOWN;
  if(lease->epoch!=s->info.epoch || memcmp(lease->connection,s->info.connection,33) || memcmp(lease->instance,s->info.instance,33) || memcmp(lease->token,s->lease,33))return GT_BRIDGE_ACCESS_DENIED;
  if(strnlen(capability,129)>128 || !contains(s->config.required,s->config.required_count,capability))return GT_BRIDGE_ACCESS_UNSUPPORTED;
  return GT_BRIDGE_ACCESS_ALLOWED;
}
void gt__bridge_session_revoke(gt_bridge_session_t *s){if(s)end_session(s,GT_BRIDGE_END_REVOKED,GT_ERR_CANCELLED,true);}
void gt__bridge_session_stop(gt_bridge_session_t *s){if(s){atomic_store_explicit(&s->stopping,true,memory_order_release);gt__bridge_stream_stop(s->stream);}}
gt_bridge_close_t gt__bridge_session_close(gt_bridge_session_t *s,int ms) {
  if(!s || ms<0)return GT_BRIDGE_CLOSE_ERROR;if(s->released)return GT_BRIDGE_RELEASED;
  gt__bridge_session_stop(s);end_session(s,GT_BRIDGE_END_LOCAL,GT_OK,false);
  gt_bridge_close_t result=gt__bridge_stream_close(s->stream,ms);
  if(result==GT_BRIDGE_RELEASED){s->released=true;s->info.phase=GT_BRIDGE_SESSION_CLOSED;}
  return result;
}
static void free_parts(gt_bridge_session_t *s) {
  gt__bridge_transfer_free(s->resources);
  gt__bridge_rpc_free(s->rpc);
  gt__bridge_writer_free(s->writer);gt__bridge_decoder_free(s->decoder);gt__bridge_wire_free(s->wire);
  gt_allocator_t a=s->allocator;gt__secure_erase(s,sizeof(*s));gt__plugin_memory_free(&a,s,sizeof(*s));
}
gt_status_t gt__bridge_session_destroy(gt_bridge_session_t *s) {
  if(!s || !s->released)return GT_ERR_INVALID_ARG;
  gt_status_t cleanup=gt__bridge_transfer_cleanup(s->resources);if(cleanup!=GT_OK)return cleanup;
  gt_status_t status=gt__bridge_stream_destroy(s->stream);if(status!=GT_OK)return status;free_parts(s);return GT_OK;
}
gt_status_t gt__bridge_session_open(gt_bridge_stream_t **owned,gt_bridge_role_t role,const gt_bridge_session_config_t *config,
 const gt_allocator_t *allocator,gt_bridge_session_t **out) {
  if(out)*out=NULL;
  if(!out || !owned || !*owned || !config || (role!=GT_BRIDGE_HOST && role!=GT_BRIDGE_EXECUTOR) || (*owned)->closed || atomic_load_explicit(&(*owned)->stopping,memory_order_acquire))return GT_ERR_INVALID_ARG;
  if(config->peer.kind!=GT_BRIDGE_PEER_LOCAL && config->peer.kind!=GT_BRIDGE_PEER_SSH)return GT_ERR_UNSUPPORTED;
  if(!text_safe(config->peer.identity,128) || !text_safe(config->plugin_id,129) || !text_safe(config->plugin_version,65) ||
     !text_safe(config->client_digest,65) || !text_safe(config->executor_digest,65) || !text_safe(config->host_version,65) || !text_safe(config->sdk_version,65) ||
     config->required_count>16 || config->available_count>16 || !names_valid(config->required,config->required_count) || !names_valid(config->available,config->available_count) ||
     !local_budgets(config->budgets) || !config->clock || !config->handshake_ms || config->handshake_ms>60000 || !config->consent_ms || config->consent_ms>600000)return GT_ERR_INVALID_ARG;
  if(strcmp(config->sdk_version,"1.0.0"))return GT_ERR_UNSUPPORTED;
  if(config->rpc_method_count>16 || (!config->rpc_enabled && config->rpc_method_count))return GT_ERR_INVALID_ARG;
  if((!config->resource_store && config->resource_scope_bytes) || (config->resource_store && (!config->resource_scope_bytes || config->resource_scope_bytes>GT_BRIDGE_RESOURCE_TOTAL_MAX)))return GT_ERR_INVALID_ARG;
  for(size_t i=0;i<config->rpc_method_count;i++) {
    const gt_bridge_rpc_method_t *m=&config->rpc_methods[i];
    if(!name_safe(m->method) || !name_safe(m->capability) || !m->input || !m->output || m->receiver>1)return GT_ERR_INVALID_ARG;
    for(size_t j=0;j<i;j++)if(!strcmp(m->method,config->rpc_methods[j].method))return GT_ERR_INVALID_ARG;
  }
  for(size_t i=0;i<config->required_count;i++)if(!contains(config->available,config->available_count,config->required[i]))return GT_ERR_UNSUPPORTED;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  uint64_t now,end;if(!config->clock(config->clock_context,&now) || !add_time(now,config->handshake_ms,&end))return GT_ERR_IO;
  gt_bridge_session_t *s=gt__plugin_memory_alloc(&a,sizeof(*s));if(!s)return GT_ERR_IO;
  memset(s,0,sizeof(*s));s->allocator=a;s->config=*config;s->role=role;s->stream=*owned;s->now=now;s->network_end=end;s->consent_ms=config->consent_ms;
  atomic_init(&s->stopping,false);s->info.budgets=config->budgets;s->info.peer=config->peer;
  gt_status_t status=gt__bridge_wire_new(&a,&s->wire);
  if(status==GT_OK)status=gt__bridge_decoder_new(GT_BRIDGE_FRAME_MAX,&a,&s->decoder);
  if(status==GT_OK)status=gt__bridge_writer_new(GT_BRIDGE_FRAME_MAX,&a,&s->writer);
  if(status==GT_OK && config->rpc_enabled)status=gt__bridge_rpc_new(s,s->wire,&a,&s->rpc);
  if(status==GT_OK && config->resource_store)status=gt__bridge_transfer_new(s,s->wire,config->resource_store,config->resource_scope_bytes,&a,&s->resources);
  if(status==GT_OK)status=random_id(s->lease);
  if(status==GT_OK && role==GT_BRIDGE_HOST)status=random_id(s->info.connection);
  if(status==GT_OK && role==GT_BRIDGE_HOST)status=random_id(s->info.instance);
  if(status==GT_OK)status=random_id(role==GT_BRIDGE_HOST?s->host_nonce:s->executor_nonce);
  if(status==GT_OK && role==GT_BRIDGE_HOST)status=random_id(s->ticket);
  if(status==GT_OK) {
    char bytes[GT_BRIDGE_SESSION_CONTROL_MAX+1];size_t size;gt_bridge_message_t message;
    if(!frame_json(s,GT_BRIDGE_HELLO,bytes,sizeof(bytes),&size) || gt__bridge_wire_parse(s->wire,bytes,size,&message)!=GT_OK ||
       !frame_json(s,GT_BRIDGE_OFFER,bytes,sizeof(bytes),&size) || gt__bridge_wire_parse(s->wire,bytes,size,&message)!=GT_OK)status=GT_ERR_INVALID_ARG;
  }
  if(status!=GT_OK){free_parts(s);return status;}
  s->info.epoch=role==GT_BRIDGE_HOST?1:0;
  if(role==GT_BRIDGE_HOST) {
    if(!queue(s,GT_BRIDGE_HELLO,GT_BRIDGE_SESSION_WAIT_OFFER)){status=s->info.error;free_parts(s);return status;}
  } else s->info.phase=GT_BRIDGE_SESSION_WAIT_HELLO;
  *owned=NULL;*out=s;return GT_OK;
}
const gt_bridge_rpc_method_t *gt__bridge_session_methods(const gt_bridge_session_t *s,size_t *count,gt_bridge_role_t *role){if(!s || !count || !role)return NULL;*count=s->config.rpc_method_count;*role=s->role;return s->config.rpc_methods;}
gt_bridge_rpc_status_t gt__bridge_session_request(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,const char *method,const void *input,size_t size,uint32_t timeout,uint64_t *handle){if(handle)*handle=0;if(!s || !s->rpc)return GT_BRIDGE_RPC_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RPC_CLOSED;return gt__bridge_rpc_start(s->rpc,lease,method,input,size,timeout,s->now,handle);}
gt_bridge_rpc_status_t gt__bridge_session_cancel_request(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,uint64_t handle){return s && s->rpc?gt__bridge_rpc_cancel(s->rpc,lease,handle):GT_BRIDGE_RPC_UNSUPPORTED;}
bool gt__bridge_session_request_result(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,uint64_t handle,gt_bridge_rpc_result_t *out){if(out)memset(out,0,sizeof(*out));if(s)(void)tick(s);return s && s->rpc && gt__bridge_rpc_result(s->rpc,lease,handle,s->now,out);}
gt_bridge_rpc_status_t gt__bridge_session_forget_request(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,uint64_t handle){return s && s->rpc?gt__bridge_rpc_forget(s->rpc,lease,handle):GT_BRIDGE_RPC_UNSUPPORTED;}
gt_bridge_rpc_status_t gt__bridge_session_take_request(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,gt_bridge_rpc_request_t *out){if(out)memset(out,0,sizeof(*out));if(!s || !s->rpc)return GT_BRIDGE_RPC_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RPC_CLOSED;return gt__bridge_rpc_take(s->rpc,lease,s->now,out);}
gt_bridge_rpc_status_t gt__bridge_session_check_request(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,uint64_t ticket){if(!s || !s->rpc)return GT_BRIDGE_RPC_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RPC_CLOSED;return gt__bridge_rpc_check(s->rpc,lease,ticket,s->now);}
gt_bridge_rpc_status_t gt__bridge_session_respond(gt_bridge_session_t *s,const gt_bridge_lease_t *lease,uint64_t ticket,const void *outcome,size_t n){if(!s || !s->rpc)return GT_BRIDGE_RPC_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RPC_CLOSED;return gt__bridge_rpc_respond(s->rpc,lease,ticket,outcome,n,s->now);}

bool gt__bridge_session_scope(const gt_bridge_session_t *s,const gt_bridge_lease_t *lease){return s && lease && lease->epoch==s->info.epoch && !memcmp(lease->connection,s->info.connection,33) && !memcmp(lease->instance,s->info.instance,33) && !memcmp(lease->token,s->lease,33);}
bool gt__bridge_session_has_capability(const gt_bridge_session_t *s,const char *capability){return s && capability && strnlen(capability,129)<=128 && contains(s->config.available,s->config.available_count,capability);}
gt_bridge_resource_status_t gt__bridge_session_send_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l,const gt_bridge_resource_descriptor_t *d,uint64_t *handle){if(handle)*handle=0;if(!s || !s->resources)return GT_BRIDGE_RESOURCE_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RESOURCE_CLOSED;return gt__bridge_transfer_start(s->resources,l,d,handle);}
gt_bridge_resource_status_t gt__bridge_session_write_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l,uint64_t h,const void *p,size_t n,size_t *accepted){if(accepted)*accepted=0;if(!s || !s->resources)return GT_BRIDGE_RESOURCE_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RESOURCE_CLOSED;return gt__bridge_transfer_write(s->resources,l,h,p,n,accepted);}
gt_bridge_resource_status_t gt__bridge_session_finish_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l,uint64_t h,bool complete){if(!s || !s->resources)return GT_BRIDGE_RESOURCE_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RESOURCE_CLOSED;return gt__bridge_transfer_finish(s->resources,l,h,complete);}
gt_bridge_resource_status_t gt__bridge_session_cancel_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l,uint64_t h){if(s)(void)tick(s);return s && s->resources?gt__bridge_transfer_cancel(s->resources,l,h):GT_BRIDGE_RESOURCE_UNSUPPORTED;}
bool gt__bridge_session_resource_result(gt_bridge_session_t *s,const gt_bridge_lease_t *l,uint64_t h,gt_bridge_transfer_result_t *out){if(out)memset(out,0,sizeof(*out));if(s)(void)tick(s);return s && s->resources && gt__bridge_transfer_result(s->resources,l,h,out);}
gt_bridge_resource_status_t gt__bridge_session_forget_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l,uint64_t h){if(s)(void)tick(s);return s && s->resources?gt__bridge_transfer_forget(s->resources,l,h):GT_BRIDGE_RESOURCE_UNSUPPORTED;}
gt_bridge_resource_status_t gt__bridge_session_take_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l,gt_bridge_snapshot_t *out){if(out)memset(out,0,sizeof(*out));if(s)(void)tick(s);return s && s->resources?gt__bridge_transfer_take(s->resources,l,out):GT_BRIDGE_RESOURCE_UNSUPPORTED;}
gt_bridge_resource_status_t gt__bridge_session_reject_resource(gt_bridge_session_t *s,const gt_bridge_lease_t *l){if(!s || !s->resources)return GT_BRIDGE_RESOURCE_UNSUPPORTED;if(!tick(s))return GT_BRIDGE_RESOURCE_CLOSED;return gt__bridge_transfer_reject(s->resources,l);}

bool gt__bridge_session_runtime_admissible(const gt_bridge_session_t *s){return s && s->rpc && !gt__bridge_rpc_has_owners(s->rpc) && !gt__bridge_transfer_has_owner(s->resources);}
