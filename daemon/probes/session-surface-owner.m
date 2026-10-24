/* P0 integration only: Core worker/restore stream + actual Metal surface. */
#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#include <ghostty.h>
#include <stdatomic.h>
#include "surface-restore-probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MIB (1024u*1024u)
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)

static ghostty_app_t surface_app;
static ghostty_config_t surface_config;
static atomic_uint surface_legacy_writes,surface_effects;
static atomic_uint surface_search_reports;
static _Atomic int64_t surface_search_total;
typedef struct {
    const gt_surface_probe_t *probe;
    atomic_size_t used;
    atomic_uint rejected;
    uint8_t bytes[128];
} surface_input_t;
static void surface_wakeup(void *context) {(void)context;}
static bool surface_action(ghostty_app_t app,ghostty_target_s target,ghostty_action_s action) {
    (void)app;(void)target;
    if(action.tag==GHOSTTY_ACTION_SEARCH_TOTAL){atomic_store(&surface_search_total,action.action.search_total.total);atomic_fetch_add(&surface_search_reports,1);}
    if(action.tag==GHOSTTY_ACTION_DESKTOP_NOTIFICATION)atomic_fetch_add(&surface_effects,1);
    return false;
}
static ghostty_clipboard_read_result_e surface_clipboard_read(void *u,ghostty_clipboard_e c,void *s,const char *const *m,size_t n,bool l) {
    (void)u;(void)c;(void)s;(void)m;(void)n;(void)l;
    atomic_fetch_add(&surface_effects,1);return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
}
static void surface_clipboard_confirm(void *u,const ghostty_clipboard_confirm_s *c,void *s,ghostty_clipboard_request_e r) {
    (void)u;(void)c;(void)s;(void)r;atomic_fetch_add(&surface_effects,1);
}
static void surface_clipboard_write(void *u,ghostty_clipboard_e c,const ghostty_clipboard_content_s *p,size_t n,bool b) {
    (void)u;(void)c;(void)p;(void)n;(void)b;atomic_fetch_add(&surface_effects,1);
}
static ghostty_app_t surface_create_app_options(const ghostty_p0_allocator_s *allocator,const ghostty_p0_allocator_s *threads,void *context,void (*observe)(void *,uint32_t,uint32_t)) {
    ghostty_runtime_config_s runtime={0};runtime.wakeup_cb=surface_wakeup;runtime.action_cb=surface_action;
    runtime.read_clipboard_cb=surface_clipboard_read;runtime.confirm_read_clipboard_cb=surface_clipboard_confirm;
    runtime.write_clipboard_cb=surface_clipboard_write;runtime.p0_allocator=allocator;
    runtime.p0_thread_allocator=threads;runtime.p0_warmup_userdata=context;runtime.p0_warmup_observer=observe;
    return ghostty_app_new(&runtime,surface_config);
}
static ghostty_app_t surface_create_app(const ghostty_p0_allocator_s *allocator) {
    return surface_create_app_options(allocator,NULL,NULL,NULL);
}
static void surface_legacy_write(void *u,const uint8_t *p,size_t n) {
    (void)u;(void)p;(void)n;atomic_fetch_add(&surface_legacy_writes,1);
}
static void surface_resize_request(void *u,uint16_t c,uint16_t r) {(void)u;(void)c;(void)r;}
static void surface_input(void *u,uint64_t generation,uint64_t sequence,const uint8_t *bytes,size_t length) {
    surface_input_t *context=u;
    /* 闸门不过就地丢弃，不缓存等恢复后重放；用例按“字节有没有到”判红。 */
    if(!gt__attachment_input_allowed(context->probe->attachment,generation,sequence)) {
        atomic_fetch_add(&context->rejected,1);return;
    }
    size_t used=atomic_load(&context->used);CHECK(length<=sizeof(context->bytes)-used);
    memcpy(context->bytes+used,bytes,length);atomic_store(&context->used,used+length);
}
/* 每一步输入都从零开始数：先清计数，发一次事件，等够时间，再逐字对。
 * expected 为空串表示「这一步不许产生任何字节」，等满 spin 再确认仍是零。 */
