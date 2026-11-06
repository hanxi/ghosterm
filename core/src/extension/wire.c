#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/wire.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>

#define GT_JSON_TAG gt_bridge_wire
#define GT_JSON_DOC gt_bridge_wire_t
#define GT_JSON_ERROR gt_bridge_wire_error_t
#define GT_JSON_NODES GT_BRIDGE_WIRE_NODES_MAX
#define GT_JSON_BYTES GT_BRIDGE_FRAME_MAX
#define GT_JSON_DEPTH GT_BRIDGE_WIRE_DEPTH_MAX
#define GT_JSON_KEYS 8192u
#define GT_JSON_WORK 262144u
#define GT_JSON_LIMIT GT_BRIDGE_WIRE_LIMIT
#define GT_JSON_SYNTAX GT_BRIDGE_WIRE_SYNTAX
#define GT_JSON_UTF8 GT_BRIDGE_WIRE_UTF8
#define GT_JSON_DUPLICATE GT_BRIDGE_WIRE_DUPLICATE
#include "base/json-reader.inc"
static bool lower(uint8_t c){return c>='a' && c<='z';}
static bool alpha(uint8_t c){return lower(c) || (c>='A' && c<='Z');}
static bool alnum(uint8_t c){return alpha(c) || digit(c);}
static bool node_string_is(const gt_bridge_wire_t *d,uint16_t id,const char *s) {
  const json_node_t *n=&d->nodes[id];return n->kind==J_STRING && n->size==strlen(s) && !memcmp(d->strings+n->text,s,n->size);
}
static bool node_key_is(const gt_bridge_wire_t *d,uint16_t id,const char *s) {
  const json_node_t *n=&d->nodes[id];return n->key_size==strlen(s) && !memcmp(d->strings+n->key,s,n->key_size);
}
static bool node_strings_equal(const gt_bridge_wire_t *d,uint16_t a,uint16_t b) {
  const json_node_t *x=&d->nodes[a],*y=&d->nodes[b];return x->kind==J_STRING && y->kind==J_STRING && x->size==y->size && !memcmp(d->strings+x->text,d->strings+y->text,x->size);
}
static uint16_t member(const gt_bridge_wire_t *d,uint16_t id,const char *key) {
  if(!id || id>d->used || d->nodes[id].kind!=J_OBJECT || !key)return 0;
  for(uint16_t child=d->nodes[id].child;child;child=d->nodes[child].next)if(node_key_is(d,child,key))return child;
  return 0;
}
static bool counter(const uint8_t *s,size_t n,uint64_t *out) {
  if(!n || n>20 || (n>1 && s[0]=='0'))return false;
  uint64_t value=0;
  for(size_t i=0;i<n;i++){if(!digit(s[i]))return false;unsigned v=s[i]-'0';if(value>(UINT64_MAX-v)/10)return false;value=value*10+v;}
  *out=value;return true;
}
static bool version(const uint8_t *s,size_t n) {
  size_t at=0;
  for(unsigned i=0;i<3;i++) {
    size_t start=at;while(at<n && digit(s[at]))at++;
    if(at==start || (at-start>1 && s[start]=='0'))return false;
    if(i<2){if(at==n || s[at++]!='.')return false;}
  }
  if(at<n && s[at]=='-') {
    at++;
    for(;;) {
      size_t start=at;bool numeric=true;
      while(at<n && (alnum(s[at]) || s[at]=='-')){if(!digit(s[at]))numeric=false;at++;}
      if(at==start || (numeric && at-start>1 && s[start]=='0'))return false;
      if(at==n || s[at]!='.')break;
      at++;
    }
  }
  if(at<n && s[at]=='+') {
    at++;
    for(;;) {
      size_t start=at;while(at<n && (alnum(s[at]) || s[at]=='-'))at++;
      if(at==start)return false;
      if(at==n || s[at]!='.')break;
      at++;
    }
  }
  return at==n;
}
static int sextet(uint8_t c) {
  if(c>='A' && c<='Z')return c-'A';if(lower(c))return c-'a'+26;if(digit(c))return c-'0'+52;
  return c=='+'?62:c=='/'?63:-1;
}
typedef enum {PAT_ID,PAT_DIGEST,PAT_COUNTER,PAT_PLUGIN,PAT_VERSION,PAT_NAME,PAT_DISPLAY,PAT_MIME,PAT_BASE64} pattern_t;
static bool node_pattern(const gt_bridge_wire_t *d,uint16_t id,pattern_t pattern) {
  const json_node_t *node=&d->nodes[id];const uint8_t *s=d->strings+node->text;size_t n=node->size;
  switch(pattern) {
    case PAT_ID:case PAT_DIGEST:
      if(n!=(pattern==PAT_ID?32u:64u))return false;
      for(size_t i=0;i<n;i++)if(!digit(s[i]) && !(s[i]>='a' && s[i]<='f'))return false;
      return true;
    case PAT_COUNTER:{uint64_t value;return counter(s,n,&value);}
    case PAT_VERSION:return version(s,n);
    case PAT_PLUGIN:case PAT_NAME: {
      if(!n || (pattern==PAT_NAME && !lower(s[0])))return false;
      bool separated=true,any=false;
      for(size_t i=0;i<n;i++) {
        if((pattern==PAT_NAME?alnum(s[i]):lower(s[i]) || digit(s[i]))){separated=false;continue;}
        if(s[i]!='.' && s[i]!='-' && !(pattern==PAT_NAME && s[i]=='_'))return false;
        if(separated)return false;separated=true;any=true;
      }
      return !separated && (pattern==PAT_NAME || any);
    }
    case PAT_DISPLAY:
      if(!n)return false;
      for(size_t i=0;i<n;i++)if(s[i]<32 || s[i]==127)return false;
      return true;
    case PAT_MIME: {
      bool slash=false,segment=false;
      for(size_t i=0;i<n;i++) {
        if(s[i]=='/'){if(slash || !segment)return false;slash=true;segment=false;continue;}
        if(!lower(s[i]) && !digit(s[i]) && (!s[i] || !strchr("!#$&^_.+-",s[i])))return false;
        segment=true;
      }
      return slash && segment;
    }
    case PAT_BASE64: {
      if(n%4)return false;
      size_t padding=n && s[n-1]=='='?1:0;if(n>1 && s[n-2]=='=')padding++;
      for(size_t i=0;i<n-padding;i++)if(sextet(s[i])<0)return false;
      if(padding==1 && (n<4 || (sextet(s[n-2])&3)))return false;
      if(padding==2 && (n<4 || (sextet(s[n-3])&15)))return false;
      return true;
    }
  }
  return false;
}
#include "extension/wire-schema.inc"

