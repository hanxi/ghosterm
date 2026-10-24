/* Real attachment + largest admitted summary/output envelope, no fake queue. */
#include "session-summary-wire.h"
#include "session-process-wire.h"
#include "session/attachment.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
  assert(argc==2);
  bool pressure=!strcmp(argv[1],"pressure");assert(pressure || !strcmp(argv[1],"largest"));
  gt_program_status_store_t *store=gt__program_status_open(PSW_RECORDS,PSW_STRINGS);assert(store);
  char text[4097];memset(text,'x',4096);text[2041]=0;
  for(unsigned i=0;i<32;i++) {
    char id[16];snprintf(id,sizeof(id),"r%02u",i);
    gt_program_status_record_t r={id,text,"","",1,0,-1};
    assert(gt__program_status_apply(store,&r)==GT_STATUS_OK);
  }
  assert(gt__program_status_bytes(store)==PSW_STRINGS);
  uint8_t *bundle=calloc(PSW_RESTORE_BYTES,1);assert(bundle);
  size_t n=0;assert(psw_encode(store,bundle+16+PW_CHUNK,PSW_MAX,&n));
  assert(n==66056 && n>65536 && n<=PSW_MAX);
  gt_program_status_store_t *decoded=NULL;assert(psw_decode(bundle+16+PW_CHUNK,n,&decoded));
  assert(gt__program_status_count(decoded)==32 && gt__program_status_bytes(decoded)==PSW_STRINGS);
  gt_program_status_record_t last;assert(gt__program_status_at(decoded,31,&last));
  assert(!strcmp(last.id,"r31") && strlen(last.app)==2041 && last.progress==-1);
  gt__program_status_close(decoded);
  gt_command_status_t command={.has_exit_code=true,.exit_code=-2,.has_duration=true,.duration_ns=UINT64_MAX};
  memset(command.command,'c',4096);memset(command.error,'e',4096);size_t command_length=0;
  assert(csw_encode(&command,bundle+16+PW_CHUNK+n,CSW_MAX,&command_length) && command_length==CSW_MAX);
  gt_command_status_t copy;assert(csw_decode(bundle+16+PW_CHUNK+n,command_length,&copy));
  assert(copy.has_exit_code && copy.exit_code==-2 && copy.has_duration && copy.duration_ns==UINT64_MAX);
  assert(strlen(copy.command)==4096 && strlen(copy.error)==4096);
  pw_put64(bundle,PW_CHUNK);pw_put64(bundle+8,n);memset(bundle+16,0x5a,PW_CHUNK);
  const size_t length=16+PW_CHUNK+n+command_length;assert(length<=PSW_RESTORE_BYTES && length>65536);
  gt_checkpoint_tasks_t *pool=gt__tasks_open(1);assert(pool);
  gt_session_attachment_t *a=gt__attachment_open(pool,PSW_RESTORE_BYTES,4);assert(a);
  uint64_t generation=0;assert(gt__attachment_attach(a,&generation)==GT_TASK_OK);
  assert(gt__attachment_begin(a,generation,10)==GT_RESTORE_OK);
  assert(gt__attachment_push(a,11,PW_STATE_OUTPUT_SUMMARY,bundle,length)==GT_RESTORE_OK);
  gt_attachment_restore_info_t info;assert(gt__attachment_restore_info(a,generation,&info)==GT_RESTORE_OK);
  assert(info.bytes==length && info.events==1 && info.last==11);
  if(pressure) {
    assert(gt__attachment_push(a,12,PW_STATE_OUTPUT_SUMMARY,bundle,length)==GT_RESTORE_LIMIT);
    assert(!gt__attachment_current(a,generation) && !gt__attachment_input_allowed(a,generation,11));
    assert(gt__attachment_restore_info(a,generation,&info)==GT_RESTORE_OK);
    assert(info.phase==GT_RESTORE_FAILED && info.failure==GT_RESTORE_LIMIT && !info.bytes && !info.events);
    uint8_t bytes[16];gt_restore_event_t event;
    assert(gt__attachment_next(a,generation,&event,bytes,sizeof(bytes))==GT_RESTORE_LIMIT);
    uint64_t next=0;assert(gt__attachment_attach(a,&next)==GT_TASK_OK && next>generation);
    assert(gt__attachment_begin(a,next,20)==GT_RESTORE_OK);
    assert(gt__attachment_push(a,21,PW_STATE_OUTPUT_SUMMARY,bundle,length)==GT_RESTORE_OK);
    assert(!gt__attachment_current(a,generation) && gt__attachment_current(a,next));generation=next;
  }
  assert(gt__attachment_detach(a,generation)==GT_TASK_OK);
  assert(gt__attachment_close(a)==GT_TASK_OK && gt__tasks_close(pool)==GT_TASK_OK);
  gt__program_status_close(store);free(bundle);return 0;
}
