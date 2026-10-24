#import <AppKit/AppKit.h>
#include <ghostty.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static atomic_uint writes, clipboards, notifications;
static unsigned char input[128];
static atomic_size_t input_len;
static void wakeup(void *u) { (void)u; }
static bool action(ghostty_app_t app, ghostty_target_s t, ghostty_action_s a) {
  (void)app; (void)t;
  if (a.tag == GHOSTTY_ACTION_DESKTOP_NOTIFICATION) atomic_fetch_add(&notifications,1);
  return false;
}
static ghostty_clipboard_read_result_e clipboard_read(void *u, ghostty_clipboard_e c, void *s, const char *const *m, size_t n, bool l) {
  (void)u;(void)c;(void)s;(void)m;(void)n;(void)l;atomic_fetch_add(&clipboards,1); return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
}
static void clipboard_confirm(void *u, const ghostty_clipboard_confirm_s *c, void *s, ghostty_clipboard_request_e r) {
  (void)u;(void)c;(void)s;(void)r;atomic_fetch_add(&clipboards,1);
}
static void clipboard_write(void *u, ghostty_clipboard_e c, const ghostty_clipboard_content_s *p, size_t n, bool b) {
  (void)u;(void)c;(void)p;(void)n;(void)b;atomic_fetch_add(&clipboards,1);
}
static void host_write(void *u, const uint8_t *p, size_t n) {
  (void)u; size_t off=atomic_load(&input_len); CHECK(n <= sizeof input-off); memcpy(input+off,p,n); atomic_store(&input_len,off+n); atomic_fetch_add(&writes,1);
}
static void host_resize(void *u, uint16_t c, uint16_t r) { (void)u;(void)c;(void)r; }
static NSData *fixture(NSString *dir, NSString *name) {
 NSData *d = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:name]]; CHECK(d != nil); return d;
}
static void spin(ghostty_app_t app, double seconds) {
 NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
 do { ghostty_app_tick(app); [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]]; } while([end timeIntervalSinceNow]>0);
}
static void equal_snapshot(ghostty_surface_t surface, NSData *expected) {
 size_t required=0;
 CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&required));
 CHECK(required==expected.length);
 uint8_t *data=malloc(required); CHECK(data!=NULL);
 CHECK(ghostty_p0_surface_snapshot(surface,data,required,&required));
 CHECK(memcmp(data,expected.bytes,required)==0); free(data);
}
#ifdef GHOSTERM_P0_RESOURCE_HOOKS
typedef struct { const uint8_t *path,*data;size_t path_len,len;uint32_t medium,status;unsigned calls;bool consumed; } SurfaceResource;
static uint32_t little32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static bool resource_resolve(void *ctx,uint32_t medium,const uint8_t *path,size_t n,uint32_t *status,const uint8_t **data,size_t *len) {
 SurfaceResource *r=ctx;r->calls++;
 if(r->consumed || medium!=r->medium || n!=r->path_len || memcmp(path,r->path,n)!=0)return false;
 r->consumed=true;*status=r->status;*data=r->data;*len=r->len;return true;
}
#endif
int main(int argc, char **argv) { @autoreleasepool {
 CHECK(argc==2 || argc==3);
 const char *mode=argc==3?argv[2]:"all";
 bool input_only=strcmp(mode,"input")==0, effects_only=strcmp(mode,"effects")==0, state_only=strcmp(mode,"state")==0;
 bool pty_only=strcmp(mode,"pty")==0;
 bool vim_only=strcmp(mode,"vim")==0;
 bool tmux_only=strcmp(mode,"tmux")==0;
 [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
 CHECK(ghostty_init(0,NULL)==0);
 ghostty_config_t config=ghostty_config_new(); CHECK(config);
 ghostty_config_finalize(config);
 ghostty_runtime_config_s rt={0}; rt.wakeup_cb=wakeup;rt.action_cb=action;
 rt.read_clipboard_cb=clipboard_read;rt.confirm_read_clipboard_cb=clipboard_confirm;rt.write_clipboard_cb=clipboard_write;
 ghostty_app_t app=ghostty_app_new(&rt,config); CHECK(app);
 NSString *dir=[NSString stringWithUTF8String:argv[1]];
#ifdef GHOSTERM_P0_RESOURCE_HOOKS
#ifdef GHOSTERM_P0_SOURCE_QUOTA
#ifdef GHOSTERM_P0_STATIC_RESOURCES
 NSArray *names=@[@"resource-rgba",@"static-rgba",@"static-png",@"static-rgb",@"static-screens"];
#else
 NSArray *names=@[@"resource-rgba",@"resource-png",@"resource-failure",@"resource-reject"];
#endif
#else
 NSArray *names=@[@"resource-rgba",@"resource-png",@"resource-failure"];
#endif
#else
 NSArray *names=@[@"ground",@"csi",@"utf8",@"osc",@"esc",@"dcs",@"apc",@"pty",@"vim",@"tmux"];
#endif
 for (NSString *name in names) {
  if ((strncmp(mode,"resource-",9)==0 || strncmp(mode,"static-",7)==0) && strcmp(mode,name.UTF8String)!=0) continue;
  if (pty_only && ![name isEqualToString:@"pty"]) continue;
  if (vim_only && ![name isEqualToString:@"vim"]) continue;
  if (tmux_only && ![name isEqualToString:@"tmux"]) continue;
  if ((input_only || effects_only) && ![name isEqualToString:@"ground"]) continue;
  NSData *initial=fixture(dir,[name stringByAppendingString:@".initial"]);
  NSData *delta=fixture(dir,[name stringByAppendingString:@".delta"]);
  NSData *final=fixture(dir,[name stringByAppendingString:@".final"]);
  NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
  ghostty_surface_config_s opts=ghostty_surface_config_new();
  opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void *)view;
  opts.scale_factor=1;opts.host=true;opts.host_write=host_write;opts.host_resize=host_resize;
  opts.p0_snapshot=initial.bytes;opts.p0_snapshot_len=initial.length;
#ifdef GHOSTERM_P0_RESOURCE_HOOKS
  __attribute__((objc_precise_lifetime)) NSData *event=fixture(dir,[name stringByAppendingString:@".event"]);CHECK(event.length>=16);
  const uint8_t *wire=event.bytes;SurfaceResource resource={.medium=little32(wire),.status=little32(wire+4),.path_len=little32(wire+8),.len=little32(wire+12)};
  size_t header=16;
#ifdef GHOSTERM_P0_SOURCE_QUOTA
  CHECK(event.length>=20);header=20;opts.p0_image_storage_limit=little32(wire+16);opts.p0_image_storage_limit_set=strcmp(mode,"no-limit")!=0;
#ifdef GHOSTERM_P0_STATIC_RESOURCES
  __attribute__((objc_precise_lifetime)) NSData *resources=fixture(dir,[name stringByAppendingString:@".resources"]);
  opts.p0_resources_snapshot=resources.bytes;opts.p0_resources_snapshot_len=resources.length;
  if(strcmp(mode,"wrong-limit")==0)opts.p0_image_storage_limit++;
  if(strcmp(mode,"missing-bundle")==0)opts.p0_resources_snapshot=NULL;
#endif
#endif
  CHECK(resource.path_len<=4096 && resource.len<=4096 && event.length==header+resource.path_len+resource.len);
  resource.path=wire+header;resource.data=wire+header+resource.path_len;
  if(strcmp(mode,"missing")==0)resource.consumed=true;
  opts.p0_resource_userdata=&resource;opts.p0_resource_resolve=resource_resolve;
#endif
  atomic_store(&writes,0);atomic_store(&clipboards,0);atomic_store(&notifications,0);
  ghostty_surface_t surface=ghostty_surface_new(app,&opts);
#ifdef GHOSTERM_P0_SOURCE_QUOTA
  if(strcmp(mode,"no-limit")==0 || strcmp(mode,"wrong-limit")==0 || strcmp(mode,"missing-bundle")==0) {CHECK(!surface);printf("PASS full resource rejects missing quota/%s\n",name.UTF8String);continue;}
#endif
  CHECK(surface);
  equal_snapshot(surface,initial);
#ifdef GHOSTERM_P0_STATIC_RESOURCES
  if([name hasPrefix:@"static-"]) {
    uint8_t rgba[4];size_t n=0;uint32_t col=0,row=0;const uint8_t red[]={255,0,0,255};
    CHECK(ghostty_p0_surface_read_image(surface,7,rgba,sizeof rgba,&n,&col,&row));
    CHECK(n==([name isEqualToString:@"static-rgb"]?3u:4u) && memcmp(rgba,red,n)==0);
    CHECK(col==([name isEqualToString:@"static-screens"]?8u:4u) && row==([name isEqualToString:@"static-screens"]?7u:2u));
  }
#endif
  if (!input_only) {
    ghostty_surface_process_output(surface,delta.bytes,delta.length);
#ifdef GHOSTERM_P0_RESOURCE_HOOKS
    if(strcmp(mode,"missing")==0 || strcmp(mode,"duplicate")==0) {
      if(strcmp(mode,"duplicate")==0)ghostty_surface_process_output(surface,delta.bytes,delta.length);
      size_t failed_size=99;CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&failed_size));CHECK(failed_size==0);
      CHECK(resource.calls==(strcmp(mode,"duplicate")==0?2u:1u));
      CHECK(atomic_load(&writes)==0);ghostty_surface_free(surface);
      printf("PASS rejected full resource/%s/%s\n",mode,name.UTF8String);continue;
    }
#endif
    equal_snapshot(surface,final);
#ifdef GHOSTERM_P0_RESOURCE_HOOKS
    CHECK(resource.calls==1 && resource.consumed);
    uint8_t rgba[4];size_t n=0;uint32_t col=0,row=0;
    bool image=ghostty_p0_surface_read_image(surface,7,rgba,sizeof rgba,&n,&col,&row);
    if(resource.status==0 && ![name isEqualToString:@"resource-reject"]) {const uint8_t red[]={255,0,0,255};CHECK(image && n==4 && memcmp(rgba,red,4)==0 && col==4 && row==2);}
    else CHECK(!image && n==0);
#ifdef GHOSTERM_P0_STATIC_RESOURCES
    if([name isEqualToString:@"static-screens"]) {
      ghostty_surface_process_output(surface,(const uint8_t*)"\x1b[?47h",6);
      equal_snapshot(surface,fixture(dir,[name stringByAppendingString:@".alternate"]));
      n=0;CHECK(ghostty_p0_surface_read_image(surface,7,rgba,sizeof rgba,&n,&col,&row));
      const uint8_t red[]={255,0,0,255};CHECK(n==4 && memcmp(rgba,red,4)==0 && col==8 && row==7);
    }
#endif
#ifdef GHOSTERM_P0_SOURCE_QUOTA
    if([name isEqualToString:@"resource-reject"]) {
      ghostty_surface_update_config(surface,config);resource.consumed=false;resource.calls=0;
      ghostty_surface_process_output(surface,delta.bytes,delta.length);
      n=0;CHECK(!ghostty_p0_surface_read_image(surface,7,rgba,sizeof rgba,&n,&col,&row) && n==0);
      CHECK(resource.calls==1 && resource.consumed);
    }
#endif
#endif
  }
  spin(app,0.15);
  if (!state_only) { CHECK(atomic_load(&writes)==0); CHECK(atomic_load(&clipboards)==0);CHECK(atomic_load(&notifications)==0); }
  if (!state_only && !effects_only) {
  atomic_store(&input_len,0);
  ghostty_surface_text(surface,"USER",4);
  spin(app,0.15);
  const char expected_input[]="\x1b[200~USER\x1b[201~";
  CHECK(atomic_load(&writes)>0);CHECK(atomic_load(&input_len)==sizeof expected_input-1);CHECK(memcmp(input,expected_input,sizeof expected_input-1)==0);
  }
  ghostty_surface_draw(surface); spin(app,0.1);
  ghostty_surface_free(surface);
  printf("PASS full-surface/%s/%s\n",mode,name.UTF8String);
 }
 ghostty_app_free(app);ghostty_config_free(config);
 return 0;
}}
