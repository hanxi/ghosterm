#define _POSIX_C_SOURCE 200809L
#include "vt-state.h"
#include "vt-snapshot.h"
#include "budget-posix.h"
#include "checkpoint-budget.h"
#include "ipc/socket-stream.h"
#include "ipc/attachment-client.h"
#include "ipc/client-bootstrap.h"
#include "ipc/control-client.h"
#include <sys/socket.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MIB (1024u*1024u)
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
typedef union {max_align_t align;size_t bytes;} allocation;
typedef struct {atomic_size_t calls,fail,live;atomic_bool erased;} memory;
static void *allocate(void *context,size_t n) {
  memory *m=context;size_t call=atomic_fetch_add(&m->calls,1)+1;
  if(call==atomic_load(&m->fail))return NULL;
  allocation *a=malloc(sizeof(*a)+n);CHECK(a);a->bytes=n;atomic_fetch_add(&m->live,1);return a+1;
}
static void release(void *context,void *p,size_t n) {
  memory *m=context;allocation *a=(allocation *)p-1;CHECK(a->bytes==n && atomic_fetch_sub(&m->live,1)>0);
  if(atomic_load(&m->erased)){const uint8_t *bytes=p;for(size_t i=0;i<n;i++)CHECK(!bytes[i]);}
  free(a);
}
static gt_allocator_t allocator(memory *m){return (gt_allocator_t){m,allocate,release};}
static gt_vt_config_t config(void) {
  return (gt_vt_config_t){.budget_bytes=256*MIB,.state_bytes=32*MIB,.frozen_bytes=32*MIB,
    .encoder_bytes=32*MIB,.checkpoint_bytes=8*MIB,.continuation_bytes=MIB,
    .history_bytes=MIB,.history_lines=100,.image_bytes=MIB,.cell_width=10,.cell_height=20,
    .light={{0xab,0xcd,0xef},{0,0,0}},.dark={{0x12,0x34,0x56},{255,255,255}}};
}
typedef struct {uint8_t bytes[4096];size_t used;} replies;
static gt_status_t reply(void *context,const uint8_t *p,size_t n) {
  replies *r=context;CHECK(n<=sizeof(r->bytes)-r->used);memcpy(r->bytes+r->used,p,n);r->used+=n;return GT_OK;
}
static gt_vt_factory_t *factory(memory *m,gt_vt_config_t *c) {
  gt_vt_factory_t *f=NULL;gt_allocator_t a=allocator(m);CHECK(gt__vt_factory_open(c,&a,&f)==GT_OK && f);return f;
}
static void *state(gt_runtime_state_factory_t a,replies *r) {
  gt_create_spec_t spec={.cols=80,.rows=24};void *s=NULL;
  CHECK(a.open(a.context,1,1,&spec,(gt_worker_reply_sink_t){r,reply},&s)==GT_OK && s);return s;
}
static void feed(gt_runtime_state_factory_t a,void *s,const char *text) {
  gt_state_event_t e={GT_STATE_OUTPUT,text,strlen(text)};CHECK(a.ops.apply(s,&e)==GT_OK);
}
static void *capture(gt_runtime_state_factory_t a,void *s) {
  void *v=NULL;CHECK(a.ops.freeze(s,&v)==GT_OK && v);return v;
}
static gt_vt_encoded_t *encode(gt_vt_factory_t *f,const void *v) {
  gt_vt_encoded_t *e=NULL;CHECK(gt__vt_checkpoint_encode(f,v,8*MIB,&e)==GT_OK && e);return e;
}
static GhosttyTerminal decode(gt_vt_snapshot_view_t v,p0_budget_scope *scope) {
  CHECK(v.canonical_bytes && v.resource_bytes && v.continuation_bytes<=SIZE_MAX);
  const GhosttyAllocator *a=p0_budget_allocator(scope,P0_BUDGET_GENERAL);
  GhosttySnapshotDecoder d=NULL;GhosttyTerminal t=NULL;bool retain=true;
  CHECK(ghostty_snapshot_decoder_new_buf(a,&d,v.canonical,v.canonical_bytes)==GHOSTTY_SUCCESS);
  CHECK(ghostty_snapshot_decoder_p0_page_allocator(d,p0_budget_allocator(scope,P0_BUDGET_PAGES))==GHOSTTY_SUCCESS);
  size_t continuation=(size_t)v.continuation_bytes;
  CHECK(ghostty_snapshot_decoder_set(d,GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES,&continuation)==GHOSTTY_SUCCESS);
  CHECK(ghostty_snapshot_decoder_set(d,GHOSTTY_SNAPSHOT_DECODER_OPT_RETAIN_CONTINUATION,&retain)==GHOSTTY_SUCCESS);
  CHECK(ghostty_snapshot_decoder_decode(d,&t)==GHOSTTY_SUCCESS);
  size_t offset=0;CHECK(ghostty_snapshot_decoder_get(d,GHOSTTY_SNAPSHOT_DECODER_DATA_SOURCE_OFFSET,&offset)==GHOSTTY_SUCCESS && offset==v.canonical_bytes);
  ghostty_snapshot_decoder_free(d);
  CHECK(ghostty_terminal_p0_resources_decode(t,v.resources,v.resource_bytes,8*MIB)==0);
  bool enabled=true;
  CHECK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_IMAGE_MEDIUM_FILE,&enabled)==GHOSTTY_SUCCESS && !enabled);
  CHECK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_IMAGE_MEDIUM_SHARED_MEM,&enabled)==GHOSTTY_SUCCESS && !enabled);
  CHECK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_WRITE_PTY,NULL)==GHOSTTY_SUCCESS);
  return t;
}
static char *terminal_text(GhosttyTerminal t,const GhosttyAllocator *a) {
  GhosttyFormatter fmt=NULL;GhosttyFormatterTerminalOptions options={.size=sizeof(options),.emit=GHOSTTY_FORMATTER_FORMAT_PLAIN,.trim=true};
  CHECK(ghostty_formatter_terminal_new(a,&fmt,t,options)==GHOSTTY_SUCCESS);
  uint8_t *p=NULL;size_t n=0;CHECK(ghostty_formatter_format_alloc(fmt,a,&p,&n)==GHOSTTY_SUCCESS);
  char *result=malloc(n+1);CHECK(result);if(n)memcpy(result,p,n);result[n]=0;
  ghostty_free(a,p,n);ghostty_formatter_free(fmt);return result;
}
static char *inspect_view(gt_vt_snapshot_view_t v,uint16_t *cols,uint16_t *rows,bool image) {
  p0_budget b;p0_budget_scope scope;CHECK(p0_budget_init(&b,64*MIB));CHECK(p0_budget_scope_admit(&scope,&b,64*MIB));
  const GhosttyAllocator *a=p0_budget_allocator(&scope,P0_BUDGET_GENERAL);GhosttyTerminal t=decode(v,&scope);
  if(image) {
    GhosttyKittyGraphics g=NULL;CHECK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&g)==GHOSTTY_SUCCESS);
    GhosttyKittyGraphicsImage i=ghostty_kitty_graphics_image(g,7);CHECK(i);
    const uint8_t *pixels=NULL;size_t length=0;
    CHECK(ghostty_kitty_graphics_image_get(i,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels)==GHOSTTY_SUCCESS);
    CHECK(ghostty_kitty_graphics_image_get(i,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&length)==GHOSTTY_SUCCESS);
    const uint8_t red[]={255,0,0,255};CHECK(length==sizeof(red) && !memcmp(pixels,red,sizeof(red)));
  }
  CHECK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_COLS,cols)==GHOSTTY_SUCCESS);
  CHECK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_ROWS,rows)==GHOSTTY_SUCCESS);
  char *result=terminal_text(t,a);ghostty_terminal_free(t);
  CHECK(p0_budget_scope_close(&scope) && p0_budget_destroy(&b));return result;
}
static char *inspect(gt_vt_encoded_t *e,uint16_t *cols,uint16_t *rows,bool image){return inspect_view(gt__vt_encoded_view(e),cols,rows,image);}
static char *text(gt_vt_encoded_t *e,uint16_t *cols,uint16_t *rows){return inspect(e,cols,rows,false);}
static void resources(void) {
  memory m={0};gt_vt_config_t c=config();gt_vt_factory_t *f=factory(&m,&c);
  gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  feed(a,s,"\033_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\033\\");
  void *v=capture(a,s);a.close(s);gt_vt_encoded_t *e=encode(f,v);a.release_frozen(f,v);
  uint16_t cols,rows;char *out=inspect(e,&cols,&rows,true);free(out);
  gt__vt_encoded_release(e);CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
static void authority(void) {
  memory m={0};gt_vt_config_t c=config();gt_vt_factory_t *f=factory(&m,&c);
  gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  feed(a,s,"FIRST\033[6n\033[?996n");
  const char light_reply[]="\033[1;6R\033[?997;2n";
  CHECK(r.used==sizeof(light_reply)-1 && !memcmp(r.bytes,light_reply,sizeof(light_reply)-1));
  gt_state_size_t size={51,17,1000,1000};gt_state_event_t e={GT_STATE_RESIZE,&size,sizeof(size)};
  CHECK(a.ops.apply(s,&e)==GT_OK);
  gt_state_color_scheme_t dark={1};e=(gt_state_event_t){GT_STATE_COLOR_SCHEME,&dark,sizeof(dark)};
  CHECK(a.ops.apply(s,&e)==GT_OK);r.used=0;feed(a,s,"\033[?996n");
  const char dark_reply[]="\033[?997;1n";
  CHECK(r.used==sizeof(dark_reply)-1 && !memcmp(r.bytes,dark_reply,sizeof(dark_reply)-1));
  void *v=capture(a,s);gt_vt_encoded_t *encoded=encode(f,v);gt_vt_snapshot_view_t view=gt__vt_encoded_view(encoded);
  CHECK(view.is_dark && view.cell_width==10 && view.cell_height==20 && view.history_bytes==MIB && view.history_lines==100);
  CHECK(!memcmp(&view.light,&c.light,sizeof(c.light)) && !memcmp(&view.dark,&c.dark,sizeof(c.dark)));
  uint16_t cols=0,rows=0;char *out=text(encoded,&cols,&rows);CHECK(cols==51 && rows==17 && strstr(out,"FIRST"));free(out);
  gt__vt_encoded_release(encoded);a.release_frozen(f,v);a.close(s);
  CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
static void style_defaults(void) {
  memory m={0};gt_vt_config_t c=config();gt_vt_factory_t *f=factory(&m,&c);
  gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  CHECK(a.style_supported);
  gt_session_style_t style={.dark=1,.background={1,2,3},.foreground={4,5,6},.cursor={7,8,9}};
  for(size_t i=0;i<256;i++)for(size_t j=0;j<3;j++)style.palette[i][j]=(uint8_t)(i*3+j);
  gt_state_event_t e={GT_STATE_STYLE,&style,sizeof(style)};CHECK(a.ops.apply(s,&e)==GT_OK);
  feed(a,s,"\033]4;5;rgb:22/33/44\033\\\033]11;rgb:55/66/77\033\\");
  void *v=capture(a,s);
  style.dark=0;style.background[0]=10;style.foreground[0]=11;style.cursor[0]=12;style.palette[5][0]=13;
  CHECK(a.ops.apply(s,&e)==GT_OK);
  void *second=capture(a,s);a.close(s);
  for(unsigned n=0;n<2;n++) {
    gt_vt_encoded_t *encoded=encode(f,n?second:v);gt_vt_snapshot_view_t view=gt__vt_encoded_view(encoded);
    CHECK(view.is_dark==!n);CHECK((n?view.light.background[0]:view.dark.background[0])==(n?10:1));
    p0_budget budget;p0_budget_scope scope;CHECK(p0_budget_init(&budget,64*MIB) && p0_budget_scope_admit(&scope,&budget,64*MIB));
    GhosttyTerminal terminal=decode(view,&scope);GhosttyColorRgb color,palette[256];
    CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND,&color)==GHOSTTY_SUCCESS && color.r==0x55 && color.g==0x66 && color.b==0x77);
    CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND_DEFAULT,&color)==GHOSTTY_SUCCESS && color.r==(n?10:1));
    CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_FOREGROUND_DEFAULT,&color)==GHOSTTY_SUCCESS && color.r==(n?11:4));
    CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_CURSOR_DEFAULT,&color)==GHOSTTY_SUCCESS && color.r==(n?12:7));
    CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_PALETTE,palette)==GHOSTTY_SUCCESS && palette[5].r==0x22 && palette[5].g==0x33 && palette[5].b==0x44);
    CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT,palette)==GHOSTTY_SUCCESS && palette[5].r==(n?13:15) && palette[255].b==255);
    ghostty_terminal_free(terminal);CHECK(p0_budget_scope_close(&scope) && p0_budget_destroy(&budget));
    gt__vt_encoded_release(encoded);
  }
  a.release_frozen(f,v);a.release_frozen(f,second);CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
