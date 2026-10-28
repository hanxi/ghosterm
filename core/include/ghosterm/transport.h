/*
 * ghosterm/transport.h — 字节流传输（本地 PTY / SSH shell）的统一接缝
 *
 * 这是整套架构的**核心 seam**：TerminalSurface 只认 transport，
 * 不关心背后是本地 shell 还是网络 SSH。
 *
 *          ┌──────────────┐  write(用户输入)   ┌─────────────┐
 *          │   Terminal   │ ─────────────────► │  transport  │
 *          │   Surface    │ ◄───────────────── │             │
 *          └──────────────┘  read(远端输出)     └─────────────┘
 *
 * 线程纪律（铁律 #1/#2）：
 *   - read/write/resize 由**会话线程**独占调用，不跨线程。
 *   - 字节流**不进事件队列**、**不过 UI 线程**；事件队列只承载低频状态。
 *   - read 是阻塞 + 超时，调用方在一次循环里 read → 送 surface。
 *   - 实现内部可能有后台线程（如 PTY 的 waitpid 收尸线程），但它只发
 *     低频事件，绝不搬运字节流。
 */
#ifndef GHOSTERM_TRANSPORT_H
#define GHOSTERM_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ghosterm/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gt_transport gt_transport_t;
typedef struct gt_ssh gt_ssh_t;

typedef enum {
  GT_TRANSPORT_LOCAL_PTY = 0,
  GT_TRANSPORT_SSH_SHELL = 1,
} gt_transport_kind_t;

/*
 * 要起的是「用户的交互 shell」还是「一条具体命令」。
 *
 * 这**不能**从 `executable` 给没给推断出来：用户在设置里挑了 /bin/zsh，
 * 那依然是他的交互 shell，照样要有登录语义（macOS 上 PATH 全靠它，见
 * transport.c 里 macos_login_dash0 的长注释）。早先就是拿「给了 exe」
 * 当「不要登录 shell」用，于是分屏那格的 brew 全不见了——屏幕上不报错，
 * 只是 ~/.zshrc 里一片 command not found。
 *
 * 上游同样是二元 tag 而不是推断：Ghostty 的 `union(enum){ shell, direct }`
 * 只对 `.shell` 做 `exec -l`；iTerm2 把「自定义 shell」与「自定义命令」
 * 做成两个菜单项，后者另配一个「先过一遍登录 shell」开关。
 *
 * **默认（零值）是 SHELL**：终端的常态是起交互 shell，要跑命令的调用方
 * 必须显式说 DIRECT（铁律 #6：不靠推断、调用点必须表态）。
 */
typedef enum {
  GT_PTY_CMD_SHELL = 0,  /* 用户的交互 shell → 起成登录 shell */
  GT_PTY_CMD_DIRECT = 1, /* 跑一条具体命令 → 不加任何登录语义 */
} gt_pty_cmd_kind_t;

typedef struct {
  const char *executable; /* NULL → 平台默认 shell */
  const char *const *argv; /* NULL 结尾，不含 argv[0]；NULL → 单参数 shell */
  const char *const *env;  /* NULL 结尾的 "K=V"；NULL → 平台默认 */
  const char *cwd;         /* NULL → 家目录 */
  const char *term;        /* NULL → "xterm-256color" */
  gt_pty_cmd_kind_t cmd_kind; /* 零值 = GT_PTY_CMD_SHELL，见上 */
  uint16_t cols, rows;
  uint32_t px_width, px_height;
} gt_pty_opts_t;

typedef struct {
  const char *term; /* NULL → "xterm-256color" */
  uint16_t cols, rows;
  uint32_t px_width, px_height;
} gt_shell_opts_t;

/* 打开本地 PTY。POSIX=forkpty；Windows=ConPTY；iOS/HarmonyOS 不支持。 */
gt_transport_t *gt_transport_open_pty(const gt_pty_opts_t *opts);

/* 在已连接的 SSH 会话上开交互式 shell（pty-req + shell）。 */
gt_transport_t *gt_transport_open_ssh_shell(gt_ssh_t *ssh, const gt_shell_opts_t *opts);

/* Dedicated authenticated owner; PTY request followed by exec, never shell injection.
 * timeout_ms=0 means 5000ms; cancelled may be NULL, invoked on the IO owner. */
gt_transport_t *gt_transport_open_ssh_command(gt_ssh_t *ssh, const char *command,
    const gt_shell_opts_t *opts, uint32_t timeout_ms, bool (*cancelled)(void *), void *context, gt_status_t *out_status);