gt_status_t gt__bridge_wire_new(const gt_allocator_t *allocator,gt_bridge_wire_t **out) {
  if(out)*out=NULL;if(!out)return GT_ERR_INVALID_ARG;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  gt_bridge_wire_t *d=gt__plugin_memory_alloc(&a,sizeof(*d));if(!d)return GT_ERR_IO;
  memset(d,0,sizeof(*d));d->allocator=a;*out=d;return GT_OK;
}
gt_status_t gt__bridge_wire_parse(gt_bridge_wire_t *d,const void *bytes,size_t size,gt_bridge_message_t *out) {
  if(out)memset(out,0,sizeof(*out));if(!d)return GT_ERR_INVALID_ARG;
  gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));d->allocator=a;d->input=bytes;d->input_size=size;
  if(!out || (!bytes && size)){fail(d,GT_BRIDGE_WIRE_SYNTAX);return GT_ERR_INVALID_ARG;}
  if(size>GT_BRIDGE_FRAME_MAX){fail(d,GT_BRIDGE_WIRE_LIMIT);return GT_ERR_PROTOCOL;}
  if(!value(d,0,0,0,0))return GT_ERR_PROTOCOL;
  space(d);if(d->at!=size){fail(d,GT_BRIDGE_WIRE_SYNTAX);return GT_ERR_PROTOCOL;}
  if(!wire_schema_0(d,1)){fail(d,GT_BRIDGE_WIRE_SCHEMA);return GT_ERR_PROTOCOL;}
  const char *fields[]={"connectionId","instanceId","streamId"};char *targets[]={out->connection,out->instance,out->stream};
  for(size_t i=0;i<3;i++){const json_node_t *n=&d->nodes[member(d,1,fields[i])];memcpy(targets[i],d->strings+n->text,n->size);}
  const json_node_t *epoch=&d->nodes[member(d,1,"epoch")],*seq=&d->nodes[member(d,1,"seq")];
  (void)counter(d->strings+epoch->text,epoch->size,&out->epoch);(void)counter(d->strings+seq->text,seq->size,&out->sequence);
  const char *kinds[]={"offer","request","response","event","cancel","credit","resource.begin","resource.chunk","resource.end","close","hello","probe","accept","ready","resource.result","resource.cancel"};
  uint16_t kind=member(d,1,"kind");for(size_t i=0;i<sizeof(kinds)/sizeof(*kinds);i++)if(node_string_is(d,kind,kinds[i]))out->kind=(gt_bridge_kind_t)i;
  out->payload=member(d,1,"payload");d->valid=true;return GT_OK;
}
gt_bridge_wire_error_t gt__bridge_wire_error(const gt_bridge_wire_t *d){return d?d->error:GT_BRIDGE_WIRE_SYNTAX;}
uint16_t gt__bridge_wire_member(const gt_bridge_wire_t *d,uint16_t id,const char *key){return d && d->valid?member(d,id,key):0;}
bool gt__bridge_wire_string(const gt_bridge_wire_t *d,uint16_t id,const uint8_t **p,size_t *size) {
  if(p)*p=NULL;if(size)*size=0;
  if(!d || !d->valid || !id || id>d->used || !p || !size || d->nodes[id].kind!=J_STRING)return false;
  *p=d->strings+d->nodes[id].text;*size=d->nodes[id].size;return true;
}
bool gt__bridge_wire_raw(const gt_bridge_wire_t *d,uint16_t id,const uint8_t **p,size_t *size) {
  if(p)*p=NULL;if(size)*size=0;
  if(!d || !d->valid || !id || id>d->used || !p || !size)return false;
  *p=d->input+d->nodes[id].start;*size=d->nodes[id].bytes;return true;
}
bool gt__bridge_wire_uint(const gt_bridge_wire_t *d,uint16_t id,uint64_t *out) {
  if(out)*out=0;
  return d && d->valid && id && id<=d->used && out && node_uint(d,id,out);
}
size_t gt__bridge_wire_count(const gt_bridge_wire_t *d,uint16_t id) {
  return d && d->valid && id && id<=d->used && d->nodes[id].kind==J_ARRAY?d->nodes[id].count:0;
}
bool gt__bridge_wire_counter(const gt_bridge_wire_t *d,uint16_t id,uint64_t *out){
  if(out)*out=0;if(!d || !d->valid || !id || id>d->used || !out || d->nodes[id].kind!=J_STRING)return false;
  return counter(d->strings+d->nodes[id].text,d->nodes[id].size,out);
}
uint16_t gt__bridge_wire_at(const gt_bridge_wire_t *d,uint16_t id,size_t at) {
  if(!d || !d->valid || !id || id>d->used || d->nodes[id].kind!=J_ARRAY)return 0;
  uint16_t child=d->nodes[id].child;while(child && at--){child=d->nodes[child].next;}return child;
}
void gt__bridge_wire_free(gt_bridge_wire_t *d) {
  if(!d)return;gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));gt__plugin_memory_free(&a,d,sizeof(*d));
}
