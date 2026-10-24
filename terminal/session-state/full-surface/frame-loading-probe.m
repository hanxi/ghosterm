#define main baseline_surface_main
#include "probe.m"
#undef main
static void frame_input(void *ctx,uint64_t g,uint64_t s,const uint8_t *data,size_t n) {(void)g;(void)s;host_write(ctx,data,n);}
static void frame_compare(ghostty_surface_t surface,NSString *dir,NSString *name,NSString *stage,uint64_t sequence) {
 equal_snapshot(surface,fixture(dir,[NSString stringWithFormat:@"%@-%@.canonical",name,stage]));
 NSData *expected=fixture(dir,[NSString stringWithFormat:@"%@-%@.resources",name,stage]);size_t n=0;
 CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n) && n==expected.length);uint8_t *data=malloc(n);CHECK(data);
 CHECK(ghostty_p0_surface_resources_snapshot(surface,data,n,&n) && memcmp(data,expected.bytes,n)==0);free(data);
 uint64_t observed=99;CHECK(ghostty_p0_surface_applied_sequence_for(surface,81,&observed) && observed==sequence);
 CHECK(atomic_load(&writes)==0 && atomic_load(&clipboards)==0 && atomic_load(&notifications)==0);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);NSString *dir=[NSString stringWithUTF8String:argv[1]];
 for(NSString *kind in @[@"append",@"number",@"edit",@"stale",@"tick-after",@"missing",@"blend",@"overwrite",@"background",@"png",@"zlib",@"quiet",@"continuation",@"negative-gap",@"static"]) {
  if(strcmp(mode,"all")!=0 && strcmp(mode,kind.UTF8String)!=0)continue;
  NSString *name=kind;NSData *manifest=fixture(dir,[name stringByAppendingString:@"-manifest.bin"]);CHECK(manifest.length==16);
  const uint8_t *meta=manifest.bytes;uint64_t limit=0,steps=0;for(unsigned i=0;i<8;i++) {limit|=(uint64_t)meta[i]<<(8*i);steps|=(uint64_t)meta[i+8]<<(8*i);}
  CHECK(steps>0 && steps<=5);
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,[name stringByAppendingString:@"-initial.canonical"]);
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,[name stringByAppendingString:@"-initial.resources"]);
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void*)view;opts.scale_factor=1;
  opts.host=true;opts.host_write=host_write;opts.host_resize=host_resize;opts.p0_host_write_input=frame_input;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;opts.p0_image_storage_limit=limit;opts.p0_image_storage_limit_set=true;
  opts.p0_checkpoint_bound=true;opts.p0_checkpoint_generation=81;opts.p0_checkpoint_sequence=40;
  NSData *digest=fixture(dir,[name stringByAppendingString:@".digest"]);CHECK(digest.length==32);memcpy(opts.p0_checkpoint_digest,digest.bytes,32);
  atomic_store(&writes,0);atomic_store(&input_len,0);atomic_store(&clipboards,0);atomic_store(&notifications,0);
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);frame_compare(surface,dir,name,@"initial",40);
  spin(app,0.18);frame_compare(surface,dir,name,@"initial",40);
  CHECK(!ghostty_p0_surface_event_output_for(surface,80,41,(const uint8_t*)"X",1));frame_compare(surface,dir,name,@"initial",40);
  for(unsigned step=1;step<=steps;step++) {
   NSData *event=fixture(dir,[NSString stringWithFormat:@"%@-step%u.event",name,step]);CHECK(event.length>=1);
   const uint8_t *bytes=event.bytes;
   if(bytes[0]==0) {
    CHECK(event.length==9);uint64_t at=0;for(unsigned i=0;i<8;i++)at|=(uint64_t)bytes[i+1]<<(8*i);
    CHECK(ghostty_p0_surface_event_animation_tick_for(surface,81,40+step,at));
   } else {CHECK(bytes[0]==1);CHECK(ghostty_p0_surface_event_output_for(surface,81,40+step,bytes+1,event.length-1));}
   NSString *stage=[NSString stringWithFormat:@"after%u",step];frame_compare(surface,dir,name,stage,40+step);
   spin(app,0.08);frame_compare(surface,dir,name,stage,40+step);
  }
  ghostty_surface_free(surface);printf("PASS full surface frame loading/%s\n",kind.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