static void surface_spin(double duration);
static void expect_input(surface_input_t *input,const char *step,const char *expected) {
    const size_t n=strlen(expected);
    NSDate *until=[NSDate dateWithTimeIntervalSinceNow:2];
    while(atomic_load(&input->used)<n && [until timeIntervalSinceNow]>0)surface_spin(0.01);
    surface_spin(0.05);
    if(atomic_load(&input->used)!=n || memcmp(input->bytes,expected,n)) {
        fprintf(stderr,"FAIL input/%s: want %zu bytes, got %zu\n",step,n,atomic_load(&input->used));
        for(size_t i=0;i<atomic_load(&input->used);i++)fprintf(stderr," %02x",input->bytes[i]);
        fprintf(stderr,"\n");
    }
    CHECK(atomic_load(&input->used)==n && !memcmp(input->bytes,expected,n));
    CHECK(atomic_load(&surface_legacy_writes)==0 && atomic_load(&surface_effects)==0);
    atomic_store(&input->used,0);
}
static void surface_spin(double duration) {
    NSDate *until=[NSDate dateWithTimeIntervalSinceNow:duration];
    do {
        ghostty_app_tick(surface_app);
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    } while([until timeIntervalSinceNow]>0);
}
/* 抓自己进程的 Metal 视图，不依赖屏幕录制权限。颜色来自真实 PTY 的
 * truecolor SGR；恢复前后分别验，避免拿一张空帧或固定背景冒充渲染。 */
static void surface_pixels(NSView *view,bool final) {
    surface_spin(0.5);
    NSBitmapImageRep *rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];CHECK(rep);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    CGImageRef image=rep.CGImage;CHECK(image);
    size_t w=CGImageGetWidth(image),h=CGImageGetHeight(image);CHECK(w && h);
    uint8_t *rgba=calloc(w*h,4);CHECK(rgba);
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx=CGBitmapContextCreate(rgba,w,h,8,w*4,space,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CHECK(ctx);CGContextDrawImage(ctx,CGRectMake(0,0,w,h),image);
    size_t red=0,green=0,white=0;
    for(size_t i=0;i<w*h;i++) {
        unsigned r=rgba[4*i],g=rgba[4*i+1],b=rgba[4*i+2];
        red+=r>150 && g<80 && b<80;
        green+=g>150 && r<80 && b<80;
        white+=r>200 && g>200 && b>200;
    }
    printf("pixels/%s: %zux%zu red=%zu green=%zu white=%zu\n",final ? "live" : "import",w,h,red,green,white);
    CHECK(red>100 && white>10);
    CHECK(final ? green>100 : green==0);
    NSString *path=[NSString stringWithFormat:@"tmp/worker-surface/pixels-%s.png",final ? "live" : "import"];
    CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]);
    CGContextRelease(ctx);CGColorSpaceRelease(space);free(rgba);
}
static void append_u64(NSMutableData *data,uint64_t value) {
    uint8_t bytes[8];for(unsigned i=0;i<8;i++)bytes[i]=(uint8_t)(value>>(8*i));
    [data appendBytes:bytes length:sizeof(bytes)];
}
static void bind_checkpoint(ghostty_surface_config_s *opts,uint64_t generation,uint64_t sequence,const gt_surface_checkpoint_view_t *checkpoint) {
    NSMutableData *data=[NSMutableData data];
    const char domain[]="GHOSTERM-P0-CHECKPOINT-1";
    [data appendBytes:domain length:sizeof(domain)-1];
    append_u64(data,generation);append_u64(data,sequence);append_u64(data,MIB);
    append_u64(data,checkpoint->canonical_len);[data appendBytes:checkpoint->canonical length:checkpoint->canonical_len];
    append_u64(data,checkpoint->resources_len);[data appendBytes:checkpoint->resources length:checkpoint->resources_len];
    if(opts->p0_color_scheme_set) {
        const char scheme[]="GHOSTERM-P0-SCHEME-1";[data appendBytes:scheme length:sizeof(scheme)-1];
        append_u64(data,opts->p0_color_scheme_dark?1:0);
    }
    CHECK(data.length<=UINT32_MAX);
    CC_SHA256(data.bytes,(CC_LONG)data.length,opts->p0_checkpoint_digest);
    opts->p0_checkpoint_bound=true;opts->p0_checkpoint_generation=generation;opts->p0_checkpoint_sequence=sequence;
}
/* gui-race：恢复全程都有另一个客户端在抢同一个会话。在位的那一代不会被顶掉，
 * 抢的那一方始终只拿到 BUSY，也碰不到快照、输入闸门和增量队列。 */
