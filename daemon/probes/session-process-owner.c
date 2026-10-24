/* Headless P0 authority: real worker/PTY survives actual Metal-client SIGKILL.
 * Inherited socketpair only, no production listener or public protocol. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "session-process-wire.h"
#include "session-main-stack.h"
#include "session-process-build.h"
#include "session/manager.h"
#include "session/program-status.h"
#include "session/command-status.h"
#include "session-summary-wire.h"
#include "session-command-wire.h"
#include "session-result-wire.h"
#include "session-history-wire.h"
#include "ipc/attachment-peer.h"
#include "frozen-budget.h"
#include <errno.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
#define MIB (1024u*1024u)
extern char **environ;
typedef struct server server;
typedef struct {
  server *owner;
  p0_budget_scope capture,encoder;
  p0_frozen_checkpoint frozen;
  p0_checkpoint wire;
  gt_program_status_store_t *summary;
  unsigned summary_phase;uint32_t dark;hsw_state history_state;
  gt_command_status_t command;
  uint64_t sequence,task;
  double freeze_ms,encode_ms;
  bool captured;
} checkpoint;
struct server {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  p0_budget budget;
  p0_budget_scope main_stack,diagnostic,source,reference,worker_stack,encoder_stack,waiter_stack;
  gt_thread_options_t thread_options;size_t thread_reserve,main_reserve;
  GhosttyTerminal terminal;
  gt_program_status_store_t *summary;
  gt_command_status_t command;
  gt_status_t summary_error;
  bool summary_dirty,command_dirty;
  gt_session_manager_t *manager;uint64_t session;bool managed_disposed;
  gt_session_attachment_t *attachment;
  gt_checkpoint_tasks_t *tasks;
  checkpoint *capture;
  gt_worker_reply_sink_t sink;
  uint64_t generation,events;
  _Atomic uint64_t published;
  char output[128*1024];size_t used;
  bool backend_started,backend_oom,backend_attempted,state_oom,state_ready,engine_attempted;unsigned partial_cleanup;bool pointer,ime,history,read_error,apply_error,reply_error,invalid_state;atomic_bool inject_read_error;
  bool finished;gt_status_t status;gt_worker_result_t worker_result;
  bool history_policy;atomic_uint history_state;
  bool native_ime,native_pointer;unsigned tui;bool tmux,scheme;uint32_t dark;unsigned scheme_replies;
  unsigned replies;size_t reply_bytes,peer_bytes,backend_bytes;
  p0_checkpoint final;
};
/* Probe composition root retains saved state/attachment storage after the
 * manager has released its worker-context pin. Stack leases stay admitted
 * until that actual join completes; final source cleanup remains below. */
static void process_dispose(void *context) {
  server *s=context;CHECK(!s->managed_disposed && s->finished && !s->terminal && !s->backend_bytes);
  s->managed_disposed=true;
}
static bool process_submit(server *s,gt_worker_command_t kind,const void *bytes,size_t length,
  gt_worker_completion_t callback,void *context) {
  gt_worker_submit_t result=GT_WORKER_INVALID;
  return gt__manager_submit_internal(s->manager,s->session,kind,bytes,length,callback,context,&result)==GT_MANAGER_OK && result==GT_WORKER_ACCEPTED;
}
static bool process_result(server *s,gt_worker_result_t *result) {
  gt_managed_info_t info;
  if(gt__manager_info(s->manager,s->session,&info)!=GT_MANAGER_OK || !info.has_result)return false;
  *result=info.result;return true;
}
static void process_reap(server *s) {
  CHECK(gt__manager_reap(s->manager,s->session)==GT_MANAGER_OK);
  gt_managed_info_t info;CHECK(gt__manager_info(s->manager,s->session,&info)==GT_MANAGER_OK);
  CHECK(info.phase==GT_MANAGED_RETAINED && info.has_result && s->managed_disposed);
}
static void process_running(server *s) {
  gt_managed_info_t rows[1];size_t count=0;
  CHECK(gt__manager_list(s->manager,rows,1,&count)==GT_MANAGER_OK && count==1);
  CHECK(rows[0].id==s->session && rows[0].phase==GT_MANAGED_WORKER && !rows[0].has_result && !rows[0].termination_requested && !s->managed_disposed);
}
static void process_registry_close(server *s) {
  CHECK(s->managed_disposed && gt__manager_forget(s->manager,s->session)==GT_MANAGER_OK);
  size_t count=99;CHECK(gt__manager_list(s->manager,NULL,0,&count)==GT_MANAGER_OK && !count);
  CHECK(gt__manager_close(s->manager)==GT_MANAGER_OK);s->manager=NULL;s->session=0;
}
/* Measurements are observations, never a performance pass threshold. -1
 * explicitly records a clock failure instead of fabricating zero latency. */
