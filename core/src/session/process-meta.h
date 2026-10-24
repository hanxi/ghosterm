/* Private canonical UTF-8 validation shared by Source observation and DATA. */
#ifndef GT_SESSION_PROCESS_META_H
#define GT_SESSION_PROCESS_META_H
#include "ghosterm/session.h"
#include <string.h>
static inline bool gt__process_valid(const gt_session_process_t *m) {
  if(!m || (unsigned)m->child>GT_TRANSPORT_CHILD_JOB)return false;
  const char *end=memchr(m->cwd,0,sizeof(m->cwd));
  if(!end || (!m->has_cwd && end!=m->cwd) || (m->has_cwd && m->cwd[0]!='/'))return false;
  const unsigned char *p=(const unsigned char *)m->cwd,*stop=(const unsigned char *)end;
  while(p<stop) {
    uint32_t cp=*p++;unsigned tails=0;uint32_t minimum=0;
    if(cp<128)continue;
    if(cp>=0xc2 && cp<=0xdf){cp&=31;tails=1;minimum=0x80;}
    else if(cp>=0xe0 && cp<=0xef){cp&=15;tails=2;minimum=0x800;}
    else if(cp>=0xf0 && cp<=0xf4){cp&=7;tails=3;minimum=0x10000;}
    else return false;
    if((size_t)(stop-p)<tails)return false;
    for(unsigned i=0;i<tails;i++){if((*p&0xc0)!=0x80)return false;cp=(cp<<6)|(*p++&63);}
    if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return false;
  }
  return true;
}
static inline bool gt__process_equal(const gt_session_process_t *a,const gt_session_process_t *b) {
  return a->child==b->child && a->has_cwd==b->has_cwd && !strcmp(a->cwd,b->cwd);
}
#endif
