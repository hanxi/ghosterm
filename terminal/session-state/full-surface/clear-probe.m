#define main baseline_surface_main
#include "probe.m"
#undef main

typedef struct { ghostty_surface_t surface;bool accept;unsigned requests;atomic_uint writes;atomic_size_t bytes;uint8_t data[32]; } ClearContext;
static void clear_legacy_write(void *opaque,const uint8_t *data,size_t n) {
 ClearContext *ctx=opaque;size_t offset=atomic_load(&ctx->bytes);CHECK(n<=sizeof ctx->data-offset);memcpy(ctx->data+offset,data,n);atomic_store(&ctx->bytes,offset+n);atomic_fetch_add(&ctx->writes,1);
}
static void clear_input(void *opaque,uint64_t g,uint64_t s,const uint8_t *data,size_t n) {(void)g;(void)s;clear_legacy_write(opaque,data,n);}
static bool clear_request(void *opaque,uint64_t generation,uint64_t sequence,uint32_t kind) {
 ClearContext *ctx=opaque;uint64_t observed=99;CHECK(generation==81 && sequence==40 && kind==2);
 CHECK(ghostty_p0_surface_applied_sequence_for(ctx->surface,generation,&observed) && observed==sequence);ctx->requests++;return ctx->accept;
}
static void clear_compare(ghostty_surface_t surface,NSString *dir,NSString *name,NSString *stage,uint64_t generation,uint64_t sequence) {
 equal_snapshot(surface,fixture(dir,[NSString stringWithFormat:@"%@-%@.canonical",name,stage]));
 NSData *expected=fixture(dir,[NSString stringWithFormat:@"%@-%@.resources",name,stage]);size_t n=0;
 CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n) && n==expected.length);uint8_t *data=malloc(n);CHECK(data);
 CHECK(ghostty_p0_surface_resources_snapshot(surface,data,n,&n));CHECK(memcmp(data,expected.bytes,n)==0);free(data);
 uint64_t observed=99;CHECK(ghostty_p0_surface_applied_sequence_for(surface,generation,&observed) && observed==sequence);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);NSString *dir=[NSString stringWithUTF8String:argv[1]];
 for(NSString *kind in @[@"normal",@"prompt",@"alternate",@"keep-history",@"declined",@"missing",@"stale",@"mismatch",@"legacy",@"legacy-prompt"]) {
  if(strcmp(mode,"all")!=0 && strcmp(mode,kind.UTF8String)!=0)continue;
  bool legacy=[kind hasPrefix:@"legacy"],missing=[kind isEqualToString:@"missing"],declined=[kind isEqualToString:@"declined"],stale=[kind isEqualToString:@"stale"],mismatch=[kind isEqualToString:@"mismatch"];
  NSString *name=[kind isEqualToString:@"legacy-prompt"]?@"prompt":(legacy||missing||declined||stale||mismatch)?@"normal":kind;
  ClearContext ctx={0};atomic_init(&ctx.writes,0);atomic_init(&ctx.bytes,0);ctx.accept=!declined;
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,[name stringByAppendingString:@"-initial.canonical"]);
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,[name stringByAppendingString:@"-initial.resources"]);
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void*)view;opts.scale_factor=1;
  opts.host=true;opts.userdata=&ctx;opts.host_write=clear_legacy_write;opts.host_resize=host_resize;opts.p0_host_write_input=clear_input;opts.p0_host_action=missing?NULL:clear_request;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;opts.p0_image_storage_limit=1024*1024;opts.p0_image_storage_limit_set=true;
  opts.p0_checkpoint_bound=!legacy;opts.p0_checkpoint_generation=legacy?0:81;opts.p0_checkpoint_sequence=legacy?0:40;
  if(!legacy) {NSData *digest=fixture(dir,[name stringByAppendingString:@".digest"]);CHECK(digest.length==32);memcpy(opts.p0_checkpoint_digest,digest.bytes,32);}
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);ctx.surface=surface;clear_compare(surface,dir,name,@"initial",legacy?0:81,legacy?0:40);
  if(mismatch) {
   CHECK(!ghostty_p0_surface_event_clear_for(surface,81,41,true,2));uint64_t seq=99;size_t n=99;
   CHECK(!ghostty_p0_surface_applied_sequence_for(surface,81,&seq) && seq==0);CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && n==0);
  } else if(legacy) {
   const char *action="clear_screen";CHECK(ghostty_surface_binding_action(surface,action,strlen(action)));spin(app,0.1);clear_compare(surface,dir,name,@"after",0,0);
   CHECK(atomic_load(&ctx.writes)==([name isEqualToString:@"prompt"]?1u:0u));if([name isEqualToString:@"prompt"])CHECK(atomic_load(&ctx.bytes)==1 && ctx.data[0]==0x0c);
  } else {
   bool alternate=[name isEqualToString:@"alternate"],keep=[name isEqualToString:@"keep-history"];
   if(!stale && !keep) {const char *action="clear_screen";CHECK(ghostty_surface_binding_action(surface,action,strlen(action))==!(missing||declined||alternate));CHECK(ctx.requests==(missing||alternate?0u:1u));}
   spin(app,0.05);clear_compare(surface,dir,name,@"initial",81,40);CHECK(atomic_load(&ctx.writes)==0);
   if(stale) {CHECK(!ghostty_p0_surface_event_clear_for(surface,80,41,true,1));clear_compare(surface,dir,name,@"initial",81,40);}
   if(!missing && !declined) {
    uint32_t outcome=alternate?0:[name isEqualToString:@"prompt"]?2:1;
    CHECK(ghostty_p0_surface_event_clear_for(surface,81,41,!keep,outcome));clear_compare(surface,dir,name,@"after",81,41);spin(app,0.05);
    CHECK(atomic_load(&ctx.writes)==0);clear_compare(surface,dir,name,@"after",81,41);
   }
  }
  ghostty_surface_free(surface);printf("PASS authority clear/%s\n",kind.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
