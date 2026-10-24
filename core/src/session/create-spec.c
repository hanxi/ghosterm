#include "create-spec.h"
#include "io/pty-size.h"
#include "base/secure-random.h"
#include <string.h>
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static bool utf8(const uint8_t *p,size_t n) {
  for(size_t i=0;i<n;) {
    unsigned c=p[i++],count=0;uint8_t lo=0x80,hi=0xbf;
    if(c && c<0x80)continue;
    if(c>=0xc2 && c<=0xdf)count=1;
    else if(c>=0xe0 && c<=0xef){count=2;if(c==0xe0)lo=0xa0;if(c==0xed)hi=0x9f;}
    else if(c>=0xf0 && c<=0xf4){count=3;if(c==0xf0)lo=0x90;if(c==0xf4)hi=0x8f;}
    else return false;
    if(n-i<count || p[i]<lo || p[i]>hi)return false;
    i++;for(unsigned k=1;k<count;k++,i++)if(p[i]<0x80 || p[i]>0xbf)return false;
  }
  return true;
}
static size_t length(const char *s){if(!s)return SIZE_MAX;for(size_t i=0;i<=GT_CREATE_MAX_BYTES;i++)if(!s[i])return i;return SIZE_MAX;}
static bool key_start(unsigned char c){return c=='_' || (c>='A' && c<='Z') || (c>='a' && c<='z');}
static bool key_char(unsigned char c){return key_start(c) || (c>='0' && c<='9');}
static bool environment(const gt_create_spec_t *s) {
  for(uint32_t i=0;i<s->envc;i++) {
    const char *p=s->env[i];size_t n=length(p),key=0;if(n==SIZE_MAX || !key_start((unsigned char)p[0]))return false;
    while(key<n && p[key]!='='){if(!key_char((unsigned char)p[key]))return false;key++;}
    if(key==n || (key==4 && !memcmp(p,"TERM",4)))return false;
    for(uint32_t j=0;j<i;j++) {
      const char *eq=strchr(s->env[j],'=');
      if(eq && (size_t)(eq-s->env[j])==key && !memcmp(s->env[j],p,key))return false;
    }
  }
  return true;
}
static bool fields(const gt_create_spec_t *s) {
  if(!s || !nonzero(s->nonce,GT_CREATE_NONCE_BYTES) || !s->cols || !s->rows ||
     s->argc>GT_CREATE_MAX_ARGS || s->envc>GT_CREATE_MAX_ENV || !s->executable || s->executable[0]!='/' ||
     !s->cwd || s->cwd[0]!='/' || !s->term || !s->term[0] || length(s->term)>128 || !environment(s))return false;
  const char *fixed[]={s->executable,s->cwd,s->term};
  for(unsigned i=0;i<3;i++){size_t n=length(fixed[i]);if(n==SIZE_MAX || !utf8((const uint8_t *)fixed[i],n))return false;}
  for(uint32_t i=0;i<s->argc;i++){size_t n=length(s->argv[i]);if(n==SIZE_MAX || !utf8((const uint8_t *)s->argv[i],n))return false;}
  for(uint32_t i=0;i<s->envc;i++){size_t n=length(s->env[i]);if(n==SIZE_MAX || !utf8((const uint8_t *)s->env[i],n))return false;}
  return true;
}
static bool supported(const gt_create_spec_t *s){return (s->command==GT_PTY_CMD_SHELL || s->command==GT_PTY_CMD_DIRECT) && s->environment==GT_CREATE_HOST_ENV;}
static bool read_text(const uint8_t **p,size_t *remaining,char *storage,size_t capacity,size_t *used,const char **out) {
  if(*remaining<4)return false;
  size_t n=get32(*p);*p+=4;*remaining-=4;
  if(n>*remaining || *used>=capacity || n>=capacity-*used || !utf8(*p,n))return false;
  memcpy(storage+*used,*p,n);storage[*used+n]=0;*out=storage+*used;*used+=n+1;*p+=n;*remaining-=n;return true;
}
gt_status_t gt__create_spec_decode(const void *bytes,size_t n,gt_create_spec_t *out,char *storage,size_t capacity) {
  if(out)memset(out,0,sizeof(*out));
  if(!bytes || !out || !storage || capacity<GT_CREATE_MAX_BYTES)return GT_ERR_INVALID_ARG;
  capacity=GT_CREATE_MAX_BYTES;
  gt_status_t rc=GT_ERR_PROTOCOL;
  if(n<GT_CREATE_PREFIX_BYTES || n>GT_CREATE_MAX_BYTES)goto bad;
  const uint8_t *p=bytes;
  if(!nonzero(p,GT_CREATE_NONCE_BYTES) || get32(p+44))goto bad;
  memcpy(out->nonce,p,GT_CREATE_NONCE_BYTES);out->command=(gt_pty_cmd_kind_t)get32(p+16);
  out->cols=(uint16_t)(((uint16_t)p[20]<<8)|p[21]);out->rows=(uint16_t)(((uint16_t)p[22]<<8)|p[23]);
  out->px_width=get32(p+24);out->px_height=get32(p+28);out->environment=get32(p+32);out->argc=get32(p+36);out->envc=get32(p+40);
  if(!out->cols || !out->rows || out->argc>GT_CREATE_MAX_ARGS || out->envc>GT_CREATE_MAX_ENV)goto bad;
  size_t remaining=n-GT_CREATE_PREFIX_BYTES,used=0;p+=GT_CREATE_PREFIX_BYTES;
  if(!read_text(&p,&remaining,storage,capacity,&used,&out->executable) ||
     !read_text(&p,&remaining,storage,capacity,&used,&out->cwd) ||
     !read_text(&p,&remaining,storage,capacity,&used,&out->term))goto bad;
  if(out->executable[0]!='/' || out->cwd[0]!='/' || !out->term[0] || length(out->term)>128)goto bad;
  for(uint32_t i=0;i<out->argc;i++)if(!read_text(&p,&remaining,storage,capacity,&used,&out->argv[i]))goto bad;
  for(uint32_t i=0;i<out->envc;i++)if(!read_text(&p,&remaining,storage,capacity,&used,&out->env[i]))goto bad;
  if(remaining || !environment(out))goto bad;
  if(supported(out))return GT_OK;
  rc=GT_ERR_UNSUPPORTED;
bad:
  memset(out,0,sizeof(*out));gt__secure_erase(storage,capacity);return rc;
}
static bool write_text(uint8_t *p,size_t capacity,size_t *used,const char *s) {
  size_t n=length(s);if(n==SIZE_MAX || capacity-*used<4 || n>capacity-*used-4 || !utf8((const uint8_t *)s,n))return false;
  put32(p+*used,(uint32_t)n);memcpy(p+*used+4,s,n);*used+=n+4;return true;
}
gt_status_t gt__create_spec_encode(const gt_create_spec_t *s,uint8_t *out,size_t capacity,size_t *n) {
  if(n)*n=0;
  if(!s || !out || !n || capacity<GT_CREATE_PREFIX_BYTES)return GT_ERR_INVALID_ARG;
  if(capacity>GT_CREATE_MAX_BYTES)capacity=GT_CREATE_MAX_BYTES;
  size_t used=GT_CREATE_PREFIX_BYTES;memset(out,0,used);
  if(!fields(s))goto invalid;
  if(!supported(s)){gt__secure_erase(out,used);return GT_ERR_UNSUPPORTED;}
  memcpy(out,s->nonce,GT_CREATE_NONCE_BYTES);put32(out+16,(uint32_t)s->command);
  out[20]=(uint8_t)(s->cols>>8);out[21]=(uint8_t)s->cols;out[22]=(uint8_t)(s->rows>>8);out[23]=(uint8_t)s->rows;
  put32(out+24,s->px_width);put32(out+28,s->px_height);put32(out+32,s->environment);put32(out+36,s->argc);put32(out+40,s->envc);
  if(!write_text(out,capacity,&used,s->executable) || !write_text(out,capacity,&used,s->cwd) || !write_text(out,capacity,&used,s->term))goto invalid;
  for(uint32_t i=0;i<s->argc;i++)if(!write_text(out,capacity,&used,s->argv[i]))goto invalid;
  for(uint32_t i=0;i<s->envc;i++)if(!write_text(out,capacity,&used,s->env[i]))goto invalid;
  *n=used;return GT_OK;
invalid:
  gt__secure_erase(out,used);return GT_ERR_INVALID_ARG;
}
gt_status_t gt__create_spec_pty(const gt_create_spec_t *s,gt_pty_opts_t *out,
  const char *args[GT_CREATE_MAX_ARGS+1],const char *env[GT_CREATE_MAX_ENV+1]) {
  if(out)memset(out,0,sizeof(*out));
  if(!out || !args || !env || !fields(s))return GT_ERR_INVALID_ARG;
  if(!supported(s) || gt__pty_size_check(s->cols,s->rows,s->px_width,s->px_height,true)!=GT_OK)return GT_ERR_UNSUPPORTED;
  for(uint32_t i=0;i<s->argc;i++)args[i]=s->argv[i];
  args[s->argc]=NULL;
  for(uint32_t i=0;i<s->envc;i++)env[i]=s->env[i];
  env[s->envc]=NULL;
  *out=(gt_pty_opts_t){.executable=s->executable,.argv=args,.env=env,.cwd=s->cwd,.term=s->term,
    .cmd_kind=s->command,.cols=s->cols,.rows=s->rows,.px_width=s->px_width,.px_height=s->px_height};return GT_OK;
}
