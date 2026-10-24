#include "vt-state.h"
#include "frozen-budget.h"
#include <stdatomic.h>
#include <string.h>

struct gt_vt_factory {
  gt_vt_config_t config;
  gt_allocator_t allocator;
  p0_budget budget;
  atomic_size_t objects;
};
typedef struct {
  gt_vt_factory_t *factory;
  p0_budget_scope scope;
  GhosttyTerminal terminal;
  gt_worker_reply_sink_t sink;
  bool dark,history_pruned,invalid,styled;
  gt_session_style_t style;
  gt_vt_colors_t light_colors,dark_colors;
} vt_state;
typedef struct {
  gt_vt_factory_t *factory;
  p0_budget_scope scope;
  p0_frozen_checkpoint checkpoint;
  bool dark,history_pruned;
  gt_vt_colors_t light_colors,dark_colors;
} vt_frozen;
struct gt_vt_encoded {
  gt_vt_factory_t *factory;
  p0_budget_scope scope;
  p0_checkpoint checkpoint;
  gt_vt_snapshot_view_t view;
};

static pthread_once_t png_once=PTHREAD_ONCE_INIT;
static GhosttyResult png_result=GHOSTTY_INVALID_VALUE;
static void install_png(void) {
  png_result=ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void *)ghostty_p0_decode_png);
}
static void *record_new(gt_vt_factory_t *f,size_t n) {
  void *p=f->allocator.allocate(f->allocator.context,n);
  if(p){memset(p,0,n);atomic_fetch_add(&f->objects,1);}
  return p;
}
static void record_free(gt_vt_factory_t *f,void *p,size_t n) {
  f->allocator.release(f->allocator.context,p,n);
  atomic_fetch_sub(&f->objects,1);
}
static gt_status_t status(GhosttyResult rc) {
  return rc==GHOSTTY_SUCCESS?GT_OK:GT_ERR_IO;
}
static void reply(GhosttyTerminal terminal,void *context,const uint8_t *bytes,size_t n) {
  (void)terminal;vt_state *s=context;
  /* Sink failure is sticky on worker and is reported as REPLY, not APPLY. */
  (void)s->sink.write(s->sink.context,bytes,n);
}
static bool color_query(GhosttyTerminal terminal,void *context,GhosttyColorScheme *out) {
  (void)terminal;vt_state *s=context;
  *out=s->dark?GHOSTTY_COLOR_SCHEME_DARK:GHOSTTY_COLOR_SCHEME_LIGHT;return true;
}
static void history_pruned(void *context) {((vt_state *)context)->history_pruned=true;}
static GhosttyResult defaults(vt_state *s) {
  gt_vt_colors_t colors=s->dark?s->dark_colors:s->light_colors;
  GhosttyColorRgb bg={colors.background[0],colors.background[1],colors.background[2]};
  GhosttyColorRgb fg={colors.foreground[0],colors.foreground[1],colors.foreground[2]};
  GhosttyResult rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,&bg);
  if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,&fg);
  if(s->styled) {
    GhosttyColorRgb cursor={s->style.cursor[0],s->style.cursor[1],s->style.cursor[2]};
    GhosttyColorRgb palette[256];
    for(size_t i=0;i<256;i++)palette[i]=(GhosttyColorRgb){s->style.palette[i][0],s->style.palette[i][1],s->style.palette[i][2]};
    if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_CURSOR,&cursor);
    if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_PALETTE,palette);
  }
  return p0_budget_result(&s->scope,rc);
}
static void close_state(void *context) {
  vt_state *s=context;gt_vt_factory_t *f=s->factory;
  ghostty_terminal_free(s->terminal);
  /* Every engine borrower is gone; the admitted descriptors may now die. */
  if(!p0_budget_scope_close(&s->scope))return;
  record_free(f,s,sizeof(*s));
}
static gt_status_t open_state(void *context,uint64_t principal,uint64_t id,
  const gt_create_spec_t *spec,gt_worker_reply_sink_t sink,void **out) {
  if(out)*out=NULL;
  if(!context || !principal || !id || !spec || !spec->cols || !spec->rows || !sink.write || !out)
    return GT_ERR_INVALID_ARG;
  gt_vt_factory_t *f=context;gt_vt_config_t *c=&f->config;
  vt_state *s=record_new(f,sizeof(*s));if(!s)return GT_ERR_IO;
  s->factory=f;s->sink=sink;s->dark=c->initial_dark;s->light_colors=c->light;s->dark_colors=c->dark;
  if(!p0_budget_scope_admit(&s->scope,&f->budget,c->state_bytes)) {
    close_state(s);return GT_ERR_IO;
  }
  GhosttyResult rc=ghostty_terminal_p0_new_with_page_allocator(
    p0_budget_allocator(&s->scope,P0_BUDGET_GENERAL),
    p0_budget_allocator(&s->scope,P0_BUDGET_PAGES),&s->terminal,spec->cols,spec->rows);
  rc=p0_budget_result(&s->scope,rc);
  bool disabled=false;
#define SET(option,value) do {if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(s->terminal,option,value);} while(0)
  SET(GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES,&c->continuation_bytes);
  SET(GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,&c->history_bytes);
  SET(GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&c->history_lines);
  SET(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&c->image_bytes);
  SET(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE,&disabled);
  SET(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE,NULL);
  SET(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM,&disabled);
  SET(GHOSTTY_TERMINAL_OPT_USERDATA,s);
  SET(GHOSTTY_TERMINAL_OPT_WRITE_PTY,(const void *)reply);
  SET(GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,(const void *)color_query);
#undef SET
  if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_p0_set_history_observer(s->terminal,s,history_pruned);
  if(rc==GHOSTTY_SUCCESS)rc=defaults(s);
  if(rc==GHOSTTY_SUCCESS)rc=p0_budget_resize_cells(&s->scope,s->terminal,
    spec->cols,spec->rows,c->cell_width,c->cell_height);
  rc=p0_budget_result(&s->scope,rc);
  if(rc!=GHOSTTY_SUCCESS){close_state(s);return status(rc);}
  *out=s;return GT_OK;
}
static gt_status_t apply(void *context,const gt_state_event_t *e) {
  vt_state *s=context;if(!s || !e || s->invalid || (!e->payload && e->length))return GT_ERR_INVALID_ARG;
  gt_vt_config_t *c=&s->factory->config;GhosttyResult rc;
  if(e->kind==GT_STATE_OUTPUT)rc=p0_budget_feed(&s->scope,s->terminal,e->payload,e->length);
  else if(e->kind==GT_STATE_RESIZE && e->length==sizeof(gt_state_size_t)) {
    gt_state_size_t size;memcpy(&size,e->payload,sizeof(size));
    rc=p0_budget_resize_cells(&s->scope,s->terminal,size.cols,size.rows,c->cell_width,c->cell_height);
  } else if(e->kind==GT_STATE_COLOR_SCHEME && e->length==sizeof(gt_state_color_scheme_t)) {
    gt_state_color_scheme_t color;memcpy(&color,e->payload,sizeof(color));
    if(color.dark>1)return GT_ERR_INVALID_ARG;
    s->dark=color.dark!=0;rc=defaults(s);
  } else if(e->kind==GT_STATE_STYLE && e->length==sizeof(gt_session_style_t)) {
    gt_session_style_t style;memcpy(&style,e->payload,sizeof(style));
    if(style.dark>1)return GT_ERR_INVALID_ARG;
    s->style=style;s->styled=true;s->dark=style.dark!=0;
    gt_vt_colors_t *colors=s->dark?&s->dark_colors:&s->light_colors;
    memcpy(colors->background,style.background,3);memcpy(colors->foreground,style.foreground,3);
    rc=defaults(s);
  } else return GT_ERR_INVALID_ARG;
  return status(rc);
}
static void invalidated(void *context,gt_status_t why,uint64_t sequence) {
  (void)why;(void)sequence;((vt_state *)context)->invalid=true;
}
static void release_frozen(void *context,void *object) {
  gt_vt_factory_t *f=context;vt_frozen *v=object;
  if(!v)return;
  p0_frozen_release(&v->checkpoint);
  if(!p0_budget_scope_close(&v->scope))return;
  record_free(f,v,sizeof(*v));
}
static gt_status_t freeze(void *context,void **out) {
  if(out)*out=NULL;
  vt_state *s=context;
  if(!out || !s || s->invalid)return GT_ERR_INVALID_ARG;
  gt_vt_factory_t *f=s->factory;vt_frozen *v=record_new(f,sizeof(*v));if(!v)return GT_ERR_IO;
  v->factory=f;v->dark=s->dark;v->history_pruned=s->history_pruned;v->light_colors=s->light_colors;v->dark_colors=s->dark_colors;
  if(!p0_budget_scope_admit(&v->scope,&f->budget,f->config.frozen_bytes)) {
    release_frozen(f,v);return GT_ERR_IO;
  }
  GhosttyResult rc=p0_frozen_capture(&s->scope,&v->scope,s->terminal,f->config.checkpoint_bytes,&v->checkpoint);
  if(rc!=GHOSTTY_SUCCESS){release_frozen(f,v);return status(rc);}
  *out=v;return GT_OK;
}
gt_status_t gt__vt_factory_open(const gt_vt_config_t *c,const gt_allocator_t *a,gt_vt_factory_t **out) {
  if(out)*out=NULL;
  if(!out || !c || !a || !a->allocate || !a->release || !c->budget_bytes ||
    !c->state_bytes || !c->frozen_bytes || !c->encoder_bytes || !c->checkpoint_bytes ||
    c->checkpoint_bytes>64u*1024u*1024u || !c->continuation_bytes ||
    !c->history_bytes || !c->history_lines || !c->cell_width || !c->cell_height ||
    c->cell_width>65535 || c->cell_height>65535 ||
    c->state_bytes>c->budget_bytes || c->frozen_bytes>c->budget_bytes || c->encoder_bytes>c->budget_bytes)
    return GT_ERR_INVALID_ARG;
  if(c->external_resources)return GT_ERR_UNSUPPORTED;
  if(pthread_once(&png_once,install_png) || png_result!=GHOSTTY_SUCCESS)return GT_ERR_UNSUPPORTED;
  gt_vt_factory_t *f=a->allocate(a->context,sizeof(*f));if(!f)return GT_ERR_IO;
  memset(f,0,sizeof(*f));f->config=*c;f->allocator=*a;atomic_init(&f->objects,0);
  if(!p0_budget_init(&f->budget,c->budget_bytes)){a->release(a->context,f,sizeof(*f));return GT_ERR_IO;}
  *out=f;return GT_OK;
}
gt_runtime_state_factory_t gt__vt_factory_state(gt_vt_factory_t *f) {
  if(!f)return (gt_runtime_state_factory_t){0};
  return (gt_runtime_state_factory_t){.context=f,.open=open_state,.ops={apply,freeze},
    .close=close_state,.invalidated=invalidated,.color_scheme_supported=true,.style_supported=true,.release_frozen=release_frozen};
}
gt_status_t gt__vt_checkpoint_encode(gt_vt_factory_t *f,const void *object,size_t limit,gt_vt_encoded_t **out) {
  if(out)*out=NULL;
  const vt_frozen *v=object;
  if(!f || !v || v->factory!=f || !out || !limit || limit>GT_VT_SNAPSHOT_LIMIT)return GT_ERR_INVALID_ARG;
  if(limit>f->config.checkpoint_bytes)limit=f->config.checkpoint_bytes;
  gt_vt_encoded_t *e=record_new(f,sizeof(*e));if(!e)return GT_ERR_IO;e->factory=f;
  if(!p0_budget_scope_admit(&e->scope,&f->budget,f->config.encoder_bytes)) {
    gt__vt_encoded_release(e);return GT_ERR_IO;
  }
  GhosttyResult rc=p0_frozen_encode(&v->checkpoint,&e->scope,limit,&e->checkpoint);
  if(rc!=GHOSTTY_SUCCESS){gt__vt_encoded_release(e);return status(rc);}
  gt_vt_config_t *c=&f->config;
  e->view=(gt_vt_snapshot_view_t){.profile=GT_VT_PROFILE_INLINE,.canonical=e->checkpoint.canonical,.resources=e->checkpoint.resources,
    .canonical_bytes=e->checkpoint.canonical_len,.resource_bytes=e->checkpoint.resources_len,
    .cell_width=c->cell_width,.cell_height=c->cell_height,.history_bytes=c->history_bytes,.history_lines=c->history_lines,
    .continuation_bytes=c->continuation_bytes,.image_bytes=c->image_bytes,
    .light=v->light_colors,.dark=v->dark_colors,.is_dark=v->dark,.history_pruned=v->history_pruned};
  *out=e;return GT_OK;
}
gt_vt_snapshot_view_t gt__vt_encoded_view(const gt_vt_encoded_t *e) {
  return e?e->view:(gt_vt_snapshot_view_t){0};
}
void gt__vt_encoded_release(gt_vt_encoded_t *e) {
  if(!e)return;
  gt_vt_factory_t *f=e->factory;
  p0_checkpoint_release(&e->checkpoint);
  if(!p0_budget_scope_close(&e->scope))return;
  record_free(f,e,sizeof(*e));
}
gt_runtime_rc_t gt__vt_factory_close(gt_vt_factory_t *f) {
  if(!f)return GT_RUNTIME_INVALID;
  if(atomic_load(&f->objects))return GT_RUNTIME_BUSY;
  if(!p0_budget_destroy(&f->budget))return GT_RUNTIME_INTERNAL;
  gt_allocator_t a=f->allocator;a.release(a.context,f,sizeof(*f));return GT_RUNTIME_OK;
}
