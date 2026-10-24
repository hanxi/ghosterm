#define main baseline_surface_main
#include "probe.m"
#undef main

typedef struct {
 ghostty_surface_t surface;
 bool accept,inline_apply;
 unsigned requests;uint64_t request_generation,request_sequence;uint32_t request_kind;
 atomic_size_t input_len;uint8_t input[128];size_t used;uint64_t input_generation,input_sequence;
} ResetContext;
static void reset_input(void *opaque,uint64_t generation,uint64_t sequence,const uint8_t *data,size_t len) {
 ResetContext *ctx=opaque;CHECK(len<=sizeof ctx->input-ctx->used);memcpy(ctx->input+ctx->used,data,len);ctx->used+=len;
 ctx->input_generation=generation;ctx->input_sequence=sequence;atomic_store(&ctx->input_len,ctx->used);
}
static void reset_legacy_input(void *opaque,const uint8_t *data,size_t len) {reset_input(opaque,0,0,data,len);}
static bool reset_request(void *opaque,uint64_t generation,uint64_t sequence,uint32_t kind) {
 ResetContext *ctx=opaque;uint64_t observed=99;CHECK(ghostty_p0_surface_applied_sequence_for(ctx->surface,generation,&observed));
 CHECK(observed==sequence && generation==81 && sequence==40 && kind==1);
 ctx->requests++;ctx->request_generation=generation;ctx->request_sequence=sequence;ctx->request_kind=kind;
 if(ctx->inline_apply)CHECK(ghostty_p0_surface_event_reset_for(ctx->surface,generation,sequence+1));
 return ctx->accept;
}
static void reset_compare(ghostty_surface_t surface,NSString *dir,NSString *name,NSString *stage,uint64_t gen,uint64_t seq) {
 equal_snapshot(surface,fixture(dir,[NSString stringWithFormat:@"%@-%@.canonical",name,stage]));
 NSData *expected=fixture(dir,[NSString stringWithFormat:@"%@-%@.resources",name,stage]);size_t n=0;
 CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n) && n==expected.length);uint8_t *data=malloc(n);CHECK(data);
 CHECK(ghostty_p0_surface_resources_snapshot(surface,data,n,&n));CHECK(memcmp(data,expected.bytes,n)==0);free(data);
 uint64_t observed=99;CHECK(ghostty_p0_surface_applied_sequence_for(surface,gen,&observed));CHECK(observed==seq);
}
static void reset_type(ghostty_app_t app,ResetContext *ctx,bool bracketed,uint64_t sequence) {
 ctx->used=0;atomic_store(&ctx->input_len,0);ghostty_surface_text(ctx->surface,"USER",4);
 const char *expected=bracketed?"\x1b[200~USER\x1b[201~":"USER";NSDate *end=[NSDate dateWithTimeIntervalSinceNow:2];
 while(atomic_load(&ctx->input_len)<strlen(expected) && [end timeIntervalSinceNow]>0)spin(app,0.01);
 spin(app,0.02);CHECK(atomic_load(&ctx->input_len)==strlen(expected) && memcmp(ctx->input,expected,strlen(expected))==0);
 CHECK(ctx->input_sequence==sequence);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);NSString *dir=[NSString stringWithUTF8String:argv[1]];
 for(NSString *kind in @[@"queued",@"inline",@"declined",@"missing",@"stale",@"continuation",@"legacy"]) {
  if(strcmp(mode,"all")!=0 && strcmp(mode,kind.UTF8String)!=0)continue;
  bool legacy=[kind isEqualToString:@"legacy"],stale=[kind isEqualToString:@"stale"],declined=[kind isEqualToString:@"declined"],missing=[kind isEqualToString:@"missing"];
  NSString *name=[kind isEqualToString:@"continuation"]?@"continuation":@"reset";
  ResetContext ctx={0};atomic_init(&ctx.input_len,0);ctx.accept=!declined;ctx.inline_apply=[kind isEqualToString:@"inline"];
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,[name stringByAppendingString:@"-initial.canonical"]);
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,[name stringByAppendingString:@"-initial.resources"]);
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void*)view;
  opts.scale_factor=1;opts.host=true;opts.userdata=&ctx;opts.host_write=reset_legacy_input;opts.host_resize=host_resize;opts.p0_host_write_input=reset_input;opts.p0_host_action=missing?NULL:reset_request;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;opts.p0_image_storage_limit=1024*1024;opts.p0_image_storage_limit_set=true;
  opts.p0_checkpoint_bound=!legacy;opts.p0_checkpoint_generation=legacy?0:81;opts.p0_checkpoint_sequence=legacy?0:40;
  if(!legacy) {NSData *digest=fixture(dir,[name stringByAppendingString:@".digest"]);CHECK(digest.length==32);memcpy(opts.p0_checkpoint_digest,digest.bytes,32);}
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);ctx.surface=surface;
  reset_compare(surface,dir,name,@"initial",legacy?0:81,legacy?0:40);
  const char *action="reset";
  if(stale) {CHECK(!ghostty_p0_surface_event_reset_for(surface,80,41));reset_compare(surface,dir,name,@"initial",81,40);}
  else {
   CHECK(ghostty_surface_binding_action(surface,action,strlen(action))==!(declined||missing));
   CHECK(ctx.requests==(legacy||missing?0u:1u));
  }
  spin(app,0.05);CHECK(atomic_load(&ctx.input_len)==0);
  if(legacy || ctx.inline_apply) {
   reset_compare(surface,dir,name,@"reset",legacy?0:81,legacy?0:41);reset_type(app,&ctx,false,legacy?0:41);
  } else {
   reset_compare(surface,dir,name,@"initial",81,40);reset_type(app,&ctx,true,40);CHECK(ctx.input_generation==81);
   if(!declined && !missing) {
    CHECK(ghostty_p0_surface_event_reset_for(surface,81,41));reset_compare(surface,dir,name,@"reset",81,41);
    if([kind isEqualToString:@"continuation"]) {
     CHECK(ghostty_p0_surface_event_output_for(surface,81,42,(const uint8_t*)"32mZ",4));reset_compare(surface,dir,name,@"final",81,42);reset_type(app,&ctx,false,42);
    } else reset_type(app,&ctx,false,41);
   }
  }
  ghostty_surface_free(surface);printf("PASS authority reset/%s\n",kind.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
