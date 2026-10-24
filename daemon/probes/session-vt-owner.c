/* P0 composition evidence: real Core worker + PTY + budgeted libghostty-vt.
 * This is not the production daemon entry point or a public binding. */
#include "session/worker.h"
#include "session/checkpoint-task.h"
#include "session/attachment.h"
#include "frozen-budget.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
#define MIB (1024u * 1024u)
#ifdef GHOSTERM_P0_FULL_SURFACE_REPLAY
#define PROBE_CELL_WIDTH 10u
#define PROBE_CELL_HEIGHT 20u
#else
#define PROBE_CELL_WIDTH 0u
#define PROBE_CELL_HEIGHT 0u
#endif
typedef struct {
    p0_frozen_checkpoint frozen;
    p0_checkpoint expected, encoded;
    uint64_t events, sequence;
} checkpoint_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t caller;
    p0_budget budget;
    p0_budget_scope owner, capture, reference, encoder, gui_general, gui_pages;
    GhosttyTerminal terminal;
    GhosttyAllocator encoder_base;
    uint64_t events;
    _Atomic uint64_t pushed;
    checkpoint_t checkpoint;
    p0_checkpoint final;
    gt_session_attachment_t *attachment;
    gt_session_worker_t *worker;
    uint64_t generation;
    gt_restore_rc_t queue_status;
    bool first, second, captured, finished, encoding, release;
    size_t completions;
    gt_status_t status;
    gt_worker_reply_sink_t sink;
    gt_status_t reply_status;
    unsigned reply_calls;
    size_t reply_bytes;
    unsigned notifications,scheme_calls;
    size_t notification_bytes;
    unsigned clip_writes,clip_reads;
    size_t clip_write_bytes;
    char stream[65536];
    size_t stream_len;
    /* 真实 TUI 的输出不是固定字节串，镜像 oracle 改用这份独立事件日志重放。 */
    struct { uint64_t sequence; uint32_t offset, length; uint8_t kind; } log[512];
    size_t log_count, log_used;
    uint8_t log_bytes[65536];
    const char *mode;
} fixture_t;
static void wait_for(fixture_t *f, const bool *flag) {
    struct timespec until;
    CHECK(!clock_gettime(CLOCK_REALTIME, &until)); until.tv_sec += 8;
    CHECK(!pthread_mutex_lock(&f->lock));
    while (!*flag) CHECK(!pthread_cond_timedwait(&f->changed, &f->lock, &until));
    CHECK(!pthread_mutex_unlock(&f->lock));
}
/* gui-default-utf8 replays the same split UTF-8 sequence as utf8, but the
 * surface imports it on the default allocators: it is the control that keeps
 * font-ownership faults from hiding behind "UTF-8 is simply broken". */
static bool mode_utf8(const char *mode) {
    return !strcmp(mode,"utf8") || !strcmp(mode,"gui-default-utf8");
}
static const char *prefix(const char *mode) {
    if (!strcmp(mode,"pixels")) return "\x1b[?25l\x1b[48;2;220;30;30m\x1b[38;2;255;255;255mFIRST\x1b[?2004";
    if (!strcmp(mode,"query") || !strcmp(mode,"query-delta")) return "\x1b[3;5H\x1b[6nFIRST\x1b[?2004";
    if (!strcmp(mode,"query-caps")) return "\x1b[c\x1b[>c\x1b[>qFIRST\x1b[?2004";
    if (!strcmp(mode,"isolation")) return "\x1b]9;HELLONOTE\x1b\\\x1b[?996nFIRST\x1b[?2004";
    if (!strcmp(mode,"clipboard")) return "\x1b]52;c;SEVMTE8=\x1b\\\x1b]52;c;?\x1b\\FIRST\x1b[?2004";
    if (!strcmp(mode,"input")) return "\x1b[?1000h\x1b[?1006h\x1b[?1004hFIRST\x1b[?2004";
    if (mode_utf8(mode)) return "FIRST\xe4\xb8";
    if (!strcmp(mode,"resources")) return "\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\FIRST\x1b[?2004";
    return "FIRST\x1b[?2004";
}
/* tui-* 跑的是真实全屏程序：先在主屏留下 scrollback，再进副屏整屏绘制，
 * 然后在没有 GUI 的情况下继续 resize 重绘和接收输入。之前所有模式都只在
 * 主屏写几十字节，既没有副屏也没有程序自己的重绘。 */
static bool mode_tui(const char *mode) { return !strncmp(mode,"tui-",4); }
static bool contains(const uint8_t *haystack,size_t length,const char *needle) {
    const size_t n=strlen(needle);
    for (size_t i=0;n && i+n<=length;i++) if (!memcmp(haystack+i,needle,n)) return true;
    return false;
}
static const char *first_marker(const char *mode) {
    if (!strcmp(mode,"resources")) return "FIRST\x1b[?2004";
    if (mode_tui(mode)) return "LINE023";
    return prefix(mode);
}
static const char *second_marker(const char *mode) {
    if (!strcmp(mode,"isolation")) return "ISOLATION_DONE";
    if (!strcmp(mode,"tui-less")) return "(END)";
    if (!strcmp(mode,"tui-vim")) return "HELLOLINE001";
    return "SECOND";
}
/* detach 期间发给程序的按键：less 跳到末页，vim 插入一段文本。 */
static const char *detached_keys(const char *mode) {
    if (!strcmp(mode,"tui-less")) return "G";
    if (!strcmp(mode,"tui-vim")) return "iHELLO\x1b";
    return "go\n";
}
static void configure(GhosttyTerminal terminal) {
    CHECK(ghostty_terminal_resize(terminal,80,24,PROBE_CELL_WIDTH,PROBE_CELL_HEIGHT)==GHOSTTY_SUCCESS);
    const size_t continuation = MIB;
    const uint64_t images = MIB;
    CHECK(ghostty_terminal_set(terminal,GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES,&continuation) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(terminal,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&images) == GHOSTTY_SUCCESS);
}
/* 复用器（tmux/screen）启动时先问能力再决定怎么画：DA1/DA2 不答它就一直等。
 * libghostty-vt 的 XTVERSION 和 DECRQM 自带答复，设备属性却要宿主装回调，
 * 不装就是静默不答——所以这是权威侧的契约，不是某个用例的布景。 */
static bool device_attributes(GhosttyTerminal terminal,void *context,GhosttyDeviceAttributes *out) {
    (void)terminal;(void)context;
    *out=(GhosttyDeviceAttributes){0};
    out->primary.conformance_level=GHOSTTY_DA_CONFORMANCE_VT220;
    out->primary.features[0]=GHOSTTY_DA_FEATURE_COLUMNS_132;
    out->primary.features[1]=GHOSTTY_DA_FEATURE_ANSI_COLOR;
    out->primary.num_features=2;
    out->secondary.device_type=GHOSTTY_DA_DEVICE_TYPE_VT220;
    out->secondary.firmware_version=10;
    return true;
}
/* 宿主动作与查询应答的 owner 只能是权威：同一串程序输出会被重放进恢复副本，
 * 副本再弹一次通知、再答一次配色，用户就会看到两份。权威这边装上回调并计数，
 * 副本那边断言计数为零，隔离才算被证明，而不是没人发过这些序列。 */
