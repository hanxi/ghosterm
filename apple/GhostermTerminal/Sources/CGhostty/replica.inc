/* Included after the private Surface/App structs. All payload methods use the
 * Transport IO owner; construction/configure/free are on the native UI owner. */
#include "ghosterm.h"
#include "../../../../terminal/state/checkpoint-wire.h"
#undef GHOSTTY_SUCCESS /* full API macro conflicts with VT allocator result enum */
#define GHOSTERM_P0_ALLOCATOR_ONLY 1
#include "../../../../terminal/session-state/budget-posix.h"
#include <pthread.h>
#define REPLICA_DEFAULT_BYTES (96u*1024u*1024u)
struct gt_ghostty_replica_private {
  p0_budget_scope scope;ghostty_p0_allocator_s general,pages;
  gt_ghostty_replica_policy_t policy;uint64_t generation;bool imported;
  ghostty_config_t config;struct gt_ghostty_replica_private *next;
  bool styled;uint8_t style[777];
};
static pthread_once_t replica_once=PTHREAD_ONCE_INIT;
static p0_budget replica_budget;static bool replica_budget_ready;
static struct gt_ghostty_replica_private *replica_retired;
static void replica_budget_init(void){replica_budget_ready=p0_budget_init(&replica_budget,(size_t)1024*1024*1024);}
static uint8_t *replica_alloc(void *ctx,size_t n,uint8_t alignment,uintptr_t ret){const GhosttyAllocator *a=ctx;return a->vtable->alloc(a->ctx,n,alignment,ret);}
static bool replica_resize_memory(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t m,uintptr_t r){const GhosttyAllocator *base=ctx;return base->vtable->resize(base->ctx,p,n,a,m,r);}
static uint8_t *replica_remap(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t m,uintptr_t r){const GhosttyAllocator *base=ctx;return base->vtable->remap(base->ctx,p,n,a,m,r);}
static void replica_free(void *ctx,uint8_t *p,size_t n,uint8_t a,uintptr_t r){const GhosttyAllocator *base=ctx;base->vtable->free(base->ctx,p,n,a,r);}
static const ghostty_p0_allocator_vtable_s replica_vtable={replica_alloc,replica_resize_memory,replica_remap,replica_free};
static bool replica_healthy(void *ctx){struct gt_ghostty_replica_private *r=ctx;return p0_budget_scope_fault(&r->scope)==P0_BUDGET_HEALTHY;}
static void replica_dispose(struct gt_ghostty_replica_private *r){if(!r)return;if(r->config)ghostty_config_free(r->config);if(p0_budget_scope_close(&r->scope))free(r);else {r->next=replica_retired;replica_retired=r;}}
static void replica_collect(void){struct gt_ghostty_replica_private **cursor=&replica_retired;while(*cursor){struct gt_ghostty_replica_private *r=*cursor;if(p0_budget_scope_close(&r->scope)){*cursor=r->next;free(r);}else cursor=&r->next;}}
void gt_ghostty_replica_budget_usage(size_t *live,size_t *committed,size_t *retired){replica_collect();p0_budget_usage usage=replica_budget_ready?p0_budget_read(&replica_budget):(p0_budget_usage){0};if(live)*live=usage.live;if(committed)*committed=usage.committed;if(retired){*retired=0;for(struct gt_ghostty_replica_private *r=replica_retired;r;r=r->next)(*retired)++;}}
static gt_ghostty_replica_rgb_t replica_rgb(const uint8_t a[3]){return (gt_ghostty_replica_rgb_t){a[0],a[1],a[2]};}
int gt_ghostty_replica_policy(const void *bytes,size_t n,gt_ghostty_replica_policy_t *out){
  if(out)memset(out,0,sizeof(*out));if(!out)return GT_ERR_INVALID_ARG;gt_vt_snapshot_view_t v;gt_status_t status=gt_vt_snapshot_parse(bytes,n,GT_VT_SNAPSHOT_LIMIT,&v);if(status!=GT_OK)return status;
  *out=(gt_ghostty_replica_policy_t){.profile=v.profile,.cell_width=v.cell_width,.cell_height=v.cell_height,.history_bytes=v.history_bytes,.history_lines=v.history_lines,.continuation_bytes=v.continuation_bytes,.image_bytes=v.image_bytes,.memory_bytes=REPLICA_DEFAULT_BYTES,.dark=v.is_dark,.light_background=replica_rgb(v.light.background),.light_foreground=replica_rgb(v.light.foreground),.dark_background=replica_rgb(v.dark.background),.dark_foreground=replica_rgb(v.dark.foreground)};return GT_OK;
}
static ghostty_config_t replica_config(struct gt_ghostty_replica_private *r,ghostty_config_t original,bool dark){
  ghostty_config_t config=ghostty_p0_config_clone(original,&r->general);if(!config)return NULL;gt_ghostty_replica_rgb_t b=dark?r->policy.dark_background:r->policy.light_background,f=dark?r->policy.dark_foreground:r->policy.light_foreground;
  bool colors=r->styled?ghostty_p0_config_replica_style(config,dark,r->style):ghostty_p0_config_replica_colors(config,dark,(ghostty_config_color_s){b.r,b.g,b.b},(ghostty_config_color_s){f.r,f.g,f.b});
  if(!colors || !ghostty_p0_config_finalize(config) || !replica_healthy(r)){ghostty_config_free(config);return NULL;}return config;
}
static struct gt_ghostty_replica_private *replica_prepare(ghostty_config_t original,uint64_t generation,const gt_ghostty_replica_policy_t *policy,ghostty_surface_config_s *config){
  if(!generation || !policy || policy->profile!=GT_VT_PROFILE_INLINE || policy->memory_bytes<16u*1024u*1024u || policy->memory_bytes>512u*1024u*1024u)return NULL;
  pthread_once(&replica_once,replica_budget_init);if(!replica_budget_ready)return NULL;replica_collect();struct gt_ghostty_replica_private *r=calloc(1,sizeof(*r));if(!r)return NULL;
  if(!p0_budget_scope_admit(&r->scope,&replica_budget,policy->memory_bytes)){(void)p0_budget_scope_close(&r->scope);free(r);return NULL;}
  r->generation=generation;r->policy=*policy;r->general=(ghostty_p0_allocator_s){(void *)p0_budget_allocator(&r->scope,P0_BUDGET_GENERAL),&replica_vtable};r->pages=(ghostty_p0_allocator_s){(void *)p0_budget_allocator(&r->scope,P0_BUDGET_PAGES),&replica_vtable};
  r->config=replica_config(r,original,policy->dark);if(!r->config){replica_dispose(r);return NULL;}
  config->p0_replica_empty=true;config->p0_replica_config=r->config;config->p0_image_storage_limit_set=true;config->p0_image_storage_limit=policy->image_bytes;config->p0_color_scheme_set=true;config->p0_color_scheme_dark=policy->dark;
  config->p0_allocator=&r->general;config->p0_page_allocator=&r->pages;config->p0_budget_userdata=r;config->p0_budget_healthy=replica_healthy;return r;
}
static void replica_input(void *ctx,uint64_t generation,uint64_t sequence,const uint8_t *bytes,size_t n){gt_ghostty_surface_t *s=ctx;if(s->replica && s->cbs.write_stamped)s->cbs.write_stamped(s->cbs.userdata,generation,sequence,bytes,n);}
gt_ghostty_surface_t *gt_ghostty_surface_new_replica(gt_ghostty_app_t *app,void *view,double scale,const gt_host_callbacks_t *callbacks,uint64_t generation,const gt_ghostty_replica_policy_t *policy){return surface_new_(app,view,scale,callbacks,generation,policy);}
static bool replica_identity(gt_ghostty_surface_t *s,uint64_t generation){return s && s->replica && s->replica->generation==generation && replica_healthy(s->replica);}
bool gt_ghostty_surface_replica_import(gt_ghostty_surface_t *s,uint64_t generation,uint64_t sequence,const void *bytes,size_t n){
  if(!replica_identity(s,generation) || s->replica->imported)return false;s->replica->imported=true;gt_vt_snapshot_view_t v;if(gt_vt_snapshot_parse(bytes,n,GT_VT_SNAPSHOT_LIMIT,&v)!=GT_OK)return false;
  gt_ghostty_replica_policy_t actual;if(gt_ghostty_replica_policy(bytes,n,&actual)!=GT_OK)return false;gt_ghostty_replica_policy_t *p=&s->replica->policy;
  if(actual.profile!=p->profile || actual.cell_width!=p->cell_width || actual.cell_height!=p->cell_height || actual.continuation_bytes!=p->continuation_bytes || actual.image_bytes!=p->image_bytes || actual.history_bytes!=p->history_bytes || actual.history_lines!=p->history_lines || actual.dark!=p->dark || memcmp(&actual.light_background,&p->light_background,sizeof(actual.light_background)) || memcmp(&actual.light_foreground,&p->light_foreground,sizeof(actual.light_foreground)) || memcmp(&actual.dark_background,&p->dark_background,sizeof(actual.dark_background)) || memcmp(&actual.dark_foreground,&p->dark_foreground,sizeof(actual.dark_foreground)))return false;
  return ghostty_p0_surface_import_once(s->surface,generation,sequence,v.canonical,v.canonical_bytes,v.resources,v.resource_bytes) && replica_healthy(s->replica);
}
bool gt_ghostty_surface_replica_output(gt_ghostty_surface_t *s,uint64_t g,uint64_t sequence,const void *bytes,size_t n){return replica_identity(s,g) && ghostty_p0_surface_event_output_for(s->surface,g,sequence,bytes,n) && replica_healthy(s->replica);}
bool gt_ghostty_surface_replica_resize(gt_ghostty_surface_t *s,uint64_t g,uint64_t sequence,uint16_t cols,uint16_t rows){return replica_identity(s,g) && ghostty_p0_surface_event_resize_for(s->surface,g,sequence,cols,rows,s->replica->policy.cell_width,s->replica->policy.cell_height) && replica_healthy(s->replica);}
bool gt_ghostty_surface_replica_scheme_begin(gt_ghostty_surface_t *s,uint64_t g,uint64_t sequence,bool dark){return replica_identity(s,g) && ghostty_p0_surface_scheme_begin_for(s->surface,g,sequence,dark);}
bool gt_ghostty_surface_replica_scheme_apply(gt_ghostty_surface_t *s,uint64_t g,uint64_t sequence,bool dark){if(!replica_identity(s,g) || !s->owner)return false;ghostty_config_t config=replica_config(s->replica,s->owner->config,dark);if(!config)return false;bool ok=ghostty_p0_surface_scheme_apply_for(s->surface,g,sequence,config);ghostty_config_free(config);return ok && replica_healthy(s->replica);}
int gt_ghostty_surface_replica_applied(gt_ghostty_surface_t *s,uint64_t g,uint64_t *sequence){if(sequence)*sequence=0;if(!s || !s->replica || !sequence)return GT_ERR_INVALID_ARG;if(!replica_healthy(s->replica))return GT_ERR_IO;return ghostty_p0_surface_applied_sequence_for(s->surface,g,sequence)?GT_OK:GT_ERR_NOT_FOUND;}
bool gt_ghostty_surface_replica_input(gt_ghostty_surface_t *s,uint64_t g,uint64_t sequence,bool enabled){return s && s->replica && s->replica->generation==g && (!enabled || replica_healthy(s->replica)) && ghostty_p0_surface_input_enable(s->surface,g,sequence,enabled);}

bool gt_ghostty_app_style(gt_ghostty_app_t *a,bool dark,uint8_t *bytes,size_t n){return a && a->config && bytes && n==777 && ghostty_p0_config_style(a->config,dark,bytes);}
bool gt_ghostty_surface_replica_style_apply(gt_ghostty_surface_t *s,uint64_t g,uint64_t sequence,bool dark,const uint8_t *bytes,size_t n){
  if(!replica_identity(s,g) || !bytes || n!=777)return false;
  memcpy(s->replica->style,bytes,n);s->replica->styled=true;
  return gt_ghostty_surface_replica_scheme_apply(s,g,sequence,dark);
}

bool gt_ghostty_surface_replica_appearance(gt_ghostty_surface_t *s,uint64_t g,bool *dark,uint32_t *background){return replica_identity(s,g) && dark && background && ghostty_p0_surface_scheme_state_for(s->surface,g,dark,background);}
