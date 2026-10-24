#define main baseline_surface_main
#include "probe.m"
#undef main
static void checkpoint_state(ghostty_surface_t surface,NSString *dir,unsigned stage,NSData *override,uint64_t generation,uint64_t sequence) {
 equal_snapshot(surface,fixture(dir,[NSString stringWithFormat:@"mixed-%u.canonical",stage]));
 NSData *expected=override?override:fixture(dir,[NSString stringWithFormat:@"mixed-%u.resources",stage]);size_t n=0;
 CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&n));CHECK(n==expected.length && n>16);
 uint8_t *data=malloc(n);CHECK(data);CHECK(ghostty_p0_surface_resources_snapshot(surface,data,n,&n));
 CHECK(n==expected.length && memcmp(data,expected.bytes,n)==0);free(data);
 uint64_t applied=UINT64_MAX;
 CHECK(ghostty_p0_surface_applied_sequence_for(surface,generation,&applied));CHECK(applied==sequence);
}
static void checkpoint_digest(ghostty_surface_config_s *opts,NSString *dir,NSString *name) {
 NSData *d=fixture(dir,name);CHECK(d.length==sizeof opts->p0_checkpoint_digest);memcpy(opts->p0_checkpoint_digest,d.bytes,d.length);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;
 rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);NSString *dir=[NSString stringWithUTF8String:argv[1]];
 for(NSString *kind in @[@"binding",@"metadata",@"sequence",@"generation",@"zero-start",@"legacy"]) {
  if(strcmp(mode,"all")!=0 && strcmp(mode,kind.UTF8String)!=0)continue;
  const bool legacy=[kind isEqualToString:@"legacy"],zero=[kind isEqualToString:@"zero-start"];
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,@"mixed-2.canonical");
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,@"mixed-2.resources");
  __attribute__((objc_precise_lifetime)) NSData *blue=fixture(dir,@"blue.resources");
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();opts.platform_tag=GHOSTTY_PLATFORM_MACOS;
  opts.platform.macos.nsview=(__bridge void*)view;opts.scale_factor=1;opts.host=true;opts.host_write=host_write;opts.host_resize=host_resize;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;
  opts.p0_image_storage_limit=1024*1024;opts.p0_image_storage_limit_set=true;
  opts.p0_checkpoint_bound=!legacy;opts.p0_checkpoint_generation=legacy?0:81;opts.p0_checkpoint_sequence=legacy||zero?0:40;
  if(!legacy)checkpoint_digest(&opts,dir,zero?@"zero.digest":@"checkpoint.digest");
  atomic_store(&writes,0);atomic_store(&clipboards,0);atomic_store(&notifications,0);
  if([kind isEqualToString:@"binding"]) {
   ghostty_surface_config_s bad=opts;bad.p0_resources_snapshot=blue.bytes;bad.p0_resources_snapshot_len=blue.length;
   CHECK(!ghostty_surface_new(app,&bad));
   __attribute__((objc_precise_lifetime)) NSData *other=fixture(dir,@"mixed-1.canonical");
   bad=opts;bad.p0_snapshot=other.bytes;bad.p0_snapshot_len=other.length;CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_checkpoint_generation++;CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_checkpoint_sequence++;CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_checkpoint_digest[0]^=1;CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_resources_snapshot=blue.bytes;bad.p0_resources_snapshot_len=blue.length;checkpoint_digest(&bad,dir,@"blue.digest");
   ghostty_surface_t accepted=ghostty_surface_new(app,&bad);CHECK(accepted);checkpoint_state(accepted,dir,2,blue,81,40);ghostty_surface_free(accepted);
  }
  if([kind isEqualToString:@"metadata"]) {
   ghostty_surface_config_s bad=opts;bad.p0_checkpoint_generation=0;checkpoint_digest(&bad,dir,@"zero-generation.digest");CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_checkpoint_bound=false;CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_resources_snapshot=NULL;bad.p0_resources_snapshot_len=0;CHECK(!ghostty_surface_new(app,&bad));
   bad=opts;bad.p0_snapshot=NULL;bad.p0_snapshot_len=0;CHECK(!ghostty_surface_new(app,&bad));
  }
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);
  checkpoint_state(surface,dir,2,nil,legacy?0:81,legacy||zero?0:40);
  spin(app,0.05);checkpoint_state(surface,dir,2,nil,legacy?0:81,legacy||zero?0:40);
  if([kind isEqualToString:@"generation"]) {
   uint64_t applied=99;CHECK(!ghostty_p0_surface_applied_sequence_for(surface,80,&applied) && applied==0);
   applied=99;CHECK(!ghostty_p0_surface_applied_sequence(surface,&applied) && applied==0);
   CHECK(!ghostty_p0_surface_event_output_for(surface,80,41,(const uint8_t*)"STALE",5));
   CHECK(!ghostty_p0_surface_event_resize_for(surface,80,41,90,30,8,16));
   CHECK(!ghostty_p0_surface_event_output(surface,41,(const uint8_t*)"OLD",3));
   CHECK(!ghostty_p0_surface_event_resize(surface,41,90,30,8,16));
   checkpoint_state(surface,dir,2,nil,81,40);
  }
  if(zero) {
   ghostty_surface_process_output(surface,(const uint8_t*)"BYPASS",6);size_t n=99;uint64_t applied=99;
   CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && n==0);
   CHECK(!ghostty_p0_surface_applied_sequence_for(surface,81,&applied) && applied==0);
   CHECK(!ghostty_p0_surface_event_output_for(surface,81,1,NULL,0));
  } else if(legacy) {
   CHECK(ghostty_p0_surface_event_resize(surface,1,90,30,8,16));checkpoint_state(surface,dir,3,nil,0,1);
   uint64_t applied=99;CHECK(ghostty_p0_surface_applied_sequence(surface,&applied) && applied==1);
  } else {
   CHECK(ghostty_p0_surface_event_resize_for(surface,81,41,90,30,8,16));checkpoint_state(surface,dir,3,nil,81,41);
   const char *off="\x1b[?2004lMODE-OFF";
   CHECK(ghostty_p0_surface_event_output_for(surface,81,42,(const uint8_t*)off,strlen(off)));checkpoint_state(surface,dir,4,nil,81,42);
   CHECK(!ghostty_p0_surface_event_output_for(surface,80,43,NULL,0));checkpoint_state(surface,dir,4,nil,81,42);
   CHECK(ghostty_p0_surface_event_output_for(surface,81,43,NULL,0));checkpoint_state(surface,dir,4,nil,81,43);
   spin(app,0.05);checkpoint_state(surface,dir,4,nil,81,43);
  }
  CHECK(atomic_load(&writes)==0 && atomic_load(&clipboards)==0 && atomic_load(&notifications)==0);
  ghostty_surface_free(surface);printf("PASS bound checkpoint/%s\n",kind.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
