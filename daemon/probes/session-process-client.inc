/* Included only by the dedicated P0 GUI process build. Network bytes are read
 * on process_consume; main only constructs/draws and generates user input. */
#include "session-process-wire.h"
#include "session-process-build.h"
#include "gui-budget.h"
#include "session-config-owner.h"
#include "session-global-owner.h"
#include <sys/mman.h>
#include <stddef.h>
#include "session-summary-wire.h"
#include "session-command-wire.h"
#include "session-result-wire.h"
#include "session-history-wire.h"
#include "session-appkit-host.inc"
#include "session-main-stack.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct {
    pthread_mutex_t lock;
    const ghostty_p0_allocator_s *base;
    ghostty_p0_allocator_s allocator;
    size_t limit,live,bytes,peak_bytes,attempts,freed,denied;
    void *second_allocation;size_t reject_attempt;
} process_thread_args;
static uint8_t *process_args_alloc(void *context,size_t n,uint8_t alignment,uintptr_t ret) {
    process_thread_args *a=context;CHECK(!pthread_mutex_lock(&a->lock));a->attempts++;
    if(a->attempts==a->reject_attempt || a->live>=a->limit || n>SIZE_MAX-a->bytes){a->denied++;pthread_mutex_unlock(&a->lock);return NULL;}
    uint8_t *p=a->base->vtable->alloc(a->base->ctx,n,alignment,ret);
    if(p && a->attempts==2)a->second_allocation=p;
    if(p){a->live++;a->bytes+=n;if(a->bytes>a->peak_bytes)a->peak_bytes=a->bytes;}else a->denied++;
    CHECK(!pthread_mutex_unlock(&a->lock));return p;
}
static bool process_args_resize(void *c,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r) {(void)c;(void)p;(void)n;(void)a;(void)to;(void)r;return false;}
static uint8_t *process_args_remap(void *c,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r) {(void)c;(void)p;(void)n;(void)a;(void)to;(void)r;return NULL;}
static void process_args_free(void *context,uint8_t *p,size_t n,uint8_t alignment,uintptr_t ret) {
    process_thread_args *a=context;CHECK(!pthread_mutex_lock(&a->lock));CHECK(a->live && a->bytes>=n);
    a->base->vtable->free(a->base->ctx,p,n,alignment,ret);a->live--;a->bytes-=n;a->freed++;
    CHECK(!pthread_mutex_unlock(&a->lock));
}
static const ghostty_p0_allocator_vtable_s process_args_vtable={process_args_alloc,process_args_resize,process_args_remap,process_args_free};
static void process_args_open(process_thread_args *a,p0_gui_budget *budget,size_t limit) {
    memset(a,0,sizeof(*a));CHECK(!pthread_mutex_init(&a->lock,NULL));a->limit=limit;a->base=p0_gui_general(budget);
    a->allocator=(ghostty_p0_allocator_s){a,&process_args_vtable};
}
static size_t process_args_live(process_thread_args *a) {
    CHECK(!pthread_mutex_lock(&a->lock));size_t n=a->live;CHECK(!pthread_mutex_unlock(&a->lock));return n;
}
static size_t process_args_bytes(process_thread_args *a) {
    CHECK(!pthread_mutex_lock(&a->lock));size_t n=a->bytes;CHECK(!pthread_mutex_unlock(&a->lock));return n;
}
static void process_args_close(process_thread_args *a) {
    CHECK(!process_args_live(a) && !a->bytes && a->attempts==a->freed+a->denied);
    printf("METRIC process/thread-args attempts=%zu freed=%zu denied=%zu peak_requested_bytes=%zu signal_stack_bytes=%zu final_live=0\n",a->attempts,a->freed,a->denied,a->peak_bytes,ghostty_p0_surface_thread_signal_stack_size());
    CHECK(!pthread_mutex_destroy(&a->lock));
}
typedef struct {
    int fd;unsigned round,admission_surfaces;bool pointer,known_start_error,ime,startup_error,reply_error,apply_error,recovery_error,expect_error,has_result;gt_worker_result_t result;
    pw_io io;
    pthread_mutex_t metadata_lock;size_t metadata_live,metadata_peak,metadata_limit;
    p0_gui_pool *pool;
    p0_config_owner config_owner;size_t config_reserve;unsigned config_failures;
    p0_global_owner global_owner;size_t global_reserve,global_io_reserve;bool global_capacity_checked;
    atomic_uint global_io_started,global_io_finished;
    p0_gui_reservation *app_reservation;size_t app_reserve;
    process_thread_args app_args,warmup_args;
    p0_gui_reservation *warmup_reservation;size_t warmup_reserve;
    atomic_uint warmup_started,warmup_finished,warmup_joined;
    p0_gui_reservation *main_reservation,*io_reservation;size_t engine_stack,engine_thread_reserve,main_reserve,io_reserve;
    p0_gui_budget *budget;
    gt_program_status_store_t *summary;
    size_t summary_bytes;
    gt_command_status_t command;
    process_thread_args thread_args;
    ghostty_surface_t surface;
    uint64_t generation,snapshot,cursor,input_generation,input_sequence;
    uint32_t cell_width,cell_height;
    bool history_policy;hsw_state history_current,snapshot_history;atomic_uint history_display;void *history_window;unsigned history_ui_updates;
    bool search,native_ime,native_pointer,tmux;unsigned tui;
    void *scheme_view;unsigned scheme_draws;
    bool scheme,dark,requested_dark;uint64_t requested_sequence;unsigned scheme_events;atomic_bool scheme_request;
    gt_surface_checkpoint_view_t initial;
    atomic_bool snapshot_ready,surface_ready,live,drawn,done,ended,prepare_inflight,inflight_rejected;
    atomic_size_t input_length,rejected_inputs;
    uint8_t input[128];
} process_client;
static void process_history_receive(process_client *c,uint64_t sequence,bool snapshot,bool final) {
    pw_frame frame;CHECK(pw_receive(&c->io,&frame) && frame.kind==PW_HISTORY && frame.sequence==sequence && frame.length==8+HSW_BYTES);
    CHECK(pw_get64(frame.bytes)==c->generation);hsw_state next={0};CHECK(hsw_decode(frame.bytes+8,HSW_BYTES,&next));
    CHECK(next.bytes==MIB && next.lines==100 && next.state!=HSW_UNKNOWN);
    if(snapshot){CHECK(next.state==(c->round==1?HSW_COMPLETE:HSW_TRUNCATED));c->snapshot_history=next;}
    else {CHECK(next.state>=c->history_current.state);if(final)CHECK(next.state==c->history_current.state && next.state==HSW_TRUNCATED);}
    c->history_current=next;if(!snapshot)atomic_store(&c->history_display,next.state);
}
static void process_history_ui(process_client *c) {
    if(!c->history_policy)return;CHECK([NSThread isMainThread] && c->history_window);
    unsigned state=atomic_load(&c->history_display);CHECK(state<=HSW_TRUNCATED);
    NSWindow *window=(__bridge NSWindow *)c->history_window;
    NSString *value=state==HSW_UNKNOWN?@"History status unknown":state==HSW_TRUNCATED?@"History truncated":@"";
    if(![window.subtitle isEqualToString:value]){window.subtitle=value;c->history_ui_updates++;}
}
static void *process_summary_allocate(void *context,size_t n) {return p0_gui_buffer_new(context,n);}
static void process_summary_release(void *context,void *p,size_t n) {p0_gui_buffer_free(context,p,n);}
static bool process_decode_summary(process_client *c,const void *bytes,size_t n,gt_program_status_store_t **out) {
    gt_status_allocator_t allocator={c->budget,process_summary_allocate,process_summary_release};
    size_t before=p0_gui_pool_read(c->pool).wire_live;
    if(!psw_decode_with_allocator(bytes,n,&allocator,out))return false;
    size_t after=p0_gui_pool_read(c->pool).wire_live;CHECK(after>before);
    c->summary_bytes=after-before;
    return true;
}
static void process_input(void *context,uint64_t generation,uint64_t sequence,const uint8_t *bytes,size_t length) {
    process_client *c=context;
    if(atomic_load(&c->ended)){atomic_fetch_add(&c->rejected_inputs,1);return;}
    size_t used=atomic_load(&c->input_length);
    CHECK(atomic_load(&c->live) && length<=sizeof(c->input)-used);
    CHECK(!used || (c->input_generation==generation && c->input_sequence==sequence));
    c->input_generation=generation;c->input_sequence=sequence;memcpy(c->input+used,bytes,length);
    atomic_store(&c->input_length,used+length);
}
static void process_wait(atomic_bool *flag) {
    struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
    while(!atomic_load(flag)) {
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));CHECK(now.tv_sec-start.tv_sec<6);
    }
}
/* Diagnostic frame capture on main only. The protocol applied cursor remains
 * an IO-state barrier; these observations do not turn it into a draw fence. */