typedef struct {gt_vt_factory_t *factory;const void *frozen;gt_vt_encoded_t *encoded;} encode_job;
static void *encode_off_owner(void *context) {encode_job *j=context;j->encoded=encode(j->factory,j->frozen);return NULL;}
static void lifetime(void) {
  memory m={0};gt_vt_config_t c=config();gt_vt_factory_t *f=factory(&m,&c);
  gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  feed(a,s,"BEFORE\033[3");void *first=capture(a,s);
  feed(a,s,"1mAFTER");void *second=capture(a,s);a.close(s);
  CHECK(gt__vt_factory_close(f)==GT_RUNTIME_BUSY);
  encode_job j={.factory=f,.frozen=first};pthread_t thread;CHECK(!pthread_create(&thread,NULL,encode_off_owner,&j));CHECK(!pthread_join(thread,NULL));
  gt_vt_encoded_t *later=encode(f,second);a.release_frozen(f,first);a.release_frozen(f,second);
  CHECK(gt__vt_factory_close(f)==GT_RUNTIME_BUSY);
  uint16_t cols,rows;char *before=text(j.encoded,&cols,&rows),*after=text(later,&cols,&rows);
  CHECK(strstr(before,"BEFORE") && !strstr(before,"AFTER") && strstr(after,"BEFOREAFTER"));free(before);free(after);
  gt__vt_encoded_release(j.encoded);gt__vt_encoded_release(later);
  CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
static void policy(void) {
  memory m={0};gt_allocator_t alloc=allocator(&m);gt_vt_config_t c=config();gt_vt_factory_t *f=(void *)1;
  c.external_resources=true;CHECK(gt__vt_factory_open(&c,&alloc,&f)==GT_ERR_UNSUPPORTED && !f && !atomic_load(&m.calls));
  c=config();c.continuation_bytes=0;CHECK(gt__vt_factory_open(&c,&alloc,&f)==GT_ERR_INVALID_ARG && !f && !atomic_load(&m.calls));
  c=config();f=factory(&m,&c);gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  void *v=capture(a,s);gt_vt_encoded_t *e=(void *)1;
  CHECK(gt__vt_checkpoint_encode(f,v,1,&e)!=GT_OK && !e);e=encode(f,v);gt__vt_encoded_release(e);
  a.invalidated(s,GT_ERR_IO,0);void *bad=(void *)1;CHECK(a.ops.freeze(s,&bad)!=GT_OK && !bad);
  a.release_frozen(f,v);a.close(s);CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
static void capacity(void) {
  memory m={0};gt_vt_config_t c=config();c.budget_bytes=c.state_bytes;
  gt_vt_factory_t *f=factory(&m,&c);gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  gt_create_spec_t spec={.cols=80,.rows=24};void *extra=(void *)1;
  CHECK(a.open(f,1,2,&spec,(gt_worker_reply_sink_t){&r,reply},&extra)!=GT_OK && !extra);
  void *v=(void *)1;CHECK(a.ops.freeze(s,&v)!=GT_OK && !v);
  /* Refused snapshot admission must not poison the source scope. */
  feed(a,s,"still alive\033[6n");CHECK(r.used>0);a.close(s);
  CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
static void allocation_failures(void) {
  for(size_t fail=1;fail<=4;fail++) {
    memory m={0};atomic_store(&m.fail,fail);gt_vt_config_t c=config();gt_allocator_t alloc=allocator(&m);
    gt_vt_factory_t *f=NULL;gt_status_t rc=gt__vt_factory_open(&c,&alloc,&f);
    if(fail==1){CHECK(rc!=GT_OK && !f && !atomic_load(&m.live));continue;}
    CHECK(rc==GT_OK);gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=NULL;
    gt_create_spec_t spec={.cols=80,.rows=24};rc=a.open(f,1,1,&spec,(gt_worker_reply_sink_t){&r,reply},&s);
    if(fail==2)CHECK(rc!=GT_OK && !s);
    else {
      CHECK(rc==GT_OK);void *v=NULL;rc=a.ops.freeze(s,&v);
      if(fail==3)CHECK(rc!=GT_OK && !v);
      else {CHECK(rc==GT_OK);gt_vt_encoded_t *e=NULL;CHECK(gt__vt_checkpoint_encode(f,v,8*MIB,&e)!=GT_OK && !e);a.release_frozen(f,v);}
      a.close(s);
    }
    CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
  }
}
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
typedef struct {atomic_bool done;gt_status_t status;gt_state_capture_t capture;} capture_job;
static void captured(void *context,gt_status_t status,gt_state_capture_t capture) {
  capture_job *j=context;j->status=status;j->capture=capture;atomic_store(&j->done,true);
}
static void wait_ready(gt_vt_factory_t *f,gt_runtime_session_t *lease,const char *needle) {
  for(unsigned attempt=0;attempt<100;attempt++) {
    capture_job j={0};gt_worker_submit_t admission;
    CHECK(gt__runtime_capture(lease,captured,&j,&admission)==GT_MANAGER_OK && admission==GT_WORKER_ACCEPTED);
    for(unsigned i=0;!atomic_load(&j.done);i++){CHECK(i<5000);pause_one();}
    CHECK(j.status==GT_OK && j.capture.frozen);
    gt_vt_encoded_t *e=encode(f,j.capture.frozen);uint16_t cols,rows;char *out=text(e,&cols,&rows);
    bool ready=strstr(out,needle)!=NULL;free(out);gt__vt_encoded_release(e);
    gt__runtime_release_capture(lease,&j.capture);CHECK(!j.capture.frozen);
    if(ready)return;
    pause_one();
  }
  CHECK(false);
}
static void runtime(const char *cwd) {
  memory m={0};gt_allocator_t alloc=allocator(&m);gt_vt_config_t c=config();gt_vt_factory_t *f=factory(&m,&c);
  gt_checkpoint_tasks_t *tasks=gt__tasks_open_with_allocator(2,&alloc);CHECK(tasks);
  gt_runtime_config_t conf={.instance={1},.sessions=2,.creation_records=2,.creation_retention_ms=1000,
    .queue_bytes=8192,.queue_events=32,.tasks=tasks,.state=gt__vt_factory_state(f)};
  gt_session_runtime_t *rt=NULL;CHECK(gt__runtime_open(&conf,0,&alloc,&rt)==GT_OK);
  /* Query answer must originate from headless authority while no GUI exists. */
  gt_create_spec_t spec={.nonce={1},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,
    .executable="/bin/sh",.cwd=cwd,.term="xterm-256color",.argc=2,
    .argv={"-c","stty -echo -icanon min 1 time 0; printf 'ORIGINAL\\033[6n'; answer=$(dd bs=1 count=6 2>/dev/null); [ \"${#answer}\" -eq 6 ] || exit 91; printf '%s' '-REPLIED'; byte=$(dd bs=1 count=1 2>/dev/null); [ \"$byte\" = x ] || exit 92; printf '%s' '-REATTACHED'; exit 7"}};
  uint8_t bytes[8192];size_t n=0;CHECK(gt__create_spec_encode(&spec,bytes,sizeof(bytes),&n)==GT_OK);
  gt_creation_result_t created=gt__creation_submit(gt__runtime_creation(rt),1,bytes,n,0);
  CHECK(created.status==GT_CREATION_ACCEPTED && created.session);gt_runtime_session_t *lease=NULL;
  CHECK(gt__runtime_acquire(rt,1,created.session,&lease)==GT_RUNTIME_OK);
  gt_session_attachment_t *attachment=gt__runtime_attachment(lease);uint64_t old,next;
  CHECK(gt__attachment_attach(attachment,&old)==GT_TASK_OK);
  wait_ready(f,lease,"ORIGINAL-REPLIED");
  CHECK(gt__attachment_detach(attachment,old)==GT_TASK_OK);gt__runtime_release(&lease);
  CHECK(gt__runtime_acquire(rt,1,created.session,&lease)==GT_RUNTIME_OK);
  CHECK(gt__runtime_attachment(lease)==attachment && gt__attachment_attach(attachment,&next)==GT_TASK_OK && next>old);
  CHECK(gt__attachment_detach(attachment,old)==GT_TASK_INVALID && gt__attachment_current(attachment,next));
  /* Trusted test host submission, not client authorization or a wire shortcut. */
  gt_worker_submit_t admission;
  CHECK(gt__manager_submit_internal(gt__runtime_manager(rt),created.session,GT_WORKER_INPUT,"x",1,NULL,NULL,&admission)==GT_MANAGER_OK && admission==GT_WORKER_ACCEPTED);
  gt_runtime_final_t final={0};gt_runtime_rc_t rc=GT_RUNTIME_BUSY;
  for(unsigned i=0;i<5000 && rc==GT_RUNTIME_BUSY;i++){rc=gt__runtime_final(lease,&final);if(rc==GT_RUNTIME_BUSY)pause_one();}
  CHECK(rc==GT_RUNTIME_OK && final.result.kind==GT_WORKER_END_EOF);
  /* PTY EOF can precede the waiter's observed status; unknown stays unknown. */
  CHECK(!final.result.has_exit_code || final.result.exit_code==7);
  CHECK(final.capture_status==GT_OK && final.frozen && final.result.has_sequence && final.sequence>0);
  CHECK(gt__runtime_reap(rt,created.session)==GT_MANAGER_OK);
  gt_vt_encoded_t *e=encode(f,final.frozen);uint16_t cols,rows;char *out=text(e,&cols,&rows);
  CHECK(strstr(out,"ORIGINAL-REPLIED-REATTACHED"));free(out);gt__vt_encoded_release(e);
  CHECK(gt__attachment_detach(attachment,next)==GT_TASK_OK);
  gt__runtime_release(&lease);CHECK(gt__runtime_forget(rt,created.session)==GT_RUNTIME_OK);
  CHECK(gt__runtime_close(rt)==GT_RUNTIME_OK && gt__tasks_close(tasks)==GT_TASK_OK);
  CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live));
}
static void snapshot(void) {
  memory m={0},wire={0};atomic_store(&wire.erased,true);gt_vt_config_t c=config();gt_vt_factory_t *f=factory(&m,&c);
  gt_runtime_state_factory_t a=gt__vt_factory_state(f);replies r={0};void *s=state(a,&r);
  feed(a,s,"SNAPSHOT\033_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\033\\");void *v=capture(a,s);a.close(s);
  gt_snapshot_encoder_t encoder=gt__vt_snapshot_encoder(f);gt_allocator_t domain=allocator(&wire);
  gt_snapshot_blob_t *blob=(void *)1;
  CHECK(encoder.encode(f,v,128,&domain,&blob)==GT_ERR_IO && !blob && !atomic_load(&wire.calls));
  atomic_store(&wire.fail,1);CHECK(encoder.encode(f,v,8*MIB,&domain,&blob)==GT_ERR_IO && !blob && !atomic_load(&wire.live));
  atomic_store(&wire.fail,0);CHECK(encoder.encode(f,v,8*MIB,&domain,&blob)==GT_OK && blob && atomic_load(&wire.live)==1);
  gt_vt_snapshot_view_t view;CHECK(gt_vt_snapshot_parse(blob->bytes,blob->length,8*MIB,&view)==GT_OK);
  CHECK(view.continuation_bytes==MIB && view.image_bytes==MIB && view.profile==1);
  uint16_t cols,rows;char *out=inspect_view(view,&cols,&rows,true);CHECK(strstr(out,"SNAPSHOT"));free(out);
  size_t exact=blob->length;encoder.release(f,blob);blob=(void *)1;
  CHECK(encoder.encode(f,v,exact-1,&domain,&blob)!=GT_OK && !blob);
  CHECK(encoder.encode(f,v,exact,&domain,&blob)==GT_OK && blob->length==exact);encoder.release(f,blob);
  a.release_frozen(f,v);CHECK(gt__vt_factory_close(f)==GT_RUNTIME_OK && !atomic_load(&m.live) && !atomic_load(&wire.live));
}
typedef struct {gt_server_t *server;gt_connection_t *client;gt_socket_stream_t in,out;uint64_t id;gt_control_client_t *management;gt_control_result_t reply;unsigned replies;} peer;
typedef struct {
  memory memory;gt_vt_factory_t *factory;gt_checkpoint_tasks_t *tasks;gt_session_runtime_t *runtime;
  gt_attachment_owner_t *owner;gt_data_router_t *router;gt_server_t *server;uint64_t session;
  gt_snapshot_encoder_t encoder;atomic_bool encoder_entered,encoder_release;
} service;
static gt_status_t held_encode(void *context,const void *frozen,size_t limit,const gt_allocator_t *a,gt_snapshot_blob_t **out) {
  service *s=context;atomic_store(&s->encoder_entered,true);
  for(unsigned i=0;!atomic_load(&s->encoder_release);i++){CHECK(i<5000);pause_one();}
  return s->encoder.encode(s->encoder.context,frozen,limit,a,out);
}
static void held_release(void *context,gt_snapshot_blob_t *blob) {
  service *s=context;s->encoder.release(s->encoder.context,blob);
}
static bool now_ms(void *context,uint64_t *out) {
  (void)context;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;
  *out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;
}
static void management_result(void *context,const gt_control_result_t *result){peer *p=context;p->reply=*result;p->replies++;}
static void peer_connect(peer *p,service *s,gt_protocol_role_t role,const gt_protocol_bind_t *binding) {
  *p=(peer){.server=s->server};int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));gt_stream_ops_t in,out;
  CHECK(gt__socket_stream_adopt(sockets[0],&p->in,&in)==GT_OK && gt__socket_stream_adopt(sockets[1],&p->out,&out)==GT_OK);
  CHECK(gt__server_admit(s->server,&in,73,&p->id)==GT_SERVER_ADMITTED);
  CHECK(gt__connection_open(&out,(gt_connection_limits_t){65560,65560,2000},NULL,&p->client)==GT_CONNECTION_CREATED);
  gt_client_bootstrap_config_t config={.hello={.role=role,.compatibility={2},.expected_instance={1}},.timeout_ms=5000,.clock=now_ms};
  if(binding)config.binding=*binding;
  gt_allocator_t domain=allocator(&s->memory);gt_client_bootstrap_t *bootstrap=NULL;
  CHECK(gt__client_bootstrap_open(p->client,&config,&domain,&bootstrap)==GT_OK);p->client=NULL;
  for(unsigned i=0;;i++){CHECK(i<5000);CHECK(gt__server_step(s->server)==GT_SERVER_RUNNING);gt_bootstrap_state_t state=gt__client_bootstrap_step(bootstrap);CHECK(state!=GT_BOOTSTRAP_FAILED && state!=GT_BOOTSTRAP_DRAINING);if(state==GT_BOOTSTRAP_READY)break;pause_one();}
  gt_bootstrap_result_t result;CHECK(gt__client_bootstrap_take(bootstrap,&p->client,&result) && result.role==role && result.identity.instance[0]==1);gt__client_bootstrap_close(bootstrap);
  if(role==GT_ROLE_MANAGEMENT) {
    gt_control_client_config_t management={.heartbeat_ms=1000,.request_timeout_ms=5000,.context=p,.clock=now_ms,.result=management_result};memcpy(management.instance,result.identity.instance,GT_INSTANCE_BYTES);
    CHECK(gt__control_client_open(p->client,&management,&domain,&p->management)==GT_OK);p->client=NULL;
  } else CHECK(binding && result.session==binding->session && result.generation==binding->generation);
}
static gt_control_result_t request(peer *p,gt_control_request_t request) {
  uint64_t id;gt_status_t error;unsigned before=p->replies;CHECK(gt__control_client_submit(p->management,&request,&id,&error)==GT_CONTROL_CLIENT_QUEUED);
  for(unsigned i=0;p->replies==before;i++){CHECK(i<5000);CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING && gt__control_client_step(p->management)!=GT_CONTROL_CLIENT_FAILED);pause_one();}
  CHECK(p->replies==before+1 && p->reply.delivery==GT_CONTROL_CLIENT_REPLY && p->reply.request_id==id);return p->reply;
}
static void peer_close(peer *p) {
  if(p->management){gt__control_client_close(p->management);p->management=NULL;}
  gt__connection_destroy(p->client);p->client=NULL;
  for(unsigned i=0;gt__server_connected(p->server,p->id);i++){CHECK(i<5000);CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING);pause_one();}
}
static void open_service(service *s) {
  memset(s,0,sizeof(*s));gt_allocator_t a=allocator(&s->memory);gt_vt_config_t c=config();s->factory=factory(&s->memory,&c);
  s->tasks=gt__tasks_open_with_allocator(2,&a);CHECK(s->tasks);uint64_t now;CHECK(now_ms(NULL,&now));
  gt_runtime_config_t rc={.instance={1},.sessions=2,.creation_records=2,.creation_retention_ms=60000,
    .queue_bytes=65536,.queue_events=128,.tasks=s->tasks,.state=gt__vt_factory_state(s->factory)};
  CHECK(gt__runtime_open(&rc,now,&a,&s->runtime)==GT_OK);
  gt_attachment_owner_config_t oc={.runtime=s->runtime,.instance={1},.capacity=4,.token_ttl_ms=5000};
  CHECK(gt__attachment_owner_open(&oc,now,&a,&s->owner)==GT_OK);
  s->encoder=gt__vt_snapshot_encoder(s->factory);
  gt_data_router_config_t dc={.runtime=s->runtime,.attachments=s->owner,.instance={1},.connections=4,.user_commands=4,
    .snapshot_limit=8*MIB,.chunk_bytes=61,.format=GT_VT_SNAPSHOT_FORMAT,.restore_timeout_ms=10000,.error_timeout_ms=1000,
    .clock=now_ms,.encoder={s,held_encode,held_release}};
  CHECK(gt__data_router_open(&dc,&a,&s->router)==GT_OK);
  gt_server_config_t sc={.manager=gt__runtime_manager(s->runtime),.creation=gt__runtime_creation(s->runtime),.attachments=s->owner,
    .connections=4,.limits={65560,65560,2000},.idle_timeout_ms=60000,
    .data_context=s->router,.data_step=gt__data_router_step,.data_drop=gt__data_router_drop,.data_revoked=gt__data_router_revoked,
    .handshake={.identity={.instance={1},.compatibility={2}},.clock=now_ms,.timeout_ms=2000}};
  CHECK(gt__server_open(&sc,&a,&s->server)==GT_OK);
}
static void close_service(service *s) {
  gt__server_close(s->server);
  for(unsigned i=0;;i++){CHECK(i<5000);gt_runtime_rc_t rc=gt__data_router_close(s->router);if(rc==GT_RUNTIME_OK)break;CHECK(rc==GT_RUNTIME_BUSY);pause_one();}
  CHECK(gt__attachment_owner_close(s->owner)==GT_RUNTIME_OK && gt__runtime_close(s->runtime)==GT_RUNTIME_OK && gt__tasks_close(s->tasks)==GT_TASK_OK);
  CHECK(gt__vt_factory_close(s->factory)==GT_RUNTIME_OK && !atomic_load(&s->memory.live));
}
static gt_attach_result_t attach(peer *control,peer *data,service *s) {
  gt_control_result_t result=request(control,(gt_control_request_t){.type=GT_MSG_ATTACH,.session=s->session});gt_attach_result_t r=result.attachment;
  CHECK(result.reply_type==GT_MSG_ATTACH_REPLY && r.status==GT_ATTACH_OK && r.session==s->session);
  gt_protocol_bind_t binding={.session=r.session,.generation=r.generation,.token=r.token};memcpy(binding.instance,r.instance,16);
  peer_connect(data,s,GT_ROLE_DATA,&binding);memset(&binding.token,0,sizeof(binding.token));memset(&result,0,sizeof(result));memset(r.token.bytes,0,sizeof(r.token.bytes));memset(&control->reply,0,sizeof(control->reply));return r;
}
typedef struct {
  p0_budget budget;p0_budget_scope scope;GhosttyTerminal terminal;gt_vt_snapshot_view_t view;
  gt_attachment_client_t *client;gt_client_applied_t applied;gt_user_reply_t reply;
  uint64_t generation,sequence,snapshot;bool ended;unsigned resizes,colors,results,invalidated;
} replica;
static gt_client_sink_rc_t replica_snapshot(void *context,uint64_t generation,uint64_t sequence,uint32_t format,
  const uint8_t *bytes,size_t length,bool readonly,gt_status_t *error) {
  replica *r=context;(void)readonly;CHECK(format==GT_VT_SNAPSHOT_FORMAT && !r->terminal);
  CHECK(gt_vt_snapshot_parse(bytes,length,8*MIB,&r->view)==GT_OK);
  CHECK(p0_budget_init(&r->budget,64*MIB) && p0_budget_scope_admit(&r->scope,&r->budget,64*MIB));
  r->terminal=decode(r->view,&r->scope);r->view.canonical=r->view.resources=NULL;
  r->snapshot=r->sequence=sequence;r->applied=(gt_client_applied_t){true,generation,sequence};*error=GT_OK;
  return GT_CLIENT_SINK_ACCEPTED;
}
static gt_client_sink_rc_t replica_event(void *context,uint64_t generation,uint64_t sequence,const gt_state_event_t *event,gt_status_t *error) {
  replica *r=context;CHECK(generation==r->generation && sequence==r->sequence+1);
  if(event->kind==GT_STATE_OUTPUT)CHECK(p0_budget_feed(&r->scope,r->terminal,event->payload,event->length)==GHOSTTY_SUCCESS);
  else if(event->kind==GT_STATE_RESIZE) {
    CHECK(event->length==sizeof(gt_state_size_t));gt_state_size_t size;memcpy(&size,event->payload,sizeof(size));
    CHECK(p0_budget_resize_cells(&r->scope,r->terminal,size.cols,size.rows,r->view.cell_width,r->view.cell_height)==GHOSTTY_SUCCESS);r->resizes++;
  } else {
    CHECK(event->kind==GT_STATE_COLOR_SCHEME && event->length==sizeof(gt_state_color_scheme_t));gt_state_color_scheme_t color;memcpy(&color,event->payload,sizeof(color));
    r->view.is_dark=color.dark!=0;gt_vt_colors_t c=r->view.is_dark?r->view.dark:r->view.light;
    GhosttyColorRgb bg={c.background[0],c.background[1],c.background[2]},fg={c.foreground[0],c.foreground[1],c.foreground[2]};
    CHECK(ghostty_terminal_set(r->terminal,GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,&bg)==GHOSTTY_SUCCESS && ghostty_terminal_set(r->terminal,GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,&fg)==GHOSTTY_SUCCESS);r->colors++;
  }
  r->sequence=sequence;r->applied=(gt_client_applied_t){true,generation,sequence};*error=GT_OK;return GT_CLIENT_SINK_ACCEPTED;
}
static gt_status_t replica_applied(void *context,gt_client_applied_t *out){*out=((replica *)context)->applied;return GT_OK;}
static void replica_invalidated(void *context,uint64_t generation,gt_status_t error){replica *r=context;CHECK(generation==r->generation && error!=GT_OK && !r->invalidated);r->invalidated++;}
static void replica_result(void *context,const gt_user_reply_t *result){replica *r=context;r->reply=*result;r->results++;}
static void pump_replica(peer *p,replica *r) {
  CHECK(gt__server_step(p->server)==GT_SERVER_RUNNING);gt_client_phase_t phase=gt__attachment_client_step(r->client);
  CHECK(phase!=GT_CLIENT_FAILED && phase!=GT_CLIENT_CLOSING);gt_client_info_t info;CHECK(gt__attachment_client_info(r->client,&info));
  r->ended=info.has_terminal;if(r->ended)CHECK(info.terminal.result.has_sequence && info.terminal.result.sequence==r->sequence);
  pause_one();
}
static void restore(peer *p,replica *r,service *service,const gt_attach_result_t *identity,bool readonly) {
  uint64_t generation=identity->generation;memset(r,0,sizeof(*r));r->generation=generation;
  for(unsigned i=0;gt__connection_wants_write(p->client);i++){CHECK(i<5000);CHECK(gt__connection_step(p->client)==GT_CONNECTION_OPEN && gt__server_step(p->server)==GT_SERVER_RUNNING);}
  gt_client_config_t config={.session=identity->session,.generation=generation,.format=GT_VT_SNAPSHOT_FORMAT,.snapshot_limit=8*MIB,.commands=4,
    .restore_timeout_ms=10000,.apply_timeout_ms=1000,.heartbeat_ms=1000,.peer_timeout_ms=5000,.command_timeout_ms=1000,.error_timeout_ms=1000,
    .clock=now_ms,.sink={r,replica_snapshot,replica_event,replica_applied,replica_invalidated,replica_result}};
  gt_allocator_t domain=allocator(&service->memory);CHECK(gt__attachment_client_open(p->client,&config,&domain,&r->client)==GT_OK);p->client=NULL;
  for(unsigned i=0;;i++){CHECK(i<5000);pump_replica(p,r);gt_client_info_t info;CHECK(gt__attachment_client_info(r->client,&info));
    if(info.phase==(readonly?GT_CLIENT_READONLY:GT_CLIENT_LIVE) || (readonly && info.phase==GT_CLIENT_ENDED))break;}
}
static void close_replica(replica *r){gt__attachment_client_close(r->client);ghostty_terminal_free(r->terminal);CHECK(p0_budget_scope_close(&r->scope) && p0_budget_destroy(&r->budget));}
static gt_user_reply_t user(peer *p,replica *r,gt_user_request_t request) {
  gt_worker_stamp_t stamp;unsigned before=r->results;
  CHECK(gt__attachment_client_submit(r->client,&request,&stamp)==GT_CLIENT_QUEUED && stamp.generation==r->generation);
  for(unsigned i=0;r->results==before;i++){CHECK(i<5000);pump_replica(p,r);}
  CHECK(r->results==before+1 && r->reply.stamp.command_id==stamp.command_id);return r->reply;
}
static gt_user_request_t input(void) {return (gt_user_request_t){.kind=GT_WORKER_INPUT,.input=(const uint8_t *)"go\n",.length=3};}
static bool contains(replica *r,const char *needle) {
  char *s=terminal_text(r->terminal,p0_budget_allocator(&r->scope,P0_BUDGET_GENERAL));bool found=strstr(s,needle)!=NULL;free(s);return found;
}
static void data_flow(const char *cwd) {
  service s;open_service(&s);peer control,data;peer_connect(&control,&s,GT_ROLE_MANAGEMENT,NULL);
  gt_create_spec_t spec={.nonce={1},.command=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.environment=GT_CREATE_HOST_ENV,
    .executable="/bin/sh",.cwd=cwd,.term="xterm-256color",.argc=2,
    .argv={"-c","stty -echo -icanon min 1 time 0; printf 'READY\\033_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\\033\\\\\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); [ \"$a\" = \"$(printf '\\033[?997;2n')\" ] || exit 90; printf '%s' '-AUTH'; IFS= read -r a; [ \"$a\" = go ] || exit 91; printf '%s' '-FIRST'; IFS= read -r a; [ \"$a\" = go ] || exit 92; sleep .02; printf '%s' '-DETACHED'; printf '\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); [ \"$a\" = \"$(printf '\\033[?997;1n')\" ] || exit 93; printf '%s' '-ALIVE'; IFS= read -r a; [ \"$a\" = go ] || exit 94; printf '%s' '-END'; printf '\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); [ \"$a\" = \"$(printf '\\033[?997;1n')\" ] || exit 95; printf '%s' '-LIVE'; exit 7"}};
  gt_control_result_t response=request(&control,(gt_control_request_t){.type=GT_MSG_CREATE,.create=&spec});
  CHECK(response.reply_type==GT_MSG_CREATE_REPLY && response.creation.status==GT_CREATION_ACCEPTED);s.session=response.creation.session;
  gt_runtime_session_t *lease=NULL;CHECK(gt__runtime_acquire(s.runtime,73,s.session,&lease)==GT_RUNTIME_OK);
  wait_ready(s.factory,lease,"-AUTH");gt_attach_result_t a=attach(&control,&data,&s);
  for(unsigned i=0;!atomic_load(&s.encoder_entered);i++){CHECK(i<5000);CHECK(gt__connection_step(data.client)==GT_CONNECTION_OPEN && gt__server_step(s.server)==GT_SERVER_RUNNING);pause_one();}
  /* Test host mutation while the real encoder retains S; client remains locked. */
  gt_state_size_t during={67,21,670,420};capture_job resized={0};gt_worker_submit_t admitted;
  CHECK(gt__manager_submit_internal(gt__runtime_manager(s.runtime),s.session,GT_WORKER_RESIZE,&during,sizeof(during),captured,&resized,&admitted)==GT_MANAGER_OK && admitted==GT_WORKER_ACCEPTED);
  for(unsigned i=0;!atomic_load(&resized.done);i++){CHECK(i<5000);pause_one();}
  CHECK(resized.status==GT_OK);atomic_store(&s.encoder_release,true);
  replica first;restore(&data,&first,&s,&a,false);
  CHECK(first.sequence>first.snapshot && first.resizes==1);
  CHECK(contains(&first,"-AUTH"));gt_user_reply_t reply=user(&data,&first,input());CHECK(reply.delivery==GT_USER_OPERATION_OK);
  while(!contains(&first,"-FIRST"))pump_replica(&data,&first);
  gt_user_request_t resize={.kind=GT_WORKER_RESIZE,.stamp={a.generation,2,first.sequence},.size={51,17,510,340}};
  reply=user(&data,&first,resize);CHECK(reply.delivery==GT_USER_OPERATION_OK);while(first.resizes<2)pump_replica(&data,&first);
  gt_user_request_t color={.kind=GT_WORKER_COLOR_SCHEME,.stamp={a.generation,3,first.sequence},.color={1}};
  reply=user(&data,&first,color);CHECK(reply.delivery==GT_USER_OPERATION_OK);while(!first.colors)pump_replica(&data,&first);
  reply=user(&data,&first,input());CHECK(reply.delivery==GT_USER_OPERATION_OK);close_replica(&first);peer_close(&data);
  wait_ready(s.factory,lease,"-DETACHED-ALIVE");a=attach(&control,&data,&s);replica second;restore(&data,&second,&s,&a,false);
  CHECK(second.view.is_dark && contains(&second,"-DETACHED-ALIVE"));uint16_t cols=0,rows=0;
  CHECK(ghostty_terminal_get(second.terminal,GHOSTTY_TERMINAL_DATA_COLS,&cols)==GHOSTTY_SUCCESS && cols==51);
  CHECK(ghostty_terminal_get(second.terminal,GHOSTTY_TERMINAL_DATA_ROWS,&rows)==GHOSTTY_SUCCESS && rows==17);
  reply=user(&data,&second,input());CHECK(reply.delivery==GT_USER_OPERATION_OK);
  while(!second.ended)pump_replica(&data,&second);
  CHECK(contains(&second,"-ALIVE-END-LIVE"));
  gt_runtime_final_t final;CHECK(gt__runtime_final(lease,&final)==GT_RUNTIME_OK && final.capture_status==GT_OK && final.sequence==second.sequence);
  CHECK(gt__runtime_reap(s.runtime,s.session)==GT_MANAGER_OK);
  /* Independent authority and replica canonical/resources, not only text. */
  gt_vt_encoded_t *authority=encode(s.factory,final.frozen);gt_vt_snapshot_view_t reference=gt__vt_encoded_view(authority);
  p0_budget_scope scratch;
  /* Reserve encoder headroom independently from the replica's source scope. */
  CHECK(p0_budget_set_limit(&second.budget,96*MIB) && p0_budget_scope_admit(&scratch,&second.budget,32*MIB));
  p0_checkpoint checkpoint={0};CHECK(p0_checkpoint_capture(&second.scope,&scratch,second.terminal,8*MIB,&checkpoint,NULL)==P0_CHECKPOINT_OK);
  CHECK(reference.canonical_bytes==checkpoint.canonical_len && !memcmp(reference.canonical,checkpoint.canonical,checkpoint.canonical_len));
  CHECK(reference.resource_bytes==checkpoint.resources_len && !memcmp(reference.resources,checkpoint.resources,checkpoint.resources_len));
  p0_checkpoint_release(&checkpoint);CHECK(p0_budget_scope_close(&scratch));gt__vt_encoded_release(authority);close_replica(&second);peer_close(&data);
  a=attach(&control,&data,&s);replica readonly;restore(&data,&readonly,&s,&a,true);CHECK(contains(&readonly,"-ALIVE-END-LIVE"));
  gt_worker_stamp_t denied;gt_user_request_t request=input();CHECK(gt__attachment_client_submit(readonly.client,&request,&denied)==GT_CLIENT_LOCKED && !denied.command_id);
  close_replica(&readonly);peer_close(&data);peer_close(&control);gt__runtime_release(&lease);close_service(&s);
}
int main(int argc,char **argv) {
  CHECK(argc==3);
  if(!strcmp(argv[1],"style"))style_defaults();
  else if(!strcmp(argv[1],"authority"))authority();
  else if(!strcmp(argv[1],"lifetime"))lifetime();
  else if(!strcmp(argv[1],"policy"))policy();
  else if(!strcmp(argv[1],"capacity"))capacity();
  else if(!strcmp(argv[1],"allocation"))allocation_failures();
  else if(!strcmp(argv[1],"resources"))resources();
  else if(!strcmp(argv[1],"snapshot"))snapshot();
  else if(!strcmp(argv[1],"data-flow"))data_flow(argv[2]);
  else {CHECK(!strcmp(argv[1],"runtime"));runtime(argv[2]);}
  printf("PASS daemon VT %s\n",argv[1]);return 0;
}
