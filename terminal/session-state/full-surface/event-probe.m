#define main baseline_surface_main
#include "probe.m"
#undef main
static void event_state(ghostty_surface_t surface,NSString *dir,NSString *name,unsigned stage) {
 equal_snapshot(surface,fixture(dir,[NSString stringWithFormat:@"%@-%u.canonical",name,stage]));
 NSData *expected=fixture(dir,[NSString stringWithFormat:@"%@-%u.resources",name,stage]);size_t n=0;
 CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n));CHECK(n==expected.length && n>16);
 uint8_t *data=malloc(n);CHECK(data);CHECK(ghostty_p0_surface_resources_snapshot(surface,data,n,&n));
 CHECK(n==expected.length && memcmp(data,expected.bytes,n)==0);free(data);
 uint64_t applied=UINT64_MAX;CHECK(ghostty_p0_surface_applied_sequence(surface,&applied));CHECK(applied==stage);
}
static void event_input(ghostty_app_t app,ghostty_surface_t surface,bool bracketed) {
 atomic_store(&writes,0);atomic_store(&input_len,0);ghostty_surface_text(surface,"USER",4);spin(app,0.1);
 const char *expected=bracketed?"\x1b[200~USER\x1b[201~":"USER";
 CHECK(atomic_load(&writes)>0 && atomic_load(&input_len)==strlen(expected));CHECK(memcmp(input,expected,strlen(expected))==0);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;
 rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);NSString *dir=[NSString stringWithUTF8String:argv[1]];
 for(NSString *kind in @[@"resize",@"output",@"mixed",@"reorder",@"raw",@"import-only"]) {
  if(strcmp(mode,"all")!=0 && strcmp(mode,kind.UTF8String)!=0)continue;
  bool reorder=[kind isEqualToString:@"reorder"],raw=[kind isEqualToString:@"raw"],import_only=[kind isEqualToString:@"import-only"];
  NSString *name=reorder?@"mixed":raw?@"output":import_only?@"resize":kind;
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,[name stringByAppendingString:@"-0.canonical"]);
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,[name stringByAppendingString:@"-0.resources"]);
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];ghostty_surface_config_s opts=ghostty_surface_config_new();
  opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void*)view;opts.scale_factor=1;
  opts.host=true;opts.host_write=host_write;opts.host_resize=host_resize;opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;
  opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;
  opts.p0_image_storage_limit=1024*1024;opts.p0_image_storage_limit_set=true;
  atomic_store(&writes,0);atomic_store(&clipboards,0);atomic_store(&notifications,0);atomic_store(&input_len,0);
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);unsigned stage=0;event_state(surface,dir,name,0);
  if(import_only) {ghostty_surface_free(surface);printf("PASS ordered events/import-only\n");continue;}
  spin(app,0.05);event_state(surface,dir,name,0);
  if(reorder) {
   CHECK(!ghostty_p0_surface_event_resize(surface,1,0,17,8,16));event_state(surface,dir,name,0);
   CHECK(!ghostty_p0_surface_event_resize(surface,1,51,17,UINT32_MAX,16));event_state(surface,dir,name,0);
   CHECK(!ghostty_p0_surface_event_resize(surface,1,65535,65535,8,16));event_state(surface,dir,name,0);
   CHECK(!ghostty_p0_surface_event_output(surface,2,(const uint8_t*)"SKIP",4));event_state(surface,dir,name,0);
   CHECK(!ghostty_p0_surface_event_resize(surface,0,51,17,8,16));event_state(surface,dir,name,0);
  }
  if(![name isEqualToString:@"output"]) {
   CHECK(ghostty_p0_surface_event_resize(surface,++stage,51,17,8,16));event_state(surface,dir,name,stage);
   spin(app,0.05);event_state(surface,dir,name,stage);
  }
  if(![name isEqualToString:@"resize"]) {
   const char *on="\x1b[?2004hMODE-ON";
   CHECK(ghostty_p0_surface_event_output(surface,++stage,(const uint8_t*)on,strlen(on)));event_state(surface,dir,name,stage);
   event_input(app,surface,true);event_state(surface,dir,name,stage);
  }
  if(raw) {
   ghostty_surface_process_output(surface,(const uint8_t*)"BYPASS",6);size_t n=99;uint64_t seq=99;
   CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && n==0);
   CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n) && n==0);
   CHECK(!ghostty_p0_surface_applied_sequence(surface,&seq) && seq==0);
   CHECK(!ghostty_p0_surface_event_output(surface,stage+1,(const uint8_t*)"LATER",5));
   ghostty_surface_free(surface);printf("PASS ordered events/raw invalidation\n");continue;
  }
  if(reorder) {
   CHECK(!ghostty_p0_surface_event_resize(surface,stage,90,30,8,16));event_state(surface,dir,name,stage);
   CHECK(!ghostty_p0_surface_event_output(surface,stage+2,(const uint8_t*)"GAP",3));event_state(surface,dir,name,stage);
  }
  if(![name isEqualToString:@"output"]) {
   CHECK(ghostty_p0_surface_event_resize(surface,++stage,90,30,8,16));event_state(surface,dir,name,stage);
  }
  if(![name isEqualToString:@"resize"]) {
   const char *off="\x1b[?2004lMODE-OFF";
   CHECK(ghostty_p0_surface_event_output(surface,++stage,(const uint8_t*)off,strlen(off)));event_state(surface,dir,name,stage);
   event_input(app,surface,false);event_state(surface,dir,name,stage);
  }
  if(reorder) {CHECK(!ghostty_p0_surface_event_output(surface,UINT64_MAX,NULL,0));event_state(surface,dir,name,stage);}
  spin(app,0.05);ghostty_surface_draw(surface);event_state(surface,dir,name,stage);
  CHECK(atomic_load(&clipboards)==0 && atomic_load(&notifications)==0);ghostty_surface_free(surface);
  printf("PASS ordered events/%s\n",kind.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
