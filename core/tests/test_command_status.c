#include "session/command-status.h"
#include <assert.h>
#include <string.h>
static void start(gt_command_status_t *s,const char *command,bool time,uint64_t now) {
  gt_command_event_t e={.kind=3,.command=command,.has_time=time,.monotonic_ns=now};
  assert(gt__command_status_apply(s,&e)==GT_OK);
}
static void end(gt_command_status_t *s,bool code,int32_t value,bool time,uint64_t now,const char *error) {
  gt_command_event_t e={.kind=4,.error=error,.has_exit_code=code,.exit_code=value,.has_time=time,.monotonic_ns=now};
  assert(gt__command_status_apply(s,&e)==GT_OK);
}
int main(int argc,char **argv) {
  assert(argc==2);gt_command_status_t s={0};
  if(!strcmp(argv[1],"lifecycle")) {
    start(&s,"make test",true,100);
    assert(s.running && s.has_start_time && !s.has_exit_code && !s.has_duration && !strcmp(s.command,"make test"));
    end(&s,true,-2,true,145,"failed");
    assert(!s.running && !s.has_start_time && s.has_exit_code && s.exit_code==-2 && s.has_duration && s.duration_ns==45);
    assert(!strcmp(s.command,"make test") && !strcmp(s.error,"failed"));
    gt_command_status_t before=s;
    for(unsigned kind=1;kind<=2;kind++) {
      gt_command_event_t e={.kind=kind};assert(gt__command_status_apply(&s,&e)==GT_OK && !memcmp(&s,&before,sizeof(s)));
    }
    gt_command_event_t future={.kind=123};assert(gt__command_status_apply(&s,&future)==GT_OK && !memcmp(&s,&before,sizeof(s)));
  } else if(!strcmp(argv[1],"unknown")) {
    end(&s,false,17,true,100,"");assert(!s.running && !s.has_exit_code && !s.has_duration);
    start(&s,"tool",true,200);end(&s,false,19,true,200,"");
    assert(!s.running && !s.has_exit_code && s.has_duration && s.duration_ns==0);
    end(&s,true,0,true,300,"");assert(s.has_exit_code && !s.exit_code && !s.has_duration);
  } else if(!strcmp(argv[1],"clock")) {
    start(&s,"a",false,0);assert(s.running && !s.has_start_time);
    end(&s,true,0,true,100,"");assert(!s.running && !s.has_duration);
    start(&s,"a",true,200);end(&s,true,0,false,0,"");assert(!s.has_duration);
    start(&s,"a",true,200);end(&s,true,0,true,199,"");assert(!s.has_duration);
    start(&s,"a",true,0);end(&s,true,0,true,UINT64_MAX,"");assert(s.has_duration && s.duration_ns==UINT64_MAX);
  } else if(!strcmp(argv[1],"copy")) {
    char command[]="编译",error[]="错误";
    start(&s,command,true,100);gt_command_status_t copy=s;command[0]='X';
    end(&s,true,7,true,150,error);error[0]='Y';
    assert(!strcmp(copy.command,"编译") && copy.running && !copy.has_exit_code && !copy.has_duration);
    assert(!strcmp(s.error,"错误"));
    start(&s,s.command,true,300);assert(!strcmp(s.command,"编译"));
    end(&s,true,9,true,301,s.command);assert(!strcmp(s.error,"编译"));
  } else if(!strcmp(argv[1],"restart")) {
    start(&s,"old",true,100);end(&s,true,4,true,110,"bad");start(&s,"new",true,300);
    assert(s.running && !s.has_exit_code && !s.has_duration && !s.error[0] && !strcmp(s.command,"new"));
    start(&s,"replacement",true,400);end(&s,true,0,true,425,"");
    assert(s.has_duration && s.duration_ns==25 && !strcmp(s.command,"replacement"));
  } else {
    assert(!strcmp(argv[1],"invalid"));start(&s,"retained",true,100);gt_command_status_t before=s;
    gt_command_event_t e={0};assert(gt__command_status_apply(&s,&e)==GT_ERR_INVALID_ARG);
    e.kind=3;assert(gt__command_status_apply(&s,&e)==GT_ERR_INVALID_ARG);
    char huge[4098];memset(huge,'x',sizeof(huge));huge[4097]=0;e.command=huge;
    assert(gt__command_status_apply(&s,&e)==GT_ERR_INVALID_ARG);
    e.kind=4;e.error=huge;assert(gt__command_status_apply(&s,&e)==GT_ERR_INVALID_ARG);
    assert(!memcmp(&s,&before,sizeof(s)));
    assert(gt__command_status_apply(NULL,&e)==GT_ERR_INVALID_ARG && gt__command_status_apply(&s,NULL)==GT_ERR_INVALID_ARG);
  }
  return 0;
}
