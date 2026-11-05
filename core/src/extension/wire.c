#define _POSIX_C_SOURCE 200809L
#include "extension/wire.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>

typedef enum {J_OBJECT,J_ARRAY,J_STRING,J_NUMBER,J_TRUE,J_FALSE,J_NULL} json_kind_t;
typedef struct {
  json_kind_t kind;
  uint16_t child,next,parent,count;
  uint32_t key,key_size,text,size,points,start,bytes;
} json_node_t;
#define KEY_SLOTS 8192u
#define KEY_WORK_MAX 262144u
struct gt_bridge_wire {
  gt_allocator_t allocator;
  const uint8_t *input;
  size_t input_size,at,strings_size,key_work;
  uint16_t used;
  bool valid;
  gt_bridge_wire_error_t error;
  json_node_t nodes[GT_BRIDGE_WIRE_NODES_MAX+1];
  uint16_t keys[KEY_SLOTS];
  uint8_t strings[GT_BRIDGE_FRAME_MAX];
};
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static bool fail(gt_bridge_wire_t *d,gt_bridge_wire_error_t error){d->error=error;return false;}
static bool digit(uint8_t c){return c>='0' && c<='9';}
static bool lower(uint8_t c){return c>='a' && c<='z';}
static bool alpha(uint8_t c){return lower(c) || (c>='A' && c<='Z');}
static bool alnum(uint8_t c){return alpha(c) || digit(c);}
static void space(gt_bridge_wire_t *d) {
  while(d->at<d->input_size) {
    uint8_t c=d->input[d->at];
    if(c!=' ' && c!='\t' && c!='\r' && c!='\n')break;
    d->at++;
  }
}
static bool append_scalar(gt_bridge_wire_t *d,uint32_t cp) {
  uint8_t out[4];size_t n;
  if(cp<0x80){out[0]=(uint8_t)cp;n=1;}
  else if(cp<0x800){out[0]=(uint8_t)(0xc0|(cp>>6));out[1]=(uint8_t)(0x80|(cp&63));n=2;}
  else if(cp<0x10000){out[0]=(uint8_t)(0xe0|(cp>>12));out[1]=(uint8_t)(0x80|((cp>>6)&63));out[2]=(uint8_t)(0x80|(cp&63));n=3;}
  else{out[0]=(uint8_t)(0xf0|(cp>>18));out[1]=(uint8_t)(0x80|((cp>>12)&63));out[2]=(uint8_t)(0x80|((cp>>6)&63));out[3]=(uint8_t)(0x80|(cp&63));n=4;}
  if(n>sizeof(d->strings)-d->strings_size)return fail(d,GT_BRIDGE_WIRE_LIMIT);
  memcpy(d->strings+d->strings_size,out,n);d->strings_size+=n;return true;
}
static bool hex4(gt_bridge_wire_t *d,uint32_t *out) {
  if(d->input_size-d->at<4)return fail(d,GT_BRIDGE_WIRE_SYNTAX);
  uint32_t value=0;
  for(unsigned i=0;i<4;i++) {
    uint8_t c=d->input[d->at++];unsigned v;
    if(digit(c))v=c-'0';else if(c>='a' && c<='f')v=c-'a'+10;else if(c>='A' && c<='F')v=c-'A'+10;else return fail(d,GT_BRIDGE_WIRE_SYNTAX);
    value=(value<<4)|v;
  }
  *out=value;return true;
}
static bool string(gt_bridge_wire_t *d,uint32_t *offset,uint32_t *size,uint32_t *points) {
  if(d->at>=d->input_size || d->input[d->at++]!='"')return fail(d,GT_BRIDGE_WIRE_SYNTAX);
  *offset=(uint32_t)d->strings_size;*points=0;
  while(d->at<d->input_size) {
    uint32_t cp=d->input[d->at++];
    if(cp=='"'){*size=(uint32_t)d->strings_size-*offset;return true;}
    if(cp<32)return fail(d,GT_BRIDGE_WIRE_SYNTAX);
    if(cp=='\\') {
      if(d->at==d->input_size)return fail(d,GT_BRIDGE_WIRE_SYNTAX);
      cp=d->input[d->at++];
      switch(cp) {
        case '"':case '\\':case '/':break;
        case 'b':cp=8;break;case 'f':cp=12;break;case 'n':cp=10;break;case 'r':cp=13;break;case 't':cp=9;break;
        case 'u': {
          if(!hex4(d,&cp))return false;
          if(cp>=0xd800 && cp<=0xdbff) {
            if(d->input_size-d->at<2 || d->input[d->at]!='\\' || d->input[d->at+1]!='u')return fail(d,GT_BRIDGE_WIRE_UTF8);
            d->at+=2;uint32_t low;if(!hex4(d,&low))return false;
            if(low<0xdc00 || low>0xdfff)return fail(d,GT_BRIDGE_WIRE_UTF8);
            cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
          } else if(cp>=0xdc00 && cp<=0xdfff)return fail(d,GT_BRIDGE_WIRE_UTF8);
          break;
        }
        default:return fail(d,GT_BRIDGE_WIRE_SYNTAX);
      }
    } else if(cp>=0x80) {
      unsigned continuation;uint32_t minimum;
      if(cp>=0xc2 && cp<=0xdf){continuation=1;minimum=0x80;cp&=31;}
      else if(cp>=0xe0 && cp<=0xef){continuation=2;minimum=0x800;cp&=15;}
      else if(cp>=0xf0 && cp<=0xf4){continuation=3;minimum=0x10000;cp&=7;}
      else return fail(d,GT_BRIDGE_WIRE_UTF8);
      if(d->input_size-d->at<continuation)return fail(d,GT_BRIDGE_WIRE_UTF8);
      for(unsigned i=0;i<continuation;i++) {
        uint8_t c=d->input[d->at++];if((c&0xc0)!=0x80)return fail(d,GT_BRIDGE_WIRE_UTF8);
        cp=(cp<<6)|(c&63);
      }
      if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return fail(d,GT_BRIDGE_WIRE_UTF8);
    }
    if(!append_scalar(d,cp))return false;
    (*points)++;
  }
  return fail(d,GT_BRIDGE_WIRE_SYNTAX);
}
static bool key_insert(gt_bridge_wire_t *d,uint16_t id) {
  const json_node_t *n=&d->nodes[id];uint64_t hash=UINT64_C(14695981039346656037)^n->parent;
  for(size_t i=0;i<n->key_size;i++){hash^=d->strings[n->key+i];hash*=UINT64_C(1099511628211);}
  size_t slot=(size_t)hash&(KEY_SLOTS-1);
  for(;;slot=(slot+1)&(KEY_SLOTS-1)) {
    if(++d->key_work>KEY_WORK_MAX)return fail(d,GT_BRIDGE_WIRE_LIMIT);
    uint16_t other=d->keys[slot];if(!other){d->keys[slot]=id;return true;}
    const json_node_t *o=&d->nodes[other];
    if(o->parent==n->parent && o->key_size==n->key_size && !memcmp(d->strings+o->key,d->strings+n->key,n->key_size))return fail(d,GT_BRIDGE_WIRE_DUPLICATE);
  }
}
static bool number(gt_bridge_wire_t *d) {
  if(d->input[d->at]=='-')d->at++;
  if(d->at==d->input_size || !digit(d->input[d->at]))return fail(d,GT_BRIDGE_WIRE_SYNTAX);
  if(d->input[d->at]=='0')d->at++;else while(d->at<d->input_size && digit(d->input[d->at]))d->at++;
  if(d->at<d->input_size && d->input[d->at]=='.') {
    d->at++;size_t start=d->at;while(d->at<d->input_size && digit(d->input[d->at]))d->at++;
    if(d->at==start)return fail(d,GT_BRIDGE_WIRE_SYNTAX);
  }
  if(d->at<d->input_size && (d->input[d->at]=='e' || d->input[d->at]=='E')) {
    d->at++;if(d->at<d->input_size && (d->input[d->at]=='+' || d->input[d->at]=='-'))d->at++;
    size_t start=d->at;while(d->at<d->input_size && digit(d->input[d->at]))d->at++;
    if(d->at==start)return fail(d,GT_BRIDGE_WIRE_SYNTAX);
  }
  return true;
}
static uint16_t value(gt_bridge_wire_t *d,uint16_t parent,uint32_t key,uint32_t key_size,unsigned depth) {
  space(d);
  if(d->at==d->input_size){fail(d,GT_BRIDGE_WIRE_SYNTAX);return 0;}
  if(d->used==GT_BRIDGE_WIRE_NODES_MAX){fail(d,GT_BRIDGE_WIRE_LIMIT);return 0;}
  uint16_t id=++d->used;json_node_t *n=&d->nodes[id];n->parent=parent;n->key=key;n->key_size=key_size;n->start=(uint32_t)d->at;
  if(parent && d->nodes[parent].kind==J_OBJECT && !key_insert(d,id))return 0;
  uint8_t c=d->input[d->at];
  if(c=='{' || c=='[') {
    if(depth>=GT_BRIDGE_WIRE_DEPTH_MAX){fail(d,GT_BRIDGE_WIRE_LIMIT);return 0;}
    n->kind=c=='{'?J_OBJECT:J_ARRAY;uint8_t end=c=='{'?'}':']';d->at++;space(d);
    if(d->at<d->input_size && d->input[d->at]==end)d->at++;
    else {
      uint16_t tail=0;
      for(;;) {
        uint32_t offset=0,size=0,points=0;
        if(n->kind==J_OBJECT) {
          if(!string(d,&offset,&size,&points))return 0;
          space(d);if(d->at==d->input_size || d->input[d->at++]!=':'){fail(d,GT_BRIDGE_WIRE_SYNTAX);return 0;}
        }
        uint16_t child=value(d,id,offset,size,depth+1);if(!child)return 0;
        if(tail)d->nodes[tail].next=child;else n->child=child;tail=child;n->count++;
        space(d);if(d->at==d->input_size){fail(d,GT_BRIDGE_WIRE_SYNTAX);return 0;}
        c=d->input[d->at++];if(c==end)break;
        if(c!=','){fail(d,GT_BRIDGE_WIRE_SYNTAX);return 0;}
        space(d);
      }
    }
  } else if(c=='"') {
    n->kind=J_STRING;if(!string(d,&n->text,&n->size,&n->points))return 0;
  } else if(c=='-' || digit(c)) {
    n->kind=J_NUMBER;if(!number(d))return 0;
  } else {
    const char *literal=NULL;
    if(c=='t'){literal="true";n->kind=J_TRUE;}else if(c=='f'){literal="false";n->kind=J_FALSE;}else if(c=='n'){literal="null";n->kind=J_NULL;}
    if(!literal || d->input_size-d->at<strlen(literal) || memcmp(d->input+d->at,literal,strlen(literal))){fail(d,GT_BRIDGE_WIRE_SYNTAX);return 0;}
    d->at+=strlen(literal);
  }
  n->bytes=(uint32_t)d->at-n->start;return id;
}
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
/* Exact nonnegative integer value of a JSON number. Scan decimal coefficient
 * and exponent without floating point or locale. Huge zero exponents stay zero;
 * all other out-of-range/fractional results fail before conversion can wrap. */
