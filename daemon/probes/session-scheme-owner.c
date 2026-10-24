/* P0 headless color-query adapter. No GUI or production protocol. */
#define _DARWIN_C_SOURCE
#include "session/worker.h"
#include "budget-posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MIB ((size_t)1024*1024)
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL scheme-owner:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {
    pthread_mutex_t lock;pthread_cond_t changed;
    p0_budget budget;p0_budget_scope source,worker_stack,waiter_stack;
    gt_thread_options_t thread_options;size_t reserve;
    gt_session_worker_t *worker;GhosttyTerminal terminal;gt_worker_reply_sink_t sink;
    uint32_t dark;unsigned queries,replies,completed;uint64_t color_sequence;
    gt_status_t completion,ending;gt_state_capture_t capture;
    bool finished;char output[256];size_t used;
} fixture;
static void *allocate(void *context,size_t n){const GhosttyAllocator *a=p0_budget_allocator(context,P0_BUDGET_SCRATCH);return a->vtable->alloc(a->ctx,n,0,0);}
static void release(void *context,void *p,size_t n){const GhosttyAllocator *a=p0_budget_allocator(context,P0_BUDGET_SCRATCH);a->vtable->free(a->ctx,p,n,0,0);}
static void stack(fixture *f){size_t actual=pthread_get_stacksize_np(pthread_self());CHECK(actual>=MIB && actual<=f->reserve-f->thread_options.guard_size);}
static bool color(GhosttyTerminal terminal,void *context,GhosttyColorScheme *out){
    (void)terminal;fixture *f=context;stack(f);f->queries++;
    *out=f->dark?GHOSTTY_COLOR_SCHEME_DARK:GHOSTTY_COLOR_SCHEME_LIGHT;return true;
}
static void reply(GhosttyTerminal terminal,void *context,const uint8_t *p,size_t n){
    (void)terminal;fixture *f=context;CHECK(n==9 && !memcmp(p,"\x1b[?997;",7));f->replies++;CHECK(f->sink.write(f->sink.context,p,n)==GT_OK);
}
static gt_status_t open_state(void *context,gt_worker_reply_sink_t sink,void **out){
    fixture *f=context;stack(f);f->sink=sink;
    CHECK(ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(&f->source,P0_BUDGET_GENERAL),p0_budget_allocator(&f->source,P0_BUDGET_PAGES),&f->terminal,80,24)==GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_USERDATA,f)==GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_WRITE_PTY,(const void *)reply)==GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,(const void *)color)==GHOSTTY_SUCCESS);
    *out=f;return GT_OK;
}
static void close_state(void *context){fixture *f=context;ghostty_terminal_free(f->terminal);f->terminal=NULL;}
static gt_io_backend_t *open_backend(void *context){
    fixture *f=context;
    const char *script="stty -echo -icanon min 1 time 0; i=0; for expected in 2 1 2; do i=$((i+1)); "
      "printf '\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); "
      "[ \"$a\" = \"$(printf '\\033[?997;%sn' \"$expected\")\" ] || exit 31; "
      "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); [ -z \"$extra\" ] || exit 32; stty min 1 time 0; "
      "printf 'OK%s_%s' \"$i\" \"$expected\"; read line; done";
    const char *args[]={"-c",script,NULL};gt_pty_opts_t options={.executable="/bin/sh",.cmd_kind=GT_PTY_CMD_DIRECT,.argv=args,.cols=80,.rows=24};
    gt_allocator_t domain={&f->source,allocate,release};return gt__pty_open_with_options(&options,&domain,&f->thread_options);
}
static gt_status_t apply(void *context,const gt_state_event_t *e){
    fixture *f=context;stack(f);
    if(e->kind==GT_STATE_COLOR_SCHEME){CHECK(e->length==sizeof(gt_state_color_scheme_t));const gt_state_color_scheme_t *scheme=e->payload;f->dark=scheme->dark;return GT_OK;}
    CHECK(e->kind==GT_STATE_OUTPUT);return p0_budget_feed(&f->source,f->terminal,e->payload,e->length)==GHOSTTY_SUCCESS?GT_OK:GT_ERR_IO;
}
static gt_status_t freeze(void *context,void **out){fixture *f=context;uint32_t *copy=allocate(&f->source,sizeof(*copy));if(!copy)return GT_ERR_IO;*copy=f->dark;*out=copy;return GT_OK;}
static void event(void *context,const gt_state_event_t *e,uint64_t sequence){fixture *f=context;if(e->kind==GT_STATE_COLOR_SCHEME)f->color_sequence=sequence;}
static void output(void *context,const uint8_t *p,size_t n,uint64_t sequence){
    (void)sequence;fixture *f=context;pthread_mutex_lock(&f->lock);CHECK(n<sizeof(f->output)-f->used);memcpy(f->output+f->used,p,n);f->used+=n;f->output[f->used]=0;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);
}
static void complete(void *context,gt_status_t rc,gt_state_capture_t capture){fixture *f=context;pthread_mutex_lock(&f->lock);f->completion=rc;f->capture=capture;f->completed++;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);}
static void finished(void *context,gt_status_t rc){fixture *f=context;pthread_mutex_lock(&f->lock);f->ending=rc;f->finished=true;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);}
static void wait_for(fixture *f,const char *marker,unsigned completions){
    struct timespec deadline;CHECK(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=6;
    pthread_mutex_lock(&f->lock);
    while((marker?!strstr(f->output,marker):f->completed<completions) && !f->finished)CHECK(!pthread_cond_timedwait(&f->changed,&f->lock,&deadline));
    CHECK(marker?strstr(f->output,marker)!=NULL:f->completed>=completions);pthread_mutex_unlock(&f->lock);
}
static void change(fixture *f,uint32_t dark,unsigned completions){
    gt_state_color_scheme_t scheme={dark};CHECK(gt__worker_submit_internal(f->worker,GT_WORKER_COLOR_SCHEME,&scheme,sizeof(scheme),complete,f)==GT_WORKER_ACCEPTED);
    CHECK(gt__worker_submit_internal(f->worker,GT_WORKER_CAPTURE,NULL,0,complete,f)==GT_WORKER_ACCEPTED);wait_for(f,NULL,completions);
    CHECK(f->completion==GT_OK && f->capture.frozen && *(uint32_t *)f->capture.frozen==dark && f->capture.sequence==f->color_sequence);
    release(&f->source,f->capture.frozen,sizeof(uint32_t));
    CHECK(gt__worker_submit_internal(f->worker,GT_WORKER_INPUT,"next\n",5,NULL,NULL)==GT_WORKER_ACCEPTED);
}
int main(void){
    fixture f={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};long page=sysconf(_SC_PAGESIZE);CHECK(page>0);f.thread_options=(gt_thread_options_t){MIB,(size_t)page};f.reserve=MIB+2*(size_t)page;
    CHECK(p0_budget_init(&f.budget,8*MIB) && p0_budget_scope_admit(&f.source,&f.budget,4*MIB));
    CHECK(p0_budget_scope_admit(&f.worker_stack,&f.budget,f.reserve) && p0_budget_scope_admit(&f.waiter_stack,&f.budget,f.reserve));
    gt_allocator_t domain={&f.source,allocate,release};gt_worker_config_t config={.context=&f,.open_state=open_state,.close_state=close_state,.open_backend=open_backend,.state_ops={apply,freeze},.event=event,.output=output,.finished=finished,.color_scheme_supported=true};
    f.worker=gt__worker_start_with_options(&config,&domain,&f.thread_options);CHECK(f.worker);wait_for(&f,"OK1_2",0);
    change(&f,1,2);wait_for(&f,"OK2_1",0);change(&f,0,4);
    /* Distinct ordinal markers prevent matching the initial light response twice. */
    wait_for(&f,"OK3_2",0);gt__worker_stop(f.worker);
    struct timespec deadline;CHECK(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;pthread_mutex_lock(&f.lock);while(!f.finished)CHECK(!pthread_cond_timedwait(&f.changed,&f.lock,&deadline));pthread_mutex_unlock(&f.lock);
    gt_worker_result_t result;CHECK(gt__worker_result(f.worker,&result)==GT_WORKER_RESULT_READY && result.kind==GT_WORKER_END_STOPPED && result.status==GT_OK && f.ending==GT_OK);
    CHECK(f.queries==3 && f.replies==3);gt__worker_join(f.worker);
    CHECK(p0_budget_scope_close(&f.source) && p0_budget_scope_close(&f.worker_stack) && p0_budget_scope_close(&f.waiter_stack));p0_budget_usage u=p0_budget_read(&f.budget);CHECK(!u.live && !u.committed && u.mapped==u.unmapped && p0_budget_destroy(&f.budget));
    CHECK(!pthread_cond_destroy(&f.changed) && !pthread_mutex_destroy(&f.lock));
    printf("PASS scheme-owner: real VT light/dark/light queries, ordered configuration capture, sole replies, peak=%zu final_live=0\n",u.peak);return 0;
}
