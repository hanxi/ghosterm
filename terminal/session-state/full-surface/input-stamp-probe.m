#define main baseline_surface_main
#include "probe.m"
#undef main

typedef struct { uint64_t generation,sequence;size_t offset,len; } InputPacket;
typedef struct {
 ghostty_surface_t surface;
 atomic_size_t published;
 atomic_uint legacy_calls,packet_count,hooks;
 bool flip,key_flip;
 uint8_t bytes[32768];size_t used;
 InputPacket packets[128];
} InputContext;
static void legacy_input(void *opaque,const uint8_t *data,size_t len) {
 InputContext *ctx=opaque;atomic_fetch_add(&ctx->legacy_calls,1);(void)data;(void)len;
}
static void stamped_input(void *opaque,uint64_t generation,uint64_t sequence,const uint8_t *data,size_t len) {
 InputContext *ctx=opaque;unsigned i=atomic_load(&ctx->packet_count);CHECK(i<128 && len<=sizeof ctx->bytes-ctx->used);
 ctx->packets[i]=(InputPacket){generation,sequence,ctx->used,len};memcpy(ctx->bytes+ctx->used,data,len);ctx->used+=len;
 atomic_store(&ctx->packet_count,i+1);atomic_store(&ctx->published,ctx->used);
}
static void encoded_input(void *opaque,uint64_t generation,uint64_t sequence) {
 InputContext *ctx=opaque;unsigned count=atomic_fetch_add(&ctx->hooks,1);CHECK(generation==81);
 if(ctx->flip && count==0) {
  const char *delta=ctx->key_flip?"\x1b[?1h\x1b[20h":"\x1b[?2004l\x1b[20h";
  CHECK(sequence==40);CHECK(ghostty_p0_surface_event_output_for(ctx->surface,81,41,(const uint8_t*)delta,strlen(delta)));
 }
}
static void wait_input(ghostty_app_t app,InputContext *ctx,size_t expected) {
 NSDate *end=[NSDate dateWithTimeIntervalSinceNow:2];
 while(atomic_load(&ctx->published)<expected && [end timeIntervalSinceNow]>0)spin(app,0.01);
 spin(app,0.05);CHECK(atomic_load(&ctx->published)==expected);
}
static void key_input(ghostty_surface_t surface,uint32_t code,const char *text,uint32_t scalar,ghostty_input_mods_e mods) {
 ghostty_input_key_s key={0};key.action=GHOSTTY_ACTION_PRESS;key.keycode=code;key.text=text;key.unshifted_codepoint=scalar;key.mods=mods;
 CHECK(ghostty_surface_key(surface,key));
}
static void check_packets(InputContext *ctx,uint64_t sequence) {
 CHECK(atomic_load(&ctx->legacy_calls)==0 && atomic_load(&ctx->packet_count)>0);
 for(unsigned i=0;i<atomic_load(&ctx->packet_count);i++)CHECK(ctx->packets[i].generation==81 && ctx->packets[i].sequence==sequence);
}
static void check_applied(ghostty_surface_t surface,uint64_t sequence) {
 uint64_t current=99;CHECK(ghostty_p0_surface_applied_sequence_for(surface,81,&current));CHECK(current==sequence);
}
int main(int argc,char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);const char *mode=argc==3?argv[2]:"all";
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new();CHECK(config);
 NSString *dir=[NSString stringWithUTF8String:argv[1]];NSString *path=[dir stringByAppendingPathComponent:@"input.config"];
 CHECK([@"keybind = ctrl+a>ctrl+b=ignore\n" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
 ghostty_config_load_file(config,path.UTF8String);ghostty_config_finalize(config);CHECK(ghostty_config_diagnostics_count(config)==0);
 ghostty_runtime_config_s rt={0};rt.wakeup_cb=wakeup;rt.action_cb=action;rt.read_clipboard_cb=clipboard_read;
 rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config);CHECK(app);
 for(NSString *kind in @[@"paste",@"allocated",@"key",@"sequence",@"cursor",@"linefeed",@"mouse",@"reply-source",@"missing-callback",@"readonly",@"unstamped",@"wrong-generation",@"future",@"behind",@"legacy"]) {
  if(strcmp(mode,"all")!=0 && strcmp(mode,kind.UTF8String)!=0)continue;
  const bool legacy=[kind isEqualToString:@"legacy"],missing=[kind isEqualToString:@"missing-callback"];
  InputContext *ctx=calloc(1,sizeof *ctx);CHECK(ctx);
  __attribute__((objc_precise_lifetime)) NSData *initial=fixture(dir,@"mixed-2.canonical");
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,@"mixed-2.resources");
  __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();opts.platform_tag=GHOSTTY_PLATFORM_MACOS;
  opts.platform.macos.nsview=(__bridge void*)view;opts.scale_factor=1;opts.host=true;opts.host_write=legacy_input;opts.host_resize=host_resize;opts.userdata=ctx;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;
  opts.p0_image_storage_limit=1024*1024;opts.p0_image_storage_limit_set=true;
  opts.p0_checkpoint_bound=!legacy;opts.p0_checkpoint_generation=legacy?0:81;opts.p0_checkpoint_sequence=legacy?0:40;
  if(!legacy) {NSData *digest=fixture(dir,@"checkpoint.digest");CHECK(digest.length==32);memcpy(opts.p0_checkpoint_digest,digest.bytes,32);}
  opts.p0_host_write_input=missing?NULL:stamped_input;opts.p0_input_encoded=encoded_input;
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);CHECK(surface);ctx->surface=surface;spin(app,0.05);
  if([kind isEqualToString:@"paste"] || [kind isEqualToString:@"allocated"]) {
   ctx->flip=true;size_t n=[kind isEqualToString:@"allocated"]?6001:5;
   uint8_t *text=malloc(n+1);CHECK(text);memset(text,'X',n);if(n==5)memcpy(text,"USER",4);text[n-1]='\r';text[n]=0;
   uint8_t *expected=malloc(n+12);CHECK(expected);memcpy(expected,"\x1b[200~",6);memcpy(expected+6,text,n);memcpy(expected+6+n,"\x1b[201~",6);
   ghostty_surface_text(surface,(const char*)text,n);wait_input(app,ctx,n+12);
   CHECK(memcmp(ctx->bytes,expected,n+12)==0);check_packets(ctx,40);check_applied(surface,41);free(text);free(expected);
  } else if([kind isEqualToString:@"key"]) {
   ctx->flip=true;ctx->key_flip=true;key_input(surface,126,NULL,0,GHOSTTY_MODS_NONE);
   wait_input(app,ctx,3);CHECK(memcmp(ctx->bytes,"\x1b[A",3)==0);check_packets(ctx,40);check_applied(surface,41);
  } else if([kind isEqualToString:@"sequence"]) {
   key_input(surface,0,"a",'a',GHOSTTY_MODS_CTRL);spin(app,0.05);CHECK(atomic_load(&ctx->published)==0);
   const char *delta="\x1b[?1h";CHECK(ghostty_p0_surface_event_output_for(surface,81,41,(const uint8_t*)delta,strlen(delta)));
   key_input(surface,8,"c",'c',GHOSTTY_MODS_NONE);wait_input(app,ctx,2);
   CHECK(ctx->bytes[0]==1 && ctx->bytes[1]=='c');CHECK(atomic_load(&ctx->packet_count)==2 && atomic_load(&ctx->legacy_calls)==0);
   CHECK(ctx->packets[0].generation==81 && ctx->packets[0].sequence==40);
   CHECK(ctx->packets[1].generation==81 && ctx->packets[1].sequence==41);check_applied(surface,41);
  } else if([kind isEqualToString:@"cursor"]) {
   CHECK(ghostty_p0_surface_test_cursor(surface));
   wait_input(app,ctx,3);CHECK(memcmp(ctx->bytes,"\x1b[A",3)==0);check_packets(ctx,40);check_applied(surface,40);
  } else if([kind isEqualToString:@"linefeed"]) {
   const char *delta="\x1b[20h";CHECK(ghostty_p0_surface_event_output_for(surface,81,41,(const uint8_t*)delta,strlen(delta)));
   size_t n=6000;char *text=malloc(n);uint8_t *expected=malloc(2*n+12);CHECK(text && expected);memset(text,'\r',n);
   memcpy(expected,"\x1b[200~",6);for(size_t i=0;i<n;i++) {expected[6+2*i]='\r';expected[7+2*i]='\n';}memcpy(expected+6+2*n,"\x1b[201~",6);
   ghostty_surface_text(surface,text,n);wait_input(app,ctx,2*n+12);CHECK(memcmp(ctx->bytes,expected,2*n+12)==0);
   check_packets(ctx,41);check_applied(surface,41);free(text);free(expected);
  } else if([kind isEqualToString:@"mouse"]) {
   const char *delta="\x1b[?1000h\x1b[?1006h";CHECK(ghostty_p0_surface_event_output_for(surface,81,41,(const uint8_t*)delta,strlen(delta)));
   ghostty_surface_mouse_pos(surface,0,0,GHOSTTY_MODS_NONE);
   CHECK(ghostty_surface_mouse_button(surface,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE));
   const char *expected="\x1b[<0;1;1M";wait_input(app,ctx,strlen(expected));CHECK(memcmp(ctx->bytes,expected,strlen(expected))==0);check_packets(ctx,41);
  } else if([kind isEqualToString:@"reply-source"]) {
   CHECK(!ghostty_p0_surface_test_reply(surface));spin(app,0.05);CHECK(atomic_load(&ctx->published)==0 && atomic_load(&ctx->legacy_calls)==0);
   const char *action="csi:6n";CHECK(ghostty_surface_binding_action(surface,action,strlen(action)));
   wait_input(app,ctx,4);CHECK(memcmp(ctx->bytes,"\x1b[6n",4)==0);check_packets(ctx,40);check_applied(surface,40);
  } else if(missing || [kind isEqualToString:@"unstamped"] || [kind isEqualToString:@"wrong-generation"] || [kind isEqualToString:@"future"] || [kind isEqualToString:@"behind"]) {
   if(missing)ghostty_surface_text(surface,"USER",4);
   else CHECK(ghostty_p0_surface_test_input(surface,[kind isEqualToString:@"wrong-generation"]?80:81,[kind isEqualToString:@"future"]?41:[kind isEqualToString:@"behind"]?39:40,[kind isEqualToString:@"unstamped"],(const uint8_t*)"BAD",3));
   spin(app,0.1);uint64_t seq=99;size_t n=99;
   CHECK(!ghostty_p0_surface_applied_sequence_for(surface,81,&seq) && seq==0);
   CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && n==0);
   CHECK(atomic_load(&ctx->legacy_calls)==0 && atomic_load(&ctx->published)==0);
  } else if([kind isEqualToString:@"readonly"]) {
   const char *action="toggle_readonly";CHECK(ghostty_surface_binding_action(surface,action,strlen(action)));
   ctx->flip=true;char data[6000];memset(data,'X',sizeof data);ghostty_surface_text(surface,data,sizeof data);spin(app,0.1);
   CHECK(atomic_load(&ctx->hooks)==0 && atomic_load(&ctx->legacy_calls)==0 && atomic_load(&ctx->published)==0);check_applied(surface,40);
  } else {
   ghostty_surface_text(surface,"USER",4);spin(app,0.1);CHECK(atomic_load(&ctx->legacy_calls)>0 && atomic_load(&ctx->published)==0 && atomic_load(&ctx->hooks)==0);
  }
  ghostty_surface_free(surface);free(ctx);printf("PASS input stamp/%s\n",kind.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);return 0;
} }
