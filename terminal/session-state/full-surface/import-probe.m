#include "import-fixture.h"
#import <AppKit/AppKit.h>
#include <ghostty.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL native:%d: %s\n", __LINE__, #x);                   \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static atomic_uint writes, effects, legacy;
static atomic_bool done, prepared, advance;
static atomic_uint stage;
static const char *mode;
static ghostty_app_t app;
static ghostty_surface_t surface;
static import_fixture fixture;
static void wake(void *ctx) { (void)ctx; }
static bool action(ghostty_app_t a, ghostty_target_s t,
                   ghostty_action_s action) {
  (void)a;
  (void)t;
  if (action.tag == GHOSTTY_ACTION_DESKTOP_NOTIFICATION)
    atomic_fetch_add(&effects, 1);
  return false;
}
static ghostty_clipboard_read_result_e read_clip(void *u, ghostty_clipboard_e c,
                                                 void *s, const char *const *m,
                                                 size_t n, bool l) {
  (void)u;
  (void)c;
  (void)s;
  (void)m;
  (void)n;
  (void)l;
  atomic_fetch_add(&effects, 1);
  return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
}
static void confirm_clip(void *u, const ghostty_clipboard_confirm_s *c, void *s,
                         ghostty_clipboard_request_e r) {
  (void)u;
  (void)c;
  (void)s;
  (void)r;
  atomic_fetch_add(&effects, 1);
}
static void write_clip(void *u, ghostty_clipboard_e c,
                       const ghostty_clipboard_content_s *p, size_t n, bool b) {
  (void)u;
  (void)c;
  (void)p;
  (void)n;
  (void)b;
  atomic_fetch_add(&effects, 1);
}
static void raw_write(void *ctx, const uint8_t *p, size_t n) {
  (void)ctx;
  (void)p;
  (void)n;
  atomic_fetch_add(&legacy, 1);
}
static void resize(void *ctx, uint16_t c, uint16_t r) {
  (void)ctx;
  (void)c;
  (void)r;
}
static void input(void *ctx, uint64_t generation, uint64_t sequence,
                  const uint8_t *p, size_t n) {
  (void)ctx;
  CHECK(generation == 7 && sequence == 10 && n == 1 && *p == 'x');
  atomic_fetch_add(&writes, 1);
}
static void spin(double seconds) {
  NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
  do {
    ghostty_app_tick(app);
    [[NSRunLoop mainRunLoop]
        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
  } while ([end timeIntervalSinceNow] > 0);
}
static void equal(import_parts p) {
  size_t n = 0;
  CHECK(!ghostty_p0_surface_snapshot(surface, NULL, 0, &n) &&
        n == p.canonical_len);
  uint8_t *b = malloc(n);
  CHECK(b && ghostty_p0_surface_snapshot(surface, b, n, &n) &&
        n == p.canonical_len && !memcmp(b, p.canonical, n));
  free(b);
  n = 0;
  CHECK(!ghostty_p0_surface_resources_snapshot(surface, NULL, 0, &n) &&
        n == p.resources_len);
  b = malloc(n);
  CHECK(b && ghostty_p0_surface_resources_snapshot(surface, b, n, &n) &&
        !memcmp(b, p.resources, n));
  free(b);
}
static void pixels(NSView *view, bool final) {
  spin(.2);
  ghostty_surface_draw(surface);
  spin(.2);
  NSBitmapImageRep *rep =
      [view bitmapImageRepForCachingDisplayInRect:view.bounds];
  CHECK(rep);
  [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
  CGImageRef image = rep.CGImage;
  CHECK(image);
  size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
  uint8_t *p = calloc(w * h, 4);
  CHECK(p);
  CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(
      p, w, h, 8, w * 4, colors, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  CHECK(c);
  CGContextDrawImage(c, CGRectMake(0, 0, w, h), image);
  size_t red = 0, green = 0;
  for (size_t i = 0; i < w * h; i++) {
    unsigned r = p[i * 4], g = p[i * 4 + 1], b = p[i * 4 + 2];
    red += r > 150 && g < 80 && b < 80;
    green += g > 150 && r < 80 && b < 80;
  }
  printf("pixels final=%d red=%zu green=%zu\n", final, red, green);
  CHECK(red > 100 && (final ? green > 100 : green == 0));
  NSString *file = [NSString
      stringWithFormat:@"tmp/surface-import-probe/frame-%d.png", final];
  CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG
                           properties:@{}] writeToFile:file atomically:YES]);
  CGContextRelease(c);
  CGColorSpaceRelease(colors);
  free(p);
}
static void *prepare(void *ctx) {
  CHECK(![NSThread isMainThread]);
  (void)ctx;
  import_fixture_open(&fixture, mode);
  atomic_store_explicit(&prepared, true, memory_order_release);
  return NULL;
}
static void *finish_fixture(void *ctx) {
  (void)ctx;
  import_fixture_close();
  return NULL;
}
static void deny_file(void) {
  @autoreleasepool {
    char cwd[4096], path[4300];
    CHECK(getcwd(cwd, sizeof(cwd)));
    CHECK(snprintf(path, sizeof(path),
                   "%s/tmp/tty-graphics-protocol-session-import-%d", cwd,
                   getpid()) > 0);
    FILE *f = fopen(path, "wb");
    const uint8_t red[] = {255, 0, 0, 255};
    CHECK(f && fwrite(red, 1, 4, f) == 4 && !fclose(f));
    NSData *data = [NSData dataWithBytes:path length:strlen(path)];
    NSString *command = [NSString
        stringWithFormat:@"\033_Ga=T,i=8,f=32,s=1,v=1,t=t,q=2;%@\033\\",
                         [data base64EncodedStringWithOptions:0]];
    CHECK(!ghostty_p0_surface_event_output_for(
        surface, 7, 11, (const uint8_t *)command.UTF8String,
        strlen(command.UTF8String)));
    CHECK(!access(path, F_OK));
    uint64_t sequence = 0;
    CHECK(!ghostty_p0_surface_applied_sequence_for(surface, 7, &sequence));
    CHECK(!unlink(path));
  }
}
static void *consume(void *ctx) {
  (void)ctx;
  bool image = !strcmp(mode, "image");
  CHECK(![NSThread isMainThread]);
  uint64_t sequence = 0;
  CHECK(!ghostty_p0_surface_applied_sequence_for(surface, 0, &sequence));
  CHECK(!ghostty_p0_surface_input_enable(surface, 7, 0, true));
  if (!strcmp(mode, "bad-canonical") || !strcmp(mode, "bad-resources") ||
      !strcmp(mode, "raw") || !strcmp(mode, "budget")) {
    const uint8_t *c = fixture.initial.canonical,
                  *r = fixture.initial.resources;
    uint8_t *bad = NULL;
    if (!strcmp(mode, "bad-canonical")) {
      bad = malloc(fixture.initial.canonical_len);
      CHECK(bad);
      memcpy(bad, c, fixture.initial.canonical_len);
      bad[0] ^= 1;
      c = bad;
    }
    if (!strcmp(mode, "bad-resources")) {
      bad = malloc(fixture.initial.resources_len);
      CHECK(bad);
      memcpy(bad, r, fixture.initial.resources_len);
      bad[0] ^= 1;
      r = bad;
    }
    if (!strcmp(mode, "raw"))
      ghostty_surface_process_output(surface, (const uint8_t *)"WRONG", 5);
    if (!strcmp(mode, "budget"))
      import_fixture_revoke();
    CHECK(!ghostty_p0_surface_import_once(surface, 7, 9, c,
                                          fixture.initial.canonical_len, r,
                                          fixture.initial.resources_len));
    free(bad);
    CHECK(!ghostty_p0_surface_applied_sequence_for(surface, 7, &sequence));
    CHECK(!ghostty_p0_surface_input_enable(surface, 7, 9, true));
    CHECK(!ghostty_p0_surface_import_once(
        surface, 7, 9, fixture.initial.canonical, fixture.initial.canonical_len,
        fixture.initial.resources, fixture.initial.resources_len));
    atomic_store_explicit(&done, true, memory_order_release);
    return NULL;
  }
  CHECK(ghostty_p0_surface_import_once(
      surface, 7, 9, fixture.initial.canonical, fixture.initial.canonical_len,
      fixture.initial.resources, fixture.initial.resources_len));
  equal(fixture.initial);
  if (image) {
    uint8_t pixel[4];
    size_t n = 0;
    uint32_t col, row;
    const uint8_t red[] = {255, 0, 0, 255};
    CHECK(ghostty_p0_surface_read_image(surface, 7, pixel, 4, &n, &col, &row) &&
          n == 4 && !memcmp(pixel, red, 4));
  }
  atomic_store_explicit(&stage, 1, memory_order_release);
  while (!atomic_load_explicit(&advance, memory_order_acquire)) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
  }
  CHECK(ghostty_p0_surface_applied_sequence_for(surface, 7, &sequence) &&
        sequence == 9);
  CHECK(!ghostty_p0_surface_applied_sequence_for(surface, 8, &sequence));
  CHECK(!ghostty_p0_surface_import_once(
      surface, 8, 9, fixture.initial.canonical, fixture.initial.canonical_len,
      fixture.initial.resources, fixture.initial.resources_len));
  CHECK(ghostty_p0_surface_event_output_for(surface, 7, 10, fixture.delta,
                                            fixture.delta_len));
  equal(fixture.final);
  if (!strcmp(mode, "deny-file")) {
    deny_file();
    atomic_store_explicit(&done, true, memory_order_release);
    return NULL;
  }
  CHECK(!ghostty_p0_surface_input_enable(surface, 6, 10, true));
  CHECK(!ghostty_p0_surface_input_enable(surface, 7, 11, true));
  atomic_store_explicit(&done, true, memory_order_release);
  return NULL;
}
int main(int argc, char **argv) {
  @autoreleasepool {
    CHECK(argc == 2);
    mode = argv[1];
    bool image = !strcmp(mode, "image"), empty = !strcmp(mode, "empty"),
         fault = !strcmp(mode, "bad-canonical") ||
                 !strcmp(mode, "bad-resources") || !strcmp(mode, "raw") ||
                 !strcmp(mode, "budget");
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    CHECK(!ghostty_init(0, NULL));
    ghostty_config_t config = ghostty_config_new();
    CHECK(config);
    ghostty_config_finalize(config);
    ghostty_runtime_config_s rt = {.wakeup_cb = wake,
                                   .action_cb = action,
                                   .read_clipboard_cb = read_clip,
                                   .confirm_read_clipboard_cb = confirm_clip,
                                   .write_clipboard_cb = write_clip};
    app = ghostty_app_new(&rt, config);
    CHECK(app);
    pthread_t prep;
    CHECK(!pthread_create(&prep, NULL, prepare, NULL));
    while (!atomic_load_explicit(&prepared, memory_order_acquire))
      spin(.005);
    CHECK(!pthread_join(prep, NULL));
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 600)];
    NSWindow *window =
        [[NSWindow alloc] initWithContentRect:view.frame
                                    styleMask:NSWindowStyleMaskTitled
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.contentView = view;
    [window orderFrontRegardless];
    ghostty_surface_config_s c = ghostty_surface_config_new();
    c.platform_tag = GHOSTTY_PLATFORM_MACOS;
    c.platform.macos.nsview = (__bridge void *)view;
    c.scale_factor = 1;
    c.host = true;
    c.host_write = raw_write;
    c.host_resize = resize;
    c.p0_host_write_input = input;
    c.p0_replica_empty = true;
    c.p0_image_storage_limit_set = true;
    c.p0_image_storage_limit = 1024 * 1024;
    c.p0_allocator = fixture.general;
    c.p0_page_allocator = fixture.pages;
    c.p0_budget_userdata = fixture.scope;
    c.p0_budget_healthy = fixture.healthy;
    bool plain_legacy = !strcmp(mode, "legacy");
    if (plain_legacy) {
      c.p0_replica_empty = false;
      c.p0_host_write_input = NULL;
      c.p0_allocator = NULL;
      c.p0_page_allocator = NULL;
      c.p0_budget_userdata = NULL;
      c.p0_budget_healthy = NULL;
      c.p0_image_storage_limit_set = false;
      c.p0_image_storage_limit = 0;
    }

    ghostty_config_t replica_config = NULL;
    if (!strcmp(mode, "config")) {
      char config_path[4096];
      CHECK(getcwd(config_path, sizeof(config_path)));
      CHECK(strlen(config_path) + 64 < sizeof(config_path));
      strcat(config_path, "/tmp/import-config-XXXXXX");
      int fd = mkstemp(config_path);
      CHECK(fd >= 0);
      const char text[] = "font-size = 30\n";
      CHECK(write(fd, text, sizeof(text) - 1) == sizeof(text) - 1 &&
            !close(fd));
      replica_config = ghostty_p0_config_clone(config, fixture.general);
      CHECK(replica_config);
      ghostty_config_load_file(replica_config, config_path);
      CHECK(!unlink(config_path));
      ghostty_config_finalize(replica_config);
      CHECK(!ghostty_config_diagnostics_count(replica_config));
      CHECK(ghostty_p0_config_replica_colors(
          replica_config, true, (ghostty_config_color_s){1, 2, 3},
          (ghostty_config_color_s){4, 5, 6}));
      ghostty_config_color_s color;
      CHECK(ghostty_config_get(replica_config, &color, "background", 10) &&
            color.r == 1 && color.g == 2 && color.b == 3);
      CHECK(ghostty_config_get(replica_config, &color, "foreground", 10) &&
            color.r == 4 && color.g == 5 && color.b == 6);
      c.p0_color_scheme_set = true;
      c.p0_color_scheme_dark = true;
      c.p0_replica_config = replica_config;
      ghostty_surface_config_s unbound = c;
      unbound.p0_replica_empty = false;
      unbound.p0_color_scheme_set = false;
      unbound.p0_color_scheme_dark = false;
      unbound.p0_allocator = NULL;
      unbound.p0_page_allocator = NULL;
      unbound.p0_thread_allocator = NULL;
      unbound.p0_budget_userdata = NULL;
      unbound.p0_budget_healthy = NULL;
      unbound.p0_image_storage_limit_set = false;
      unbound.p0_image_storage_limit = 0;
      CHECK(!ghostty_surface_new(app, &unbound));
    }
    surface = ghostty_surface_new(app, &c);
    CHECK(surface);
    if (replica_config) {
      ghostty_surface_size_s size = ghostty_surface_size(surface);
      CHECK(size.columns > 0 && size.columns < 50);
      ghostty_config_free(replica_config);
    }

    ghostty_surface_text(surface, "x", 1);
    spin(.03);
    if (plain_legacy) {
      CHECK(atomic_load(&legacy) == 1 && !atomic_load(&writes));
      goto release_surface;
    }
    CHECK(!atomic_load(&legacy) && !atomic_load(&writes));
    CHECK(ghostty_p0_surface_test_input(surface, 7, 9, false,
                                        (const uint8_t *)"x", 1));
    spin(.03);
    CHECK(!atomic_load(&writes) && !atomic_load(&legacy));
    if (!empty) {
      pthread_t worker;
      CHECK(!pthread_create(&worker, NULL, consume, NULL));
      NSDate *end = [NSDate dateWithTimeIntervalSinceNow:10];
      if (!fault) {
        while (!atomic_load_explicit(&stage, memory_order_acquire) &&
               [end timeIntervalSinceNow] > 0)
          spin(.005);
        CHECK(atomic_load(&stage) == 1);
        if (!image)
          pixels(view, false);
        atomic_store_explicit(&advance, true, memory_order_release);
      }
      while (!atomic_load_explicit(&done, memory_order_acquire) &&
             [end timeIntervalSinceNow] > 0)
        spin(.01);
      CHECK(atomic_load(&done) && !pthread_join(worker, NULL));
      if (!image && !fault)
        pixels(view, true);
      if (!fault && strcmp(mode, "deny-file")) {
        CHECK(ghostty_p0_surface_test_input(surface, 7, 10, false,
                                            (const uint8_t *)"x", 1));
        spin(.03);
        CHECK(!atomic_load(&writes));
        CHECK(ghostty_p0_surface_input_enable(surface, 7, 10, true));
        CHECK(ghostty_p0_surface_test_input(surface, 6, 10, false,
                                            (const uint8_t *)"x", 1));
        spin(.03);
        CHECK(!atomic_load(&writes));
        uint64_t sequence;
        CHECK(ghostty_p0_surface_applied_sequence_for(surface, 7, &sequence) &&
              sequence == 10);
        CHECK(ghostty_p0_surface_test_input(surface, 7, 10, false,
                                            (const uint8_t *)"x", 1));
        spin(.05);
        CHECK(atomic_load(&writes) == 1);
        CHECK(ghostty_p0_surface_input_enable(surface, 7, 10, false));
        CHECK(ghostty_p0_surface_test_input(surface, 7, 10, false,
                                            (const uint8_t *)"x", 1));
        spin(.03);
        CHECK(atomic_load(&writes) == 1 && !atomic_load(&effects) &&
              !atomic_load(&legacy));
      }
    } else {
      size_t size = 0;
      CHECK(!ghostty_p0_surface_snapshot(surface, NULL, 0, &size) && size > 0);
    }
    CHECK(!atomic_load(&effects) && !atomic_load(&legacy));
  release_surface:
    ghostty_surface_free(surface);
    spin(.1);
    ghostty_app_free(app);
    ghostty_config_free(config);
    [window orderOut:nil];
    pthread_t finish;
    CHECK(!pthread_create(&finish, NULL, finish_fixture, NULL) &&
          !pthread_join(finish, NULL));
    printf("PASS native IO import, continuation/resources, one-shot "
           "generations and LIVE input/%s\n",
           argv[1]);
    return 0;
  }
}