static void desktop_notification(GhosttyTerminal terminal,void *context,
                                 const GhosttyTerminalDesktopNotification *notification) {
    (void)terminal;
    fixture_t *f=context;
    CHECK(notification->size>=sizeof(*notification));
    f->notifications++;f->notification_bytes+=notification->body.len;
}
static bool color_scheme(GhosttyTerminal terminal,void *context,GhosttyColorScheme *out) {
    (void)terminal;
    fixture_t *f=context;
    f->scheme_calls++;*out=GHOSTTY_COLOR_SCHEME_DARK;return true;
}
/* 剪贴板是唯一一类「宿主必须当场答复，否则程序拿到空」的动作：写要回
 * SUCCESS，读要把内容交回去由引擎编成 OSC 52 答复。两者都只能由权威做——
 * 副本再写一次用户的剪贴板，或者再答一次读，都是用户能直接看见的重影。 */
static void clipboard_write(GhosttyTerminal terminal,void *context,const GhosttyClipboardWrite *write) {
    (void)terminal;
    fixture_t *f=context;
    CHECK(write->size>=sizeof(*write) && write->location==GHOSTTY_CLIPBOARD_LOCATION_STANDARD);
    /* 引擎交过来的必须是解完 base64 的原文，宿主不该再碰协议层编码。 */
    const char *expected=f->clip_writes ? "SECONDCLIP" : "HELLO";
    CHECK(write->contents_len==1 && write->contents[0].data.len==strlen(expected));
    CHECK(!memcmp(write->contents[0].data.ptr,expected,strlen(expected)));
    f->clip_writes++;f->clip_write_bytes+=write->contents[0].data.len;
    const GhosttyClipboardWriteReply reply={.size=sizeof(reply),.result=GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS};
    write->reply(write,&reply);
}
static void clipboard_read(GhosttyTerminal terminal,void *context,const GhosttyClipboardRead *read) {
    (void)terminal;
    fixture_t *f=context;
    CHECK(read->size>=sizeof(*read) && read->location==GHOSTTY_CLIPBOARD_LOCATION_STANDARD);
    f->clip_reads++;
    static const char mime[]="text/plain",text[]="PASTE";
    const GhosttyClipboardContent content={{(const uint8_t *)mime,sizeof(mime)-1},
                                           {(const uint8_t *)text,sizeof(text)-1}};
    const GhosttyClipboardReadReply reply={.size=sizeof(reply),.result=GHOSTTY_CLIPBOARD_READ_RESULT_SUCCESS,
                                           .contents=&content,.contents_len=1};
    read->reply(read,&reply);
}
static void on_reply(GhosttyTerminal terminal,void *context,const uint8_t *bytes,size_t length) {
    (void)terminal;
    fixture_t *f=context;
    f->reply_calls++;f->reply_bytes+=length;
    f->reply_status = f->sink.write(f->sink.context,bytes,length);
}
static gt_status_t state_open(void *context, gt_worker_reply_sink_t sink, void **out) {
    fixture_t *f = context;
    f->sink=sink;
    CHECK(!pthread_equal(f->caller, pthread_self()));
    CHECK(ghostty_terminal_p0_new_with_page_allocator(
        p0_budget_allocator(&f->owner,P0_BUDGET_GENERAL),
        p0_budget_allocator(&f->owner,P0_BUDGET_PAGES),&f->terminal,80,24) == GHOSTTY_SUCCESS);
    configure(f->terminal);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_USERDATA,f) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_WRITE_PTY,(const void *)on_reply) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,(const void *)device_attributes) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,(const void *)desktop_notification) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,(const void *)color_scheme) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,(const void *)clipboard_write) == GHOSTTY_SUCCESS);
    CHECK(ghostty_terminal_set(f->terminal,GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ,(const void *)clipboard_read) == GHOSTTY_SUCCESS);
    *out = f;
    return GT_OK;
}
static void state_close(void *context) {
    fixture_t *f = context;
    CHECK(!pthread_equal(f->caller, pthread_self()));
    CHECK(p0_checkpoint_capture(&f->owner,&f->reference,f->terminal,16*MIB,&f->final,NULL)==P0_CHECKPOINT_OK);
    ghostty_terminal_free(f->terminal); f->terminal = NULL;
    CHECK(p0_budget_scope_close(&f->owner));
}
static gt_io_backend_t *backend_open(void *context) {
    fixture_t *f = context;
    /* A controlled real child pauses with a partial VT sequence. Its input
     * gates output phases; marker observation never assumes PTY read splits. */
    const char *script = mode_utf8(f->mode) ?
        "stty -echo; printf 'FIRST\\344\\270'; read x; printf '\\255SECOND'; read x" :
        "stty -echo; printf 'FIRST\\033[?2004'; read x; printf 'hSECOND'; stty size; read x";
    if (!strcmp(f->mode,"pixels")) script = "stty -echo; printf '\\033[?25l\\033[48;2;220;30;30m\\033[38;2;255;255;255mFIRST\\033[?2004'; read x; printf 'h\\033[48;2;30;220;30mSECOND'; read x";
    if (!strcmp(f->mode,"resources")) script = "stty -echo; printf '\\033_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\\033\\\\FIRST\\033[?2004'; read x; printf 'hSECOND'; read x";
    if (!strcmp(f->mode,"query")) script =
        "stty -echo -icanon min 1 time 0; printf '\\033[3;5H\\033[6n'; "
        "answer=$(dd bs=1 count=6 2>/dev/null); expected=$(printf '\\033[3;5R'); "
        "[ \"$answer\" = \"$expected\" ] || exit 17; "
        "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); "
        "[ -z \"$extra\" ] || exit 18; stty min 1 time 0; "
        "printf 'FIRST\\033[?2004'; read x; printf 'hSECOND'; read x";
    if (!strcmp(f->mode,"query-delta")) script =
        "stty -echo -icanon min 1 time 0; printf '\\033[3;5H\\033[6n'; "
        "answer=$(dd bs=1 count=6 2>/dev/null); expected=$(printf '\\033[3;5R'); "
        "[ \"$answer\" = \"$expected\" ] || exit 17; "
        "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); "
        "[ -z \"$extra\" ] || exit 18; stty min 1 time 0; "
        "printf 'FIRST\\033[?2004'; read x; printf 'h\\033[3;5H\\033[6n'; "
        "answer=$(dd bs=1 count=6 2>/dev/null); [ \"$answer\" = \"$expected\" ] || exit 19; printf SECOND; read x";
    /* query-caps 用确定性脚本复刻复用器开机时那串能力探测：tmux 只在 Homebrew
     * 里有，拿它当闸门会让闸门依赖机器，所以这里直接发它要问的东西，并逐字
     * 校验答复。DA1/DA2 来自宿主回调，XTVERSION 与 DECRQM 来自引擎自身；
     * DECRQM 问的是 detach 之后才补完的 2004，覆盖「detach 期间还能查询」。 */
    if (!strcmp(f->mode,"query-caps")) script =
        "stty -echo -icanon min 1 time 0; "
        "printf '\\033[c'; a=$(dd bs=1 count=11 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033[?62;1;22c')\" ] || exit 21; "
        "printf '\\033[>c'; a=$(dd bs=1 count=10 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033[>1;10;0c')\" ] || exit 22; "
        "printf '\\033[>q'; a=$(dd bs=1 count=16 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033P>|libghostty\\033\\\\')\" ] || exit 23; "
        "printf 'FIRST\\033[?2004'; read x; "
        "printf 'h\\033[?2004$p'; a=$(dd bs=1 count=11 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033[?2004;1$y')\" ] || exit 24; "
        "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); "
        "[ -z \"$extra\" ] || exit 25; stty min 1 time 0; "
        "printf SECOND; read x";
    /* isolation 发的是会产生宿主副作用和应答的序列：OSC 9 桌面通知（只有动作、
     * 没有回复）与配色查询 CSI ? 996 n。detach 之后两样再各发一次，既覆盖「无
     * GUI 时宿主动作和应答仍归权威」，也让这两段字节落进增量流被重放给副本。 */
    if (!strcmp(f->mode,"isolation")) script =
        "stty -echo -icanon min 1 time 0; "
        "printf '\\033]9;HELLONOTE\\033\\\\'; "
        "printf '\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033[?997;1n')\" ] || exit 31; "
        "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); "
        "[ -z \"$extra\" ] || exit 32; stty min 1 time 0; "
        "printf 'FIRST\\033[?2004'; read x; "
        "printf 'h\\033]9;SECONDNOTE\\033\\\\'; "
        "sleep 0.2; " /* Notification text must not complete this phase. */
        "printf '\\033[?996n'; a=$(dd bs=1 count=9 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033[?997;1n')\" ] || exit 33; "
        "printf ISOLATION_DONE; read x";
    /* clipboard 走 OSC 52 的两个方向：写（无回复，纯动作）和读（宿主当场交回
     * 内容、引擎编成 OSC 52 答复）。同样 detach 前后各一轮，后一轮落进增量流。 */
    if (!strcmp(f->mode,"clipboard")) script =
        "stty -echo -icanon min 1 time 0; "
        "printf '\\033]52;c;SEVMTE8=\\033\\\\'; "
        "printf '\\033]52;c;?\\033\\\\'; a=$(dd bs=1 count=17 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033]52;c;UEFTVEU=\\033\\\\')\" ] || exit 41; "
        "stty min 0 time 1; extra=$(dd bs=1 count=1 2>/dev/null); "
        "[ -z \"$extra\" ] || exit 42; stty min 1 time 0; "
        "printf 'FIRST\\033[?2004'; read x; "
        "printf 'h\\033]52;c;U0VDT05EQ0xJUA==\\033\\\\'; "
        "printf '\\033]52;c;?\\033\\\\'; a=$(dd bs=1 count=17 2>/dev/null); "
        "[ \"$a\" = \"$(printf '\\033]52;c;UEFTVEU=\\033\\\\')\" ] || exit 43; "
        "printf SECOND; read x";
    /* input 的要害在 prefix 里那两个鼠标模式：它们在 detach 前就进了权威的快照，
     * 恢复出来的副本必须按「被恢复的状态」编码鼠标和粘贴，而不是按探针或 GUI
     * 自己的默认值。程序这边不读鼠标字节——没有 IPC，输入止于 p0 通道。 */
    if (!strcmp(f->mode,"input")) script =
        "stty -echo; printf '\\033[?1000h\\033[?1006h\\033[?1004hFIRST\\033[?2004'; read x; "
        "printf 'hSECOND'; read x";
    /* 100 行先滚进主屏 scrollback，再 exec 真实全屏程序进副屏。文件固定名放
     * 工程 tmp/，宿主会拦截新目录新文件名。 */
    const char *tui_prelude =
        "stty -echo; awk 'BEGIN{for(i=1;i<=100;i++)printf \"SCROLL%03d\\n\",i}'; "
        "awk 'BEGIN{for(i=1;i<=200;i++)printf \"LINE%03d\\n\",i}' > \"${TMPDIR:?}/tui-source.txt\"; ";
    static char tui_script[512];
    if (mode_tui(f->mode)) {
        const char *program = !strcmp(f->mode,"tui-vim")
            ? "exec /usr/bin/vim -u NONE -i NONE -n \"${TMPDIR:?}/tui-source.txt\""
            : "exec /usr/bin/less \"${TMPDIR:?}/tui-source.txt\"";
        CHECK(snprintf(tui_script,sizeof(tui_script),"%s%s",tui_prelude,program) < (int)sizeof(tui_script));
        script = tui_script;
    }
    const char *args[] = {"-c", script, NULL};
    gt_pty_opts_t opts = {.executable="/bin/sh",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
    return gt__pty_open(&opts);
}
static gt_status_t state_apply(void *context, const gt_state_event_t *event) {
    fixture_t *f = context;
    CHECK(!pthread_equal(f->caller,pthread_self()));
    GhosttyResult rc;
    if (event->kind == GT_STATE_OUTPUT)
        rc = p0_budget_feed(&f->owner,f->terminal,event->payload,event->length);
    else {
        CHECK(event->kind == GT_STATE_RESIZE && event->length == sizeof(gt_state_size_t));
        const gt_state_size_t *size = event->payload;
#ifdef GHOSTERM_P0_FULL_SURFACE_REPLAY
        rc = p0_budget_resize_cells(&f->owner,f->terminal,size->cols,size->rows,PROBE_CELL_WIDTH,PROBE_CELL_HEIGHT);
#else
        rc = p0_budget_resize(&f->owner,f->terminal,size->cols,size->rows);
#endif
    }
    if (rc != GHOSTTY_SUCCESS) return GT_ERR_IO;
    if (f->reply_status != GT_OK) return f->reply_status;
    f->events++;
    return GT_OK;
}
static gt_status_t state_freeze(void *context, void **out) {
    fixture_t *f = context;
    checkpoint_t *c = &f->checkpoint;
    CHECK(!c->frozen.handle);
    /* Independent synchronous wire oracle, test-only, under the same owner.
     * Production capture will not perform this reference encoding. */
    CHECK(p0_checkpoint_capture(&f->owner,&f->reference,f->terminal,16*MIB,
                               &c->expected,NULL) == P0_CHECKPOINT_OK);
    CHECK(p0_frozen_capture(&f->owner,&f->capture,f->terminal,16*MIB,&c->frozen) == GHOSTTY_SUCCESS);
    c->events = f->events;
    CHECK(gt__attachment_begin(f->attachment,f->generation,f->events)==GT_RESTORE_OK);
    *out = c;
    return GT_OK;
}
static void authority_event(void *context,const gt_state_event_t *event,uint64_t sequence) {
    fixture_t *f=context;
    if(mode_tui(f->mode)) {
        CHECK(f->log_count<sizeof(f->log)/sizeof(f->log[0]));
        CHECK(f->log_used+event->length<=sizeof(f->log_bytes));
        memcpy(f->log_bytes+f->log_used,event->payload,event->length);
        f->log[f->log_count].sequence=sequence;f->log[f->log_count].offset=(uint32_t)f->log_used;
        f->log[f->log_count].length=(uint32_t)event->length;f->log[f->log_count].kind=(uint8_t)event->kind;
        f->log_count++;f->log_used+=event->length;
    }
    atomic_fetch_add(&f->pushed,1);
    gt_restore_rc_t rc=gt__attachment_push(f->attachment,sequence,event->kind,event->payload,event->length);
    CHECK(rc==GT_RESTORE_OK || rc==GT_RESTORE_IDLE || rc==GT_RESTORE_LIMIT);
    if(rc==GT_RESTORE_LIMIT)f->queue_status=rc;
}
static void output(void *context,const uint8_t *bytes,size_t length,uint64_t sequence) {
    fixture_t *f = context;
    CHECK(sequence == f->events);
    CHECK(f->stream_len + length < sizeof(f->stream));
    memcpy(f->stream + f->stream_len,bytes,length); f->stream_len += length;
    CHECK(!pthread_mutex_lock(&f->lock));
    /* Wait for the full first phase, including the incomplete sequence. */
    f->first = strstr(f->stream,first_marker(f->mode)) != NULL;
    f->second = strstr(f->stream,second_marker(f->mode)) != NULL;
    CHECK(!pthread_cond_broadcast(&f->changed));
    CHECK(!pthread_mutex_unlock(&f->lock));
}
static void completion(void *context,gt_status_t status,gt_state_capture_t capture) {
    fixture_t *f = context;
    CHECK(status == GT_OK);
    CHECK(!pthread_mutex_lock(&f->lock));
    if (capture.frozen) {
        CHECK(capture.frozen == &f->checkpoint);
        f->checkpoint.sequence = capture.sequence;
        CHECK(capture.sequence == f->checkpoint.events);
        f->captured = true;
    }
    f->completions++;
    CHECK(!pthread_cond_broadcast(&f->changed));
    CHECK(!pthread_mutex_unlock(&f->lock));
}
static void finished(void *context,gt_status_t status) {
    fixture_t *f = context;
    CHECK(!pthread_mutex_lock(&f->lock));
    f->status = status; f->finished = true;
    CHECK(!pthread_cond_broadcast(&f->changed));
    CHECK(!pthread_mutex_unlock(&f->lock));
}
/* Stop inside the first real encoder allocation while owner remains runnable. */
static void *encoding_alloc(void *context,size_t n,uint8_t a,uintptr_t ret) {
    fixture_t *f = context;
    CHECK(!pthread_mutex_lock(&f->lock));
    f->encoding = true; CHECK(!pthread_cond_broadcast(&f->changed));
    while (!f->release) CHECK(!pthread_cond_wait(&f->changed,&f->lock));
    CHECK(!pthread_mutex_unlock(&f->lock));
    const GhosttyAllocator *base = &f->encoder_base;
    return base->vtable->alloc(base->ctx,n,a,ret);
}
static bool no_resize(void *c,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {
    (void)c;(void)p;(void)n;(void)a;(void)m;(void)r;return false;
}
static void *no_remap(void *c,void *p,size_t n,uint8_t a,size_t m,uintptr_t r) {
    (void)c;(void)p;(void)n;(void)a;(void)m;(void)r;return NULL;
}
static void encoding_free(void *context,void *p,size_t n,uint8_t a,uintptr_t ret) {
    fixture_t *f=context;
    const GhosttyAllocator *base=&f->encoder_base;
    base->vtable->free(base->ctx,p,n,a,ret);
}
static gt_status_t encode_task(void *context,void **result) {
    fixture_t *f=context;
    GhosttyResult rc=p0_frozen_encode(&f->checkpoint.frozen,&f->encoder,16*MIB,&f->checkpoint.encoded);
    if(rc!=GHOSTTY_SUCCESS)return GT_ERR_IO;
    *result=&f->checkpoint.encoded;return GT_OK;
}
static void task_input_free(void *context) {
    fixture_t *f=context;
    p0_frozen_release(&f->checkpoint.frozen);
    CHECK(p0_budget_scope_close(&f->capture));
    if(!f->checkpoint.encoded.canonical) CHECK(p0_budget_scope_close(&f->encoder));
}
static void task_result_free(void *context,void *result) {
    fixture_t *f=context;CHECK(result==&f->checkpoint.encoded);
    p0_checkpoint_release(result);CHECK(p0_budget_scope_close(&f->encoder));
}
static void wait_task(gt_checkpoint_tasks_t *tasks,uint64_t id,gt_task_state_t state) {
    for(unsigned i=0;i<5000;i++) {
        gt_task_info_t info;CHECK(gt__tasks_info(tasks,id,&info)==GT_TASK_OK);
        if(info.state==state)return;
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CHECK(!"task state timeout");
}
static void roles(void) {
    p0_budget b,other; p0_budget_scope owner,cap,enc,foreign;
    CHECK(p0_budget_init(&b,128*MIB) && p0_budget_init(&other,32*MIB));
    CHECK(p0_budget_scope_admit(&owner,&b,32*MIB));
    CHECK(p0_budget_scope_admit(&cap,&b,32*MIB));
    CHECK(p0_budget_scope_admit(&enc,&b,32*MIB));
    CHECK(p0_budget_scope_admit(&foreign,&other,32*MIB));
    GhosttyTerminal terminal=NULL;
    CHECK(ghostty_terminal_p0_new_with_page_allocator(p0_budget_allocator(&owner,P0_BUDGET_GENERAL),
        p0_budget_allocator(&owner,P0_BUDGET_PAGES),&terminal,80,24) == GHOSTTY_SUCCESS);
    configure(terminal);
    p0_frozen_checkpoint frozen={0};p0_checkpoint wire={0};
    CHECK(p0_frozen_capture(&owner,&owner,terminal,16*MIB,&frozen) == GHOSTTY_INVALID_VALUE);
    CHECK(!frozen.handle);
    CHECK(p0_frozen_capture(&owner,&foreign,terminal,16*MIB,&frozen) == GHOSTTY_INVALID_VALUE);
    CHECK(!frozen.handle);
    CHECK(p0_budget_scope_close(&cap));p0_budget_scope_init(&cap,&b,32*MIB);
    CHECK(p0_frozen_capture(&owner,&cap,terminal,16*MIB,&frozen) == GHOSTTY_INVALID_VALUE);
    CHECK(!frozen.handle && p0_budget_scope_close(&cap));
    CHECK(p0_budget_scope_admit(&cap,&b,0));
    CHECK(p0_frozen_capture(&owner,&cap,terminal,16*MIB,&frozen) == GHOSTTY_LIMIT_EXCEEDED);
    CHECK(!frozen.handle && !p0_budget_scope_read(&cap).live && p0_budget_scope_close(&cap));
    CHECK(p0_budget_result(&owner,GHOSTTY_SUCCESS) == GHOSTTY_SUCCESS);
    CHECK(p0_budget_scope_admit(&cap,&b,32*MIB));
    CHECK(p0_frozen_capture(&owner,&cap,terminal,16*MIB,&frozen) == GHOSTTY_SUCCESS);
    CHECK(p0_frozen_encode(&frozen,&cap,16*MIB,&wire) == GHOSTTY_INVALID_VALUE);
    CHECK(p0_frozen_encode(&frozen,&foreign,16*MIB,&wire) == GHOSTTY_INVALID_VALUE);
    CHECK(p0_frozen_encode(&frozen,&enc,0,&wire) == GHOSTTY_LIMIT_EXCEEDED);
    CHECK(!wire.canonical && !wire.resources && !p0_budget_scope_read(&enc).live);
    CHECK(p0_budget_result(&enc,GHOSTTY_SUCCESS) == GHOSTTY_SUCCESS);
    ghostty_terminal_free(terminal);CHECK(p0_budget_scope_close(&owner));
    CHECK(p0_frozen_encode(&frozen,&enc,16*MIB,&wire) == GHOSTTY_SUCCESS);
    p0_checkpoint_release(&wire);p0_frozen_release(&frozen);
    CHECK(p0_budget_scope_close(&cap) && p0_budget_scope_close(&enc) && p0_budget_scope_close(&foreign));
    p0_budget_usage all=p0_budget_read(&b);CHECK(!all.live && !all.committed && all.mapped==all.unmapped);
    CHECK(p0_budget_destroy(&b) && p0_budget_destroy(&other));
    puts("PASS worker/libVT/roles");
}
#ifdef GHOSTERM_P0_FULL_SURFACE_REPLAY
#include "surface-restore-probe.h"
static bool gui_budget_healthy(void *context) {
    fixture_t *f=context;
    return p0_budget_result(&f->gui_general,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS &&
           p0_budget_result(&f->gui_pages,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS;
}
static bool gui_freeze_budget(void *context) {
    fixture_t *f=context;
    for(unsigned i=0;i<32;i++) {
        size_t live=p0_budget_scope_read(&f->gui_general).live;
        if(p0_budget_scope_set_limit(&f->gui_general,live)) {
            if(!strcmp(f->mode,"gui-revoked")) {
                const GhosttyAllocator *a=p0_budget_allocator(&f->gui_general,P0_BUDGET_GENERAL);
                CHECK(!a->vtable->alloc(a->ctx,1,0,0));
            }
            return true;
        }
    }
    return false;
}
/* gui-live：恢复进行到排空阶段时，权威 worker 还活着。emit 让它再做一次
 * resize，并等到事件真的进了恢复流；quiesce 停掉它，交出最终状态。两者都在
 * surface 的消费线程上被调用。 */
static uint64_t authority_emit(void *context) {
    fixture_t *f=context;
    gt_attachment_restore_info_t info;
    CHECK(gt__attachment_restore_info(f->attachment,f->generation,&info)==GT_RESTORE_OK);
    const uint64_t before=info.last;
    gt_state_size_t size={100,40,1000,800};
    CHECK(gt__worker_submit_internal(f->worker,GT_WORKER_RESIZE,&size,sizeof(size),completion,f)==GT_WORKER_ACCEPTED);
    for(unsigned i=0;i<5000;i++) {
        CHECK(gt__attachment_restore_info(f->attachment,f->generation,&info)==GT_RESTORE_OK);
        if(info.last>before)break;
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CHECK(info.last>before);
    return info.last;
}
/* gui-slow：客户端停在排空中途不再取事件，权威却还在跑。一直提交 resize，直到
 * 有界队列放不下为止——权威不会被慢客户端堵住，溢出只作废这一代。 */
static uint64_t authority_flood(void *context) {
    fixture_t *f=context;
    gt_attachment_restore_info_t info;
    CHECK(gt__attachment_restore_info(f->attachment,f->generation,&info)==GT_RESTORE_OK);
    CHECK(info.phase==GT_RESTORE_DRAINING);
    CHECK(!pthread_mutex_lock(&f->lock));const size_t done=f->completions;CHECK(!pthread_mutex_unlock(&f->lock));
    size_t sent=0;
    for(unsigned i=0;i<2000;i++) {
        gt_state_size_t size={(uint16_t)(90+i%16),(uint16_t)(30+i%8),900,600};
        gt_worker_submit_t rc=gt__worker_submit_internal(f->worker,GT_WORKER_RESIZE,&size,sizeof(size),completion,f);
        CHECK(rc==GT_WORKER_ACCEPTED || rc==GT_WORKER_BACKPRESSURE);
        if(rc==GT_WORKER_ACCEPTED)sent++;
        CHECK(gt__attachment_restore_info(f->attachment,f->generation,&info)==GT_RESTORE_OK);
        if(info.phase==GT_RESTORE_FAILED)break;
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CHECK(info.phase==GT_RESTORE_FAILED && info.failure==GT_RESTORE_LIMIT);
    /* 队列满之后权威还要能继续推进，否则「慢客户端不拖死会话」就没被证明。 */
    const uint64_t stuck=atomic_load(&f->pushed);
    gt_state_size_t size={88,28,880,560};
    for(unsigned i=0;i<5000 && atomic_load(&f->pushed)<=stuck;i++) {
        gt_worker_submit_t rc=gt__worker_submit_internal(f->worker,GT_WORKER_RESIZE,&size,sizeof(size),completion,f);
        CHECK(rc==GT_WORKER_ACCEPTED || rc==GT_WORKER_BACKPRESSURE);
        if(rc==GT_WORKER_ACCEPTED)sent++;
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CHECK(atomic_load(&f->pushed)>stuck);
    /* 停机要排空已提交的命令，否则 quiesce 会把它们判成取消。 */
    for(unsigned i=0;i<5000;i++) {
        CHECK(!pthread_mutex_lock(&f->lock));const size_t now=f->completions;CHECK(!pthread_mutex_unlock(&f->lock));
        if(now>=done+sent)break;
        const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CHECK(!pthread_mutex_lock(&f->lock));CHECK(f->completions==done+sent);CHECK(!pthread_mutex_unlock(&f->lock));
    return atomic_load(&f->pushed);
}
static gt_surface_checkpoint_view_t authority_final;
static const gt_surface_checkpoint_view_t *authority_quiesce(void *context) {
    fixture_t *f=context;
    gt__worker_stop(f->worker);gt__worker_join(f->worker);
    authority_final=(gt_surface_checkpoint_view_t){f->final.canonical,f->final.resources,
                                                  f->final.canonical_len,f->final.resources_len};
    return &authority_final;
}
static void replay_checkpoint(fixture_t *f,uint64_t generation,const p0_checkpoint *checkpoint) {
    const bool emit=!strcmp(f->mode,"gui-live"),flood=!strcmp(f->mode,"gui-slow");
    const bool live=emit||flood;
    CHECK(p0_budget_scope_admit(&f->gui_general,&f->budget,!strcmp(f->mode,"gui-denied") ? 0 : 64*MIB));
    CHECK(p0_budget_scope_admit(&f->gui_pages,&f->budget,!strcmp(f->mode,"gui-pages") ? 0 : 64*MIB));
    gt_surface_probe_t probe={.attachment=f->attachment,.mode=f->mode,.generation=generation,
        .snapshot=f->checkpoint.sequence,.last=live ? 0 : f->events,
        .cell_width=PROBE_CELL_WIDTH,.cell_height=PROBE_CELL_HEIGHT,
        .authority_context=f,.authority_emit=emit ? authority_emit : NULL,
        .authority_flood=flood ? authority_flood : NULL,
        .authority_quiesce=live ? authority_quiesce : NULL,
        .general_allocator=p0_budget_allocator(&f->gui_general,P0_BUDGET_GENERAL),
        .page_allocator=p0_budget_allocator(&f->gui_pages,P0_BUDGET_PAGES),
        .budget_context=f,.budget_healthy=gui_budget_healthy,.freeze_budget=gui_freeze_budget,
        .initial={checkpoint->canonical,checkpoint->resources,checkpoint->canonical_len,checkpoint->resources_len},
        .final={f->final.canonical,f->final.resources,f->final.canonical_len,f->final.resources_len}};
    ghosterm_probe_surface_replay(&probe);
    p0_budget_usage general,pages;
    for(unsigned tick=0;;tick++) { /* teardown returns budget on the app tick, not on wall clock */
        general=p0_budget_scope_read(&f->gui_general);pages=p0_budget_scope_read(&f->gui_pages);
        if(tick==100 || (!general.live && !pages.live && general.mapped==general.unmapped && pages.mapped==pages.unmapped))break;
        ghosterm_probe_surface_tick(0.01);
    }
    if(general.live || pages.live)fprintf(stderr,"GUI budget remaining general=%zu pages=%zu general_maps=%zu/%zu pages_maps=%zu/%zu\n",general.live,pages.live,general.mapped,general.unmapped,pages.mapped,pages.unmapped);
    CHECK(!general.live && !pages.live && general.mapped==general.unmapped && pages.mapped==pages.unmapped);
    bool fault=!strcmp(f->mode,"gui-denied") || !strcmp(f->mode,"gui-pages") || !strcmp(f->mode,"gui-growth") || !strcmp(f->mode,"gui-revoked");
    CHECK(gui_budget_healthy(f)!=fault);
    if(gt_surface_mode_gui_default(f->mode) || !strcmp(f->mode,"gui-partial"))CHECK(!general.attempts && !pages.attempts);
    else if(!strcmp(f->mode,"gui-denied"))CHECK(general.denied>0);
    else if(!strcmp(f->mode,"gui-pages"))CHECK(pages.denied>0 && general.peak>0);
    else CHECK(general.peak>0 && pages.peak>0);
    CHECK(p0_budget_result(&f->reference,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS && p0_budget_result(&f->encoder,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS);
    CHECK(p0_budget_scope_close(&f->gui_general) && p0_budget_scope_close(&f->gui_pages));
}
#else
static void replay_checkpoint(fixture_t *f,uint64_t generation,const p0_checkpoint *checkpoint) {
    GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal replica=NULL;
    CHECK(ghostty_snapshot_decoder_new_buf(NULL,&decoder,checkpoint->canonical,checkpoint->canonical_len)==GHOSTTY_SUCCESS);
    const bool retain=true;const size_t continuation=MIB;
    CHECK(ghostty_snapshot_decoder_set(decoder,GHOSTTY_SNAPSHOT_DECODER_OPT_RETAIN_CONTINUATION,&retain)==GHOSTTY_SUCCESS);
    CHECK(ghostty_snapshot_decoder_set(decoder,GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES,&continuation)==GHOSTTY_SUCCESS);
    CHECK(ghostty_snapshot_decoder_decode(decoder,&replica)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
    CHECK(ghostty_terminal_p0_resources_decode(replica,checkpoint->resources,checkpoint->resources_len,16*MIB)==0);
    CHECK(!gt__attachment_input_allowed(f->attachment,generation,f->checkpoint.sequence));
    CHECK(gt__attachment_ready_snapshot(f->attachment,generation,f->checkpoint.sequence)==GT_RESTORE_OK);
    uint64_t cursor=f->checkpoint.sequence;
    uint8_t bytes[16384];gt_restore_event_t event;
    for(;;) {
        gt_restore_rc_t rc=gt__attachment_next(f->attachment,generation,&event,bytes,sizeof(bytes));
        if(rc==GT_RESTORE_WAIT)break;
        CHECK(rc==GT_RESTORE_OK && event.generation==generation && event.sequence==cursor+1);cursor=event.sequence;
        if(event.kind==GT_STATE_OUTPUT) ghostty_terminal_vt_write(replica,bytes,event.length);
        else {
            CHECK(event.kind==GT_STATE_RESIZE && event.length==sizeof(gt_state_size_t));
            gt_state_size_t size;memcpy(&size,bytes,sizeof(size));
            CHECK(ghostty_terminal_resize(replica,size.cols,size.rows,0,0)==GHOSTTY_SUCCESS);
        }
    }
    uint64_t cut=0;
    CHECK(gt__attachment_live_begin(f->attachment,generation,&cut)==GT_RESTORE_OK && cut==cursor && cut==f->events);
    CHECK(!gt__attachment_input_allowed(f->attachment,generation,cut));
    CHECK(gt__attachment_ready_live(f->attachment,generation,cut)==GT_RESTORE_OK);
    CHECK(gt__attachment_input_allowed(f->attachment,generation,cut));
    CHECK(!gt__attachment_input_allowed(f->attachment,generation+1,cut));
    CHECK(!gt__attachment_input_allowed(f->attachment,generation,cut+1));
    size_t n=0;CHECK(ghostty_snapshot_encode_buf(replica,NULL,0,&n)==GHOSTTY_OUT_OF_SPACE);
    uint8_t *canonical=malloc(n);CHECK(canonical);
    CHECK(ghostty_snapshot_encode_buf(replica,canonical,n,&n)==GHOSTTY_SUCCESS);
    CHECK(n==f->final.canonical_len && !memcmp(canonical,f->final.canonical,n));free(canonical);
    size_t m=0;CHECK(ghostty_terminal_p0_resources_encode(replica,16*MIB,NULL,0,&m)==5);
    uint8_t *resources=malloc(m);CHECK(resources);
    CHECK(ghostty_terminal_p0_resources_encode(replica,16*MIB,resources,m,&m)==0);
    CHECK(m==f->final.resources_len && !memcmp(resources,f->final.resources,m));free(resources);
    ghostty_terminal_free(replica);
}
#endif
int main(int argc,char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1],"roles")) {roles();return 0;}
    fixture_t f = {.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,
                  .caller=pthread_self(),.mode=argv[1]};
    /* 恢复期间保留权威时（gui-live/gui-slow），owner 作用域在 GUI 作用域准入时
     * 还没关，总额要容得下两边同时在册。 */
    const bool live=!strcmp(f.mode,"gui-live") || !strcmp(f.mode,"gui-slow");
    CHECK(p0_budget_init(&f.budget,live ? 320*MIB : 256*MIB));
    CHECK(p0_budget_scope_admit(&f.owner,&f.budget,64*MIB));
    CHECK(p0_budget_scope_admit(&f.capture,&f.budget,64*MIB));
    CHECK(p0_budget_scope_admit(&f.reference,&f.budget,64*MIB));
    CHECK(p0_budget_scope_admit(&f.encoder,&f.budget,64*MIB));
    gt_checkpoint_tasks_t *tasks=gt__tasks_open(1);CHECK(tasks);
    /* gui-slow 要让慢客户端在排空中途把队列撑满，事件上限收到够小。 */
    gt_session_attachment_t *attachment=gt__attachment_open(tasks,!strcmp(f.mode,"overflow") ? 8 : 65536,!strcmp(f.mode,"gui-slow") ? 8 : 64);CHECK(attachment);
    uint64_t generation=0;CHECK(gt__attachment_attach(attachment,&generation)==GT_TASK_OK);
    uint64_t active_generation=generation;f.attachment=attachment;f.generation=generation;
    gt_worker_config_t config = {.context=&f,.open_backend=backend_open,.open_state=state_open,
        .close_state=state_close,.state_ops={state_apply,state_freeze},.output=output,.event=authority_event,.finished=finished};
    gt_session_worker_t *worker = gt__worker_start(&config); CHECK(worker);
    f.worker=worker;
    wait_for(&f,&f.first);
    gt_state_size_t size = {91,31,910,620};
    if (!strcmp(f.mode,"resize"))
        CHECK(gt__worker_submit_internal(worker,GT_WORKER_RESIZE,&size,sizeof(size),completion,&f) == GT_WORKER_ACCEPTED);
    CHECK(gt__worker_submit_internal(worker,GT_WORKER_CAPTURE,NULL,0,completion,&f) == GT_WORKER_ACCEPTED);
    wait_for(&f,&f.captured);
    CHECK(f.checkpoint.sequence > 0);
    const GhosttyAllocator *base = p0_budget_allocator(&f.encoder,P0_BUDGET_SCRATCH);
    f.encoder_base = *base;
    static const GhosttyAllocatorVtable callbacks = {encoding_alloc,no_resize,no_remap,encoding_free};
    f.encoder.allocators[P0_BUDGET_SCRATCH] = (GhosttyAllocator){.ctx=&f,.vtable=&callbacks};
    gt_task_spec_t spec={&f,encode_task,task_input_free,task_result_free,7,f.checkpoint.sequence};
    uint64_t task_id=0;CHECK(gt__attachment_submit(attachment,generation,&spec,&task_id)==GT_TASK_OK);
    wait_for(&f,&f.encoding);
    bool cancelled=!strcmp(f.mode,"cancel") || !strcmp(f.mode,"overflow");
    if(!strcmp(f.mode,"cancel")) {
        CHECK(gt__attachment_detach(attachment,generation)==GT_TASK_OK);
        CHECK(!gt__attachment_current(attachment,generation));
        CHECK(gt__attachment_attach(attachment,&active_generation)==GT_TASK_OK && active_generation>generation);
        gt_task_info_t info;CHECK(gt__tasks_info(tasks,task_id,&info)==GT_TASK_OK && info.state==GT_TASK_CANCELLING);
        CHECK(f.checkpoint.frozen.handle && !f.capture.closed && !f.encoder.closed);
        CHECK(p0_budget_scope_read(&f.capture).live>0);
        CHECK(!pthread_mutex_lock(&f.lock));CHECK(!f.finished);CHECK(!pthread_mutex_unlock(&f.lock));
    }
    if(strcmp(f.mode,"idle")) {
        CHECK(gt__worker_submit_internal(worker,GT_WORKER_RESIZE,&size,sizeof(size),completion,&f) == GT_WORKER_ACCEPTED);
        const char *keys=detached_keys(f.mode);
        CHECK(gt__worker_submit_internal(worker,GT_WORKER_INPUT,keys,strlen(keys),completion,&f) == GT_WORKER_ACCEPTED);
        wait_for(&f,&f.second);
    }
    /* 保留权威的用例在恢复进行时不停机：停机交给 surface 消费线程。 */
    if(!live) {gt__worker_stop(worker); gt__worker_join(worker);}
    const bool caps=!strcmp(f.mode,"query-caps");
    const bool isolation=!strcmp(f.mode,"isolation");
    const bool clipboard=!strcmp(f.mode,"clipboard");
    /* caps：DA1 11 + DA2 10 + XTVERSION 16 + DECRQM 11；isolation：配色应答 9。
     * 一问一答，不多答也不少答。 */
    unsigned expected_replies=caps ? 4u : !strcmp(f.mode,"query-delta") ? 2u : isolation || clipboard ? 2u : !strcmp(f.mode,"query") ? 1u : 0u;
    const size_t expected_reply_bytes=caps ? 48u : isolation ? 18u : clipboard ? 34u : expected_replies*6u;
    if(!live) {
        CHECK(f.finished && f.status == GT_OK && f.terminal == NULL);
        CHECK(f.reply_calls==expected_replies && f.reply_bytes==expected_reply_bytes);
    }
    /* 通知和配色查询各发两次，一次在 detach 前（进快照）、一次在 detach 后
     * （进增量流）。进增量流的那一份会被原样重放给恢复副本，副本不许再弹通知、
     * 也不许再答一次配色——那个零由 surface 探针的 effects/legacy_writes 断言。 */
    CHECK(f.notifications==(isolation ? 2u : 0u) && f.scheme_calls==(isolation ? 2u : 0u));
    CHECK(f.notification_bytes==(isolation ? strlen("HELLONOTE")+strlen("SECONDNOTE") : 0u));
    /* 剪贴板同理：写两次、读两次，detach 后的那一轮进增量流。 */
    CHECK(f.clip_writes==(clipboard ? 2u : 0u) && f.clip_reads==(clipboard ? 2u : 0u));
    CHECK(f.clip_write_bytes==(clipboard ? strlen("HELLO")+strlen("SECONDCLIP") : 0u));
    if(!strcmp(f.mode,"overflow")) {
        gt_attachment_restore_info_t info;
        CHECK(gt__attachment_restore_info(attachment,generation,&info)==GT_RESTORE_OK);
        CHECK(info.phase==GT_RESTORE_FAILED && info.failure==GT_RESTORE_LIMIT && f.queue_status==GT_RESTORE_LIMIT);
        CHECK(!gt__attachment_current(attachment,generation));
        CHECK(gt__attachment_attach(attachment,&active_generation)==GT_TASK_OK && active_generation>generation);
    }
    if(cancelled) CHECK(f.events>f.checkpoint.sequence);
    CHECK(!pthread_mutex_lock(&f.lock));f.release=true;CHECK(!pthread_cond_broadcast(&f.changed));CHECK(!pthread_mutex_unlock(&f.lock));
    p0_checkpoint *expected=&f.checkpoint.expected,*actual=&f.checkpoint.encoded;
    if(cancelled) {
        wait_task(tasks,task_id,GT_TASK_CANCELLED);
        gt_attachment_checkpoint_t hidden;
        CHECK(gt__attachment_take(attachment,generation,&hidden)==GT_TASK_INVALID && !hidden.result);
        CHECK(gt__attachment_take(attachment,active_generation,&hidden)==GT_TASK_INVALID && !hidden.result);
        CHECK(!actual->canonical && !actual->resources && !f.checkpoint.frozen.handle);
        goto cleanup;
    }
    wait_task(tasks,task_id,GT_TASK_READY);
    gt_attachment_checkpoint_t lease;
    CHECK(gt__attachment_take(attachment,generation,&lease)==GT_TASK_OK);
    CHECK(lease.result==actual && lease.sequence==f.checkpoint.sequence && lease.generation==generation);
    CHECK(actual->canonical_len == expected->canonical_len && actual->resources_len == expected->resources_len);
    CHECK(!memcmp(actual->canonical,expected->canonical,actual->canonical_len));
    CHECK(!memcmp(actual->resources,expected->resources,actual->resources_len));
    /* A separate terminal receives the known child prefix, independent of the
     * worker's state callback and the synchronous source encoding oracle. */
    GhosttyTerminal mirror=NULL;
    CHECK(ghostty_terminal_new(NULL,&mirror,80,24) == GHOSTTY_SUCCESS);
    configure(mirror);
    if (mode_tui(f.mode)) {
        for (size_t i=0;i<f.log_count && f.log[i].sequence<=f.checkpoint.sequence;i++) {
            const uint8_t *payload=f.log_bytes+f.log[i].offset;
            if (f.log[i].kind == GT_STATE_OUTPUT) ghostty_terminal_vt_write(mirror,payload,f.log[i].length);
            else {
                gt_state_size_t logged;memcpy(&logged,payload,sizeof(logged));
                CHECK(ghostty_terminal_resize(mirror,logged.cols,logged.rows,PROBE_CELL_WIDTH,PROBE_CELL_HEIGHT) == GHOSTTY_SUCCESS);
            }
        }
        /* 没进副屏、或主屏没留下 scrollback，这个用例就没覆盖到新东西。 */
        GhosttyTerminalScreen screen=GHOSTTY_TERMINAL_SCREEN_PRIMARY;size_t scrollback=0;
        CHECK(ghostty_terminal_get(mirror,GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,&screen) == GHOSTTY_SUCCESS);
        CHECK(ghostty_terminal_get(mirror,GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS,&scrollback) == GHOSTTY_SUCCESS);
        CHECK(screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE && !scrollback);
        /* 副屏自己没有 scrollback，所以 SCROLL001 只可能来自主屏滚出去的历史：
         * 它出现在 checkpoint 里，才说明这一跳带走了看不见的那部分。 */
        CHECK(contains(actual->canonical,actual->canonical_len,"SCROLL001"));
    } else {
        const char *initial=prefix(f.mode);
        ghostty_terminal_vt_write(mirror,(const uint8_t *)initial,strlen(initial));
        if (!strcmp(f.mode,"resize")) CHECK(ghostty_terminal_resize(mirror,91,31,PROBE_CELL_WIDTH,PROBE_CELL_HEIGHT) == GHOSTTY_SUCCESS);
    }
    size_t oracle_len=0;
    CHECK(ghostty_snapshot_encode_buf(mirror,NULL,0,&oracle_len) == GHOSTTY_OUT_OF_SPACE);
    uint8_t *oracle=malloc(oracle_len);CHECK(oracle);
    CHECK(ghostty_snapshot_encode_buf(mirror,oracle,oracle_len,&oracle_len) == GHOSTTY_SUCCESS);
    CHECK(oracle_len == actual->canonical_len && !memcmp(oracle,actual->canonical,oracle_len));
    free(oracle);ghostty_terminal_free(mirror);
    if (!strcmp(f.mode,"resources")) {
        GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal restored=NULL;
        CHECK(ghostty_snapshot_decoder_new_buf(NULL,&decoder,actual->canonical,actual->canonical_len) == GHOSTTY_SUCCESS);
        CHECK(ghostty_snapshot_decoder_decode(decoder,&restored) == GHOSTTY_SUCCESS);
        ghostty_snapshot_decoder_free(decoder);
        CHECK(ghostty_terminal_p0_resources_decode(restored,actual->resources,actual->resources_len,16*MIB) == 0);
        GhosttyKittyGraphics graphics=NULL;
        CHECK(ghostty_terminal_get(restored,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics) == GHOSTTY_SUCCESS);
        GhosttyKittyGraphicsImage image=ghostty_kitty_graphics_image(graphics,7);CHECK(image);
        const uint8_t *pixels=NULL;size_t n=0;
        CHECK(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels) == GHOSTTY_SUCCESS);
        CHECK(ghostty_kitty_graphics_image_get(image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&n) == GHOSTTY_SUCCESS);
        const uint8_t red[]={255,0,0,255};CHECK(n==sizeof(red) && !memcmp(pixels,red,n));
        ghostty_terminal_free(restored);
    }
    replay_checkpoint(&f,generation,actual);
    if(live) {
        /* 权威是被恢复线程停掉的，停完它的收尾契约仍然要成立。 */
        CHECK(f.finished && f.status == GT_OK && f.terminal == NULL);
        CHECK(f.reply_calls==expected_replies && f.reply_bytes==expected_reply_bytes);
        CHECK(f.events>f.checkpoint.sequence);
        CHECK(f.queue_status==(!strcmp(f.mode,"gui-slow") ? GT_RESTORE_LIMIT : GT_RESTORE_OK));
    }
    /* 抢注的客户端在恢复全程只拿到 BUSY，在位的一代让出之后它才接得上。 */
    if(!strcmp(f.mode,"gui-race")) {
        uint64_t rival=99;
        CHECK(gt__attachment_attach(attachment,&rival)==GT_TASK_BUSY && !rival);
        CHECK(gt__attachment_detach(attachment,generation)==GT_TASK_OK && !gt__attachment_current(attachment,generation));
        CHECK(gt__attachment_attach(attachment,&active_generation)==GT_TASK_OK && active_generation>generation);
    }
    /* 恢复中断、慢客户端溢出都只作废这一代，槽位可以立刻被新一代占用。 */
    if(!strcmp(f.mode,"gui-interrupt") || !strcmp(f.mode,"gui-slow")) {
        CHECK(gt__attachment_attach(attachment,&active_generation)==GT_TASK_OK && active_generation>generation);
        CHECK(!gt__attachment_current(attachment,generation));
    }
    CHECK(gt__attachment_release(attachment,&lease)==GT_TASK_OK);
    wait_task(tasks,task_id,GT_TASK_FINISHED);
cleanup:
    if(gt__attachment_current(attachment,active_generation)) CHECK(gt__attachment_detach(attachment,active_generation)==GT_TASK_OK);
    gt_attachment_retired_t retired;
    CHECK(gt__attachment_reap(attachment,&retired)==GT_TASK_OK);
    CHECK(retired.task==task_id && retired.generation==generation && retired.context==&f);
    CHECK(retired.state==(cancelled ? GT_TASK_CANCELLED : GT_TASK_FINISHED));
    CHECK(gt__attachment_close(attachment)==GT_TASK_OK && gt__tasks_close(tasks)==GT_TASK_OK);
    p0_checkpoint_release(expected);p0_checkpoint_release(&f.final);
    CHECK(f.capture.closed && f.encoder.closed && p0_budget_scope_close(&f.reference));
    p0_budget_usage usage=p0_budget_read(&f.budget);
    CHECK(!usage.live && !usage.committed && usage.mapped == usage.unmapped && p0_budget_destroy(&f.budget));
    CHECK(!pthread_cond_destroy(&f.changed) && !pthread_mutex_destroy(&f.lock));
    printf("PASS worker/libVT/%s S=%llu final=%llu\n",argv[1],(unsigned long long)f.checkpoint.sequence,(unsigned long long)f.events);
    return 0;
}