static bool node_uint(const gt_bridge_wire_t *d,uint16_t id,uint64_t *out) {
  const json_node_t *n=&d->nodes[id];if(n->kind!=J_NUMBER)return false;
  const uint8_t *s=d->input+n->start;size_t len=n->bytes,at=s[0]=='-'?1:0;
  bool negative=at!=0,decimal=false;size_t digits=0,fraction=0,first=SIZE_MAX,last=0,coefficient_end;
  for(;at<len && s[at]!='e' && s[at]!='E';at++) {
    if(s[at]=='.'){decimal=true;continue;}
    if(s[at]!='0'){if(first==SIZE_MAX)first=digits;last=digits;}
    digits++;if(decimal)fraction++;
  }
  coefficient_end=at;int exponent=0;
  if(at<len) {
    at++;bool minus=false;if(s[at]=='+' || s[at]=='-'){minus=s[at]=='-';at++;}
    for(;at<len;at++)if(exponent<100000)exponent=exponent*10+s[at]-'0';
    if(minus)exponent=-exponent;
  }
  if(first==SIZE_MAX){*out=0;return true;}
  if(negative)return false;
  int64_t shift=(int64_t)exponent-(int64_t)fraction;
  if(shift<0 && (uint64_t)(-shift)>digits-1-last)return false;
  int64_t length=(int64_t)(digits-first)+shift;if(length<=0 || length>20)return false;
  size_t keep=shift<0?digits-(size_t)(-shift):digits,index=0;uint64_t result=0;
  for(at=0;at<coefficient_end;at++) {
    if(s[at]=='.')continue;
    if(index>=first && index<keep) {
      unsigned v=s[at]-'0';if(result>(UINT64_MAX-v)/10)return false;result=result*10+v;
    }
    index++;
  }
  for(int64_t i=0;i<shift;i++){if(result>UINT64_MAX/10)return false;result*=10;}
  *out=result;return true;
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
  gt_bridge_wire_t *d=a.allocate(a.context,sizeof(*d));if(!d)return GT_ERR_IO;
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
  const char *kinds[]={"offer","request","response","event","cancel","credit","resource.begin","resource.chunk","resource.end","close","hello","probe","accept","ready"};
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
uint16_t gt__bridge_wire_at(const gt_bridge_wire_t *d,uint16_t id,size_t at) {
  if(!d || !d->valid || !id || id>d->used || d->nodes[id].kind!=J_ARRAY)return 0;
  uint16_t child=d->nodes[id].child;while(child && at--){child=d->nodes[child].next;}return child;
}
void gt__bridge_wire_free(gt_bridge_wire_t *d) {
  if(!d)return;gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));a.release(a.context,d,sizeof(*d));
}