static void process_scheme_pixels(process_client *c,bool dark,const char *stage) {
    CHECK([NSThread isMainThread] && c->scheme && c->scheme_view);
    NSView *view=(__bridge NSView *)c->scheme_view;
    ghostty_surface_draw(c->surface);surface_spin(0.2);
    NSBitmapImageRep *rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];CHECK(rep);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    CGImageRef image=rep.CGImage;CHECK(image);
    size_t width=CGImageGetWidth(image),height=CGImageGetHeight(image);CHECK(width && height);
    uint8_t *rgba=calloc(width*height,4);CHECK(rgba);
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef scheme_context=CGBitmapContextCreate(rgba,width,height,8,width*4,space,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);CHECK(scheme_context);
    CGContextDrawImage(scheme_context,CGRectMake(0,0,width,height),image);
    size_t dark_pixels=0,light_pixels=0;
    for(size_t i=0;i<width*height;i++) {
        int r=rgba[4*i],g=rgba[4*i+1],b=rgba[4*i+2];
        dark_pixels+=abs(r-0x12)<=5 && abs(g-0x34)<=5 && abs(b-0x56)<=5;
        light_pixels+=abs(r-0xab)<=5 && abs(g-0xcd)<=5 && abs(b-0xef)<=5;
    }
    printf("pixels/process-scheme stage=%s frame=%u dark=%zu light=%zu total=%zu\n",stage,c->scheme_draws,dark_pixels,light_pixels,width*height);
    CHECK((dark?dark_pixels:light_pixels)>width*height/2);
    CHECK((dark?light_pixels:dark_pixels)<64);
    NSString *path=[NSString stringWithFormat:@"tmp/worker-surface/process-scheme-%u-%s.png",c->scheme_draws,stage];
    CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]);
    c->scheme_draws++;CGContextRelease(scheme_context);CGColorSpaceRelease(space);free(rgba);
}
static void process_scheme_main(process_client *c) {
    CHECK([NSThread isMainThread]);
    if(!atomic_exchange(&c->scheme_request,false))return;
    const uint64_t sequence=c->requested_sequence;const bool dark=c->requested_dark;
    char directory[4096],path[8192];CHECK(getcwd(directory,sizeof(directory)));
    CHECK(snprintf(path,sizeof(path),"%s/tmp/process-scheme-%ld.conf",directory,(long)getpid())>0);
    FILE *file=fopen(path,"w");CHECK(file);
    CHECK(fprintf(file,"background = #%s\nforeground = #ffffff\ncursor-style-blink = false\n",dark?"123456":"abcdef")>0);CHECK(!fclose(file));
    p0_config_owner config={0};size_t committed=p0_gui_pool_read(c->pool).committed;
    CHECK(p0_config_open(c->pool,c->config_reserve,&config));
    CHECK(!p0_gui_reserved_idle(config.reservation));
    CHECK(p0_config_load(&config,path) && p0_config_finish(&config));CHECK(!unlink(path));
    CHECK(ghostty_p0_surface_scheme_apply_for(c->surface,c->generation,sequence,config.value));
    CHECK(p0_config_close(&config));CHECK(p0_gui_pool_read(c->pool).committed==committed);
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:3];uint64_t applied=0;
    do {CHECK(ghostty_p0_surface_applied_sequence_for(c->surface,c->generation,&applied));if(applied>=sequence)break;surface_spin(0.01);} while([deadline timeIntervalSinceNow]>0);
    CHECK(applied>=sequence);
    process_scheme_pixels(c,dark,"event");
}
static void process_scheme_apply(process_client *c,const pw_frame *frame) {
    CHECK(c->scheme && frame->length==8 && pw_get64(frame->bytes)<=1);
    bool dark=(bool)pw_get64(frame->bytes);
    CHECK(c->scheme_events<3 && dark==(c->scheme_events==1));
    CHECK(ghostty_p0_surface_scheme_begin_for(c->surface,c->generation,frame->sequence,dark));
    surface_applied(c->surface,c->generation,c->cursor);
    CHECK(!atomic_load(&c->scheme_request));c->requested_sequence=frame->sequence;c->requested_dark=dark;
    atomic_store(&c->scheme_request,true);
    struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));uint64_t applied=0;
    for(;;) {
        CHECK(ghostty_p0_surface_applied_sequence_for(c->surface,c->generation,&applied));
        if(applied==frame->sequence)break;CHECK(applied==c->cursor);
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);
    }
    bool observed=false;uint32_t background=0;
    CHECK(ghostty_p0_surface_scheme_state_for(c->surface,c->generation,&observed,&background));
    CHECK(observed==dark && background==(dark?0x123456u:0xabcdefu));
    c->dark=dark;c->scheme_events++;
}
static gt_surface_checkpoint_view_t process_checkpoint(process_client *c,const pw_frame *frame) {
    CHECK(frame->length==(c->scheme?224u:216u) && pw_get64(frame->bytes)==c->generation);
    if(c->scheme) {
        uint64_t dark=pw_get64(frame->bytes+216);CHECK(dark<=1);
        if(frame->kind==PW_EXPECT)CHECK(dark==(uint64_t)c->dark && c->scheme_events==3);
        else {CHECK(dark==1);c->dark=(bool)dark;}
    }
    CHECK(!memcmp(frame->bytes+104,process_engine_build,sizeof(process_engine_build)));
    uint64_t a=pw_get64(frame->bytes+8),b=pw_get64(frame->bytes+16);
    CHECK(a && b && a<=PW_LIMIT && b<=PW_LIMIT-a);
    uint64_t summary_length=pw_get64(frame->bytes+136);
    CHECK(summary_length>=8 && summary_length<=PSW_MAX && summary_length<=PW_LIMIT-a-b);
    uint64_t command_length=pw_get64(frame->bytes+176);
    CHECK(command_length>=CSW_HEADER && command_length<=CSW_MAX && command_length<=PW_LIMIT-a-b-summary_length);
    CHECK(pw_get64(frame->bytes+24)==10 && pw_get64(frame->bytes+32)==20);
    uint8_t *canonical=p0_gui_buffer_new(c->budget,(size_t)a),*resources=p0_gui_buffer_new(c->budget,(size_t)b);CHECK(canonical && resources);
    CHECK(pw_receive_blob(&c->io,PW_CANONICAL,canonical,(size_t)a));
    CHECK(pw_receive_blob(&c->io,PW_RESOURCES,resources,(size_t)b));
    uint8_t canonical_digest[CC_SHA256_DIGEST_LENGTH],resources_digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(canonical,(CC_LONG)a,canonical_digest);
    CC_SHA256(resources,(CC_LONG)b,resources_digest);
    CHECK(!memcmp(canonical_digest,frame->bytes+40,sizeof(canonical_digest)));
    CHECK(!memcmp(resources_digest,frame->bytes+72,sizeof(resources_digest)));

    uint8_t *summary_wire=p0_gui_buffer_new(c->budget,(size_t)summary_length);CHECK(summary_wire);
    CHECK(pw_receive_blob(&c->io,PW_SUMMARY,summary_wire,(size_t)summary_length));
    uint8_t summary_digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(summary_wire,(CC_LONG)summary_length,summary_digest);
    CHECK(!memcmp(summary_digest,frame->bytes+144,sizeof(summary_digest)));
    gt_program_status_store_t *summary=NULL;CHECK(process_decode_summary(c,summary_wire,(size_t)summary_length,&summary));
    p0_gui_buffer_free(c->budget,summary_wire,(size_t)summary_length);
    unsigned phase=frame->kind==PW_EXPECT ? 2u : c->round==1 ? 0u : 1u;
    if(c->tmux || c->tui)CHECK(!gt__program_status_count(summary));
    else {
    gt_program_status_record_t record;CHECK(gt__program_status_count(summary)==1 && gt__program_status_at(summary,0,&record));
    CHECK(!strcmp(record.id,"build") && !strcmp(record.app,"ghosterm") && !strcmp(record.title,""));
    CHECK(record.state==(phase==0?1u:phase==1?3u:2u) && record.kind==(phase==1?1u:0u));
    CHECK(record.progress==(phase==0?25:-1) && !strcmp(record.message,phase==0?"Working":phase==1?"Approve?":"Done"));
    }
    uint8_t *command_wire=p0_gui_buffer_new(c->budget,(size_t)command_length);CHECK(command_wire);
    CHECK(pw_receive_blob(&c->io,PW_COMMAND,command_wire,(size_t)command_length));
    uint8_t command_digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(command_wire,(CC_LONG)command_length,command_digest);
    CHECK(!memcmp(command_digest,frame->bytes+184,sizeof(command_digest)));
    CHECK(csw_decode(command_wire,(size_t)command_length,&c->command));
    p0_gui_buffer_free(c->budget,command_wire,(size_t)command_length);
    if(c->tmux || c->tui) {
        CHECK(!c->command.command[0] && !c->command.error[0] && !c->command.running && !c->command.has_exit_code && !c->command.exit_code);
        CHECK(!c->command.has_duration && !c->command.duration_ns && !c->command.has_start_time);
    } else {
    CHECK(!strcmp(c->command.command,phase==2?"next command":"ghosterm test"));
    CHECK(!strcmp(c->command.error,phase==0?"":phase==1?"detached failure":"unknown exit failure"));
    CHECK(c->command.running==(phase==0) && c->command.has_exit_code==(phase==1));
    if(phase==1)CHECK(c->command.exit_code==7);
    CHECK(c->command.has_duration==(phase!=0) && !c->command.has_start_time);
    }
    if(c->history_policy)process_history_receive(c,frame->sequence,frame->kind!=PW_EXPECT,frame->kind==PW_EXPECT);
    gt__program_status_close(c->summary);c->summary=summary;
    p0_gui_usage usage=p0_gui_pool_read(c->pool);CHECK(usage.wire_live>=a+b);
    return (gt_surface_checkpoint_view_t){canonical,resources,(size_t)a,(size_t)b};
}
static void process_free_checkpoint(process_client *owner,gt_surface_checkpoint_view_t *c) {
    p0_gui_buffer_free(owner->budget,(void *)c->canonical,c->canonical_len);
    p0_gui_buffer_free(owner->budget,(void *)c->resources,c->resources_len);memset(c,0,sizeof(*c));
}
static void process_summary_update(process_client *c,uint64_t sequence) {
    pw_frame frame;CHECK(pw_receive(&c->io,&frame));
    CHECK(frame.kind==PW_SUMMARY_UPDATE && frame.sequence==sequence && frame.length==88);
    CHECK(pw_get64(frame.bytes)==c->generation);
    uint64_t n=pw_get64(frame.bytes+8);CHECK(n>=8 && n<=PSW_MAX);
    uint8_t *bytes=p0_gui_buffer_new(c->budget,(size_t)n);CHECK(bytes);
    CHECK(pw_receive_blob(&c->io,PW_SUMMARY,bytes,(size_t)n));
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes,(CC_LONG)n,digest);
    CHECK(!memcmp(digest,frame.bytes+16,sizeof(digest)));
    gt_program_status_store_t *summary=NULL;CHECK(process_decode_summary(c,bytes,(size_t)n,&summary));
    p0_gui_buffer_free(c->budget,bytes,(size_t)n);
    uint64_t command_length=pw_get64(frame.bytes+48);CHECK(command_length>=CSW_HEADER && command_length<=CSW_MAX);
    uint8_t *command_wire=p0_gui_buffer_new(c->budget,(size_t)command_length);CHECK(command_wire);
    CHECK(pw_receive_blob(&c->io,PW_COMMAND,command_wire,(size_t)command_length));
    uint8_t command_digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(command_wire,(CC_LONG)command_length,command_digest);
    CHECK(!memcmp(command_digest,frame.bytes+56,sizeof(command_digest)));
    gt_command_status_t next_command;CHECK(csw_decode(command_wire,(size_t)command_length,&next_command));
    p0_gui_buffer_free(c->budget,command_wire,(size_t)command_length);
    c->command=next_command;
    gt__program_status_close(c->summary);c->summary=summary;
}
static void process_inflight(process_client *c,const pw_frame *request) {
    CHECK(!c->has_result && request->sequence==c->cursor && !request->length);
    atomic_store(&c->input_length,0);atomic_store(&c->prepare_inflight,true);
    struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
    while(!atomic_load(&c->input_length)) {
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));CHECK(now.tv_sec-start.tv_sec<5);
    }
    size_t length=atomic_load(&c->input_length);CHECK(c->tui ? length>0 && length<=sizeof(c->input) : length==20);
    CHECK(c->input_generation==c->generation && c->input_sequence==c->cursor);
    uint8_t bytes[8+sizeof(c->input)];pw_put64(bytes,c->input_generation);memcpy(bytes+8,c->input,length);
    CHECK(pw_send(&c->io,PW_INPUT,request->sequence,bytes,length+8));pw_frame reply;CHECK(pw_receive(&c->io,&reply));
    CHECK(reply.kind==PW_INPUT_REJECTED && reply.sequence==request->sequence && reply.length==16);
    CHECK(pw_get64(reply.bytes)==c->generation && pw_get64(reply.bytes+8)==(((uint64_t)GT_ERR_CANCELLED<<32)|PW_INPUT_ENDED));
    CHECK(!atomic_load(&c->ended));atomic_store(&c->inflight_rejected,true);
}
static void process_result(process_client *c,const pw_frame *frame) {
    CHECK(!c->has_result && frame->sequence==c->cursor);uint64_t generation=0;
    CHECK(rsw_decode(frame->bytes,frame->length,&generation,&c->result));
    CHECK(generation==c->generation && c->result.has_sequence && c->result.sequence==c->cursor);
    CHECK(c->result.kind==(c->expect_error?GT_WORKER_END_ERROR:GT_WORKER_END_STOPPED));
    CHECK(c->result.status==(c->expect_error?GT_ERR_IO:GT_OK) && c->result.stage==(c->reply_error?GT_WORKER_STAGE_REPLY:c->apply_error?GT_WORKER_STAGE_APPLY:c->expect_error?GT_WORKER_STAGE_READ:GT_WORKER_STAGE_NONE));
    CHECK(!c->result.has_exit_code && c->result.termination_status==GT_ERR_NOT_FOUND);
    c->has_result=true;atomic_store(&c->ended,true);
    puts("PASS process/GUI received authoritative terminal result before EXPECT");
}
static void process_apply(process_client *c,const pw_frame *frame) {
    CHECK(!c->has_result);
    CHECK(frame->sequence==c->cursor+1);
    if(frame->kind==PW_OUTPUT || frame->kind==PW_OUTPUT_SUMMARY) {
        CHECK(frame->length);
        CHECK(ghostty_p0_surface_event_output_for(c->surface,c->generation,frame->sequence,frame->bytes,frame->length));
    } else if(frame->kind==PW_SCHEME)process_scheme_apply(c,frame);
    else {
        CHECK(frame->kind==PW_RESIZE && frame->length==32);
        uint64_t cols=pw_get64(frame->bytes),rows=pw_get64(frame->bytes+8);
        CHECK(cols && rows && cols<=UINT16_MAX && rows<=UINT16_MAX);
        CHECK(pw_get64(frame->bytes+16)==cols*c->cell_width && pw_get64(frame->bytes+24)==rows*c->cell_height);
        CHECK(ghostty_p0_surface_event_resize_for(c->surface,c->generation,frame->sequence,(uint16_t)cols,(uint16_t)rows,c->cell_width,c->cell_height));
    }
    if(frame->kind==PW_OUTPUT_SUMMARY)process_summary_update(c,frame->sequence);
    c->cursor=surface_applied(c->surface,c->generation,frame->sequence);
    if(c->history_policy)process_history_receive(c,frame->sequence,false,false);
}
static void *process_consume(void *context) {
    process_client *c=context;CHECK(![NSThread isMainThread]);pw_frame frame;
    size_t actual=pthread_get_stacksize_np(pthread_self());
    CHECK(actual>=MIB && actual<=c->io_reserve-(size_t)sysconf(_SC_PAGESIZE));
    CHECK(c->main_reservation && c->io_reservation && p0_gui_pool_read(c->pool).reservations==7);
    CHECK(pw_receive(&c->io,&frame));
    if(c->startup_error) {
        CHECK(frame.kind==PW_RESULT && !frame.sequence);uint64_t generation=0;
        CHECK(rsw_decode(frame.bytes,frame.length,&generation,&c->result) && generation);
        CHECK(c->result.kind==GT_WORKER_END_ERROR && c->result.stage==(c->known_start_error?GT_WORKER_STAGE_BACKEND_OPEN:GT_WORKER_STAGE_STATE_OPEN) && c->result.status==GT_ERR_IO && c->result.has_sequence==c->known_start_error && !c->result.sequence);
        c->has_result=true;atomic_store(&c->ended,true);
        CHECK(pw_send(&c->io,PW_FINISHED,0,NULL,0));
        CHECK(pw_receive(&c->io,&frame) && frame.kind==PW_FINISHED_ACK && !frame.sequence && !frame.length);
        atomic_store(&c->done,true);return NULL;
    }
    CHECK(frame.kind==PW_SNAPSHOT && frame.length==(c->scheme?224u:216u));
    c->generation=pw_get64(frame.bytes);CHECK(c->generation && frame.sequence);
    c->snapshot=c->cursor=frame.sequence;c->cell_width=10;c->cell_height=20;
    CHECK(p0_gui_admit(c->pool,&c->budget)==P0_GUI_OK);
    c->initial=process_checkpoint(c,&frame);
    atomic_store(&c->snapshot_ready,true);process_wait(&c->surface_ready);
    CHECK(c->admission_surfaces==2);
    surface_applied(c->surface,c->generation,c->snapshot);
    CHECK(pw_send(&c->io,PW_READY_S,c->snapshot,NULL,0));
    for(;;) {
        CHECK(pw_receive(&c->io,&frame));
        if(frame.kind==PW_LIVE_BEGIN)break;
        process_apply(c,&frame);
    }
    CHECK(!frame.length && frame.sequence==c->cursor);
    const uint64_t live_cut=c->cursor;
    CHECK(pw_send(&c->io,PW_READY_L,live_cut,NULL,0));
    for(;;) {
        CHECK(pw_receive(&c->io,&frame));
        if(frame.kind==PW_LIVE_OK)break;
        process_apply(c,&frame);
    }
    CHECK(frame.sequence==live_cut && !frame.length);
    atomic_store(&c->live,true);process_wait(&c->drawn);
    CHECK(pw_send(&c->io,PW_CLIENT_LIVE,c->cursor,NULL,0));
    if(c->round==1) {
        /* A live, rendered GUI, idle on its I/O thread until real SIGKILL. */
        CHECK(pw_receive(&c->io,&frame));CHECK(false);return NULL;
    }
    if(!c->recovery_error) {
        struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
        while(atomic_load(&c->input_length)<(c->tui?1u:c->pointer?40u:c->native_ime?23u:c->ime?22u:16u)) {
            const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
            CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));CHECK(now.tv_sec-start.tv_sec<5);
        }
        size_t n=atomic_load(&c->input_length);CHECK(c->tui ? n>0 && n<=sizeof(c->input) : n==(c->pointer?40u:c->native_ime?23u:c->ime?22u:16u));uint8_t bytes[8+sizeof(c->input)];
        CHECK(c->input_generation==c->generation && c->input_sequence==c->cursor);
        pw_put64(bytes,c->input_generation);memcpy(bytes+8,c->input,n);
        CHECK(pw_send(&c->io,PW_INPUT,c->input_sequence,bytes,n+8));
        CHECK(pw_receive(&c->io,&frame) && frame.kind==PW_INPUT_OK && frame.sequence==c->cursor && !frame.length);
    }
    for(;;) {
        CHECK(pw_receive(&c->io,&frame));if(frame.kind==PW_EXPECT)break;
        if(frame.kind==PW_PRE_END_INPUT)process_inflight(c,&frame);
        else if(frame.kind==PW_RESULT)process_result(c,&frame);else process_apply(c,&frame);
    }
    CHECK(c->has_result && atomic_load(&c->ended) && atomic_load(&c->inflight_rejected) && frame.sequence==c->cursor && c->cursor>c->snapshot);
    if(c->tmux || c->tui) {
        CHECK(!gt__program_status_count(c->summary));
        CHECK(!c->command.command[0] && !c->command.error[0] && !c->command.running && !c->command.has_exit_code && !c->command.has_duration);
    } else {
    gt_program_status_record_t live_record;
    CHECK(gt__program_status_count(c->summary)==1 && gt__program_status_at(c->summary,0,&live_record));
    CHECK(live_record.state==2 && live_record.kind==0 && live_record.progress==-1 && !strcmp(live_record.message,"Done"));
    CHECK(!c->command.running && !c->command.has_exit_code && c->command.has_duration);
    CHECK(!strcmp(c->command.command,"next command") && !strcmp(c->command.error,"unknown exit failure"));
    puts("PASS process/live command and program summary applied before final EXPECT import");
    }
    gt_surface_checkpoint_view_t final=process_checkpoint(c,&frame);equal_surface(c->surface,&final);
    process_free_checkpoint(c,&final);
    if(c->scheme)puts("PASS process/scheme: frozen dark metadata, light/dark catchup and LIVE light applied before cursor publication, full canonical match");
    CHECK(!atomic_load(&surface_legacy_writes) && !atomic_load(&surface_effects));
    CHECK(pw_send(&c->io,PW_FINISHED,c->cursor,NULL,0));
    CHECK(pw_receive(&c->io,&frame) && frame.kind==PW_FINISHED_ACK && frame.sequence==c->cursor && !frame.length);
    atomic_store(&c->done,true);return NULL;
}
static void process_tui_pixels(NSView *view,unsigned tui,unsigned round) {
    surface_spin(.3);NSBitmapImageRep *rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];CHECK(rep);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];CGImageRef image=rep.CGImage;CHECK(image);
    size_t w=CGImageGetWidth(image),h=CGImageGetHeight(image);CHECK(w && h);
    uint8_t *rgba=calloc(w*h,4);CHECK(rgba);CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef tui_context=CGBitmapContextCreate(rgba,w,h,8,w*4,space,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);CHECK(tui_context);
    CGContextDrawImage(tui_context,CGRectMake(0,0,w,h),image);size_t white=0;
    for(size_t i=0;i<w*h;i++)white+=rgba[4*i]>200 && rgba[4*i+1]>200 && rgba[4*i+2]>200;
    CHECK(white>100);printf("pixels/TUI-%u round=%u white=%zu\n",tui,round,white);
    NSString *path=[NSString stringWithFormat:@"tmp/worker-surface/process-tui-%u-%u.png",tui,round];
    CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]);
    CGContextRelease(tui_context);CGColorSpaceRelease(space);free(rgba);
}
static void process_final_pixels(NSView *view) {
    surface_spin(0.5);
    NSBitmapImageRep *rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];CHECK(rep);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];CGImageRef image=rep.CGImage;CHECK(image);
    size_t width=CGImageGetWidth(image),height=CGImageGetHeight(image);CHECK(width && height);
    uint8_t *rgba=calloc(width*height,4);CHECK(rgba);CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx=CGBitmapContextCreate(rgba,width,height,8,width*4,space,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CHECK(ctx);CGContextDrawImage(ctx,CGRectMake(0,0,width,height),image);
    size_t blue=0;
    for(size_t i=0;i<width*height;i++)blue+=rgba[4*i+2]>150 && rgba[4*i]<80 && rgba[4*i+1]<80;
    printf("pixels/process-final: blue=%zu\n",blue);CHECK(blue>100);
    CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
           writeToFile:@"tmp/worker-surface/process-final.png" atomically:YES]);
    CGContextRelease(ctx);CGColorSpaceRelease(space);free(rgba);
}
static void process_engine_threads(process_client *c,p0_gui_budget *budget,ghostty_surface_t surface,const void *args_allocator) {
    CHECK(p0_gui_thread_capacity(budget)==c->engine_thread_reserve && c->engine_thread_reserve==3*(c->engine_stack+2*(size_t)sysconf(_SC_PAGESIZE)));
    uintptr_t handles[2]={0,0},bad=1;
    CHECK(!ghostty_p0_surface_thread_args_allocator(NULL,1,&bad) && !bad);
    bad=1;CHECK(!ghostty_p0_surface_thread_args_allocator(surface,0,&bad) && !bad);
    CHECK(!ghostty_p0_surface_thread_args_allocator(surface,1,NULL));
    CHECK(!ghostty_p0_surface_thread_handle(NULL,1,&bad) && !bad);
    bad=1;CHECK(!ghostty_p0_surface_thread_handle(surface,3,&bad) && !bad);
    CHECK(!ghostty_p0_surface_thread_handle(surface,1,NULL));
    for(unsigned role=1;role<=2;role++) {
        uintptr_t allocator=0;CHECK(ghostty_p0_surface_thread_args_allocator(surface,role,&allocator) && allocator==(uintptr_t)args_allocator);
        CHECK(ghostty_p0_surface_thread_handle(surface,role,&handles[role-1]) && handles[role-1]);
        pthread_t thread=(pthread_t)handles[role-1];CHECK(!pthread_equal(thread,pthread_self()));
        size_t stack=pthread_get_stacksize_np(thread);uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(thread);
        CHECK(stack>=c->engine_stack && stack<(c->engine_stack+2*(size_t)sysconf(_SC_PAGESIZE)) && top>stack);
        mach_vm_address_t guard=top-stack-1,address=guard;mach_vm_size_t size=0;
        vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
        CHECK(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
        CHECK(address<=guard && address+size>guard && info.protection==VM_PROT_NONE && size>=(size_t)sysconf(_SC_PAGESIZE) && size<=(c->engine_stack+2*(size_t)sysconf(_SC_PAGESIZE))-stack);
        if(object)CHECK(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
        printf("METRIC process/engine-thread role=%u stack_requested_bytes=%zu stack_reported_bytes=%zu guard_bytes=%llu reserved_bytes=%zu\n",role,c->engine_stack,stack,(unsigned long long)size,(c->engine_stack+2*(size_t)sysconf(_SC_PAGESIZE)));
    }
    CHECK(handles[0]!=handles[1]);
}
/* Two real surfaces share the process pool. The third import must be refused
 * before constructing an engine; after deferred frees its slot is reusable. */
static void process_admission_probe(process_client *c,ghostty_surface_config_s opts) {
    {
        __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
        ghostty_surface_config_s incomplete=opts;incomplete.platform.macos.nsview=(__bridge void *)view;incomplete.p0_allocator=NULL;incomplete.p0_page_allocator=NULL;
        incomplete.p0_budget_userdata=NULL;incomplete.p0_budget_healthy=NULL;
        CHECK(!ghostty_surface_new(surface_app,&incomplete));
        CHECK(process_args_live(&c->thread_args)==2);
    }
    // Omitted optional frame allocator must use the host general allocator.
    {
        p0_gui_budget *fallback=NULL;CHECK(p0_gui_admit(c->pool,&fallback)==P0_GUI_OK);
        __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
        ghostty_surface_config_s inherited=opts;inherited.platform.macos.nsview=(__bridge void *)view;
        inherited.p0_allocator=p0_gui_general(fallback);inherited.p0_page_allocator=p0_gui_pages(fallback);
        inherited.p0_budget_userdata=fallback;inherited.p0_thread_allocator=NULL;
        ghostty_surface_t imported=ghostty_surface_new(surface_app,&inherited);CHECK(imported);
        process_engine_threads(c,fallback,imported,p0_gui_general(fallback));equal_surface(imported,&c->initial);
        ghostty_surface_free(imported);
        for(unsigned i=0;i<200 && !p0_gui_idle(fallback);i++)surface_spin(.01);
        CHECK(p0_gui_idle(fallback) && p0_gui_release(fallback)==P0_GUI_OK);
    }
    // Force real allocator denials before the first and second pthread starts.
    for(size_t allowed=0;allowed<2;allowed++) {
        p0_gui_budget *failed=NULL;CHECK(p0_gui_admit(c->pool,&failed)==P0_GUI_OK);
        process_thread_args frames;process_args_open(&frames,failed,allowed);
        __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
        ghostty_surface_config_s bad=opts;bad.platform.macos.nsview=(__bridge void *)view;
        bad.p0_allocator=p0_gui_general(failed);bad.p0_page_allocator=p0_gui_pages(failed);
        bad.p0_budget_userdata=failed;bad.p0_thread_allocator=&frames.allocator;
        CHECK(!ghostty_surface_new(surface_app,&bad));
        CHECK(!process_args_live(&frames) && frames.denied==1 && frames.freed==allowed && frames.attempts==allowed+1);
        for(unsigned i=0;i<200 && !p0_gui_idle(failed);i++)surface_spin(.01);
        CHECK(p0_gui_idle(failed) && p0_gui_healthy(failed) && p0_gui_healthy(c->budget));
        process_args_close(&frames);CHECK(p0_gui_release(failed)==P0_GUI_OK);
        CHECK(process_args_live(&c->thread_args)==2);
    }
    puts("PASS process/thread-args: first and second entry-frame allocator denial, joined rollback and incumbent isolation");
    p0_gui_usage before=p0_gui_pool_read(c->pool);
    CHECK(before.surfaces==1 && before.committed==160*MIB+c->main_reserve+c->io_reserve+c->metadata_limit+c->app_reserve+c->warmup_reserve+c->engine_thread_reserve+c->config_reserve+c->global_reserve+c->global_io_reserve && before.wire_live>0);
    for(unsigned attempt=0;attempt<2;attempt++) {
        p0_gui_budget *sibling=NULL;CHECK(p0_gui_admit(c->pool,&sibling)==P0_GUI_OK && sibling);
        __attribute__((objc_precise_lifetime)) NSView *view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
        opts.platform.macos.nsview=(__bridge void *)view;
        opts.p0_allocator=p0_gui_general(sibling);opts.p0_page_allocator=p0_gui_pages(sibling);
        opts.p0_budget_userdata=sibling;
        process_thread_args frames;process_args_open(&frames,sibling,3);opts.p0_thread_allocator=&frames.allocator;
        ghostty_surface_t surface=ghostty_surface_new(surface_app,&opts);CHECK(surface);
        process_engine_threads(c,sibling,surface,&frames.allocator);
        CHECK(process_args_live(&frames)==2 && frames.attempts==2);
        CHECK(process_args_bytes(&frames)>=2*ghostty_p0_surface_thread_signal_stack_size());
        equal_surface(surface,&c->initial);CHECK(!p0_gui_idle(sibling));c->admission_surfaces++;
        p0_gui_budget *denied=(void *)1;
        CHECK(p0_gui_admit(c->pool,&denied)==P0_GUI_LIMIT && !denied);
        p0_gui_usage full=p0_gui_pool_read(c->pool);CHECK(full.surfaces==2 && full.committed==320*MIB+c->main_reserve+c->io_reserve+c->metadata_limit+c->app_reserve+c->warmup_reserve+2*c->engine_thread_reserve+c->config_reserve+c->global_reserve+c->global_io_reserve);
        CHECK(p0_gui_pool_close(c->pool)==P0_GUI_BUSY);
        CHECK(p0_gui_healthy(c->budget) && p0_gui_healthy(sibling));
        equal_surface(c->surface,&c->initial);
        if(!attempt) {
            CHECK(!p0_gui_buffer_new(sibling,32*MIB+1));
            CHECK(!p0_gui_healthy(sibling) && p0_gui_healthy(c->budget));
            uint64_t sequence=99;
            CHECK(!ghostty_p0_surface_applied_sequence_for(surface,c->generation,&sequence) && !sequence);
            CHECK(!ghostty_p0_surface_event_output_for(surface,c->generation,c->snapshot+1,(const uint8_t *)"X",1));
            equal_surface(c->surface,&c->initial);
        }
        ghostty_surface_free(surface);process_args_close(&frames);
        for(unsigned i=0;i<200 && !p0_gui_idle(sibling);i++)surface_spin(0.01);
        CHECK(p0_gui_release(sibling)==P0_GUI_OK);
        p0_gui_usage after=p0_gui_pool_read(c->pool);CHECK(after.surfaces==1 && after.committed==160*MIB+c->main_reserve+c->io_reserve+c->metadata_limit+c->app_reserve+c->warmup_reserve+c->engine_thread_reserve+c->config_reserve+c->global_reserve+c->global_io_reserve);
    }
    puts("PASS process/shared GUI pool: two surfaces, third refused, wire fault isolated, deferred release and reuse");
}
static void process_pointer_expect(process_client *c,const char *bytes,size_t n) {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:2];
    while(atomic_load(&c->input_length)<n && [deadline timeIntervalSinceNow]>0)surface_spin(.01);
    surface_spin(.03);CHECK(atomic_load(&c->input_length)==n && !memcmp(c->input,bytes,n));
}
static void process_pointer_input(process_client *c) {
    const char reports[]="\x1b[<0;1;1M\x1b[<0;1;1m\x1b[O\x1b[I";
    ghostty_surface_mouse_pos(c->surface,0,0,GHOSTTY_MODS_NONE);process_pointer_expect(c,reports,0);
    ghostty_surface_mouse_button(c->surface,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);
    process_pointer_expect(c,reports,9);
    ghostty_surface_mouse_button(c->surface,GHOSTTY_MOUSE_RELEASE,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);
    process_pointer_expect(c,reports,18);
    ghostty_surface_set_focus(c->surface,false);process_pointer_expect(c,reports,21);
    ghostty_surface_set_focus(c->surface,false);process_pointer_expect(c,reports,21);
    ghostty_surface_set_focus(c->surface,true);process_pointer_expect(c,reports,24);
    puts("PASS process/pointer: restored SGR mouse and focus modes encode exact ordered reports, repeated focus stays silent");
}
static bool process_search_action(process_client *c,const char *action) {
    return ghostty_surface_binding_action(c->surface,action,strlen(action));
}
static void process_search_live(process_client *c) {
    CHECK(ghostty_p0_surface_search_exists(c->surface) && process_args_live(&c->thread_args)==3);
    uintptr_t handle=0,allocator=0;CHECK(ghostty_p0_surface_thread_handle(c->surface,3,&handle) && handle);
    CHECK(ghostty_p0_surface_thread_args_allocator(c->surface,3,&allocator) && allocator==(uintptr_t)&c->thread_args.allocator);
    uintptr_t renderer=0,io=0;CHECK(ghostty_p0_surface_thread_handle(c->surface,1,&renderer) && ghostty_p0_surface_thread_handle(c->surface,2,&io));
    CHECK(handle!=renderer && handle!=io);
    pthread_t thread=(pthread_t)handle;CHECK(!pthread_equal(thread,pthread_self()));
    const size_t per=c->engine_stack+2*(size_t)sysconf(_SC_PAGESIZE),stack=pthread_get_stacksize_np(thread);
    CHECK(stack>=c->engine_stack && stack<per && p0_gui_thread_capacity(c->budget)==3*per);
    uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(thread);CHECK(top>stack);
    mach_vm_address_t guard=top-stack-1,address=guard;mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    CHECK(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
    CHECK(address<=guard && address+size>guard && info.protection==VM_PROT_NONE && size>=(size_t)sysconf(_SC_PAGESIZE) && size<=per-stack);
    if(object)CHECK(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
    printf("METRIC process/search-thread stack_bytes=%zu guard_bytes=%llu reserved_bytes=%zu\n",stack,(unsigned long long)size,per);
}
static void process_search_probe(process_client *c) {
    CHECK(!pthread_mutex_lock(&c->thread_args.lock));c->thread_args.limit=2;CHECK(!pthread_mutex_unlock(&c->thread_args.lock));
    CHECK(!process_search_action(c,"search:FIRST"));CHECK(!ghostty_p0_surface_search_exists(c->surface));
    uintptr_t value=1;CHECK(!ghostty_p0_surface_thread_handle(c->surface,3,&value) && !value);
    CHECK(process_args_live(&c->thread_args)==2 && p0_gui_healthy(c->budget));
    CHECK(!pthread_mutex_lock(&c->thread_args.lock));c->thread_args.limit=3;CHECK(!pthread_mutex_unlock(&c->thread_args.lock));
    for(unsigned round=0;round<2;round++) {
        surface_spin(.05);atomic_store(&surface_search_total,-1);unsigned reports=atomic_load(&surface_search_reports);
        CHECK(process_search_action(c,"search:FIRST"));process_search_live(c);
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:3];
        while((atomic_load(&surface_search_reports)==reports || atomic_load(&surface_search_total)<=0) && [deadline timeIntervalSinceNow]>0)surface_spin(.01);
        CHECK(atomic_load(&surface_search_reports)>reports && atomic_load(&surface_search_total)>0);
        CHECK(!atomic_load(&c->input_length));
        if(!round) {
            CHECK(process_search_action(c,"end_search"));CHECK(!ghostty_p0_surface_search_exists(c->surface));CHECK(process_args_live(&c->thread_args)==2);
            value=1;CHECK(!ghostty_p0_surface_thread_handle(c->surface,3,&value) && !value);
        }
    }
    puts("PASS process/search: allocator denial leaves no search, retry finds actual text, stop/restart and active search retained for SIGKILL/free");
}
static void process_native_mouse(NSWindow *window,NSView *view,NSEventType type) {
    NSPoint point=[view convertPoint:NSMakePoint(2,NSHeight(view.bounds)-2) toView:nil];
    NSEvent *event=[NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil eventNumber:1 clickCount:1 pressure:type==NSEventTypeLeftMouseDown?1:0];CHECK(event);
    [NSApp sendEvent:event];
}
static void process_native_pointer(process_client *c,NSWindow *window,P0AppKitHost *host) {
    const char reports[]="\x1b[<0;1;1M\x1b[<0;1;1m\x1b[O\x1b[I";
    host.inputEnabled=YES;CHECK([window makeFirstResponder:window]);
    process_pointer_expect(c,reports,0);
    process_native_mouse(window,host,NSEventTypeLeftMouseDown);process_pointer_expect(c,reports,9);
    CHECK(window.firstResponder==host);
    process_native_mouse(window,host,NSEventTypeLeftMouseUp);process_pointer_expect(c,reports,18);
    CHECK(host.downs==1 && host.ups==1);
    ghostty_surface_set_focus(c->surface, false);process_pointer_expect(c,reports,21);
    ghostty_surface_set_focus(c->surface, false);process_pointer_expect(c,reports,21);
    ghostty_surface_set_focus(c->surface, true);process_pointer_expect(c,reports,24);
    puts("PASS process/native-pointer: NSApp mouse dispatch, coordinate conversion and firstResponder claim; focus via engine API");
}
static void process_native_key(NSWindow *window) {
    NSEvent *event=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:7];CHECK(event);
    [window sendEvent:event];
}
static void process_native_ime(process_client *c,NSWindow *window,P0AppKitHost *host) {
    host.inputEnabled=YES;CHECK([window makeFirstResponder:host]);
    process_native_key(window);process_pointer_expect(c,"x",1);CHECK(host.keys==1 && host.handled);
    [host setMarkedText:@"中" selectedRange:NSMakeRange(1,0) replacementRange:NSMakeRange(NSNotFound,0)];
    CHECK(host.hasMarkedText && NSEqualRanges(host.markedRange,NSMakeRange(0,1)) && NSEqualRanges(host.selectedRange,NSMakeRange(1,0)));
    double x=0,y=0,w=0,h=0;ghostty_surface_ime_point(c->surface,&x,&y,&w,&h);CHECK(w>0 && atomic_load(&c->input_length)==1);
    NSRect expected=[window convertRectToScreen:[host convertRect:NSMakeRect(x,NSHeight(host.bounds)-y,w,h) toView:nil]];
    NSRange actual;NSRect rect=[host firstRectForCharacterRange:NSMakeRange(0,1) actualRange:&actual];CHECK(NSEqualRects(expected,rect) && NSEqualRanges(actual,NSMakeRange(0,1)));
    [host unmarkText];ghostty_surface_ime_point(c->surface,&x,&y,&w,&h);CHECK(!host.hasMarkedText && host.markedRange.location==NSNotFound && w==0 && atomic_load(&c->input_length)==1);
    [host setMarkedText:[[NSAttributedString alloc] initWithString:@"中文"] selectedRange:NSMakeRange(2,0) replacementRange:NSMakeRange(NSNotFound,0)];
    [host insertText:[[NSAttributedString alloc] initWithString:@"中文"] replacementRange:NSMakeRange(NSNotFound,0)];
    process_pointer_expect(c,"x中文",7);ghostty_surface_ime_point(c->surface,&x,&y,&w,&h);CHECK(!host.hasMarkedText && w==0);
    puts("PASS process/native-ime: NSWindow key dispatch and interpretKeyEvents; NSTextInputClient preedit, cancellation, screen anchor and typed commit");
}
static void process_ime_commit(process_client *c) {
    CHECK(!atomic_load(&c->input_length));
    ghostty_surface_preedit(c->surface,"中",3);
    ghostty_input_key_s composing={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,
        .consumed_mods=GHOSTTY_MODS_NONE,.keycode=0,.text="中",.composing=true};
    ghostty_surface_key(c->surface,composing);surface_spin(.05);
    double x=0,y=0,width=0,height=0;ghostty_surface_ime_point(c->surface,&x,&y,&width,&height);
    CHECK(width>0 && !atomic_load(&c->input_length));
    ghostty_surface_preedit(c->surface,NULL,0);
    ghostty_surface_ime_point(c->surface,&x,&y,&width,&height);CHECK(width==0);
    ghostty_input_key_s commit={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,
        .consumed_mods=GHOSTTY_MODS_NONE,.keycode=0,.text="中文",.composing=false};
    ghostty_surface_key(c->surface,commit);
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:2];
    while(atomic_load(&c->input_length)<6 && [deadline timeIntervalSinceNow]>0)surface_spin(.01);
    CHECK(atomic_load(&c->input_length)==6 && !memcmp(c->input,"中文",6));
    puts("PASS process/IME: inline preedit silent, cleared, commit raw UTF-8 without paste wrappers");
}
typedef union {struct {size_t requested,mapped;} sizes;max_align_t alignment;} process_meta_header;
static void *process_meta_allocate(void *context,size_t n) {
    process_client *c=context;size_t page=(size_t)sysconf(_SC_PAGESIZE);
    if(!page || n>SIZE_MAX-sizeof(process_meta_header) || n+sizeof(process_meta_header)>SIZE_MAX-(page-1))return NULL;
    size_t mapped=((n+sizeof(process_meta_header)+page-1)/page)*page;
    CHECK(!pthread_mutex_lock(&c->metadata_lock));
    if(mapped>c->metadata_limit-c->metadata_live){pthread_mutex_unlock(&c->metadata_lock);return NULL;}
    process_meta_header *h=mmap(NULL,mapped,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(h==MAP_FAILED){pthread_mutex_unlock(&c->metadata_lock);return NULL;}
    h->sizes.requested=n;h->sizes.mapped=mapped;c->metadata_live+=mapped;
    if(c->metadata_live>c->metadata_peak)c->metadata_peak=c->metadata_live;
    CHECK(!pthread_mutex_unlock(&c->metadata_lock));return h+1;
}
static void process_meta_release(void *context,void *p,size_t n) {
    process_client *c=context;process_meta_header *h=(process_meta_header *)p-1;
    CHECK(h->sizes.requested==n);size_t mapped=h->sizes.mapped;
    CHECK(!pthread_mutex_lock(&c->metadata_lock));CHECK(c->metadata_live>=mapped && !munmap(h,mapped));
    c->metadata_live-=mapped;CHECK(!pthread_mutex_unlock(&c->metadata_lock));
}
static void process_warmup_observe(void *context,uint32_t role,uint32_t phase) {
    process_client *c=context;CHECK(role<2);
    if(phase==2){CHECK([NSThread isMainThread]);CHECK(!(atomic_fetch_or(&c->warmup_joined,1u<<role)&(1u<<role)));return;}
    CHECK(phase<=1 && ![NSThread isMainThread]);
    const size_t per=c->engine_stack+2*(size_t)sysconf(_SC_PAGESIZE);
    CHECK(c->warmup_reserve==2*per && c->warmup_reservation);
    size_t stack=pthread_get_stacksize_np(pthread_self());CHECK(stack>=c->engine_stack && stack<per);
    uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());CHECK(top>stack);
    mach_vm_address_t guard=top-stack-1,address=guard;mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    CHECK(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
    CHECK(address<=guard && address+size>guard && info.protection==VM_PROT_NONE && size>=(size_t)sysconf(_SC_PAGESIZE) && size<=per-stack);
    if(object)CHECK(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
    stack_t signal;CHECK(!sigaltstack(NULL,&signal) && !(signal.ss_flags&SS_DISABLE) && signal.ss_size==ghostty_p0_surface_thread_signal_stack_size());
    CHECK(process_args_live(&c->warmup_args)>0);
    unsigned mask=1u<<role;CHECK(!(atomic_fetch_or(phase?&c->warmup_finished:&c->warmup_started,mask)&mask));
    printf("METRIC process/warmup role=%u finished=%d stack_bytes=%zu guard_bytes=%llu reserved_bytes=%zu\n",role,phase,stack,(unsigned long long)size,per);
}
static void process_config_failure(process_client *c) {
    CHECK(c->surface && c->config_owner.value==surface_config);
    p0_gui_usage before=p0_gui_pool_read(c->pool);p0_config_owner failed={0};
    CHECK(!p0_config_open(c->pool,(size_t)sysconf(_SC_PAGESIZE),&failed));
    CHECK(!failed.value && !failed.reservation);c->config_failures++;
    char directory[4096],path[8192];CHECK(getcwd(directory,sizeof(directory)));
    CHECK(snprintf(path,sizeof(path),"%s/tmp/process-config-invalid-%ld.conf",directory,(long)getpid())>0);
    FILE *file=fopen(path,"w");CHECK(file);for(unsigned i=0;i<100000;i++)CHECK(fputc('x',file)!=EOF);CHECK(!fclose(file));
    CHECK(p0_config_open(c->pool,c->config_reserve,&failed));CHECK(!p0_config_load(&failed,path));CHECK(!unlink(path));
    CHECK(!failed.value && !failed.reservation);c->config_failures++;
    CHECK(p0_gui_pool_read(c->pool).committed==before.committed && p0_gui_pool_read(c->pool).reservations==before.reservations);
    CHECK(p0_gui_reserved_healthy(c->config_owner.reservation) && p0_gui_reserved_healthy(c->app_reservation) && p0_gui_healthy(c->budget));
    CHECK(c->config_owner.value==surface_config);
    CHECK(c->global_owner.initialized && p0_gui_reserved_healthy(c->global_owner.reservation));
    CHECK(ghostty_p0_global_allocator()==(uintptr_t)p0_gui_reserved_allocator(c->global_owner.reservation));
    puts("PASS process/config: true capacity denial and invalid load reclaimed; incumbent domains healthy before real input");
}
/* Prove the advertised promises coexist: two surfaces plus one unpublished
 * config, while the actual global/App/main/I/O leases remain admitted. */
static void process_global_capacity(process_client *c) {
    p0_gui_usage before=p0_gui_pool_read(c->pool);
    CHECK(c->global_owner.initialized && c->global_owner.reservation && before.reservations==7);
    p0_gui_budget *first=NULL,*second=NULL;
    p0_gui_reservation *temporary=NULL,*excess=NULL;
    CHECK(p0_gui_admit(c->pool,&first)==P0_GUI_OK);
    CHECK(p0_gui_admit(c->pool,&second)==P0_GUI_OK);
    CHECK(p0_gui_reserve(c->pool,c->config_reserve,&temporary)==P0_GUI_OK);
    CHECK(p0_gui_reserve(c->pool,(size_t)sysconf(_SC_PAGESIZE),&excess)==P0_GUI_LIMIT && !excess);
    CHECK(p0_gui_unreserve(temporary)==P0_GUI_OK);
    CHECK(p0_gui_release(second)==P0_GUI_OK && p0_gui_release(first)==P0_GUI_OK);
    p0_gui_usage after=p0_gui_pool_read(c->pool);
    CHECK(after.committed==before.committed && after.reservations==before.reservations && after.surfaces==before.surfaces);
    c->global_capacity_checked=true;
    puts("PASS process/global capacity: two surfaces and temporary config coexist with all process leases; excess rejected");
}
static void process_global_io_observe(void *context,uint32_t role,uint32_t phase) {
    process_client *c=context;CHECK(![NSThread isMainThread] && role<2 && phase<=1 && c->global_owner.threads);
    const size_t per=ghostty_p0_global_io_thread_stack_size()+2*(size_t)sysconf(_SC_PAGESIZE);
    CHECK(c->global_io_reserve==2*per);
    size_t stack=pthread_get_stacksize_np(pthread_self());CHECK(stack>=ghostty_p0_global_io_thread_stack_size() && stack<per);
    uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());CHECK(top>stack);
    mach_vm_address_t guard=top-stack-1,address=guard;mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    CHECK(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
    CHECK(address<=guard && address+size>guard && info.protection==VM_PROT_NONE && size>=(size_t)sysconf(_SC_PAGESIZE) && size<=per-stack);
    if(object)CHECK(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
    stack_t signal;CHECK(!sigaltstack(NULL,&signal) && !(signal.ss_flags&SS_DISABLE) && signal.ss_size==ghostty_p0_surface_thread_signal_stack_size());
    if(!phase){uintptr_t allocator=0;CHECK(ghostty_p0_global_io_thread_allocator(role,&allocator) && allocator==(uintptr_t)p0_gui_reserved_allocator(c->global_owner.reservation));}
    unsigned mask=1u<<role;CHECK(!(atomic_fetch_or(phase?&c->global_io_finished:&c->global_io_started,mask)&mask));
    printf("METRIC process/global-io role=%u finished=%u stack_bytes=%zu guard_bytes=%llu reserved_bytes=%zu\n",role,phase,stack,(unsigned long long)size,per);
}
static bool process_pool_close(process_client *c) {
    CHECK(surface_app && c->global_capacity_checked && p0_gui_reserved_healthy(c->app_reservation));
    ghostty_app_free(surface_app);surface_app=NULL;process_args_close(&c->app_args);
    CHECK(atomic_load(&c->warmup_started)==3 && atomic_load(&c->warmup_finished)==3 && atomic_load(&c->warmup_joined)==3);
    CHECK(c->warmup_args.attempts==2 && c->warmup_args.freed==2 && !c->warmup_args.denied);
    process_args_close(&c->warmup_args);
    CHECK(p0_gui_unreserve(c->warmup_reservation)==P0_GUI_OK);c->warmup_reservation=NULL;
    CHECK(p0_config_close(&c->config_owner));surface_config=NULL;
    CHECK(p0_gui_reserved_idle(c->app_reservation));
    CHECK(p0_gui_unreserve(c->app_reservation)==P0_GUI_OK);c->app_reservation=NULL;
    CHECK(c->global_owner.initialized && p0_gui_reserved_healthy(c->global_owner.reservation));
    CHECK(ghostty_p0_global_allocator()==(uintptr_t)p0_gui_reserved_allocator(c->global_owner.reservation));
    CHECK(atomic_load(&c->global_io_started)==3 && !atomic_load(&c->global_io_finished));
    CHECK(p0_global_close(&c->global_owner));
    ghostty_p0_io_stats_s io_stats;CHECK(ghostty_p0_global_io_stats(&io_stats) && !io_stats.threads && io_stats.started==2 && io_stats.finished==2 && io_stats.joined==2 && !io_stats.failures);
    CHECK(atomic_load(&c->global_io_finished)==3 && !c->global_owner.threads);
    CHECK(!c->global_owner.initialized && !c->global_owner.reservation && !ghostty_p0_global_allocator());
    CHECK(p0_gui_unreserve(c->main_reservation)==P0_GUI_OK);c->main_reservation=NULL;
    CHECK(p0_gui_pool_close(c->pool)==P0_GUI_OK);
    CHECK(!c->metadata_live && !pthread_mutex_destroy(&c->metadata_lock));
    printf("METRIC process/GUI metadata_peak_bytes=%zu final_live=0\n",c->metadata_peak);return true;
}
static bool process_join(process_client *c,pthread_t thread) {
    if(pthread_join(thread,NULL))return false;
    CHECK(p0_gui_pool_read(c->pool).reservations==7);
    CHECK(p0_gui_unreserve(c->io_reservation)==P0_GUI_OK);c->io_reservation=NULL;
    CHECK(p0_gui_pool_read(c->pool).reservations==6 && c->main_reservation);return true;
}
static int ghosterm_process_client(int argc,char **argv) {
    setbuf(stdout,NULL);
    CHECK(argc==5 && !strcmp(argv[1],"--process-client") && (!strcmp(argv[4],"0") || !strcmp(argv[4],"1") || !strcmp(argv[4],"2") || !strcmp(argv[4],"3") || !strcmp(argv[4],"4") || !strcmp(argv[4],"5") || !strcmp(argv[4],"6") || !strcmp(argv[4],"7") || !strcmp(argv[4],"8") || !strcmp(argv[4],"9") || !strcmp(argv[4],"10") || !strcmp(argv[4],"11") || !strcmp(argv[4],"12") || !strcmp(argv[4],"13") || !strcmp(argv[4],"14") || !strcmp(argv[4],"15") || !strcmp(argv[4],"16")));
    process_client c={.history_policy=atoi(argv[4])==16,.search=atoi(argv[4])==15,.fd=atoi(argv[2]),.round=(unsigned)atoi(argv[3]),.expect_error=atoi(argv[4])>0 && atoi(argv[4])<6,.recovery_error=atoi(argv[4])==2,.apply_error=atoi(argv[4])==3,.reply_error=atoi(argv[4])==4,.startup_error=(atoi(argv[4])==5 || atoi(argv[4])==7),.known_start_error=atoi(argv[4])==7,.ime=(atoi(argv[4])==6 || atoi(argv[4])==14),.native_ime=atoi(argv[4])==14,.pointer=(atoi(argv[4])==8 || atoi(argv[4])==13),.native_pointer=atoi(argv[4])==13,.scheme=atoi(argv[4])==9,.tmux=atoi(argv[4])==10,.tui=atoi(argv[4])==11?1u:atoi(argv[4])==12?2u:0u};CHECK(c.fd==3 && (c.round==1 || c.round==2));
    CHECK(!fcntl(c.fd,F_SETFD,FD_CLOEXEC));int flags=fcntl(c.fd,F_GETFL);CHECK(flags>=0 && !fcntl(c.fd,F_SETFL,flags|O_NONBLOCK));
#ifdef SO_NOSIGPIPE
    int enabled=1;CHECK(!setsockopt(c.fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)));
#endif
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);c.io_reserve=MIB+2*(size_t)page;
    p0_main_stack_info main_stack;CHECK(p0_main_stack_measure(&main_stack));c.main_reserve=main_stack.capacity;
    c.metadata_limit=4*MIB;c.app_reserve=64*MIB;c.config_reserve=8*MIB;c.global_reserve=8*MIB;c.global_io_reserve=p0_global_thread_capacity();CHECK(c.global_io_reserve>0);
    c.engine_stack=ghostty_p0_surface_thread_stack_size();CHECK(c.engine_stack>0 && c.engine_stack<=64*MIB && (size_t)page<=MIB);
    c.engine_thread_reserve=3*(c.engine_stack+2*(size_t)page);
    c.warmup_reserve=2*(c.engine_stack+2*(size_t)page);
    const p0_gui_limits limits={320*MIB+c.main_reserve+c.io_reserve+c.metadata_limit+c.app_reserve+c.warmup_reserve+2*c.engine_thread_reserve+2*c.config_reserve+c.global_reserve+c.global_io_reserve,64*MIB,64*MIB,32*MIB,3,c.metadata_limit,c.engine_thread_reserve};
    CHECK(!pthread_mutex_init(&c.metadata_lock,NULL));
    p0_gui_metadata_allocator metadata_allocator={&c,process_meta_allocate,process_meta_release};
    CHECK(p0_gui_pool_create_with_allocator(limits,&metadata_allocator,&c.pool)==P0_GUI_OK);
    CHECK(c.metadata_live>0);
    CHECK(p0_gui_reserve(c.pool,c.main_reserve,&c.main_reservation)==P0_GUI_OK);
    CHECK(p0_main_stack_admitted(&main_stack,p0_gui_pool_read(c.pool).committed-c.metadata_limit));
    printf("METRIC process/GUI main_stack_bytes=%zu main_guard_bytes=%zu main_reserved_bytes=%zu\n",main_stack.stack,main_stack.guard,c.main_reserve);
    CHECK(p0_gui_reserve(c.pool,c.io_reserve,&c.io_reservation)==P0_GUI_OK);
    CHECK(p0_global_open(c.pool,c.global_reserve,&c.global_owner));
    CHECK(c.global_owner.initialized && c.global_owner.reservation);
    CHECK(c.global_owner.threads && ghostty_p0_global_io_set_observer(&c,process_global_io_observe));
    CHECK(ghostty_p0_global_io_probe(0));
    CHECK(atomic_load(&c.global_io_started)==3 && !atomic_load(&c.global_io_finished));
    CHECK(p0_gui_unreserve(c.global_owner.reservation)==P0_GUI_BUSY);
    CHECK(ghostty_p0_global_allocator()==(uintptr_t)p0_gui_reserved_allocator(c.global_owner.reservation));
    CHECK(p0_gui_reserve(c.pool,c.app_reserve,&c.app_reservation)==P0_GUI_OK);
    CHECK(p0_gui_reserved_idle(c.app_reservation) && p0_gui_reserved_healthy(c.app_reservation));
    CHECK(p0_config_open(c.pool,c.config_reserve,&c.config_owner) && p0_config_finish(&c.config_owner));
    CHECK(!p0_gui_reserved_idle(c.config_owner.reservation));
    CHECK(p0_gui_unreserve(c.config_owner.reservation)==P0_GUI_BUSY);
    surface_config=c.config_owner.value;
    for(size_t limit=0;limit<4;limit++) {
        process_thread_args denied={0};CHECK(!pthread_mutex_init(&denied.lock,NULL));
        denied.limit=limit;denied.base=p0_gui_reserved_allocator(c.app_reservation);
        denied.allocator=(ghostty_p0_allocator_s){&denied,&process_args_vtable};
        CHECK(!surface_create_app(&denied.allocator));
        CHECK(denied.denied==1 && denied.attempts==limit+1 && denied.freed==limit && !process_args_live(&denied) && p0_gui_reserved_idle(c.app_reservation));
        process_args_close(&denied);
    }
    CHECK(!pthread_mutex_init(&c.app_args.lock,NULL));c.app_args.limit=SIZE_MAX;
    c.app_args.base=p0_gui_reserved_allocator(c.app_reservation);
    c.app_args.allocator=(ghostty_p0_allocator_s){&c.app_args,&process_args_vtable};
    CHECK(p0_gui_reserve(c.pool,c.warmup_reserve,&c.warmup_reservation)==P0_GUI_OK);
    for(size_t failed=1;failed<=2;failed++) {
        process_thread_args denied={0};CHECK(!pthread_mutex_init(&denied.lock,NULL));
        denied.limit=SIZE_MAX;denied.reject_attempt=failed;denied.base=p0_gui_reserved_allocator(c.app_reservation);
        denied.allocator=(ghostty_p0_allocator_s){&denied,&process_args_vtable};
        CHECK(!surface_create_app_options(&c.app_args.allocator,&denied.allocator,NULL,NULL));
        CHECK(denied.attempts==failed && denied.freed==failed-1 && denied.denied==1);
        CHECK(!process_args_live(&c.app_args) && p0_gui_reserved_idle(c.app_reservation));process_args_close(&denied);
    }
    process_args_close(&c.app_args);memset(&c.app_args,0,sizeof(c.app_args));
    CHECK(!pthread_mutex_init(&c.app_args.lock,NULL));c.app_args.limit=SIZE_MAX;
    c.app_args.base=p0_gui_reserved_allocator(c.app_reservation);
    c.app_args.allocator=(ghostty_p0_allocator_s){&c.app_args,&process_args_vtable};
    CHECK(!pthread_mutex_init(&c.warmup_args.lock,NULL));c.warmup_args.limit=SIZE_MAX;
    c.warmup_args.base=p0_gui_reserved_allocator(c.app_reservation);
    c.warmup_args.allocator=(ghostty_p0_allocator_s){&c.warmup_args,&process_args_vtable};
    CHECK(!surface_create_app_options(NULL,&c.warmup_args.allocator,NULL,NULL) && !c.warmup_args.attempts);
    surface_app=surface_create_app_options(&c.app_args.allocator,&c.warmup_args.allocator,&c,process_warmup_observe);CHECK(surface_app);
    CHECK(surface_app==c.app_args.second_allocation);
    CHECK(ghostty_p0_app_allocator(surface_app)==(uintptr_t)&c.app_args.allocator);
    CHECK(process_args_live(&c.app_args)>=3 && !p0_gui_reserved_idle(c.app_reservation));
    CHECK(p0_gui_unreserve(c.app_reservation)==P0_GUI_BUSY);
    puts("PASS process/app: four allocation denials clean, shared domain selected, live app prevents release");
    process_global_capacity(&c);
    c.io=(pw_io){&c.fd,pw_socket_transfer};pthread_t thread;
    gt_thread_options_t io_options={MIB,(size_t)page};
    CHECK(!gt__thread_create(&thread,process_consume,&c,&io_options));
    if(c.startup_error) {
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:5];
        while(!atomic_load(&c.done) && [deadline timeIntervalSinceNow]>0)surface_spin(.01);
        CHECK(atomic_load(&c.done) && process_join(&c,thread) && c.has_result && atomic_load(&c.ended));
        CHECK(!c.surface && !c.budget && !atomic_load(&c.snapshot_ready));
        NSWindow *failure=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,480,120) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        failure.title=@"Ghosterm · Session failed to start";
        NSTextField *message=[NSTextField labelWithString:c.known_start_error?@"Backend failed to start (BACKEND_OPEN / IO). Final sequence known: 0.":@"Session failed to start (STATE_OPEN / IO). Final sequence unknown."];
        message.frame=NSMakeRect(12,40,456,50);message.lineBreakMode=NSLineBreakByWordWrapping;
        [failure.contentView addSubview:message];[failure orderFrontRegardless];surface_spin(.05);
        CHECK([failure.title isEqualToString:@"Ghosterm · Session failed to start"] && [message.stringValue containsString:c.known_start_error?@"known: 0":@"unknown"]);
        p0_gui_usage usage=p0_gui_pool_read(c.pool);CHECK(!usage.surfaces && usage.committed==c.metadata_limit+c.main_reserve+c.app_reserve+c.warmup_reserve+c.config_reserve+c.global_reserve+c.global_io_reserve && usage.live>0);
        CHECK(process_pool_close(&c));[failure orderOut:nil];close(c.fd);
        puts("PASS process/start-error GUI: explicit failure, sequence knowledge retained, zero terminal surfaces and GUI reservation");return 0;
    }
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:5];
    while(!atomic_load(&c.snapshot_ready) && [end timeIntervalSinceNow]>0)surface_spin(0.01);
    CHECK(atomic_load(&c.snapshot_ready));
    p0_gui_budget *budget=c.budget;CHECK(budget);
    __attribute__((objc_precise_lifetime)) NSView *view=(c.native_pointer || c.native_ime)?[[P0AppKitHost alloc] initWithFrame:NSMakeRect(0,0,800,600)]:[[NSView alloc] initWithFrame:NSMakeRect(0,0,800,600)];
    NSWindow *window=[[NSWindow alloc] initWithContentRect:view.frame styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView=view;[window orderFrontRegardless];
    if(c.history_policy){c.history_window=(__bridge void *)window;process_history_ui(&c);CHECK([window.subtitle isEqualToString:@"History status unknown"]);}
    c.scheme_view=(__bridge void *)view;
    ghostty_surface_config_s opts=ghostty_surface_config_new();
    opts.platform_tag=GHOSTTY_PLATFORM_MACOS;opts.platform.macos.nsview=(__bridge void *)view;
    opts.scale_factor=1;opts.host=true;opts.host_write=surface_legacy_write;opts.host_resize=surface_resize_request;
    opts.userdata=&c;opts.p0_host_write_input=process_input;
    opts.p0_snapshot=c.initial.canonical;opts.p0_snapshot_len=c.initial.canonical_len;
    opts.p0_resources_snapshot=c.initial.resources;opts.p0_resources_snapshot_len=c.initial.resources_len;
    opts.p0_image_storage_limit=MIB;opts.p0_image_storage_limit_set=true;
    opts.p0_allocator=p0_gui_general(budget);opts.p0_page_allocator=p0_gui_pages(budget);
    opts.p0_budget_userdata=budget;opts.p0_budget_healthy=p0_gui_healthy;
    if(c.scheme){opts.p0_color_scheme_set=true;opts.p0_color_scheme_dark=c.dark;}
    process_args_open(&c.thread_args,budget,3);opts.p0_thread_allocator=&c.thread_args.allocator;
    bind_checkpoint(&opts,c.generation,c.snapshot,&c.initial);
    atomic_store(&surface_legacy_writes,0);atomic_store(&surface_effects,0);
    if(c.scheme) {
        ghostty_app_set_color_scheme(surface_app,GHOSTTY_COLOR_SCHEME_LIGHT);
        ghostty_surface_config_s changed=opts;changed.p0_color_scheme_dark=!changed.p0_color_scheme_dark;
        CHECK(!ghostty_surface_new(surface_app,&changed));
    }
    c.surface=ghostty_surface_new(surface_app,&opts);CHECK(c.surface);
    CHECK(process_args_live(&c.thread_args)==2 && c.thread_args.attempts==2);
    CHECK(process_args_bytes(&c.thread_args)>=2*ghostty_p0_surface_thread_signal_stack_size());
    process_engine_threads(&c,budget,c.surface,&c.thread_args.allocator);
    if(c.native_pointer || c.native_ime){((P0AppKitHost *)view).terminal=c.surface;CHECK([window makeFirstResponder:view]);}
    if(c.scheme) {
        bool dark=false;uint32_t background=0;CHECK(ghostty_p0_surface_scheme_state_for(c.surface,c.generation,&dark,&background));
        CHECK(dark && background==0x123456);
    }
    equal_surface(c.surface,&c.initial);process_admission_probe(&c,opts);process_free_checkpoint(&c,&c.initial);
    CHECK(c.summary_bytes && p0_gui_pool_read(c.pool).wire_live==c.summary_bytes);
    if(c.scheme)process_scheme_pixels(&c,true,"snapshot");
    if(c.native_pointer) {
        P0AppKitHost *host=(P0AppKitHost *)view;
        process_native_mouse(window,host,NSEventTypeLeftMouseDown);process_native_mouse(window,host,NSEventTypeLeftMouseUp);
        CHECK(host.blockedEvents==2 && !atomic_load(&c.input_length));
    }
    if(c.native_ime) {
        P0AppKitHost *host=(P0AppKitHost *)view;process_native_key(window);
        [host setMarkedText:@"中" selectedRange:NSMakeRange(1,0) replacementRange:NSMakeRange(NSNotFound,0)];
        [host insertText:@"中文" replacementRange:NSMakeRange(NSNotFound,0)];
        CHECK(host.blockedEvents==3 && host.keys==0 && !host.hasMarkedText && !atomic_load(&c.input_length));
    }
    if(c.history_policy){atomic_store(&c.history_display,c.snapshot_history.state);process_history_ui(&c);CHECK(c.round==1?!window.subtitle.length:[window.subtitle isEqualToString:@"History truncated"]);}
    atomic_store(&c.surface_ready,true);
    end=[NSDate dateWithTimeIntervalSinceNow:6];
    while(!atomic_load(&c.live) && [end timeIntervalSinceNow]>0){process_scheme_main(&c);process_history_ui(&c);surface_spin(0.01);}
    CHECK(atomic_load(&c.live));ghostty_surface_draw(c.surface);
    process_config_failure(&c);
    if(c.history_policy){process_history_ui(&c);CHECK([window.subtitle isEqualToString:@"History truncated"] && c.history_ui_updates==(c.round==1?3u:2u));puts("PASS process/history UI: unknown distinct, frozen snapshot and ordered LIVE truncation subtitle");}
    if(c.tui)process_tui_pixels(view,c.tui,c.round);else surface_pixels(view,c.round==2);
    if(c.scheme)process_scheme_pixels(&c,true,"live");
    NSString *from=c.round==1 ? @"tmp/worker-surface/pixels-import.png" : @"tmp/worker-surface/pixels-live.png";
    NSString *to=[NSString stringWithFormat:@"tmp/worker-surface/process-round-%u.png",c.round];
    if(!c.tui)CHECK([[NSData dataWithContentsOfFile:from] writeToFile:to atomically:YES]);
    CHECK(!atomic_load(&surface_legacy_writes) && !atomic_load(&surface_effects));
    if(c.search)process_search_probe(&c);
    atomic_store(&c.drawn,true);
    if(c.native_pointer)process_native_pointer(&c,window,(P0AppKitHost *)view);else if(c.pointer)process_pointer_input(&c);
    if(c.native_ime)process_native_ime(&c,window,(P0AppKitHost *)view);else if(c.ime)process_ime_commit(&c);
    if(c.round==2 && !c.recovery_error) {
        if(c.tui) {
            ghostty_input_key_s key={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,.consumed_mods=GHOSTTY_MODS_NONE,.keycode=0,.text=c.tui==1?"g":"USER",.composing=false};
            CHECK(ghostty_surface_key(c.surface,key));
        } else ghostty_surface_text(c.surface,"USER",4);
    }
    end=[NSDate dateWithTimeIntervalSinceNow:10];
    while(!atomic_load(&c.done) && [end timeIntervalSinceNow]>0) {
        process_scheme_main(&c);process_history_ui(&c);
        if(atomic_exchange(&c.prepare_inflight,false)){CHECK(!atomic_load(&c.ended));ghostty_surface_text(c.surface,"INFLIGHT",8);}
        surface_spin(0.01);
    }
    CHECK(atomic_load(&c.done) && process_join(&c,thread));CHECK(c.config_failures==2);
    NSString *ended_title=c.result.kind==GT_WORKER_END_ERROR?@"Ghosterm · Session failed":@"Ghosterm · Session ended";
    window.title=ended_title;CHECK([window.title isEqualToString:ended_title]);
    atomic_store(&c.input_length,0);ghostty_surface_text(c.surface,"LATE",4);
    end=[NSDate dateWithTimeIntervalSinceNow:2];
    while(!atomic_load(&c.rejected_inputs) && [end timeIntervalSinceNow]>0)surface_spin(0.01);
    CHECK(atomic_load(&c.rejected_inputs)>0 && !atomic_load(&c.input_length));
    puts("PASS process/ended GUI refuses further encoded input and retains terminal result");
    ghostty_surface_draw(c.surface);if(c.tui)process_tui_pixels(view,c.tui,3);else process_final_pixels(view);
    if(c.scheme){process_scheme_pixels(&c,false,"ended");CHECK(c.scheme_draws==6);}
    CHECK(!atomic_load(&surface_legacy_writes) && !atomic_load(&surface_effects) && p0_gui_healthy(budget));
    if(c.native_pointer || c.native_ime){((P0AppKitHost *)view).inputEnabled=NO;((P0AppKitHost *)view).terminal=NULL;}
    ghostty_surface_free(c.surface);process_args_close(&c.thread_args);
    gt__program_status_close(c.summary);c.summary=NULL;
    CHECK(!p0_gui_pool_read(c.pool).wire_live);
    for(unsigned i=0;i<200 && !p0_gui_idle(budget);i++)surface_spin(0.01);
    CHECK(p0_gui_release(budget)==P0_GUI_OK);
    p0_gui_usage usage=p0_gui_pool_read(c.pool);CHECK(!usage.surfaces && usage.committed==c.metadata_limit+c.main_reserve+c.app_reserve+c.warmup_reserve+c.config_reserve+c.global_reserve+c.global_io_reserve && usage.live>0);
    CHECK(process_pool_close(&c));[window orderOut:nil];close(c.fd);
    printf("PASS process/Metal GUI round=%u g=%llu S=%llu applied=%llu legacy=0 effects=0\n",c.round,
      (unsigned long long)c.generation,(unsigned long long)c.snapshot,(unsigned long long)c.cursor);return 0;
}
