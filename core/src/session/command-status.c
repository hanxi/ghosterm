#include "command-status.h"
#include <string.h>
static bool text_length(const char *s,size_t *length) {
  if(!s)return false;
  for(size_t i=0;i<=4096;i++)if(!s[i]){*length=i+1;return true;}
  return false;
}
gt_status_t gt__command_status_apply(gt_command_status_t *s,const gt_command_event_t *e) {
  if(!s || !e || !e->kind)return GT_ERR_INVALID_ARG;
  if(e->kind!=3 && e->kind!=4)return GT_OK;
  size_t length;
  if(!text_length(e->kind==3?e->command:e->error,&length))return GT_ERR_INVALID_ARG;
  /* Event strings may alias state fields. Stage the whole value before edit. */
  gt_command_status_t next=*s;
  if(e->kind==3) {
    memmove(next.command,e->command,length);next.error[0]=0;
    next.running=true;next.has_exit_code=false;next.exit_code=0;
    next.has_duration=false;next.duration_ns=0;
    next.has_start_time=e->has_time;next.start_ns=e->has_time?e->monotonic_ns:0;
  } else {
    memmove(next.error,e->error,length);
    next.has_exit_code=e->has_exit_code;next.exit_code=e->has_exit_code?e->exit_code:0;
    next.has_duration=s->running && s->has_start_time && e->has_time && e->monotonic_ns>=s->start_ns;
    next.duration_ns=next.has_duration?e->monotonic_ns-s->start_ns:0;
    next.running=false;next.has_start_time=false;next.start_ns=0;
  }
  *s=next;return GT_OK;
}
