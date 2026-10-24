/* Test-only POD boundary: VT and full engine headers have colliding names and
 * must remain in separate translation units. Views live through replay return. */
#ifndef GT_SURFACE_RESTORE_PROBE_H
#define GT_SURFACE_RESTORE_PROBE_H
#include "session/worker.h"
#include "session/attachment.h"
#include <string.h>
/* Modes whose surface imports on the host default allocators instead of the
 * admitted GUI budget providers. */
static inline bool gt_surface_mode_gui_default(const char *mode) {
    return !strcmp(mode,"gui-default") || !strcmp(mode,"gui-default-utf8");
}
/* 恢复出来的会话有没有开 DEC 2004，决定 GUI 输入该不该被括起来：真实
 * TUI 里 vim 开了、less 没开，这由被恢复的状态决定，不由探针选。 */
static inline bool gt_surface_mode_bracketed_paste(const char *mode) {
    return strcmp(mode,"utf8") && strcmp(mode,"gui-default-utf8") &&
           strcmp(mode,"idle") && strcmp(mode,"tui-less");
}
typedef struct {
    const uint8_t *canonical,*resources;
    size_t canonical_len,resources_len;
} gt_surface_checkpoint_view_t;
typedef struct {
    gt_session_attachment_t *attachment;
    const char *mode;
    uint64_t generation,snapshot,last;
    uint32_t cell_width,cell_height;
    const void *general_allocator,*page_allocator;
    void *budget_context;
    bool (*budget_healthy)(void *);
    bool (*freeze_budget)(void *);
    /* 恢复期间权威仍在跑（gui-live）：emit 在 READY(S) 之后让权威再产生一个
     * 事件并返回新的 last，quiesce 停掉权威并交出它最后的状态。两者都在消费
     * 线程上调用；emit 非空就表示这一代是带活权威的恢复。 */
    uint64_t (*authority_emit)(void *);
    /* 慢客户端（gui-slow）：客户端停在排空中途不再取事件，flood 让权威一直推
     * 到有界队列放不下为止，返回权威这时已经产生的事件总数。 */
    uint64_t (*authority_flood)(void *);
    const gt_surface_checkpoint_view_t *(*authority_quiesce)(void *);
    void *authority_context;
    gt_surface_checkpoint_view_t initial,final;
} gt_surface_probe_t;
void ghosterm_probe_surface_replay(const gt_surface_probe_t *);
/* Surface teardown returns host budget only while the app ticks; a plain sleep
 * never balances the ledger, so the caller drains with this. */
void ghosterm_probe_surface_tick(double seconds);
int ghosterm_worker_surface_fixture_main(int,char **);
#endif
