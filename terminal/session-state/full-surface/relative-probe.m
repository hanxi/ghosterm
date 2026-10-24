#define main baseline_surface_main
#include "probe.m"
#undef main
static void equal_resources(ghostty_surface_t surface,NSData *expected) {
 size_t n=0;CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n));CHECK(n==expected.length && n>16);
 uint8_t *data=malloc(n);CHECK(data);CHECK(ghostty_p0_surface_resources_snapshot(surface,data,n,&n));
 CHECK(n==expected.length && memcmp(data,expected.bytes,n)==0);free(data);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 const bool import_only=strcmp(mode,"import-only")==0;
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;
 rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);NSString *dir=[NSString stringWithUTF8String:argv[1]];
 for(NSString *name in @[@"static",@"relative",@"chain",@"forward",@"internal"]) {
  if(import_only ? ![name isEqualToString:@"static"] : strcmp(mode,"all")!=0 && strcmp(mode,name.UTF8String)!=0)continue;
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,[name stringByAppendingString:@".initial"]);
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,[name stringByAppendingString:@".resources"]);
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();opts.platform_tag=GHOSTTY_PLATFORM_MACOS;
  opts.platform.macos.nsview=(__bridge void*)view;opts.scale_factor=1;opts.host=true;opts.host_write=host_write;opts.host_resize=host_resize;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;
  opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;
  opts.p0_image_storage_limit=1024*1024;opts.p0_image_storage_limit_set=true;
  atomic_store(&writes,0);atomic_store(&clipboards,0);atomic_store(&notifications,0);
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);
  equal_snapshot(surface,initial);equal_resources(surface,resources);
  if(!import_only) {spin(app,0.05);ghostty_surface_draw(surface);equal_snapshot(surface,initial);equal_resources(surface,resources);}
  NSData *delta=fixture(dir,[name stringByAppendingString:@".delta"]);
  ghostty_surface_process_output(surface,delta.bytes,delta.length);
  equal_snapshot(surface,fixture(dir,[name stringByAppendingString:@".final"]));
  equal_resources(surface,fixture(dir,[name stringByAppendingString:@".after"]));
  if(!import_only) {spin(app,0.05);ghostty_surface_draw(surface);}
  CHECK(atomic_load(&writes)==0 && atomic_load(&clipboards)==0 && atomic_load(&notifications)==0);
  ghostty_surface_free(surface);printf("PASS full relative resources/%s\n",name.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