typedef struct {
    gt_session_attachment_t *attachment;
    uint64_t generation;
    atomic_bool stop;
    atomic_uint_fast64_t attempts;
} surface_contender_t;
static void *surface_contend(void *arg) {
    surface_contender_t *c=arg;
    while(!atomic_load(&c->stop)) {
        uint64_t other=99;
        CHECK(gt__attachment_attach(c->attachment,&other)==GT_TASK_BUSY && !other);
        gt_attachment_checkpoint_t lease;
        CHECK(gt__attachment_take(c->attachment,c->generation+1,&lease)==GT_TASK_INVALID && !lease.result);
        CHECK(!gt__attachment_input_allowed(c->attachment,c->generation+1,0));
        gt_restore_event_t event;uint8_t bytes[64];
        CHECK(gt__attachment_next(c->attachment,c->generation+1,&event,bytes,sizeof(bytes))==GT_RESTORE_STALE);
        atomic_fetch_add(&c->attempts,1);
        const struct timespec pause={0,200000};nanosleep(&pause,NULL);
    }
    return NULL;
}
static uint64_t surface_applied(ghostty_surface_t surface,uint64_t generation,uint64_t expected) {
    uint64_t sequence=UINT64_MAX;
    CHECK(ghostty_p0_surface_applied_sequence_for(surface,generation,&sequence));
    CHECK(sequence==expected);return sequence;
}
static void equal_surface(ghostty_surface_t surface,const gt_surface_checkpoint_view_t *expected) {
    size_t n=0;CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && n==expected->canonical_len);
    uint8_t *bytes=malloc(n);CHECK(bytes);
    CHECK(ghostty_p0_surface_snapshot(surface,bytes,n,&n));
    CHECK(n==expected->canonical_len && !memcmp(bytes,expected->canonical,n));free(bytes);
    size_t m=0;CHECK(!ghostty_p0_surface_resources_snapshot(surface,NULL,0,&m) && m==expected->resources_len);
    bytes=malloc(m);CHECK(bytes);
    CHECK(ghostty_p0_surface_resources_snapshot(surface,bytes,m,&m));
    CHECK(m==expected->resources_len && !memcmp(bytes,expected->resources,m));free(bytes);
}
typedef struct {
    const gt_surface_probe_t *probe;
    ghostty_surface_t surface;
    atomic_bool done;
    uint64_t cut,applied;
    const gt_surface_checkpoint_view_t *final;
} surface_restore_t;
static uint64_t surface_apply(ghostty_surface_t surface,const gt_surface_probe_t *probe,
                              const gt_restore_event_t *event,const uint8_t *bytes,uint64_t cursor) {
    uint64_t generation=probe->generation;
    uint64_t before=surface_applied(surface,generation,cursor);
    CHECK(before<event->sequence); /* dequeued is not applied */
    if(event->kind==GT_STATE_OUTPUT)
        CHECK(ghostty_p0_surface_event_output_for(surface,generation,event->sequence,bytes,event->length));
    else {
        CHECK(event->kind==GT_STATE_RESIZE && event->length==sizeof(gt_state_size_t));
        gt_state_size_t size;memcpy(&size,bytes,sizeof(size));
        CHECK(ghostty_p0_surface_event_resize_for(surface,generation,event->sequence,size.cols,size.rows,probe->cell_width,probe->cell_height));
    }
    return surface_applied(surface,generation,event->sequence);
}
static void *surface_consume(void *opaque) {
    surface_restore_t *restore=opaque;
    const gt_surface_probe_t *probe=restore->probe;
    ghostty_surface_t surface=restore->surface;
    uint64_t generation=probe->generation;
    CHECK(![NSThread isMainThread]);
    if(!strcmp(probe->mode,"gui-growth") || !strcmp(probe->mode,"gui-revoked")) {
        CHECK(probe->freeze_budget(probe->budget_context));
        if(!strcmp(probe->mode,"gui-growth")) {
            /* Growth must come from memory the surface itself owns: scrollback
             * rows. An OSC title grew the shared font cache instead, which the
             * grid-ownership fix correctly moved off the per-surface budget. */
            static uint8_t grow[8192];for(size_t i=0;i<sizeof(grow);i++)grow[i]=(i%2)?'\n':'X';
            CHECK(!ghostty_p0_surface_event_output_for(surface,generation,probe->snapshot+1,grow,sizeof(grow)));
        }
        uint64_t sequence=99;size_t n=99;
        CHECK(!ghostty_p0_surface_applied_sequence_for(surface,generation,&sequence) && !sequence);
        CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && !n);
        CHECK(!probe->budget_healthy(probe->budget_context));
        CHECK(gt__attachment_detach(probe->attachment,generation)==GT_TASK_OK);
        atomic_store(&restore->done,true);return NULL;
    }
    const bool live=probe->authority_emit!=NULL;
    uint64_t applied=surface_applied(surface,generation,probe->snapshot);
    CHECK(!gt__attachment_input_allowed(probe->attachment,generation,applied));
    CHECK(gt__attachment_ready_snapshot(probe->attachment,generation,applied)==GT_RESTORE_OK);
    /* 权威在 READY(S) 之后才产生这个事件：它必须排在 L 之外。 */
    const uint64_t live_last=live ? probe->authority_emit(probe->authority_context) : 0;
    uint64_t cursor=probe->snapshot;
    const bool interrupt=!strcmp(probe->mode,"gui-interrupt");
    const bool slow=probe->authority_flood!=NULL;
    uint8_t bytes[16384];gt_restore_event_t event;
    for(;;) {
        gt_restore_rc_t rc=gt__attachment_next(probe->attachment,generation,&event,bytes,sizeof(bytes));
        if(rc==GT_RESTORE_WAIT)break;
        CHECK(rc==GT_RESTORE_OK && event.generation==generation && event.sequence==cursor+1);
        cursor=surface_apply(surface,probe,&event,bytes,cursor);
        if(interrupt)break;
        if(slow)break;
    }
    if(interrupt) {
        /* 客户端在增量还没放完时掉线：这一代立刻作废，剩下的事件永远不会
         * 送达，半恢复的屏幕也没有任何通向 LIVE 的路径。 */
        gt_attachment_restore_info_t info;
        CHECK(gt__attachment_restore_info(probe->attachment,generation,&info)==GT_RESTORE_OK);
        CHECK(info.phase==GT_RESTORE_DRAINING && info.events && info.dequeued==cursor && cursor<info.cut);
        CHECK(gt__attachment_detach(probe->attachment,generation)==GT_TASK_OK);
        CHECK(gt__attachment_restore_info(probe->attachment,generation,&info)==GT_RESTORE_OK);
        CHECK(info.phase==GT_RESTORE_DETACHED && !info.events && !info.bytes);
        CHECK(gt__attachment_next(probe->attachment,generation,&event,bytes,sizeof(bytes))==GT_RESTORE_STALE);
        uint64_t stale=99;
        CHECK(gt__attachment_live_begin(probe->attachment,generation,&stale)==GT_RESTORE_STALE && !stale);
        CHECK(gt__attachment_ready_live(probe->attachment,generation,cursor)==GT_RESTORE_STALE);
        CHECK(!gt__attachment_input_allowed(probe->attachment,generation,cursor));
        CHECK(!gt__attachment_current(probe->attachment,generation));
        restore->cut=cursor;atomic_store(&restore->done,true);return NULL;
    }
    if(slow) {
        /* 慢客户端：屏幕已经吃进 S 和第一个增量，但之后不再取事件，而权威还在
         * 跑。有界队列撑满时作废的是这一代，不是会话——权威照样继续推进，这一
         * 代则再也到不了 LIVE。 */
        const uint64_t produced=probe->authority_flood(probe->authority_context);
        CHECK(produced>cursor);
        gt_attachment_restore_info_t info;
        CHECK(gt__attachment_restore_info(probe->attachment,generation,&info)==GT_RESTORE_OK);
        CHECK(info.phase==GT_RESTORE_FAILED && info.failure==GT_RESTORE_LIMIT && !info.events && !info.bytes);
        CHECK(gt__attachment_next(probe->attachment,generation,&event,bytes,sizeof(bytes))==GT_RESTORE_LIMIT);
        uint64_t stale=99;
        CHECK(gt__attachment_live_begin(probe->attachment,generation,&stale)==GT_RESTORE_LIMIT && !stale);
        CHECK(gt__attachment_ready_live(probe->attachment,generation,cursor)==GT_RESTORE_LIMIT);
        CHECK(!gt__attachment_input_allowed(probe->attachment,generation,cursor));
        CHECK(!gt__attachment_current(probe->attachment,generation));
        restore->final=probe->authority_quiesce(probe->authority_context);
        restore->cut=cursor;restore->applied=cursor;atomic_store(&restore->done,true);return NULL;
    }
    if(live) {
        /* 权威没有停：它在排空期间又推了事件，但 L 停在 READY(S) 那一刻，
         * 不会跟着 last 往前跑，否则增量永远追不上活着的会话。 */
        gt_attachment_restore_info_t info;
        CHECK(gt__attachment_restore_info(probe->attachment,generation,&info)==GT_RESTORE_OK);
        CHECK(info.phase==GT_RESTORE_DRAINING && info.cut==cursor);
        CHECK(info.last>=live_last && info.last>info.cut && info.events==info.last-info.cut);
    }
    uint64_t cut=0;CHECK(gt__attachment_live_begin(probe->attachment,generation,&cut)==GT_RESTORE_OK && cut==cursor);
    CHECK(live ? cut<live_last : cut==probe->last);
    applied=surface_applied(surface,generation,cut);
    CHECK(!gt__attachment_input_allowed(probe->attachment,generation,applied));
    CHECK(gt__attachment_ready_live(probe->attachment,generation,applied)==GT_RESTORE_OK);
    CHECK(gt__attachment_input_allowed(probe->attachment,generation,applied));
    if(live) {
        /* L 之后的事件走的是同一条流，只是现在允许越过 L。排空一轮后才停权威，
         * 停完还要再排一轮：活着的程序什么时候写 PTY 不归恢复侧决定。 */
        uint64_t crossed=0;
        for(unsigned round=0;round<2;round++) {
            for(;;) {
                gt_restore_rc_t rc=gt__attachment_next(probe->attachment,generation,&event,bytes,sizeof(bytes));
                if(rc==GT_RESTORE_WAIT)break;
                CHECK(rc==GT_RESTORE_OK && event.sequence==cursor+1);
                cursor=surface_apply(surface,probe,&event,bytes,cursor);crossed++;
            }
            if(!round) {
                CHECK(cursor>=live_last);
                restore->final=probe->authority_quiesce(probe->authority_context);
            }
        }
        CHECK(crossed>=live_last-cut);
        gt_attachment_restore_info_t info;
        CHECK(gt__attachment_restore_info(probe->attachment,generation,&info)==GT_RESTORE_OK);
        CHECK(info.phase==GT_RESTORE_LIVE && info.last==cursor && info.dequeued==cursor && !info.events);
        CHECK(gt__attachment_input_allowed(probe->attachment,generation,cursor));
    }
    restore->cut=cut;restore->applied=cursor;atomic_store(&restore->done,true);return NULL;
}
/* Standalone engine barrier proof; does not enable scheme on process IPC. */
static void surface_scheme_barrier(ghostty_surface_t surface,uint64_t generation,uint64_t sequence,surface_input_t *input) {
    char directory[4096],path[8192];CHECK(getcwd(directory,sizeof(directory)));
    CHECK(snprintf(path,sizeof(path),"%s/tmp/scheme-barrier-%ld.conf",directory,(long)getpid())>0);
    FILE *file=fopen(path,"w");CHECK(file);CHECK(fputs("background = #123456\n",file)>=0);CHECK(!fclose(file));
    ghostty_config_t config=ghostty_config_new();CHECK(config);ghostty_config_load_file(config,path);ghostty_config_finalize(config);CHECK(!unlink(path));
    for(unsigned i=0;i<4;i++) {
        const bool dark=i!=1;uint64_t next=sequence+1;bool observed=false;uint32_t background=0;
        CHECK(!ghostty_p0_surface_scheme_begin_for(surface,generation+1,next,dark));
        CHECK(!ghostty_p0_surface_scheme_begin_for(surface,generation,next+1,dark));
        CHECK(ghostty_p0_surface_scheme_begin_for(surface,generation,next,dark));
        CHECK(!ghostty_p0_surface_scheme_begin_for(surface,generation,next,dark));
        surface_applied(surface,generation,sequence);
        CHECK(!ghostty_p0_surface_event_output_for(surface,generation,next,(const uint8_t *)"BAD",3));
        CHECK(!ghostty_p0_surface_event_resize_for(surface,generation,next,80,24,8,16));
        ghostty_surface_text(surface,"BLOCKED",7);surface_spin(0.03);
        CHECK(!atomic_load(&input->used) && !atomic_load(&input->rejected));
        surface_applied(surface,generation,sequence);
        CHECK(!ghostty_p0_surface_scheme_apply_for(surface,generation+1,next,config));
        CHECK(!ghostty_p0_surface_scheme_apply_for(surface,generation,next+1,config));
        CHECK(ghostty_p0_surface_scheme_apply_for(surface,generation,next,config));
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:3];uint64_t applied=0;
        do {CHECK(ghostty_p0_surface_applied_sequence_for(surface,generation,&applied));if(applied==next)break;CHECK(applied==sequence);surface_spin(0.01);} while([deadline timeIntervalSinceNow]>0);
        CHECK(applied==next);
        CHECK(!ghostty_p0_surface_scheme_apply_for(surface,generation,next,config));
        CHECK(ghostty_p0_surface_scheme_state_for(surface,generation,&observed,&background));
        CHECK(observed==dark && background==0x123456);
        sequence=next;
        CHECK(ghostty_p0_surface_event_output_for(surface,generation,sequence+1,(const uint8_t *)"\x1b[?996n",7));sequence++;
        surface_applied(surface,generation,sequence);
        CHECK(!atomic_load(&surface_legacy_writes) && !atomic_load(&input->used));
    }
    /* A bound replica must never apply a host-local reload outside the ordered stream. */
    ghostty_surface_update_config(surface,config);
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:3];uint64_t ignored=0;bool healthy=true;
    do {healthy=ghostty_p0_surface_applied_sequence_for(surface,generation,&ignored);if(!healthy)break;surface_spin(0.01);} while([deadline timeIntervalSinceNow]>0);
    CHECK(!healthy && !ignored);
    ghostty_config_free(config);
    puts("PASS full surface/scheme barrier: pending sequence/input/output held, actual IO theme and background applied, sole query owner");
}
void ghosterm_probe_surface_tick(double seconds) {surface_spin(seconds);}
void ghosterm_probe_surface_replay(const gt_surface_probe_t *probe) {
    uint64_t generation=probe->generation;
    const gt_surface_checkpoint_view_t *checkpoint=&probe->initial;
    CHECK([NSThread isMainThread]);
    __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
    const bool pixels=!strcmp(probe->mode,"pixels");
    NSWindow *window=nil;
    if(pixels) {
        window=[[NSWindow alloc] initWithContentRect:view.frame styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        window.contentView=view;[window orderFrontRegardless];
    }
    surface_input_t input={.probe=probe};
    ghostty_surface_config_s opts=ghostty_surface_config_new();
    opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void *)view;
    opts.scale_factor=1;opts.host=true;opts.host_write=surface_legacy_write;opts.host_resize=surface_resize_request;
    opts.p0_host_write_input=surface_input;opts.userdata=&input;
    opts.p0_snapshot=checkpoint->canonical;opts.p0_snapshot_len=checkpoint->canonical_len;
    opts.p0_resources_snapshot=checkpoint->resources;opts.p0_resources_snapshot_len=checkpoint->resources_len;
    opts.p0_image_storage_limit=MIB;opts.p0_image_storage_limit_set=true;
    bind_checkpoint(&opts,generation,probe->snapshot,checkpoint);
    if(!gt_surface_mode_gui_default(probe->mode)) {
        opts.p0_allocator=probe->general_allocator;opts.p0_page_allocator=probe->page_allocator;
        opts.p0_budget_userdata=probe->budget_context;opts.p0_budget_healthy=probe->budget_healthy;
        if(!strcmp(probe->mode,"gui-partial"))opts.p0_page_allocator=NULL;
    }
    atomic_store(&surface_legacy_writes,0);atomic_store(&surface_effects,0);
    const bool race=!strcmp(probe->mode,"gui-race");
    surface_contender_t contender={.attachment=probe->attachment,.generation=generation};pthread_t rival;
    if(race)CHECK(!pthread_create(&rival,NULL,surface_contend,&contender));
    ghostty_surface_t surface=ghostty_surface_new(surface_app,&opts);
    if(!strcmp(probe->mode,"gui-denied") || !strcmp(probe->mode,"gui-pages") || !strcmp(probe->mode,"gui-partial")) {
        CHECK(!surface);
        CHECK(!gt__attachment_input_allowed(probe->attachment,generation,probe->snapshot));
        CHECK(gt__attachment_detach(probe->attachment,generation)==GT_TASK_OK);
        printf("PASS full surface/budget rejection/%s\n",probe->mode);return;
    }
    CHECK(surface);
    equal_surface(surface,checkpoint);
    if(pixels) {ghostty_surface_draw(surface);surface_pixels(view,false);}
    if(!strcmp(probe->mode,"import-only")) {
        surface_spin(0.03);equal_surface(surface,checkpoint);ghostty_surface_free(surface);
        puts("PASS full surface/import control");return;
    }
    surface_restore_t restore={.probe=probe,.surface=surface};pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,surface_consume,&restore));
    /* 保留权威的用例，消费线程还要停掉 worker，给它更长的预算。 */
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:probe->authority_emit||probe->authority_flood ? 20 : 5];
    while(!atomic_load(&restore.done) && [end timeIntervalSinceNow]>0)surface_spin(0.01);
    CHECK(atomic_load(&restore.done));CHECK(!pthread_join(thread,NULL));
    uint64_t cut=restore.cut;
    const gt_surface_checkpoint_view_t *final=restore.final ? restore.final : &probe->final;
    if(!strcmp(probe->mode,"gui-growth") || !strcmp(probe->mode,"gui-revoked")) {
        ghostty_surface_free(surface);puts("PASS full surface/budget growth fault");return;
    }
    if(!strcmp(probe->mode,"gui-interrupt") || !strcmp(probe->mode,"gui-slow")) {
        size_t n=0;CHECK(!ghostty_p0_surface_snapshot(surface,NULL,0,&n) && n);
        uint8_t *partial=malloc(n);CHECK(partial);
        CHECK(ghostty_p0_surface_snapshot(surface,partial,n,&n));
        /* 停在半路，就不该正好等于权威最后的状态。 */
        CHECK(n!=final->canonical_len || memcmp(partial,final->canonical,n));
        free(partial);
        ghostty_surface_text(surface,"USER",4);surface_spin(0.2);
        CHECK(!atomic_load(&input.used) && atomic_load(&input.rejected)>0);
        CHECK(atomic_load(&surface_legacy_writes)==0 && atomic_load(&surface_effects)==0);
        ghostty_surface_free(surface);surface_spin(0.03);
        printf("PASS full surface/restore aborted/%s S=%llu applied=%llu\n",probe->mode,
               (unsigned long long)probe->snapshot,(unsigned long long)cut);
        return;
    }
    surface_spin(0.05);equal_surface(surface,final);surface_applied(surface,generation,restore.applied);
    CHECK(atomic_load(&surface_legacy_writes)==0 && atomic_load(&surface_effects)==0 && !atomic_load(&input.used));
    if(getenv("GHOSTERM_P0_SCHEME_BARRIER")) {
        surface_scheme_barrier(surface,generation,restore.applied,&input);
        ghostty_surface_free(surface);surface_spin(0.03);return;
    }
    if(!strcmp(probe->mode,"resources")) {
        uint8_t rgba[4];size_t n=0;uint32_t col=0,row=0;
        CHECK(ghostty_p0_surface_read_image(surface,7,rgba,sizeof(rgba),&n,&col,&row));
        const uint8_t red[]={255,0,0,255};CHECK(n==sizeof(red) && !memcmp(rgba,red,n));
    }
    if(pixels) {ghostty_surface_draw(surface);surface_pixels(view,true);}
    const char *expected=gt_surface_mode_bracketed_paste(probe->mode) ? "\x1b[200~USER\x1b[201~" : "USER";
    ghostty_surface_text(surface,"USER",4);
    NSDate *until=[NSDate dateWithTimeIntervalSinceNow:2];
    while(atomic_load(&input.used)<strlen(expected) && [until timeIntervalSinceNow]>0)surface_spin(0.01);
    surface_spin(0.05);
    CHECK(atomic_load(&input.used)==strlen(expected) && !memcmp(input.bytes,expected,strlen(expected)));
    CHECK(atomic_load(&surface_legacy_writes)==0 && atomic_load(&surface_effects)==0);
    if(!strcmp(probe->mode,"input")) {
        atomic_store(&input.used,0);
        /* 中文 IME：组合中的预编辑不是已提交的文本，一个字节都不许出去，否则
         * 用户每敲一个拼音字母程序就收到一次半成品。 */
        ghostty_surface_preedit(surface,"\xe4\xb8\xad",3);
        ghostty_input_key_s composing={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,
            .consumed_mods=GHOSTTY_MODS_NONE,.keycode=0,.text="\xe4\xb8\xad",.composing=true};
        ghostty_surface_key(surface,composing);
        expect_input(&input,"preedit","");
        /* 提交走按键而不是粘贴：IME 交回来的是整串已定稿文本，按原始 UTF-8
         * 出去，不加 bracketed paste 包裹。 */
        ghostty_surface_preedit(surface,NULL,0);
        ghostty_input_key_s commit={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,
            .consumed_mods=GHOSTTY_MODS_NONE,.keycode=0,.text="\xe4\xb8\xad\xe6\x96\x87",.composing=false};
        ghostty_surface_key(surface,commit);
        expect_input(&input,"commit","\xe4\xb8\xad\xe6\x96\x87");
        /* 鼠标：?1000h 与 ?1006h 是 detach 前权威记进快照的，副本按被恢复的
         * 状态编成 SGR，按下 M、抬起 m，坐标由像素位置换算而来。 */
        ghostty_surface_mouse_pos(surface,0.0,0.0,GHOSTTY_MODS_NONE);
        expect_input(&input,"mouse-move","");
        ghostty_surface_mouse_button(surface,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);
        expect_input(&input,"mouse-press","\x1b[<0;1;1M");
        ghostty_surface_mouse_button(surface,GHOSTTY_MOUSE_RELEASE,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);
        expect_input(&input,"mouse-release","\x1b[<0;1;1m");
        /* 焦点同样是用户输入而不是副本的宿主动作：?1004h 也在快照里，切焦点
         * 得按 stamped input 交回权威，而不是被副本就地丢掉。surface 构造出来
         * 默认就是聚焦的，所以先切走再切回；只有状态真变了才报，重复设同一个
         * 值一个字节都不许出去。 */
        ghostty_surface_set_focus(surface,false);
        expect_input(&input,"focus-lost","\x1b[O");
        ghostty_surface_set_focus(surface,false);
        expect_input(&input,"focus-repeat","");
        ghostty_surface_set_focus(surface,true);
        expect_input(&input,"focus-gain","\x1b[I");
    }
    ghostty_surface_draw(surface);surface_spin(0.03);equal_surface(surface,final);
    if(race) {
        atomic_store(&contender.stop,true);CHECK(!pthread_join(rival,NULL));
        CHECK(atomic_load(&contender.attempts)>16);
    }
    ghostty_surface_free(surface);surface_spin(0.03);
    [window orderOut:nil];
    printf("PASS full surface/restore/%s S=%llu L=%llu applied=%llu\n",probe->mode,
           (unsigned long long)probe->snapshot,(unsigned long long)cut,(unsigned long long)restore.applied);
}
#ifdef GHOSTERM_P0_PROCESS_CLIENT
#include "session-process-client.inc"
#endif
int main(int argc,char **argv) {@autoreleasepool {
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    bool process=false;
#ifdef GHOSTERM_P0_PROCESS_CLIENT
    process=argc>1 && !strcmp(argv[1],"--process-client");
#endif
    if(!process){CHECK(ghostty_init(0,NULL)==0);surface_config=ghostty_config_new();CHECK(surface_config);ghostty_config_finalize(surface_config);surface_app=surface_create_app(NULL);CHECK(surface_app);}
#ifdef GHOSTERM_P0_PROCESS_CLIENT
    int rc=argc>1 && !strcmp(argv[1],"--process-client") ? ghosterm_process_client(argc,argv) : ghosterm_worker_surface_fixture_main(argc,argv);
#else
    int rc=ghosterm_worker_surface_fixture_main(argc,argv);
#endif
    if(surface_app)ghostty_app_free(surface_app);ghostty_config_free(surface_config);return rc;
}}