static double metric_clock(void) {
  struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return -1;
  return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
}
static double metric_elapsed(double start) {
  double end=metric_clock();return start<0 || end<0 ? -1 : end-start;
}
static void wait_output(server *s,const char *text) {
  struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&s->lock));
  while(!strstr(s->output,text))CHECK(!pthread_cond_timedwait(&s->changed,&s->lock,&end));
  CHECK(!pthread_mutex_unlock(&s->lock));
}
static void reply(GhosttyTerminal terminal,void *context,const uint8_t *bytes,size_t n) {
  (void)terminal;server *s=context;
  if(s->scheme && n==9) {
    const char *expected=s->dark?"\x1b[?997;1n":"\x1b[?997;2n";
    CHECK(!memcmp(bytes,expected,n));s->scheme_replies++;CHECK(s->sink.write(s->sink.context,bytes,n)==GT_OK);return;
  }
  if(!s->tmux && !s->tui) {
  if(s->reply_error && atomic_load(&s->inject_read_error))CHECK(n==4 && !memcmp(bytes,"\x1b[0n",4));
  else CHECK(n==7 && !memcmp(bytes,"\x1b[1;12R",7));
  }
  s->replies++;s->reply_bytes+=n;CHECK(s->sink.write(s->sink.context,bytes,n)==GT_OK);
}
static hsw_state history_state(server *s) {
  return s->history_policy?(hsw_state){MIB,100,atomic_load(&s->history_state)}:(hsw_state){0};
}
static void history_pruned(void *context) {
  server *s=context;atomic_store(&s->history_state,HSW_TRUNCATED);
}
static gt_restore_rc_t push_state(server *s,uint64_t sequence,uint32_t kind,const void *bytes,size_t length) {
  if(!s->history_policy)return gt__attachment_push(s->attachment,sequence,kind,bytes,length);
  uint8_t wrapped[PSW_RESTORE_BYTES+HSW_EVENT_PREFIX];CHECK(length<=sizeof(wrapped)-HSW_EVENT_PREFIX);
  pw_put64(wrapped,kind);CHECK(hsw_encode(history_state(s),wrapped+8,HSW_BYTES));
  memcpy(wrapped+HSW_EVENT_PREFIX,bytes,length);
  return gt__attachment_push(s->attachment,sequence,PW_STATE_HISTORY,wrapped,length+HSW_EVENT_PREFIX);
}
static void require_summary(const gt_program_status_store_t *summary,unsigned phase) {
  if(phase==3){CHECK(!gt__program_status_count(summary));return;}
  gt_program_status_record_t r;
  CHECK(gt__program_status_count(summary)==1 && gt__program_status_at(summary,0,&r));
  CHECK(!strcmp(r.id,"build") && !strcmp(r.app,"ghosterm") && !strcmp(r.title,""));
  CHECK(r.state==(phase==0?1u:phase==1?3u:2u));
  CHECK(r.kind==(phase==1?1u:0u) && r.progress==(phase==0?25:-1));
  CHECK(!strcmp(r.message,phase==0?"Working":phase==1?"Approve?":"Done"));
}
static void program_status(GhosttyTerminal terminal,void *context,const GhosttyTerminalProgramStatus *report) {
  (void)terminal;server *s=context;
  if(s->summary_error!=GT_OK)return;
  if(report->size<sizeof(*report)){s->summary_error=GT_ERR_PROTOCOL;return;}
  const GhosttyString fields[]={report->id,report->app,report->title,report->message};
  char strings[4][4097];
  for(unsigned i=0;i<4;i++) {
    if(fields[i].len>4096 || (fields[i].len && (!fields[i].ptr || memchr(fields[i].ptr,0,fields[i].len)))) {
      s->summary_error=GT_ERR_PROTOCOL;return;
    }
    if(fields[i].len)memcpy(strings[i],fields[i].ptr,fields[i].len);
    strings[i][fields[i].len]=0;
  }
  gt_program_status_record_t r={strings[0],strings[1],strings[2],strings[3],
    (unsigned)report->state,(unsigned)report->kind,report->progress};
  if(gt__program_status_apply(s->summary,&r)!=GT_STATUS_OK)s->summary_error=GT_ERR_IO;
  else s->summary_dirty=true;
}
static void require_command(const gt_command_status_t *c,unsigned phase) {
  if(phase==3) {
    CHECK(!c->command[0] && !c->error[0] && !c->running && !c->has_exit_code && !c->exit_code);
    CHECK(!c->has_duration && !c->duration_ns && !c->has_start_time && !c->start_ns);return;
  }
  CHECK(!strcmp(c->command,phase==2?"next command":"ghosterm test"));
  CHECK(!strcmp(c->error,phase==0?"":phase==1?"detached failure":"unknown exit failure"));
  CHECK(c->running==(phase==0));
  CHECK(c->has_exit_code==(phase==1));
  if(phase==1)CHECK(c->exit_code==7);
  CHECK(c->has_duration==(phase!=0));
}
static void semantic_prompt(GhosttyTerminal terminal,void *context,const GhosttyTerminalSemanticPrompt *report) {
  (void)terminal;server *s=context;if(s->summary_error!=GT_OK)return;
  if(report->size<sizeof(*report)){s->summary_error=GT_ERR_PROTOCOL;return;}
  const GhosttyString fields[]={report->command,report->error};char strings[2][4097];
  for(unsigned i=0;i<2;i++) {
    if(fields[i].len>4096 || (fields[i].len && (!fields[i].ptr || memchr(fields[i].ptr,0,fields[i].len)))) {
      s->summary_error=GT_ERR_PROTOCOL;return;
    }
    if(fields[i].len)memcpy(strings[i],fields[i].ptr,fields[i].len);
    strings[i][fields[i].len]=0;
  }
  gt_command_event_t e={.kind=(unsigned)report->kind,.command=strings[0],.error=strings[1],
    .has_exit_code=report->has_exit_code,.exit_code=report->exit_code};
  struct timespec now;
  if(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec>=0 && (uint64_t)now.tv_sec<=UINT64_MAX/1000000000u) {
    e.has_time=true;e.monotonic_ns=(uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
  }
  if(e.kind==1 || e.kind==2)CHECK(gt__program_status_count(s->summary)==5);
  size_t removed=gt__program_status_semantic(s->summary,e.kind);
  if(removed)s->summary_dirty=true;
  if(e.kind==2)CHECK(!removed && gt__program_status_count(s->summary)==5);
  if(e.kind==1) {
    CHECK(removed==2 && gt__program_status_count(s->summary)==3);
    gt_program_status_record_t r;
    CHECK(gt__program_status_at(s->summary,1,&r) && r.state==2 && !strcmp(r.id,"temp/done"));
    CHECK(gt__program_status_at(s->summary,2,&r) && r.state==4 && !strcmp(r.id,"temp/error"));
  }
  gt_status_t rc=gt__command_status_apply(&s->command,&e);if(rc!=GT_OK)s->summary_error=rc;
  else if(e.kind==3 || e.kind==4)s->command_dirty=true;
}
static void *scope_allocate(void *context,size_t n) {
  const GhosttyAllocator *a=p0_budget_allocator(context,P0_BUDGET_SCRATCH);
  return a->vtable->alloc(a->ctx,n,0,0);
}
static void scope_release(void *context,void *bytes,size_t n) {
  const GhosttyAllocator *a=p0_budget_allocator(context,P0_BUDGET_SCRATCH);
  a->vtable->free(a->ctx,bytes,n,0,0);
}
static gt_status_allocator_t scope_allocator(p0_budget_scope *scope) {
  return (gt_status_allocator_t){scope,scope_allocate,scope_release};
}
/* Peer adapter is used only by the IPC owner, independently of worker traffic. */
static void *peer_allocate(void *context,size_t n) {
  server *s=context;void *p=scope_allocate((s->state_oom||s->backend_oom)?&s->diagnostic:&s->source,n);if(p)s->peer_bytes+=n;return p;
}
static void peer_release(void *context,void *p,size_t n) {
  server *s=context;CHECK(n<=s->peer_bytes);scope_release((s->state_oom||s->backend_oom)?&s->diagnostic:&s->source,p,n);s->peer_bytes-=n;
}
static void require_thread_budget_for(server *s,p0_budget_scope *scope,pthread_t thread) {
  p0_budget_usage usage=p0_budget_scope_read(scope);
  CHECK(usage.committed==s->thread_reserve && !usage.live);
  size_t actual=pthread_get_stacksize_np(thread);
  CHECK(actual>=s->thread_options.stack_size && actual<=s->thread_reserve-s->thread_options.guard_size);
  uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(thread);char local;
  if(pthread_equal(thread,pthread_self()))CHECK((uintptr_t)&local<top && (uintptr_t)&local>=top-actual);
  mach_vm_address_t guard=top-actual-1,address=guard;mach_vm_size_t size=0;
  vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
  CHECK(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
  CHECK(address<=guard && size>=s->thread_options.guard_size && address+size>guard && info.protection==VM_PROT_NONE);
  if(object)CHECK(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
  printf("METRIC process/thread role=%s stack_requested_bytes=%zu stack_reported_bytes=%zu guard_region_bytes=%llu reserved_bytes=%zu\n",
    scope==&s->worker_stack?"worker":scope==&s->encoder_stack?"encoder":"waiter",s->thread_options.stack_size,actual,(unsigned long long)size,s->thread_reserve);
}
static void require_thread_budget(server *s,p0_budget_scope *scope) {
  require_thread_budget_for(s,scope,pthread_self());
}
static bool tui_device_attributes(GhosttyTerminal terminal,void *context,GhosttyDeviceAttributes *out) {
  (void)terminal;(void)context;*out=(GhosttyDeviceAttributes){0};
  out->primary.conformance_level=GHOSTTY_DA_CONFORMANCE_VT220;
  out->primary.features[0]=GHOSTTY_DA_FEATURE_COLUMNS_132;out->primary.features[1]=GHOSTTY_DA_FEATURE_ANSI_COLOR;out->primary.num_features=2;
  out->secondary.device_type=GHOSTTY_DA_DEVICE_TYPE_VT220;out->secondary.firmware_version=10;return true;
}
static bool process_color_query(GhosttyTerminal terminal,void *context,GhosttyColorScheme *out) {
  (void)terminal;server *s=context;require_thread_budget(s,&s->worker_stack);
  *out=s->dark?GHOSTTY_COLOR_SCHEME_DARK:GHOSTTY_COLOR_SCHEME_LIGHT;return true;
}
static GhosttyResult process_color_defaults(server *s) {
  GhosttyColorRgb background=s->dark?(GhosttyColorRgb){0x12,0x34,0x56}:(GhosttyColorRgb){0xab,0xcd,0xef};
  const GhosttyColorRgb foreground={255,255,255};
  GhosttyResult rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,&background);
  if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,&foreground);
  return p0_budget_result(&s->source,rc);
}
static gt_status_t open_state(void *context,gt_worker_reply_sink_t sink,void **out) {
  server *s=context;s->sink=sink;require_thread_budget(s,&s->worker_stack);
  gt_status_allocator_t allocator=scope_allocator(&s->source);
  size_t before=p0_budget_scope_read(&s->source).live;
  s->summary=gt__program_status_open_with_allocator(32,65536,&allocator);if(!s->summary)return GT_ERR_IO;
  *out=s;
  CHECK(p0_budget_scope_read(&s->source).live>before);
  s->engine_attempted=true;
  GhosttyResult created=ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(&s->source,P0_BUDGET_GENERAL),
      p0_budget_allocator(&s->source,P0_BUDGET_PAGES),&s->terminal,80,24);
  if(p0_budget_result(&s->source,created)!=GHOSTTY_SUCCESS)return GT_ERR_IO;
  if(p0_budget_resize_cells(&s->source,s->terminal,80,24,10,20)!=GHOSTTY_SUCCESS)return GT_ERR_IO;
  const size_t continuation=MIB;const uint64_t images=MIB;
  CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES,&continuation)==GHOSTTY_SUCCESS);
  CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&images)==GHOSTTY_SUCCESS);
  CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_USERDATA,s)==GHOSTTY_SUCCESS);
  CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_WRITE_PTY,(const void *)reply)==GHOSTTY_SUCCESS);
  CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_PROGRAM_STATUS,(const void *)program_status)==GHOSTTY_SUCCESS);
  CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT,(const void *)semantic_prompt)==GHOSTTY_SUCCESS);
  if(s->scheme) {
    CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,(const void *)process_color_query)==GHOSTTY_SUCCESS);
    if(process_color_defaults(s)!=GHOSTTY_SUCCESS)return GT_ERR_IO;
  }
  if(s->tui)CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,(const void *)tui_device_attributes)==GHOSTTY_SUCCESS);
  if(s->history_policy) {
    const size_t history_bytes=MIB,history_lines=100;
    if(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,&history_bytes)!=GHOSTTY_SUCCESS ||
       ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&history_lines)!=GHOSTTY_SUCCESS ||
       ghostty_terminal_p0_set_history_observer(s->terminal,s,history_pruned)!=GHOSTTY_SUCCESS)return GT_ERR_IO;
    size_t actual_bytes=0,actual_lines=0;
    CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_BYTES,&actual_bytes)==GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_LINES,&actual_lines)==GHOSTTY_SUCCESS);
    CHECK(actual_bytes==MIB && actual_lines==100);
    atomic_store(&s->history_state,HSW_COMPLETE);
  }
  s->state_ready=true;*out=s;return GT_OK;
}
static void *backend_allocate(void *context,size_t n) {
  server *s=context;void *p=scope_allocate(&s->source,n);if(p)s->backend_bytes+=n;return p;
}
static void backend_release(void *context,void *p,size_t n) {
  server *s=context;CHECK(n<=s->backend_bytes);scope_release(&s->source,p,n);s->backend_bytes-=n;
}
typedef struct {gt_io_backend_t base;gt_io_backend_t *real;server *owner;} fault_backend_t;
static int fault_read(gt_io_backend_t *base,uint8_t *p,size_t n,int timeout) {
  fault_backend_t *b=(fault_backend_t *)base;
  if(b->owner->reply_error && atomic_load(&b->owner->inject_read_error)) {
    const char query[]="\x1b[5n";CHECK(n>=sizeof(query)-1);memcpy(p,query,sizeof(query)-1);return sizeof(query)-1;
  }
  if(b->owner->apply_error && atomic_load(&b->owner->inject_read_error)) {
    const char poison[]="\x1b[2J\x1b[H\x1b[48;2;220;30;30mPOISON";
    CHECK(n>=sizeof(poison)-1);memcpy(p,poison,sizeof(poison)-1);return sizeof(poison)-1;
  }
  if(atomic_load(&b->owner->inject_read_error))return GT_TRANSPORT_READ_ERROR;
  return b->real->ops->read(b->real,p,n,timeout);
}
static int fault_write(gt_io_backend_t *base,const uint8_t *p,size_t n){
  fault_backend_t *b=(fault_backend_t *)base;
  if(b->owner->reply_error && atomic_load(&b->owner->inject_read_error))return GT_ERR_IO;
  return b->real->ops->write(b->real,p,n);
}
static int fault_resize(gt_io_backend_t *base,uint16_t c,uint16_t r,uint32_t x,uint32_t y){fault_backend_t *b=(fault_backend_t *)base;return b->real->ops->resize(b->real,c,r,x,y);}
static gt_status_t fault_exit(const gt_io_backend_t *base,gt_io_exit_t *out){const fault_backend_t *b=(const fault_backend_t *)base;return b->real->ops->observe_exit?b->real->ops->observe_exit(b->real,out):GT_ERR_UNSUPPORTED;}
static void fault_close(gt_io_backend_t *base){fault_backend_t *b=(fault_backend_t *)base;server *s=b->owner;b->real->ops->close(b->real);backend_release(s,b,sizeof(*b));}
static const gt_io_ops_t fault_ops={.read=fault_read,.write=fault_write,.resize=fault_resize,.close=fault_close,.observe_exit=fault_exit};
static gt_io_backend_t *open_backend(void *context) {
  server *s=context;
  const char *script=
    "if [ \"$0\" = scheme ]; then stty -echo -icanon min 1 time 0; printf '\\033[?996n'; "
    "color=$(dd bs=1 count=9 2>/dev/null); [ \"$color\" = \"$(printf '\\033[?997;2n')\" ] || exit 41; stty icanon; fi; "
    "if [ \"$0\" = pointer ] || [ \"$0\" = native-pointer ]; then printf '\\033[?1000h\\033[?1006h\\033[?1004h'; fi; "
    "stty -echo; printf '\\033]133;C;cmdline=ghosterm test\\007\\033]7501;id=build:app=ghosterm:state=working:progress=25:msg=V29ya2luZw==\\007\\033]2;Ghosterm initial\\007\\033]7;file:///ghosterm/initial\\007\\033[?25l\\033[48;2;220;30;30m\\033[38;2;255;255;255mFIRST\\033[?2004'; "
    "if [ \"$0\" = history-limit ]; then read prune; [ \"$prune\" = PRUNE ] || exit 44; "
    "printf h; awk 'BEGIN{for(i=0;i<1200;i++)printf \"ROW%06d\\r\\n\",i}'; "
    "printf '\\033[2J\\033[H\\033[48;2;220;30;30m\\033[38;2;255;255;255mFIRST\\033]2;HISTORY_BURST_END\\007\\033]2;Ghosterm initial\\007\\033[?2004'; fi; "
    "read x; stty -icanon min 1 time 0; printf 'h\\033]133;D;7;err=detached failure\\007\\033]7501;id=build:app=ghosterm:state=blocked:kind=permission:msg=QXBwcm92ZT8=\\007\\033]2;Ghosterm detached\\007\\033]7;file:///ghosterm/detached\\007\\033[48;2;30;220;30mSECOND\\033[6n'; "
    "answer=$(dd bs=1 count=7 2>/dev/null); expected=$(printf '\\033[1;12R'); "
    "[ \"$answer\" = \"$expected\" ] || exit 17; "
    "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); [ -z \"$extra\" ] || exit 18; "
    "printf VERIFIED; stty min 1 time 0; "
    "if [ \"$0\" = native-ime ]; then letter=$(dd bs=1 count=1 2>/dev/null); [ \"$letter\" = x ] || exit 22; fi; "
    "if [ \"$0\" = ime ] || [ \"$0\" = native-ime ]; then chinese=$(dd bs=1 count=6 2>/dev/null); [ \"$chinese\" = 中文 ] || exit 20; printf IME_OK; fi; "
    "if [ \"$0\" = pointer ] || [ \"$0\" = native-pointer ]; then count=24; expected_reports=$(printf '\\033[<0;1;1M\\033[<0;1;1m\\033[O\\033[I'); reports=$(dd bs=1 count=\"$count\" 2>/dev/null); [ \"$reports\" = \"$expected_reports\" ] || exit 21; printf POINTER_OK; fi; "
    "answer=$(dd bs=1 count=16 2>/dev/null); expected=$(printf '\\033[200~USER\\033[201~'); "
    "[ \"$answer\" = \"$expected\" ] || exit 19; "
    "if [ \"$0\" = scheme ]; then printf '\\033[?996n'; color=$(dd bs=1 count=9 2>/dev/null); "
    "[ \"$color\" = \"$(printf '\\033[?997;1n')\" ] || exit 42; stty min 0 time 1; "
    "extra=$(dd bs=1 count=1 2>/dev/null); [ -z \"$extra\" ] || exit 43; stty min 1 time 0; fi; "
    "printf '\\033]7501;id=build:app=ghosterm:state=done:msg=RG9uZQ==\\007\\033[48;2;30;30;220mTHIRD'; read x; "
    "printf '"
    "\\033]7501;id=temp:app=ghosterm:state=working\\007"
    "\\033]7501;id=temp/wait:state=blocked:kind=permission\\007"
    "\\033]7501;id=temp/done:state=done\\007\\033]7501;id=temp/error:state=error\\007"
    "\\033]133;B\\007\\033]133;A\\007\\033]7501;id=temp:state=clear\\007PROMPT'; read x; "
    "printf '\\033]133;C;cmdline=next command\\007\\033]133;D;err=unknown exit failure\\007FOURTH'; read x";
  char workload[4096];
  if(s->history) {
    CHECK(snprintf(workload,sizeof(workload),
      "stty -echo; awk 'BEGIN{for(i=1;i<=1000;i++)printf \"HISTORY%%04d:%%060d\\n\",i,i*7919}'; "
      "printf '\\033[2J\\033[H'; %s",script)<(int)sizeof(workload));
    script=workload;
  }
  if(s->tmux)script=
    "wait_size() { n=0; while [ \"$(stty size)\" != \"$1\" ]; do [ \"$n\" -lt 50 ] || exit 53; n=$((n+1)); sleep 0.02; done; }; "
    "stty -echo; printf '\\033[?25l\\033[48;2;220;30;30m\\033[38;2;255;255;255mFIRST\\033[?2004'; read x; "
    "wait_size \"31 91\"; stty -icanon min 1 time 0; printf 'h\\033[48;2;30;220;30mSECOND\\033[6n'; "
    "answer=$(dd bs=1 count=7 2>/dev/null); [ \"$answer\" = \"$(printf '\\033[1;12R')\" ] || exit 51; "
    "printf VERIFIED; answer=$(dd bs=1 count=16 2>/dev/null); "
    "[ \"$answer\" = \"$(printf '\\033[200~USER\\033[201~')\" ] || exit 52; wait_size \"32 92\"; "
    "printf '\\033[48;2;30;30;220mTHIRD'; read x; printf PROMPT; read x; printf FOURTH; read x";
  if(s->tui) {
    CHECK(getenv("GHOSTERM_P0_TUI_FILE") && getenv("GHOSTERM_P0_TUI_FILE")[0]=='/');
    script=s->tui==1?"unset LESS LESSOPEN LESSCLOSE; export LESSHISTFILE=/dev/null; exec /usr/bin/less \"${GHOSTERM_P0_TUI_FILE:?}\"":"unset VIMINIT EXINIT; exec /usr/bin/vim -u NONE -i NONE -n --cmd 'set notitle' \"${GHOSTERM_P0_TUI_FILE:?}\"";
  }
  const char *args[]={"-c",script,s->history_policy?"history-limit":s->scheme?"scheme":s->native_ime?"native-ime":s->ime?"ime":s->native_pointer?"native-pointer":s->pointer?"pointer":"plain",NULL};
  const char *tmux_args[]={"-S",getenv("GHOSTERM_P0_TMUX_SOCKET"),"-N","-T","RGB","new-session","-s","ghosterm-p0","-n","p0","/bin/sh","-c",script,NULL};
  if(s->tmux)CHECK(tmux_args[1] && tmux_args[1][0]=='/' && getenv("GHOSTERM_P0_TMUX_BIN"));
  gt_pty_opts_t options={.executable=s->tmux?getenv("GHOSTERM_P0_TMUX_BIN"):"/bin/sh",.argv=s->tmux?tmux_args:args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
  gt_allocator_t allocator={s,backend_allocate,backend_release};
  gt_io_backend_t *backend=gt__pty_open_with_options(&options,&allocator,&s->thread_options);
  CHECK(backend && s->backend_bytes);s->backend_started=true;pthread_t waiter;
  CHECK(gt__pty_waiter_thread(backend,&waiter)==GT_OK && !pthread_equal(waiter,pthread_self()));
  require_thread_budget_for(s,&s->waiter_stack,waiter);
  if(s->read_error || s->tmux) {
    fault_backend_t *b=backend_allocate(s,sizeof(*b));CHECK(b);
    *b=(fault_backend_t){.base={&fault_ops},.real=backend,.owner=s};return &b->base;
  }
  return backend;
}
static gt_status_t apply(void *context,const gt_state_event_t *event) {
  server *s=context;GhosttyResult rc;
  bool poison=s->apply_error && atomic_load(&s->inject_read_error);
  if(poison)CHECK(p0_checkpoint_capture(&s->source,&s->reference,s->terminal,PW_LIMIT,&s->final,NULL)==P0_CHECKPOINT_OK);
  if(event->kind==GT_STATE_OUTPUT)rc=p0_budget_feed(&s->source,s->terminal,event->payload,event->length);
  else if(event->kind==GT_STATE_COLOR_SCHEME) {
    CHECK(s->scheme && event->length==sizeof(gt_state_color_scheme_t));
    s->dark=((const gt_state_color_scheme_t *)event->payload)->dark;rc=process_color_defaults(s);
  } else {
    CHECK(event->kind==GT_STATE_RESIZE && event->length==sizeof(gt_state_size_t));
    const gt_state_size_t *size=event->payload;
    rc=p0_budget_resize_cells(&s->source,s->terminal,size->cols,size->rows,10,20);
  }
  if(rc!=GHOSTTY_SUCCESS)return GT_ERR_IO;
  if(poison)return GT_ERR_IO; /* Engine changed, owner must become unusable. */
  if(s->summary_error!=GT_OK)return s->summary_error;
  s->events++;return GT_OK;
}
static void require_metadata(server *,bool);
static gt_status_t freeze(void *context,void **out) {
  server *s=context;checkpoint *c=s->capture;
  require_metadata(s,s->replies!=0);
  c->dark=s->dark;c->history_state=history_state(s);
  c->summary_phase=(s->tmux || s->tui)?3u:s->replies ? 1u:0u;require_summary(s->summary,c->summary_phase);
  require_command(&s->command,c->summary_phase);c->command=s->command;
  gt_status_allocator_t allocator=scope_allocator(&c->encoder);
  size_t source_before=p0_budget_scope_read(&s->source).live;
  CHECK(gt__program_status_copy_with_allocator(s->summary,&allocator,&c->summary)==GT_STATUS_OK);
  CHECK(p0_budget_scope_read(&c->encoder).live>0 && p0_budget_scope_read(&s->source).live==source_before);
  if(s->history) {
    size_t rows=0;
    CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS,&rows)==GHOSTTY_SUCCESS);
    CHECK(rows>=900);
    printf("METRIC process/history scrollback_rows=%zu\n",rows);
  }
  double start=metric_clock();
  CHECK(p0_frozen_capture(&s->source,&c->capture,s->terminal,PW_LIMIT,&c->frozen)==GHOSTTY_SUCCESS);
  c->freeze_ms=metric_elapsed(start);
  CHECK(gt__attachment_begin(s->attachment,s->generation,s->events)==GT_RESTORE_OK);
  *out=c;return GT_OK;
}
static void output(void *context,const uint8_t *bytes,size_t n,uint64_t sequence) {
  server *s=context;CHECK(sequence==s->events);
  CHECK(!pthread_mutex_lock(&s->lock));CHECK(n<sizeof(s->output)-s->used);
  memcpy(s->output+s->used,bytes,n);s->used+=n;s->output[s->used]=0;
  CHECK(!pthread_cond_broadcast(&s->changed));CHECK(!pthread_mutex_unlock(&s->lock));
}
static void event(void *context,const gt_state_event_t *e,uint64_t sequence) {
  server *s=context;CHECK(sequence==s->events);
  gt_restore_rc_t rc;
  if(s->summary_dirty || s->command_dirty) {
    CHECK(e->kind==GT_STATE_OUTPUT && e->length<=PW_CHUNK);
    uint8_t bundle[PSW_RESTORE_BYTES];size_t summary_length=0,command_length=0;
    pw_put64(bundle,e->length);memcpy(bundle+16,e->payload,e->length);
    CHECK(psw_encode(s->summary,bundle+16+e->length,PSW_MAX,&summary_length));
    pw_put64(bundle+8,summary_length);
    CHECK(csw_encode(&s->command,bundle+16+e->length+summary_length,CSW_MAX,&command_length));
    rc=push_state(s,sequence,PW_STATE_OUTPUT_SUMMARY,bundle,16+e->length+summary_length+command_length);
    s->summary_dirty=false;s->command_dirty=false;
  } else rc=push_state(s,sequence,e->kind,e->payload,e->length);
  CHECK(rc==GT_RESTORE_OK || rc==GT_RESTORE_IDLE || rc==GT_RESTORE_LIMIT);
  atomic_store(&s->published,sequence);
}
static void require_metadata(server *s,bool detached) {
  GhosttyString title={0},pwd={0};
  if(s->tmux || s->tui) {
    GhosttyTerminalScreen screen=GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,&screen)==GHOSTTY_SUCCESS && screen==GHOSTTY_TERMINAL_SCREEN_ALTERNATE);
    CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_TITLE,&title)==GHOSTTY_SUCCESS);
    if(s->tmux)CHECK(title.len==strlen("Ghosterm tmux") && !memcmp(title.ptr,"Ghosterm tmux",title.len));
    else CHECK(!title.len);
    CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_PWD,&pwd)==GHOSTTY_SUCCESS && !pwd.len);return;
  }
  const char *expected_title=detached ? "Ghosterm detached" : "Ghosterm initial";
  const char *expected_pwd=detached ? "file:///ghosterm/detached" : "file:///ghosterm/initial";
  CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_TITLE,&title)==GHOSTTY_SUCCESS);
  CHECK(title.len==strlen(expected_title) && !memcmp(title.ptr,expected_title,title.len));
  CHECK(ghostty_terminal_get(s->terminal,GHOSTTY_TERMINAL_DATA_PWD,&pwd)==GHOSTTY_SUCCESS);
  CHECK(pwd.len==strlen(expected_pwd) && !memcmp(pwd.ptr,expected_pwd,pwd.len));
}
static void state_invalidated(void *context,gt_status_t error,uint64_t sequence) {
  server *s=context;CHECK(s->apply_error && error==GT_ERR_IO && sequence==s->events && !s->invalid_state);
  s->invalid_state=true;
  puts("PASS process/invalid state announced before disposal; final export suppressed");
}
static void close_state(void *context) {
  server *s=context;
  if(!s->state_ready || !s->backend_started) {
    s->partial_cleanup++;
    if(s->terminal){ghostty_terminal_free(s->terminal);s->terminal=NULL;}
    gt__program_status_close(s->summary);s->summary=NULL;return;
  }
  require_summary(s->summary,(s->tmux || s->tui)?3u:2u);require_command(&s->command,(s->tmux || s->tui)?3u:2u);
  if(!s->invalid_state) {
    require_metadata(s,true);
    p0_checkpoint_release(&s->final);
    CHECK(p0_checkpoint_capture(&s->source,&s->reference,s->terminal,PW_LIMIT,&s->final,NULL)==P0_CHECKPOINT_OK);
  }
  ghostty_terminal_free(s->terminal);s->terminal=NULL;
  CHECK(p0_budget_scope_read(&s->source).live>0);
}
static void finished(void *context,gt_status_t status) {
  server *s=context;CHECK(!pthread_mutex_lock(&s->lock));s->finished=true;s->status=status;
  CHECK(!pthread_cond_broadcast(&s->changed));CHECK(!pthread_mutex_unlock(&s->lock));
}
static void captured(void *context,gt_status_t status,gt_state_capture_t value) {
  checkpoint *c=context;server *s=c->owner;CHECK(status==GT_OK && value.frozen==c);
  CHECK(!pthread_mutex_lock(&s->lock));c->sequence=value.sequence;c->captured=true;
  CHECK(!pthread_cond_broadcast(&s->changed));CHECK(!pthread_mutex_unlock(&s->lock));
}
static gt_status_t encode(void *context,void **out) {
  checkpoint *c=context;require_thread_budget(c->owner,&c->owner->encoder_stack);
  double start=metric_clock();
  if(p0_frozen_encode(&c->frozen,&c->encoder,PW_LIMIT,&c->wire)!=GHOSTTY_SUCCESS)return GT_ERR_IO;
  c->encode_ms=metric_elapsed(start);
  require_summary(c->summary,c->summary_phase);require_command(&c->command,c->summary_phase);
  *out=&c->wire;return GT_OK;
}
static void release_input(void *context) {
  checkpoint *c=context;p0_frozen_release(&c->frozen);
  if(!c->wire.canonical){gt__program_status_close(c->summary);c->summary=NULL;}
  CHECK(p0_budget_scope_close(&c->capture));
}
static void release_result(void *context,void *result) {
  checkpoint *c=context;CHECK(result==&c->wire);p0_checkpoint_release(&c->wire);gt__program_status_close(c->summary);c->summary=NULL;CHECK(p0_budget_scope_close(&c->encoder));
}
static gt_attachment_checkpoint_t capture(server *s,checkpoint *c) {
  memset(c,0,sizeof(*c));c->owner=s;s->capture=c;
  CHECK(p0_budget_scope_admit(&c->capture,&s->budget,64*MIB));
  CHECK(p0_budget_scope_admit(&c->encoder,&s->budget,64*MIB));
  CHECK(process_submit(s,GT_WORKER_CAPTURE,NULL,0,captured,c));
  struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&s->lock));while(!c->captured)CHECK(!pthread_cond_timedwait(&s->changed,&s->lock,&end));
  CHECK(!pthread_mutex_unlock(&s->lock));
  gt_task_spec_t spec={c,encode,release_input,release_result,s->generation,c->sequence};
  CHECK(gt__attachment_submit(s->attachment,s->generation,&spec,&c->task)==GT_TASK_OK);
  for(unsigned i=0;i<5000;i++) {
    gt_task_info_t info;CHECK(gt__tasks_info(s->tasks,c->task,&info)==GT_TASK_OK);
    if(info.state==GT_TASK_READY) {
      gt_attachment_checkpoint_t lease;CHECK(gt__attachment_take(s->attachment,s->generation,&lease)==GT_TASK_OK);
      CHECK(lease.result==&c->wire && lease.sequence==c->sequence);return lease;
    }
    CHECK(info.state==GT_TASK_QUEUED || info.state==GT_TASK_RUNNING || info.state==GT_TASK_RELEASING);
    struct timespec pause={0,1000000};nanosleep(&pause,NULL);
  }
  CHECK(false);return (gt_attachment_checkpoint_t){0};
}
static void retire(server *s,gt_attachment_checkpoint_t *lease,checkpoint *c) {
  CHECK(gt__attachment_release(s->attachment,lease)==GT_TASK_OK);
  for(unsigned i=0;i<5000;i++) {
    gt_attachment_retired_t retired;gt_task_rc_t rc=gt__attachment_reap(s->attachment,&retired);
    if(rc==GT_TASK_OK) {
      CHECK(retired.task==c->task && retired.context==c && retired.state==GT_TASK_FINISHED);
      CHECK(c->capture.closed && c->encoder.closed);return;
    }
    CHECK(rc==GT_TASK_BUSY);struct timespec pause={0,1000000};nanosleep(&pause,NULL);
  }
  CHECK(false);
}
static ssize_t transfer(void *context,void *bytes,size_t n,bool sending) {
  gt_attachment_peer_t *p=context;
  gt_peer_result_t r=sending ? gt__peer_send(p,bytes,n,50) : gt__peer_receive(p,bytes,n,50);
  return r.kind==GT_PEER_DATA ? (ssize_t)r.length : r.kind==GT_PEER_WAIT ? 0 : -1;
}
static pid_t spawn_client(const char *binary,unsigned round,int *fd,unsigned fault) {
  int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
  for(unsigned i=0;i<2;i++)CHECK(!fcntl(pair[i],F_SETFD,FD_CLOEXEC));
  posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));
  CHECK(!posix_spawn_file_actions_addclose(&actions,pair[0]));
  CHECK(!posix_spawn_file_actions_adddup2(&actions,pair[1],3));
  if(pair[1]!=3)CHECK(!posix_spawn_file_actions_addclose(&actions,pair[1]));
  char value[16];snprintf(value,sizeof(value),"%u",round);
  char *args[]={(char *)binary,"--process-client","3",value,fault==16?"16":fault==15?"15":fault==14?"14":fault==13?"13":fault==12?"12":fault==11?"11":fault==10?"10":fault==9?"9":fault==8?"8":fault==7?"7":fault==6?"6":fault==5?"5":fault==4?"4":fault==3?"3":fault==2?"2":fault?"1":"0",NULL};pid_t pid=0;
  CHECK(!posix_spawn(&pid,binary,&actions,NULL,args,environ));
  CHECK(!posix_spawn_file_actions_destroy(&actions));close(pair[1]);*fd=pair[0];return pid;
}
static void expect(pw_io *io,uint16_t kind,uint64_t sequence,pw_frame *frame) {
  CHECK(pw_receive(io,frame) && frame->kind==kind && frame->sequence==sequence);
}
static void reject_inflight(server *s,pw_io *io,uint64_t cursor) {
  CHECK(gt__attachment_current(s->attachment,s->generation));
  CHECK(pw_send(io,PW_PRE_END_INPUT,cursor,NULL,0));pw_frame frame;
  expect(io,PW_INPUT,cursor,&frame);
  const char expected[]="\x1b[200~INFLIGHT\x1b[201~";
  if(s->tui)CHECK(frame.length>8 && frame.length<=136 && pw_get64(frame.bytes)==s->generation);
  else CHECK(frame.length==8+sizeof(expected)-1 && pw_get64(frame.bytes)==s->generation && !memcmp(frame.bytes+8,expected,sizeof(expected)-1));
  CHECK(!gt__attachment_input_allowed(s->attachment,s->generation,frame.sequence));
  uint8_t rejected[16];pw_put64(rejected,s->generation);
  pw_put64(rejected+8,((uint64_t)GT_ERR_CANCELLED<<32)|PW_INPUT_ENDED);
  CHECK(pw_send(io,PW_INPUT_REJECTED,frame.sequence,rejected,sizeof(rejected)));
  puts("PASS process/ended controller rejected valid-generation encoded inflight input");
}
static void send_checkpoint(pw_io *io,uint16_t kind,uint64_t g,uint64_t sequence,const p0_checkpoint *c,const gt_program_status_store_t *summary,const gt_command_status_t *command,bool scheme,uint32_t dark,bool history_policy,hsw_state history) {
  CHECK(c->canonical_len && c->resources_len && c->canonical_len+c->resources_len<=PW_LIMIT);
  uint8_t summary_wire[PSW_MAX];size_t summary_length=0;
  CHECK(psw_encode(summary,summary_wire,sizeof(summary_wire),&summary_length));
  CHECK(summary_length<=PW_LIMIT-c->canonical_len-c->resources_len);
  uint8_t command_wire[CSW_MAX];size_t command_length=0;
  CHECK(csw_encode(command,command_wire,sizeof(command_wire),&command_length));
  CHECK(command_length<=PW_LIMIT-c->canonical_len-c->resources_len-summary_length);
  uint8_t metadata[224];pw_put64(metadata,g);pw_put64(metadata+8,c->canonical_len);pw_put64(metadata+16,c->resources_len);
  pw_put64(metadata+24,10);pw_put64(metadata+32,20);
  CC_SHA256(c->canonical,(CC_LONG)c->canonical_len,metadata+40);
  CC_SHA256(c->resources,(CC_LONG)c->resources_len,metadata+72);
  memcpy(metadata+104,process_engine_build,sizeof(process_engine_build));
  pw_put64(metadata+136,summary_length);
  CC_SHA256(summary_wire,(CC_LONG)summary_length,metadata+144);
  pw_put64(metadata+176,command_length);CC_SHA256(command_wire,(CC_LONG)command_length,metadata+184);
  if(scheme){CHECK(dark<=1);pw_put64(metadata+216,dark);}
  CHECK(pw_send(io,kind,sequence,metadata,scheme?224:216));
  CHECK(pw_send_blob(io,PW_CANONICAL,c->canonical,c->canonical_len));
  CHECK(pw_send_blob(io,PW_RESOURCES,c->resources,c->resources_len));
  CHECK(pw_send_blob(io,PW_SUMMARY,summary_wire,summary_length));
  CHECK(pw_send_blob(io,PW_COMMAND,command_wire,command_length));
  if(history_policy){uint8_t data[8+HSW_BYTES];pw_put64(data,g);CHECK(hsw_encode(history,data+8,HSW_BYTES));CHECK(pw_send(io,PW_HISTORY,sequence,data,sizeof(data)));}
}
static uint64_t drain(server *s,pw_io *io,uint64_t cursor) {
  gt_restore_event_t e;uint8_t bytes[PSW_RESTORE_BYTES+HSW_EVENT_PREFIX];
  for(;;) {
    gt_restore_rc_t rc=gt__attachment_next(s->attachment,s->generation,&e,bytes,sizeof(bytes));
    if(rc==GT_RESTORE_WAIT)return cursor;
    CHECK(rc==GT_RESTORE_OK && e.sequence==cursor+1 && e.generation==s->generation);
    hsw_state history={0};
    if(s->history_policy) {
      CHECK(e.kind==PW_STATE_HISTORY && e.length>HSW_EVENT_PREFIX);
      CHECK(hsw_decode(bytes+8,HSW_BYTES,&history));uint64_t kind=pw_get64(bytes);CHECK(kind==GT_STATE_OUTPUT || kind==GT_STATE_RESIZE || kind==PW_STATE_OUTPUT_SUMMARY);
      e.kind=(uint32_t)kind;e.length-=HSW_EVENT_PREFIX;memmove(bytes,bytes+HSW_EVENT_PREFIX,e.length);
    }
    if(e.kind==PW_STATE_OUTPUT_SUMMARY) {
      CHECK(e.length>=16);uint64_t output_length=pw_get64(bytes),summary_length=pw_get64(bytes+8);
      CHECK(output_length && output_length<=PW_CHUNK && output_length<=e.length-16);
      size_t remaining=e.length-16-(size_t)output_length;
      CHECK(summary_length>=8 && summary_length<=PSW_MAX && summary_length<=remaining);
      size_t command_length=remaining-(size_t)summary_length;CHECK(command_length>=CSW_HEADER && command_length<=CSW_MAX);
      const uint8_t *summary=bytes+16+(size_t)output_length,*command=summary+summary_length;
      uint8_t meta[88];pw_put64(meta,s->generation);pw_put64(meta+8,summary_length);
      CC_SHA256(summary,(CC_LONG)summary_length,meta+16);
      pw_put64(meta+48,command_length);CC_SHA256(command,(CC_LONG)command_length,meta+56);
      CHECK(pw_send(io,PW_OUTPUT_SUMMARY,e.sequence,bytes+16,(size_t)output_length));
      CHECK(pw_send(io,PW_SUMMARY_UPDATE,e.sequence,meta,sizeof(meta)));
      CHECK(pw_send_blob(io,PW_SUMMARY,summary,summary_length));
      CHECK(pw_send_blob(io,PW_COMMAND,command,command_length));
    }
    else if(e.kind==GT_STATE_OUTPUT)CHECK(pw_send(io,PW_OUTPUT,e.sequence,bytes,e.length));
    else if(e.kind==GT_STATE_COLOR_SCHEME) {
      CHECK(s->scheme && e.length==sizeof(gt_state_color_scheme_t));gt_state_color_scheme_t value;memcpy(&value,bytes,sizeof(value));
      uint8_t data[8];pw_put64(data,value.dark);CHECK(pw_send(io,PW_SCHEME,e.sequence,data,sizeof(data)));
    }
    else {
      CHECK(e.kind==GT_STATE_RESIZE && e.length==sizeof(gt_state_size_t));gt_state_size_t size;memcpy(&size,bytes,sizeof(size));
      uint8_t data[32];pw_put64(data,size.cols);pw_put64(data+8,size.rows);pw_put64(data+16,size.px_width);pw_put64(data+24,size.px_height);
      CHECK(pw_send(io,PW_RESIZE,e.sequence,data,sizeof(data)));
    }
    if(s->history_policy){uint8_t data[8+HSW_BYTES];pw_put64(data,s->generation);CHECK(hsw_encode(history,data+8,HSW_BYTES));CHECK(pw_send(io,PW_HISTORY,e.sequence,data,sizeof(data)));}
    cursor=e.sequence;
  }
}
typedef struct {atomic_bool done;gt_status_t status;} color_completion;
static void color_completed(void *context,gt_status_t status,gt_state_capture_t capture) {
  (void)capture;color_completion *c=context;c->status=status;atomic_store(&c->done,true);
}
static void submit_color(server *s,uint32_t dark) {
  gt_state_color_scheme_t value={dark};color_completion completion={0};
  CHECK(process_submit(s,GT_WORKER_COLOR_SCHEME,&value,sizeof(value),color_completed,&completion));
  for(unsigned i=0;!atomic_load(&completion.done);i++){CHECK(i<5000);const struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
  CHECK(completion.status==GT_OK);
}
static void wait_output_since(server *s,const char *text,size_t offset) {
  struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&s->lock));
  while(!strstr(s->output+offset,text))CHECK(!pthread_cond_timedwait(&s->changed,&s->lock,&end));
  CHECK(!pthread_mutex_unlock(&s->lock));
}
static void advance(server *s) {
  gt_state_size_t size={91,31,910,620};
  CHECK(process_submit(s,GT_WORKER_RESIZE,&size,sizeof(size),NULL,NULL));
  if(s->tui) {
    const char *keys=s->tui==1?"G":"iHELLO";
    CHECK(process_submit(s,GT_WORKER_INPUT,keys,strlen(keys),NULL,NULL));
    wait_output(s,s->tui==1?"LASTLINE200":"HELLO");return;
  }
  CHECK(process_submit(s,GT_WORKER_INPUT,"go\n",3,NULL,NULL));
  wait_output(s,"VERIFIED");
}
static void complete_commands(server *s) {
  wait_output(s,"THIRD");
  CHECK(process_submit(s,GT_WORKER_INPUT,"go\n",3,NULL,NULL));
  wait_output(s,"PROMPT");
  CHECK(process_submit(s,GT_WORKER_INPUT,"go\n",3,NULL,NULL));
  wait_output(s,"FOURTH");
}
static void recovery_end(server *s) {
  const char pending[]="\x1b[200~USER\x1b[201~";
  CHECK(process_submit(s,GT_WORKER_INPUT,pending,sizeof(pending)-1,NULL,NULL));
  complete_commands(s);atomic_store(&s->inject_read_error,true);
  struct timespec deadline;CHECK(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&s->lock));
  while(!s->finished)CHECK(!pthread_cond_timedwait(&s->changed,&s->lock,&deadline));
  CHECK(!pthread_mutex_unlock(&s->lock));gt_worker_result_t result;
  CHECK(process_result(s,&result));
  CHECK(result.kind==GT_WORKER_END_ERROR && result.stage==GT_WORKER_STAGE_READ && result.status==GT_ERR_IO && result.has_sequence);
  CHECK(gt__attachment_end(s->attachment,s->generation,result.sequence)==GT_RESTORE_WAIT);
  gt_attachment_restore_info_t info;CHECK(gt__attachment_restore_info(s->attachment,s->generation,&info)==GT_RESTORE_OK);
  CHECK(info.session_ended && info.phase==GT_RESTORE_SNAPSHOT && info.terminal_sequence==result.sequence);
  CHECK(!gt__attachment_input_allowed(s->attachment,s->generation,result.sequence));
  puts("PASS process/recovery end latched before READY(S), snapshot and tail retained");
}
static gt_status_t startup_state(void *context,gt_worker_reply_sink_t sink,void **state) {
  server *s=context;require_thread_budget(s,&s->worker_stack);
  if(s->backend_oom)return open_state(s,sink,state);
  if(s->state_oom) {
    size_t floor=p0_budget_scope_read(&s->source).live;
    CHECK(p0_budget_scope_set_limit(&s->source,floor+65536));
    return open_state(context,sink,state);
  }
  (void)sink;*state=NULL;return GT_ERR_IO;
}
static gt_io_backend_t *startup_backend(void *context){
  server *s=context;CHECK(s->backend_oom && s->state_ready);s->backend_attempted=true;
  size_t floor=p0_budget_scope_read(&s->source).live;CHECK(p0_budget_scope_set_limit(&s->source,floor));
  const char *args[]={"-c","exit 0",NULL};
  gt_pty_opts_t startup_options={.executable="/bin/sh",.cmd_kind=GT_PTY_CMD_DIRECT,.argv=args,.cols=80,.rows=24};
  gt_allocator_t startup_allocator={s,backend_allocate,backend_release};
  gt_io_backend_t *backend=gt__pty_open_with_options(&startup_options,&startup_allocator,&s->thread_options);
  CHECK(!backend && !s->backend_bytes && p0_budget_scope_read(&s->source).denied>0);return backend;
}
static void startup_dispose(void *context){(void)context;CHECK(false);}
static int startup_probe(const char *client,bool oom,bool backend_oom) {
  setbuf(stdout,NULL);server s={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.state_oom=oom,.backend_oom=backend_oom};
  long page=sysconf(_SC_PAGESIZE);CHECK(page>0);s.thread_options=(gt_thread_options_t){MIB,(size_t)page};
  s.thread_reserve=MIB+2*(size_t)page;
  p0_main_stack_info main_stack;CHECK(p0_main_stack_measure(&main_stack));s.main_reserve=main_stack.capacity;
  CHECK(p0_budget_init(&s.budget,(backend_oom?8*MIB:4*MIB)+s.main_reserve));CHECK(p0_budget_scope_admit(&s.source,&s.budget,backend_oom?4*MIB:MIB));
  CHECK(p0_budget_scope_admit(&s.main_stack,&s.budget,s.main_reserve));
  CHECK(p0_main_stack_admitted(&main_stack,p0_budget_scope_read(&s.main_stack).committed));
  printf("METRIC process/main-stack stack_bytes=%zu guard_bytes=%zu reserved_bytes=%zu\n",main_stack.stack,main_stack.guard,s.main_reserve);
  CHECK(p0_budget_scope_admit(&s.worker_stack, &s.budget, s.thread_reserve));
  CHECK(p0_budget_scope_admit(&s.encoder_stack, &s.budget, s.thread_reserve));
  if(oom||backend_oom)CHECK(p0_budget_scope_admit(&s.diagnostic,&s.budget,65536));
  gt_allocator_t boot_allocator=scope_allocator(&s.source);
  s.tasks=gt__tasks_open_with_options(1,&boot_allocator,&s.thread_options);CHECK(s.tasks);
  size_t tasks_live=p0_budget_scope_read(&s.source).live;CHECK(tasks_live>0);
  s.attachment=gt__attachment_open_with_allocator(s.tasks,64,4,&boot_allocator);CHECK(s.attachment);
  size_t attachment_live=p0_budget_scope_read(&s.source).live;CHECK(attachment_live>tasks_live);
  size_t registry_before=p0_budget_scope_read(&s.source).by_kind[P0_BUDGET_SCRATCH];
  s.manager=gt__manager_open(1,&boot_allocator);CHECK(s.manager);
  CHECK(p0_budget_scope_read(&s.source).by_kind[P0_BUDGET_SCRATCH]>registry_before);
  size_t controller_live=p0_budget_scope_read(&s.source).live;CHECK(controller_live>attachment_live);
  CHECK(gt__attachment_attach(s.attachment,&s.generation)==GT_TASK_OK);
  gt_worker_config_t config={.context=&s,.open_state=startup_state,.open_backend=startup_backend,
    .close_state=(oom||backend_oom)?close_state:startup_dispose,.state_ops={apply,freeze},.finished=finished};
  CHECK(gt__manager_create(s.manager,&config,&boot_allocator,&s.thread_options,process_dispose,NULL,&s.session)==GT_MANAGER_OK && s.session);
  uint64_t rejected=99;CHECK(gt__manager_create(s.manager,&config,&boot_allocator,&s.thread_options,process_dispose,NULL,&rejected)==GT_MANAGER_FULL && !rejected);
  CHECK(p0_budget_scope_read(&s.source).live>controller_live);
  struct timespec deadline;CHECK(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
  CHECK(!pthread_mutex_lock(&s.lock));
  while(!s.finished)CHECK(!pthread_cond_timedwait(&s.changed,&s.lock,&deadline));
  CHECK(!pthread_mutex_unlock(&s.lock));gt_worker_result_t result;
  CHECK(process_result(&s,&result));
  CHECK(result.kind==GT_WORKER_END_ERROR && result.stage==(backend_oom?GT_WORKER_STAGE_BACKEND_OPEN:GT_WORKER_STAGE_STATE_OPEN) && result.status==GT_ERR_IO && result.has_sequence==backend_oom && !result.sequence);
  process_reap(&s);
  if(backend_oom)CHECK(s.state_ready && s.backend_attempted && !s.backend_started);
  if(oom||backend_oom)CHECK(s.engine_attempted && s.partial_cleanup==1 && !s.terminal && !s.summary && p0_budget_scope_read(&s.source).denied>0 && p0_budget_scope_fault(&s.source)==P0_BUDGET_SCOPE_LIMIT);
  CHECK(p0_budget_scope_read(&s.source).live==controller_live && p0_budget_scope_close(&s.worker_stack));
  if(backend_oom)CHECK(gt__attachment_end(s.attachment,s.generation,0)==GT_RESTORE_WAIT);
  else {
  CHECK(gt__attachment_end_unknown(s.attachment,s.generation)==GT_RESTORE_OK);
  }
  gt_attachment_restore_info_t info;CHECK(gt__attachment_restore_info(s.attachment,s.generation,&info)==GT_RESTORE_OK);
  CHECK(info.session_ended && info.has_terminal_sequence==backend_oom && !info.terminal_sequence && gt__attachment_current(s.attachment,s.generation));
  int fd=-1;pid_t child=spawn_client(client,2,&fd,backend_oom?7:5);gt_attachment_peer_t *peer=NULL;
  gt_allocator_t boot_peer_allocator={&s,peer_allocate,peer_release};
  CHECK(gt__peer_adopt_with_allocator(s.attachment,s.generation,fd,&boot_peer_allocator,&peer)==GT_OK);
  CHECK(s.peer_bytes && ((oom||backend_oom)?p0_budget_scope_read(&s.diagnostic).live>0:p0_budget_scope_read(&s.source).live>controller_live));pw_io io={peer,transfer};
  uint8_t bytes[RSW_BYTES];size_t length=0;
  CHECK(rsw_encode(s.generation,&result,bytes,sizeof(bytes),&length));
  CHECK(pw_send(&io,PW_RESULT,0,bytes,length));pw_frame frame;expect(&io,PW_FINISHED,0,&frame);CHECK(!frame.length);
  CHECK(pw_send(&io,PW_FINISHED_ACK,0,NULL,0));int status;
  CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
  uint8_t byte;CHECK(gt__peer_receive(peer,&byte,1,1000).kind==GT_PEER_CLOSED);
  CHECK(!gt__attachment_current(s.attachment,s.generation));gt__peer_close(peer);
  CHECK(!s.peer_bytes && p0_budget_scope_read(&s.source).live==controller_live);
  process_registry_close(&s);
  CHECK(gt__attachment_close(s.attachment)==GT_TASK_OK && gt__tasks_close(s.tasks)==GT_TASK_OK);
  CHECK(!pthread_cond_destroy(&s.changed) && !pthread_mutex_destroy(&s.lock));
  if(oom||backend_oom)CHECK(!p0_budget_scope_read(&s.diagnostic).live && p0_budget_scope_close(&s.diagnostic));
  CHECK(p0_budget_scope_close(&s.encoder_stack) && p0_budget_scope_close(&s.source));
  CHECK(p0_budget_scope_close(&s.main_stack));
  p0_budget_usage usage=p0_budget_read(&s.budget);
  CHECK(!usage.live && !usage.committed && usage.mapped==usage.unmapped && p0_budget_destroy(&s.budget));
  printf("METRIC process/start-error bootstrap_peak_bytes=%zu bootstrap_committed_peak_bytes=%zu final_live=0 final_committed=0\n",usage.peak,usage.peak_committed);
  if(backend_oom)puts("PASS process/backend-oom: real PTY allocator rejection, initialized state cleaned, known zero sequence delivered");
  if(oom)puts("PASS process/state-oom: actual VT budget denial, partial state reclaimed, healthy diagnostic peer delivered unknown terminal result");
  if(!backend_oom)puts("PASS process/start-error: unknown sequence, actual worker failure, no backend or snapshot, GUI error acknowledged");return 0;
}
int main(int argc,char **argv) {
  if(argc==3 && (!strcmp(argv[2],"start-error") || !strcmp(argv[2],"state-oom") || !strcmp(argv[2],"backend-oom")))return startup_probe(argv[1],!strcmp(argv[2],"state-oom"),!strcmp(argv[2],"backend-oom"));
  setbuf(stdout,NULL);CHECK(argc==3);unsigned tui=!strcmp(argv[2],"less")?1u:!strcmp(argv[2],"vim")?2u:0u;bool tmux=!strcmp(argv[2],"tmux");bool scheme=!strcmp(argv[2],"scheme");bool native_pointer=!strcmp(argv[2],"native-pointer");bool pointer=native_pointer || !strcmp(argv[2],"pointer");bool native_ime=!strcmp(argv[2],"native-ime");bool ime=native_ime || !strcmp(argv[2],"ime");bool search=!strcmp(argv[2],"search");bool history_policy=!strcmp(argv[2],"history-limit");bool killed=history_policy || search || tui || tmux || !strcmp(argv[2],"kill");bool crossing=!strcmp(argv[2],"crossing");bool history=!strcmp(argv[2],"history");bool reply_error=!strcmp(argv[2],"reply-error");bool apply_error=!strcmp(argv[2],"apply-error");bool recovery=!strcmp(argv[2],"recovery-error");bool read_error=reply_error || apply_error || recovery || !strcmp(argv[2],"read-error");CHECK(tmux || scheme || pointer || ime || killed || crossing || history || read_error || !strcmp(argv[2],"roundtrip"));
  server s={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.history_policy=history_policy,.native_ime=native_ime,.native_pointer=native_pointer,.tui=tui,.tmux=tmux,.scheme=scheme,.pointer=pointer,.ime=ime,.history=history,.read_error=read_error,.apply_error=apply_error,.reply_error=reply_error};
  long page=sysconf(_SC_PAGESIZE);CHECK(page>0);s.thread_options=(gt_thread_options_t){MIB,(size_t)page};
  s.thread_reserve=MIB+2*(size_t)page;
  p0_main_stack_info main_stack;CHECK(p0_main_stack_measure(&main_stack));s.main_reserve=main_stack.capacity;
  CHECK(p0_budget_init(&s.budget,256*MIB+s.main_reserve));CHECK(p0_budget_scope_admit(&s.source,&s.budget,64*MIB-3*s.thread_reserve));
  CHECK(p0_budget_scope_admit(&s.main_stack,&s.budget,s.main_reserve));
  CHECK(p0_main_stack_admitted(&main_stack,p0_budget_scope_read(&s.main_stack).committed));
  printf("METRIC process/main-stack stack_bytes=%zu guard_bytes=%zu reserved_bytes=%zu\n",main_stack.stack,main_stack.guard,s.main_reserve);
  CHECK(p0_budget_scope_admit(&s.worker_stack,&s.budget,s.thread_reserve));
  CHECK(p0_budget_scope_admit(&s.encoder_stack,&s.budget,s.thread_reserve));
  CHECK(p0_budget_scope_admit(&s.waiter_stack,&s.budget,s.thread_reserve));
  CHECK(p0_budget_scope_admit(&s.reference,&s.budget,64*MIB));
  gt_allocator_t queue_allocator=scope_allocator(&s.source);
  s.tasks=gt__tasks_open_with_options(1,&queue_allocator,&s.thread_options);CHECK(s.tasks);
  CHECK(p0_budget_scope_read(&s.source).live>0);
  size_t queue_before=p0_budget_scope_read(&s.source).live;
  s.attachment=gt__attachment_open_with_allocator(s.tasks,PSW_RESTORE_BYTES+(s.history_policy?HSW_EVENT_PREFIX:0),64,&queue_allocator);CHECK(s.attachment);
  CHECK(p0_budget_scope_read(&s.source).live>queue_before);
  CHECK(gt__attachment_attach(s.attachment,&s.generation)==GT_TASK_OK);
  gt_worker_config_t config={.context=&s,.open_backend=open_backend,.open_state=open_state,.close_state=close_state,
    .state_ops={apply,freeze},.output=output,.event=event,.state_invalidated=state_invalidated,.finished=finished,.color_scheme_supported=scheme};
  size_t registry_before=p0_budget_scope_read(&s.source).by_kind[P0_BUDGET_SCRATCH];
  s.manager=gt__manager_open(1,&queue_allocator);CHECK(s.manager);
  CHECK(p0_budget_scope_read(&s.source).by_kind[P0_BUDGET_SCRATCH]>registry_before);
  size_t worker_before=p0_budget_scope_read(&s.source).by_kind[P0_BUDGET_SCRATCH];
  CHECK(gt__manager_create(s.manager,&config,&queue_allocator,&s.thread_options,process_dispose,NULL,&s.session)==GT_MANAGER_OK && s.session);
  uint64_t rejected=99;CHECK(gt__manager_create(s.manager,&config,&queue_allocator,&s.thread_options,process_dispose,NULL,&rejected)==GT_MANAGER_FULL && !rejected);wait_output(&s,tui?"FIRSTLINE001":"FIRST");process_running(&s);
  CHECK(p0_budget_scope_read(&s.source).by_kind[P0_BUDGET_SCRATCH]>worker_before+65536);
  gt_attachment_peer_t *old=NULL;uint64_t old_generation=0,old_sequence=0;
  if(!killed)advance(&s);
  if(scheme)submit_color(&s,1);
  for(unsigned round=killed ? 1u : 2u;round<=2;round++) {
    checkpoint checkpoint;gt_attachment_checkpoint_t lease=capture(&s,&checkpoint);
    if(round==2 && killed)CHECK(checkpoint.sequence>old_sequence && s.generation>old_generation);
    if(scheme)submit_color(&s,0);
    if(history_policy && round==1) {
      CHECK(checkpoint.history_state.state==HSW_COMPLETE);
      const char prune[]="PRUNE\r";CHECK(process_submit(&s,GT_WORKER_INPUT,prune,sizeof(prune)-1,NULL,NULL));
      wait_output(&s,"HISTORY_BURST_END\x07\x1b]2;Ghosterm initial\x07\x1b[?2004");
    }
    gt_state_size_t pending_size={92,32,920,640};
    color_completion resized={0};
    if(history_policy) {
      CHECK(process_submit(&s,GT_WORKER_RESIZE,&pending_size,sizeof(pending_size),color_completed,&resized));
      struct timespec delay={0,1000000};for(unsigned i=0;!atomic_load(&resized.done);i++){CHECK(i<5000);nanosleep(&delay,NULL);}CHECK(resized.status==GT_OK);
    } else CHECK(process_submit(&s,GT_WORKER_RESIZE,&pending_size,sizeof(pending_size),NULL,NULL));
    for(unsigned i=0;atomic_load(&s.published)==checkpoint.sequence;i++) {
      CHECK(i<5000);const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    int fd=-1;pid_t child=spawn_client(argv[1],round,&fd,history_policy?16:search?15:native_ime?14:native_pointer?13:tui==2?12:tui==1?11:tmux?10:scheme?9:pointer?8:ime?6:reply_error?4:apply_error?3:recovery?2:read_error);gt_attachment_peer_t *peer=NULL;
    gt_allocator_t peer_allocator={&s,peer_allocate,peer_release};size_t peer_before=s.peer_bytes;
    CHECK(gt__peer_adopt_with_allocator(s.attachment,s.generation,fd,&peer_allocator,&peer)==GT_OK);
    CHECK(s.peer_bytes>peer_before);
    pw_io io={peer,transfer};pw_frame frame;
    double restore_start=metric_clock();
    send_checkpoint(&io,PW_SNAPSHOT,s.generation,checkpoint.sequence,&checkpoint.wire,checkpoint.summary,&checkpoint.command,scheme,checkpoint.dark,s.history_policy,checkpoint.history_state);
    double send_ms=metric_elapsed(restore_start);
    if(recovery)recovery_end(&s);
    expect(&io,PW_READY_S,checkpoint.sequence,&frame);CHECK(!frame.length);
    CHECK(!gt__attachment_input_allowed(s.attachment,s.generation,checkpoint.sequence));
    CHECK(gt__attachment_ready_snapshot(s.attachment,s.generation,checkpoint.sequence)==GT_RESTORE_OK);
    if(crossing || scheme) {
      if(scheme)submit_color(&s,1);
      /* READY(S) froze L. The live owner now advances beyond that boundary. */
      uint64_t before=atomic_load(&s.published);
      gt_state_size_t after_ready={93,33,930,660};
      CHECK(process_submit(&s,GT_WORKER_RESIZE,&after_ready,sizeof(after_ready),NULL,NULL));
      for(unsigned i=0;atomic_load(&s.published)==before;i++) {
        CHECK(i<5000);const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
      }
    }
    uint64_t cursor=drain(&s,&io,checkpoint.sequence),cut=0;
    CHECK(gt__attachment_live_begin(s.attachment,s.generation,&cut)==GT_RESTORE_OK && cut==cursor);
    CHECK(pw_send(&io,PW_LIVE_BEGIN,cut,NULL,0));
    if(crossing || scheme) {
      cursor=drain(&s,&io,cursor);CHECK(cursor>cut);
      CHECK(!gt__attachment_input_allowed(s.attachment,s.generation,cursor));
    }
    expect(&io,PW_READY_L,cut,&frame);CHECK(!frame.length);
    CHECK(!gt__attachment_input_allowed(s.attachment,s.generation,cut));
    CHECK(gt__attachment_ready_live(s.attachment,s.generation,cut)==GT_RESTORE_OK);
    CHECK(gt__attachment_input_allowed(s.attachment,s.generation,cut)==!recovery);
    CHECK(pw_send(&io,PW_LIVE_OK,cut,NULL,0));expect(&io,PW_CLIENT_LIVE,cursor,&frame);CHECK(!frame.length);
    p0_budget_usage capture_usage=p0_budget_scope_read(&checkpoint.capture);
    p0_budget_usage encoder_usage=p0_budget_scope_read(&checkpoint.encoder);
    p0_budget_usage current_usage=p0_budget_read(&s.budget);
    printf("METRIC process/%s round=%u freeze_ms=%.3f encode_ms=%.3f send_ms=%.3f restore_to_drawn_ms=%.3f canonical_bytes=%zu resource_bytes=%zu capture_peak=%zu encoder_peak=%zu ledger_peak=%zu committed_peak=%zu\n",
      argv[2],round,checkpoint.freeze_ms,checkpoint.encode_ms,send_ms,metric_elapsed(restore_start),
      checkpoint.wire.canonical_len,checkpoint.wire.resources_len,capture_usage.peak,encoder_usage.peak,
      current_usage.peak,current_usage.peak_committed);
    if(round==1) {
      uint64_t other=99;CHECK(gt__attachment_attach(s.attachment,&other)==GT_TASK_BUSY && !other);
      old_sequence=atomic_load(&s.published);CHECK(old_sequence==cut);old_generation=s.generation;
      CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child);
      CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
      uint8_t byte;gt_peer_result_t closed=gt__peer_receive(peer,&byte,1,1000);
      CHECK(closed.kind==GT_PEER_CLOSED && !closed.length && !gt__attachment_current(s.attachment,s.generation));
      CHECK(atomic_load(&s.published)==old_sequence); /* idle: no overflow reclaimed the slot */
      require_summary(checkpoint.summary,(tmux || tui)?3u:0u);
      retire(&s,&lease,&checkpoint);old=peer;
      CHECK(gt__attachment_attach(s.attachment,&s.generation)==GT_TASK_OK && s.generation>old_generation);
      gt__peer_close(old);old=NULL;CHECK(gt__attachment_current(s.attachment,s.generation));
      process_running(&s);advance(&s);process_running(&s);printf("PASS process/gui SIGKILL: idle g=%llu reclaimed, authority still running\n",(unsigned long long)old_generation);
      continue;
    }
    if(!recovery) {
      expect(&io,PW_INPUT,cursor,&frame);
      const char input[]="\x1b[200~USER\x1b[201~";
      size_t prefix=pointer?24:native_ime?7:ime?6:0;
      if(tui)CHECK(frame.length>8 && frame.length<=136 && pw_get64(frame.bytes)==s.generation);
      else CHECK(frame.length==8+prefix+sizeof(input)-1 && pw_get64(frame.bytes)==s.generation && !memcmp(frame.bytes+8+prefix,input,sizeof(input)-1));
      if(ime)CHECK(!memcmp(frame.bytes+8+(native_ime?1:0),"中文",6));
      if(native_ime)CHECK(frame.bytes[8]=='x');
      if(pointer)CHECK(!memcmp(frame.bytes+8,"\x1b[<0;1;1M\x1b[<0;1;1m\x1b[O\x1b[I",24));
      CHECK(gt__attachment_input_allowed(s.attachment,pw_get64(frame.bytes),frame.sequence));
      CHECK(!pthread_mutex_lock(&s.lock));size_t input_start=s.used;CHECK(!pthread_mutex_unlock(&s.lock));
      CHECK(process_submit(&s,GT_WORKER_INPUT,frame.bytes+8,frame.length-8,NULL,NULL));
      CHECK(pw_send(&io,PW_INPUT_OK,cursor,NULL,0));if(ime)wait_output(&s,"IME_OK");if(pointer)wait_output(&s,"POINTER_OK");if(tui)wait_output_since(&s,tui==1?"FIRSTLINE001":"USER",input_start);else complete_commands(&s);
    }
    if(scheme)submit_color(&s,0);
    if(read_error)atomic_store(&s.inject_read_error,true);else CHECK(gt__manager_terminate(s.manager,s.session)==GT_MANAGER_OK);
    struct timespec finished_deadline;CHECK(!clock_gettime(CLOCK_REALTIME,&finished_deadline));finished_deadline.tv_sec+=5;
    CHECK(!pthread_mutex_lock(&s.lock));
    while(!s.finished)CHECK(!pthread_cond_timedwait(&s.changed,&s.lock,&finished_deadline));
    CHECK(!pthread_mutex_unlock(&s.lock));
    CHECK(process_result(&s,&s.worker_result));
    process_reap(&s);
    CHECK(s.worker_result.kind==(read_error?GT_WORKER_END_ERROR:GT_WORKER_END_STOPPED) && s.worker_result.status==(read_error?GT_ERR_IO:GT_OK) && s.worker_result.stage==(reply_error?GT_WORKER_STAGE_REPLY:apply_error?GT_WORKER_STAGE_APPLY:read_error?GT_WORKER_STAGE_READ:GT_WORKER_STAGE_NONE));
    CHECK(s.invalid_state==apply_error);
    CHECK(s.worker_result.has_sequence && s.worker_result.sequence==s.events && !s.worker_result.has_exit_code);
    CHECK(s.worker_result.termination_status==GT_ERR_NOT_FOUND && s.worker_result.termination.kind==GT_IO_EXIT_UNKNOWN);
    puts("PASS process/worker terminal result retained after join with last authority sequence");CHECK(p0_budget_scope_close(&s.worker_stack));CHECK(p0_budget_scope_close(&s.waiter_stack));
    CHECK(s.finished && s.status==(read_error?GT_ERR_IO:GT_OK) && !s.terminal);
    if(tmux){CHECK(s.replies>0 && s.reply_bytes>0);printf("METRIC process/tmux outer_query_replies=%u outer_reply_bytes=%zu\n",s.replies,s.reply_bytes);}
    else if(!tui)CHECK(s.replies==(reply_error?2u:1u) && s.reply_bytes==(reply_error?11u:7u));
    if(scheme)CHECK(s.scheme_replies==2 && s.dark==0 && checkpoint.dark==1);
    require_summary(s.summary,(tmux || tui)?3u:2u);require_summary(checkpoint.summary,(tmux || tui)?3u:1u);
    require_command(&s.command,(tmux || tui)?3u:2u);require_command(&checkpoint.command,(tmux || tui)?3u:1u);
    if(tmux)puts("PASS process/tmux: absent program summary and command remain unknown, alternate screen/title retained");
    else if(tui)puts("PASS process/TUI: application interaction after restore, no invented command/summary");
    else puts("PASS process/command owner unknown exit, captured command still exit 7");
    if(!tmux && !tui)puts("PASS process/summary: owner done, captured blocked unchanged");
    CHECK(gt__attachment_end(s.attachment,s.generation,s.worker_result.sequence)==GT_RESTORE_OK);
    gt_attachment_restore_info_t end_info;CHECK(gt__attachment_restore_info(s.attachment,s.generation,&end_info)==GT_RESTORE_OK);
    CHECK(end_info.phase==GT_RESTORE_ENDED && end_info.session_ended && end_info.terminal_sequence==s.events);
    reject_inflight(&s,&io,cursor);
    cursor=drain(&s,&io,cursor);CHECK(cursor==s.events && (cursor>cut || (recovery && cursor==cut)));
    uint8_t result_bytes[RSW_BYTES];size_t result_length=0;
    CHECK(rsw_encode(s.generation,&s.worker_result,result_bytes,sizeof(result_bytes),&result_length));
    CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));
    send_checkpoint(&io,PW_EXPECT,s.generation,cursor,&s.final,s.summary,&s.command,scheme,s.dark,s.history_policy,history_state(&s));
    expect(&io,PW_FINISHED,cursor,&frame);CHECK(!frame.length);
    CHECK(pw_send(&io,PW_FINISHED_ACK,cursor,NULL,0));
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    uint8_t byte;CHECK(gt__peer_receive(peer,&byte,1,1000).kind==GT_PEER_CLOSED);
    CHECK(!gt__attachment_current(s.attachment,s.generation));retire(&s,&lease,&checkpoint);gt__peer_close(peer);
    printf("PASS process/%s: fresh GUI g=%llu S=%llu L=%llu final=%llu, authoritative replies and real input\n",argv[2],
      (unsigned long long)s.generation,(unsigned long long)checkpoint.sequence,(unsigned long long)cut,(unsigned long long)cursor);
  }
  CHECK(!old && s.managed_disposed && !s.peer_bytes && !s.backend_bytes);process_registry_close(&s);CHECK(gt__attachment_close(s.attachment)==GT_TASK_OK && gt__tasks_close(s.tasks)==GT_TASK_OK);CHECK(p0_budget_scope_close(&s.encoder_stack));
  p0_checkpoint_release(&s.final);gt__program_status_close(s.summary);CHECK(p0_budget_scope_close(&s.source));CHECK(p0_budget_scope_close(&s.reference));
  CHECK(p0_budget_scope_close(&s.main_stack));
  p0_budget_usage usage=p0_budget_read(&s.budget);CHECK(!usage.live && !usage.committed && usage.mapped==usage.unmapped && p0_budget_destroy(&s.budget));
  struct rusage resources;
  if(!getrusage(RUSAGE_SELF,&resources))
    printf("METRIC process/%s authority_peak_rss_bytes=%ld user_cpu_s=%.6f system_cpu_s=%.6f\n",argv[2],
      resources.ru_maxrss,resources.ru_utime.tv_sec+resources.ru_utime.tv_usec/1e6,
      resources.ru_stime.tv_sec+resources.ru_stime.tv_usec/1e6);
  else fprintf(stderr,"METRIC authority getrusage unavailable: %d\n",errno);
  CHECK(!pthread_cond_destroy(&s.changed) && !pthread_mutex_destroy(&s.lock));return 0;
}
