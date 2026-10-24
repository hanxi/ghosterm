#include "session-command-wire.h"
#include <string.h>
#include <limits.h>
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(24-8*i));}
static uint32_t get32(const uint8_t *p){uint32_t n=0;for(unsigned i=0;i<4;i++)n=(n<<8)|p[i];return n;}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static bool length(const char *s,size_t *n){for(size_t i=0;i<=4096;i++)if(!s[i]){*n=i;return true;}return false;}
bool csw_encode(const gt_command_status_t *s,void *buffer,size_t capacity,size_t *written) {
  if(written)*written=0;
  if(!s || !buffer || !written)return false;
  size_t command,error;
  if(!length(s->command,&command) || !length(s->error,&error))return false;
  if((s->running && (s->has_exit_code || s->has_duration)) || (!s->has_exit_code && s->exit_code) ||
     (!s->has_duration && s->duration_ns) || CSW_HEADER+command+error>capacity)return false;
  uint8_t *p=buffer;memset(p,0,CSW_HEADER);memcpy(p,"GCS1",4);
  p[4]=(s->running?1:0)|(s->has_exit_code?2:0)|(s->has_duration?4:0);
  put32(p+8,(uint32_t)s->exit_code);put64(p+12,s->duration_ns);
  put32(p+20,(uint32_t)command);put32(p+24,(uint32_t)error);
  memcpy(p+CSW_HEADER,s->command,command);memcpy(p+CSW_HEADER+command,s->error,error);
  *written=CSW_HEADER+command+error;return true;
}
bool csw_decode(const void *buffer,size_t n,gt_command_status_t *out) {
  if(!buffer || !out || n<CSW_HEADER || n>CSW_MAX)return false;
  const uint8_t *p=buffer;
  if(memcmp(p,"GCS1",4) || (p[4]&~7u) || p[5] || p[6] || p[7])return false;
  size_t command=get32(p+20),error=get32(p+24);
  if(command>4096 || error>4096 || command+error!=n-CSW_HEADER)return false;
  if(memchr(p+CSW_HEADER,0,command+error))return false;
  gt_command_status_t next={0};next.running=(p[4]&1)!=0;next.has_exit_code=(p[4]&2)!=0;next.has_duration=(p[4]&4)!=0;
  uint32_t code=get32(p+8);next.exit_code=code<=INT32_MAX?(int32_t)code:-1-(int32_t)(UINT32_MAX-code);
  next.duration_ns=get64(p+12);
  if((next.running && (next.has_exit_code || next.has_duration)) || (!next.has_exit_code && next.exit_code) ||
     (!next.has_duration && next.duration_ns))return false;
  memcpy(next.command,p+CSW_HEADER,command);memcpy(next.error,p+CSW_HEADER+command,error);
  *out=next;return true;
}