/*
 * 写入用户输入。**绝不无限阻塞**（M1 修正）。
 *
 * 契约：尽量把 data 交给对端；本地 PTY 的输入队列满（前台程序暂时不读
 * stdin）时，**剩下的字节在 transport 内部排队**，由后续 read（会话线程每次
 * 读都顺带补送）冲出去——与真实终端的流控同语义：输入不丢，只是晚一点送到。
 *
 * 为什么必须如此（实测根因）：pty 输入队列很小；旧实现在队满时
 * `poll(POLLOUT, -1)` 无限等。那个子进程永不读时，**会话线程被永久钉死**：
 * 读循环再也出不来（终端画面冻住），而宿主关会话时 `queue.sync` 恰好在等
 * 这条线程 → **主线程一起卡死（整个 app 无响应）**。故写是有界的：
 * 队列满最多等一小段就把余量攒起来，绝不把会话线程拖住。
 *
 * 返回 GT_OK 表示「已接受」（可能仍在内部排队）；返回负的错误码表示真失败。
 * read/write/resize 由同一会话线程独占调用，不跨线程。
 */
int gt_transport_write(gt_transport_t *t, const uint8_t *data, size_t len);
int gt_transport_resize(gt_transport_t *t, uint16_t cols, uint16_t rows,
                        uint32_t px_width, uint32_t px_height);

/*
 * gt_transport_read 的三态返回（铁律 #6：不得把超时折叠成 EOF）。
 * 返回值 >0 = 读到的字节数。
 */
enum {
  GT_TRANSPORT_READ_TIMEOUT = -2, /* 超时：本次无数据，可安全重试 */
  GT_TRANSPORT_READ_ERROR = -1,   /* 错误：详见 gt_last_error() */
  GT_TRANSPORT_READ_EOF = 0,      /* EOF：对端已退出且缓冲数据已排空 */
};

/*
 * 阻塞读取，最多等 timeout_ms（<0 = 永久）。返回值见上面的三态枚举。
 * 实现须保证「EOF 前的最后一批数据不丢」——即仅在对端退出**且**已排空后
 * 才返回 EOF；超时返回 GT_TRANSPORT_READ_TIMEOUT，绝不冒充 EOF。
 * read/write/resize 由同一会话线程独占调用，不跨线程。
 */
int gt_transport_read(gt_transport_t *t, uint8_t *buf, size_t cap, int timeout_ms);

/*
 * 关闭并释放句柄（含 join 后台线程）。close(NULL) 安全。
 * 关闭后再使用同一句柄是未定义行为（句柄已释放）。
 */
void gt_transport_close(gt_transport_t *t);

/* 仅在退出码已确认时返回 true 并写入 exit_code（可为 NULL）。
 * EOF 独立由 read 报告；SSH 缺退出状态、PTY 被外部回收时返回 false，
 * 不能把未知状态猜成 0。 */
bool gt_transport_exited(const gt_transport_t *t, int *exit_code);

/*
 * 子进程 / 前台作业的判定四态（铁律 #6：不折叠）。
 *
 * 用途：关闭标签页 / 分屏时的「有没有东西会被打断」判据——一个空闲的
 * shell（停在提示符）关掉没什么可丢，不该拦；前台有作业在跑才拦。
 *
 *   UNKNOWN —— 不能判定（SSH shell：远端前台本地看不见；ConPTY；查询失败）。
 *              **必须**当作「可能有作业」处理，绝不折叠成 IDLE。
 *   EXITED  —— 子进程已退出（没什么可丢）。
 *   IDLE    —— 活着，且前台进程组就是 shell 自己（停在提示符）。
 *   JOB     —— 活着，且前台是别的进程组（有作业在跑，含管道/TUI）。
 *
 * 实现事实来源：本地 PTY 读内核 tcgetpgrp(master)。这要求子进程**真的**
 * 拿到了控制终端（见 gt_transport_open_pty 里关于不能再 setpgid 的说明）。
 */
typedef enum {
  GT_TRANSPORT_CHILD_UNKNOWN = 0,
  GT_TRANSPORT_CHILD_EXITED = 1,
  GT_TRANSPORT_CHILD_IDLE = 2,
  GT_TRANSPORT_CHILD_JOB = 3,
} gt_transport_child_t;

gt_transport_child_t gt_transport_child_state(const gt_transport_t *t);

/*
 * 子进程的当前工作目录（**本地 PTY 才有**）。
 *
 * 成功写入 NUL 结尾的绝对路径返回 true；不可用返回 false 并置
 * `gt_last_error()`——**不猜**（铁律 #6）。不可用的情形：SSH shell（远端
 * cwd 本地看不见，走表面侧的 OSC 7，不在这里）/ 平台不支持 / 子进程已退出 /
 * 缓冲区太小。
 *
 * 只读**内核事实**（macOS `proc_pidinfo`、Linux `/proc/<pid>/cwd`），不依赖
 * shell 发任何转义序列，故本地 shell 无需 shell 集成也能拿到。缓冲区建议
 * 至少 PATH_MAX 大小。
 */
bool gt_transport_child_cwd(const gt_transport_t *t, char *buf, size_t cap);

gt_transport_kind_t gt_transport_kind(const gt_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* GHOSTERM_TRANSPORT_H */
