/*
 * CGhostty shim — Swift ↔ libghostty 的 C 桥。
 *
 * 为什么需要它（而不是让 Swift 直接调 ghostty_*）：
 *   1. `ghostty_runtime_config_s` / `ghostty_surface_config_s` 里嵌函数指针与
 *      匿名 union，Swift 侧逐个摆弄易错且噪音大；
 *   2. 嵌入侧只关心 4 件事（喂输出、收输入、收 resize、收标题），其余
 *      （clipboard / action / wakeup）在 M1 是明确的 no-op，写在这里比散在
 *      Swift 里更好审；
 *   3. 这里**不含任何业务语义**——字节流怎么来（SSH / 本地）由调用方决定。
 *
 * 线程纪律：write/resize 回调由 libghostty 的 termio 线程调用；
 * process_output / set_size / draw 必须从主线程调用（libghostty 约束）。
 */
#ifndef GHOSTTY_HOST_H
#define GHOSTTY_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gt_ghostty_app gt_ghostty_app_t;
typedef struct gt_ghostty_surface gt_ghostty_surface_t;

/*
 * 宿主回调。全部由 libghostty 内部线程调用：
 *   write   —— 终端编码后的用户输入，调用方负责写进自己的 transport；
 *   resize  —— 网格尺寸变化（列/行），调用方应转成 SSH window-change 或 pty resize；
 *   title   —— 终端标题变化（OSC 0/2）；可为 NULL；
 *   program_status —— 程序状态报告（OSC 7501）；可为 NULL；
 *   semantic_prompt —— shell 集成事件（OSC 133）；可为 NULL；
 *   pwd     —— 终端的工作目录（OSC 7）；可为 NULL。**注意**：OSC 7 由
 *              **shell 集成**发出，没有集成就没有这个回调——宿主不能假定
 *              它一定来（本地 cwd 另有内核查询，见 gt_transport_child_cwd）。
 *
 * program_status 的语义（与上游 OSC 7501 规范一致）：
 *   - 由 libghostty 的 **termio 线程同步**调用（与 write/resize 同一线程）；
 *   - report 及其字符串**只在回调期间有效**，要留存请自行拷贝；
 *   - **注册它**同时意味着终端会回答协议的 `OSC 7501 ; ?` 支持查询；
 *     不注册就不回答——程序据此判定「此终端不支持」，我们不假装支持。
 */
typedef struct {
  void *userdata;
  void (*write)(void *ud, const uint8_t *data, size_t len);
  void (*resize)(void *ud, uint16_t cols, uint16_t rows);
  void (*title)(void *ud, const char *title);
  void (*program_status)(void *ud, const ghostty_program_status_s *report);
  /* OSC 133：与 program_status 同线程纪律，事件只在回调期间有效。 */
  void (*semantic_prompt)(void *ud, const ghostty_semantic_prompt_s *event);
  /* OSC 7 工作目录；字符串只在回调期间有效，要留存请自行拷贝。 */
  void (*pwd)(void *ud, const char *pwd);
  /* Replica inputs carry the generation/actual engine sequence at encoding. */
  void (*write_stamped)(void *ud, uint64_t generation, uint64_t sequence,
                        const uint8_t *bytes, size_t length);
} gt_host_callbacks_t;

/* 创建 app（内含默认配置 + 最小 runtime 回调）。失败返回 NULL 并置错误串。 */
gt_ghostty_app_t *gt_ghostty_app_new(void);
void gt_ghostty_app_free(gt_ghostty_app_t *app);

/* 必须在主线程周期调用（由 wakeup_cb 或定时器驱动）。 */
void gt_ghostty_app_tick(gt_ghostty_app_t *app);

/*
 * app 级焦点：告诉 libghostty「整个 App 是不是活动 App」。
 *
 * 与 surface 级焦点（见下）是两回事：这一档管的是 **App 范围**的语义
 * （如今后可能接的全局快捷键）。上游 macOS 应用在 applicationDidBecomeActive /
 * DidResignActive 时各报一次（Ghostty.App.swift: ghostty_app_set_focus）。
 *
 * 为什么必须显式喂：libghostty 默认假设「App 在前台」（App.zig 的 focused
 * 默认 true，注释写明要由 apprt 调 focusEvent 纠正）。不喂的话，切到别的
 * App 之后它还以为是前台。
 */
void gt_ghostty_app_set_focus(gt_ghostty_app_t *app, bool focused);

/*
 * 创建 **host 后端** surface：不 fork 子进程、不分配 pty，字节流由调用方
 * 通过 gt_ghostty_surface_process_output 喂入。
 *   nsview  —— 渲染锚点（NSView*，以 void* 传入避免本头依赖 AppKit）；
 *   scale   —— backing scale factor（Retina=2.0）；
 * 失败返回 NULL 并置错误串。
 */
/* Transport-independent checkpoint metadata. No snapshot bytes cross to UI. */
typedef struct {uint8_t r,g,b;} gt_ghostty_replica_rgb_t;
typedef struct {
  uint32_t profile,cell_width,cell_height;uint64_t history_bytes,history_lines,
    continuation_bytes,image_bytes;size_t memory_bytes;bool dark;
  gt_ghostty_replica_rgb_t light_background,light_foreground,dark_background,dark_foreground;
} gt_ghostty_replica_policy_t;
int gt_ghostty_replica_policy(const void *,size_t,gt_ghostty_replica_policy_t *);
gt_ghostty_surface_t *gt_ghostty_surface_new_replica(gt_ghostty_app_t *,void *nsview,double,
  const gt_host_callbacks_t *,uint64_t generation,const gt_ghostty_replica_policy_t *);
bool gt_ghostty_surface_replica_import(gt_ghostty_surface_t *,uint64_t,uint64_t,const void *,size_t);
bool gt_ghostty_surface_replica_output(gt_ghostty_surface_t *,uint64_t,uint64_t,const void *,size_t);
bool gt_ghostty_surface_replica_resize(gt_ghostty_surface_t *,uint64_t,uint64_t,uint16_t,uint16_t);
bool gt_ghostty_surface_replica_scheme_begin(gt_ghostty_surface_t *,uint64_t,uint64_t,bool);
bool gt_ghostty_surface_replica_scheme_apply(gt_ghostty_surface_t *,uint64_t,uint64_t,bool);
bool gt_ghostty_surface_replica_style_apply(gt_ghostty_surface_t *,uint64_t,uint64_t,bool,const uint8_t *,size_t);
bool gt_ghostty_app_style(gt_ghostty_app_t *,bool,uint8_t *,size_t);
bool gt_ghostty_surface_replica_appearance(gt_ghostty_surface_t *,uint64_t,bool *,uint32_t *);
/* OK with known sequence (including zero); NOT_FOUND while not imported. */
int gt_ghostty_surface_replica_applied(gt_ghostty_surface_t *,uint64_t,uint64_t *);
bool gt_ghostty_surface_replica_input(gt_ghostty_surface_t *,uint64_t,uint64_t,bool);
void gt_ghostty_replica_budget_usage(size_t *live,size_t *committed,size_t *retired);

gt_ghostty_surface_t *gt_ghostty_surface_new(gt_ghostty_app_t *app, void *nsview,
                                             double scale,
                                             const gt_host_callbacks_t *cbs);
void gt_ghostty_surface_free(gt_ghostty_surface_t *s);

/*
 * surface 级焦点：这个终端面是不是**当前被操作的那一个**。
 *
 * 语义（对齐上游 libghostty 的 renderer/cursor.zig，也是分屏的业界口径）：
 *   - 聚焦：游标按程序设定的样式显示，该闪就闪；
 *   - 非聚焦：**一律画成空心块，并停掉闪烁**（位置仍看得见，但不抢注意力，
 *     也不再为闪烁反复重绘）。
 *
 * libghostty 的 surface 默认是「已聚焦」（Surface.zig 与 renderer/Thread.zig
 * 里 focused 都默认 true，注释写明「由 apprt 负责纠正」），所以**每个非聚焦
 * 的窗格都必须显式报 false**——分屏时漏报，就会出现「每个格子都在闪」。
 *
 * 由宿主在焦点变化时于**主线程**调用（上游 macOS 的 SurfaceView.focusDidChange
 * 同口径：窗口 isKeyWindow + 视图 isFirstResponder）。
 */
void gt_ghostty_surface_set_focus(gt_ghostty_surface_t *s, bool focused);

/*
 * surface 可见性（occlusion）：这个面是不是**真的在屏幕上**。
 *
 * 为什么需要：宿主为了不丢会话，会把后台标签页的 surface **保持挂载**，只用
 * `opacity(0)` 藏起来——libghostty 并不知道这件事。它的渲染线程照常跑（计时器、
 * 收到输出就重建 + 重绘），画出来却没人看，等于白烧 CPU/GPU。
 *
 * 报 `visible=false` 之后引擎会停掉绘制（renderer/Thread.zig 的 drawFrame 短路、
 * 线程 QoS 降到 utility、Metal 停掉 DisplayLink），变回 `true` 时重建 cells
 * 再画（Surface.occlusionCallback）。语义是**如实报告**：这个面现在确实不在
 * 屏幕上；程序若开了 DEC mode 2033（可见性上报）会收到 `CSI ?999;1n / 2n`，
 * 那不是撒谎，是真的。
 *
 * 宿主判据（上游 macOS 同口径）：窗口 occlusionState 含 `.visible`、未最小化、
 * 且这个标签页是当前显示的那一个。上游的兜底是同口径的：**窗口遮挡状态一变
 * 就重新同步**——否则一次误判为「不可见」之后就再没人把画面叫回来。
 */
void gt_ghostty_surface_set_occlusion(gt_ghostty_surface_t *s, bool visible);

/* 把 transport 读到的字节喂进终端。可由任意线程调用。 */
void gt_ghostty_surface_process_output(gt_ghostty_surface_t *s, const uint8_t *data,
                                       size_t len);

/* 屏幕尺寸（像素）变化 → 重算网格并触发 resize 回调。主线程。 */
void gt_ghostty_surface_set_size(gt_ghostty_surface_t *s, uint32_t w, uint32_t h);

/* 强制重绘。主线程。 */
void gt_ghostty_surface_draw(gt_ghostty_surface_t *s);

/* 网格尺寸（列 × 行）；任一指针可为 NULL。 */
void gt_ghostty_surface_grid_size(gt_ghostty_surface_t *s, uint16_t *cols, uint16_t *rows);

/* 屏幕尺寸（像素）；任一指针可为 NULL。 */
void gt_ghostty_surface_pixel_size(gt_ghostty_surface_t *s, uint32_t *width, uint32_t *height);

/*
 * 读回整个屏幕文本（调试/断言用）。成功返回 malloc 的 NUL 结尾串（调用方
 * free），失败或空返回 NULL。
 */
char *gt_ghostty_surface_read_text(gt_ghostty_surface_t *s);

/* 注入文本，走**终端输入编码路径**（与粘贴同语义）：会触发 host write 回调。
 *
 * 注意：粘贴语义**不带回车执行**——对端开启 bracketed paste 时，黏贴内容里的
 * 换行只是字面换行（这正是 bracketed paste 的意义）。要「敲命令并回车」请用
 * gt_ghostty_surface_key_press。 */
void gt_ghostty_surface_text(gt_ghostty_surface_t *s, const char *text, size_t len);

/*
 * 注入一次按键**按下**（键入语义，非粘贴）：libghostty 按物理键 + 当前终端模式
 * （应用键盘模式 / 键位映射 / Kitty 协议）编码后经 host write 回调发出。
 *
 * `native_keycode` 是**平台虚拟键码**（macOS 即 NSEvent.keyCode：Return=36、
 * Backspace=51、Escape=53、Tab=48）；不是 ghostty_input_key_e 的枚举序号——
 * libghostty 用它在 keycodes 表里反查物理键（见 embedded.zig KeyEvent.core）。
 * `codepoint` 为未加修饰的码点（可 0）；`mods` 见 ghostty_input_mods_e。
 */
void gt_ghostty_surface_key_press(gt_ghostty_surface_t *s, uint32_t native_keycode,
                                  uint32_t codepoint, int mods);

/*
 * 同上，但带上该键产生的 UTF-8 文本（可打印字符走这条）。`text` 可为 NULL/空。
 * 键盘真实输入用这个：libghostty 优先用 text，其次按物理键编码。
 */
void gt_ghostty_surface_key_text(gt_ghostty_surface_t *s, uint32_t native_keycode,
                                 uint32_t codepoint, const char *text, int mods);


/*
 * 输入法（IME）的**预编辑 / 组合中**文本（ghostty 的 preedit 语义）。
 *
 * 上游 macOS 输入法链路是：view 实现 `NSTextInputClient`，在 keyDown 里调
 * `interpretKeyEvents`，输入法再把组合中的串经 `setMarkedText` 回调上来。
 * 组合串**不进程序**（不是提交），只做**内联显示**——所以它走这条单独的通路，
 * 与 `gt_ghostty_surface_text`（粘贴）和 `gt_ghostty_surface_key_text`（键入）
 * 都不同：不产生任何输出到 transport。
 *
 * `text` 为 NULL 或 `len == 0` = **清除**组合态（提交或取消时都要清）。
 * 主线程调用（与其它 surface 绘制入口同线程）。
 */
void gt_ghostty_surface_preedit(gt_ghostty_surface_t *s, const char *text, size_t len);

/*
 * 输入法候选窗（IME candidate window）的锚点：返回**光标处**的视图坐标矩形
 * （左上原点，单位 = 视图点，未按 content scale 放大）。宿主在
 * `NSTextInputClient.firstRect(forCharacterRange:)` 里用它把候选窗贴到光标。
 * 组合中（有 preedit）时 width 为预编辑串的宽度，否则为 0。任一指针可为 NULL。
 */
void gt_ghostty_surface_ime_point(gt_ghostty_surface_t *s, double *x, double *y,
                                  double *width, double *height);

/*
 * 送一段**已提交**的文本（输入法提交、听写等）：**键入语义**，不是粘贴。
 *
 * 为什么不能用 `gt_ghostty_surface_text`：那条是**粘贴**语义，对端开了
 * bracketed paste 时会被 `ESC[200~ … ESC[201~` 包裹，程序就把你敲的中文当成
 * 一次「粘贴」——而输入法提交的字应该与「一个一个敲出来」等价。
 * 实现上走 `ghostty_surface_key`（keycode=0、无修饰键、只发 press），
 * libghostty 会把 text 原样编码下发。
 */
void gt_ghostty_surface_committed_text(gt_ghostty_surface_t *s, const char *text,
                                       size_t len);

/* ------------------------------------------------------------------ */
/* 鼠标事件                                                              */
/* ------------------------------------------------------------------ */

/*
 * 鼠标按键（按下 / 释放）。
 *   state  —— GHOSTTY_MOUSE_PRESS(1) 或 GHOSTTY_MOUSE_RELEASE(0)；
 *   button —— GHOSTTY_MOUSE_LEFT(1)/RIGHT(2)/MIDDLE(3) 等；
 *   mods   —— ghostty_input_mods_e 位掩码。
 * 返回 libghostty 是否消费了此事件（程序开了鼠标捕获时为 true）。主线程。
 */
bool gt_ghostty_surface_mouse_button(gt_ghostty_surface_t *s, int state,
                                     int button, int mods);

/*
 * 鼠标位置（移动 / 拖拽）。
 *   x, y  —— 视图坐标（左上原点，单位 = 视图点）；
 *   mods  —— ghostty_input_mods_e 位掩码。
 * 主线程。
 */
void gt_ghostty_surface_mouse_pos(gt_ghostty_surface_t *s, double x, double y,
                                  int mods);

/*
 * 鼠标滚轮 / 触控板滚动。
 *   dx, dy     —— 滚动增量（触控板为像素级精度，鼠标为行级）；
 *   scroll_mods —— ghostty_input_scroll_mods_t 打包位（precision | momentum）。
 * 主线程。
 */
void gt_ghostty_surface_mouse_scroll(gt_ghostty_surface_t *s, double dx,
                                     double dy, int scroll_mods);

/*
 * 鼠标是否被终端程序捕获（如 vim / less 开启了鼠标报告）。
 * 宿主可据此决定是否把鼠标事件转发给终端还是走系统默认行为。
 */
bool gt_ghostty_surface_mouse_captured(gt_ghostty_surface_t *s);

/*
 * Force Touch 压力变化。
 *   stage    —— 0/1/2（stage 2 = force click）；
 *   pressure —— 0.0~1.0。
 * 主线程。
 */
void gt_ghostty_surface_mouse_pressure(gt_ghostty_surface_t *s, uint32_t stage,
                                       double pressure);

/*
 * 把「现在是深色还是浅色」告诉 libghostty。
 *
 * 为什么必须显式喂：`theme = light:X,dark:Y` 这种写法下，用哪一套配色由
 * **条件状态**（`configpkg.ConditionalState.theme`）决定，而那个状态在
 * 嵌入侧只有这一个入口能改（`ghostty_app_set_color_scheme`）。上游的
 * macOS 应用也是这么做的：观察 `NSApplication.effectiveAppearance`，
 * 一变就报给 libghostty。不喂的话它永远停在默认的 light 档——
 * 「界面已经跟着系统变深色了，终端还是浅色底」就是这么来的。
 *
 * **这一处要把状态喂给两拨消费者**（踩过的坑）：app **和每个已有 surface**。
 * 每个 surface 在创建时从 app **拷贝**一份条件状态，之后 app 级事件不会
 * 再改它们——只报 app 级的话，**新开的** surface 会用新配色、**已开的**不变
 * （用户实测「切深色终端没刷新，新建终端是新的了」）。所以本函数实现里
 * 除了 app 级事件，还会逐个给已有 surface 报一遍（对齐上游 GTK apprt 的
 * `handleStyleManagerDark`）。新 surface 靠创建时继承，不需要额外喂。
 *
 * 语义是**幂等**的：同一档重复调用，libghostty 自己判「没变化」直接返回。
 * 主线程调用。
 */
void gt_ghostty_app_set_color_scheme(gt_ghostty_app_t *app, bool dark);

/* ------------------------------------------------------------------ */
/* 终端配置（设置页的真生效项）                                          */
/* ------------------------------------------------------------------ */

/*
 * 创建 app 时传入配置文件路径（Ghostty 的 config 语法，如
 * `ghostty.conf`）。`path` 为 NULL/空 = 只用默认配置。
 * 文件里**不认识的键会让 ghostty_config_new 丢弃整份配置**——加载失败
 * 返回 NULL 并置错误串（调用方决定是回落到默认配置还是显式报错，
 * 不在这里悄悄装作成功）。
 */
gt_ghostty_app_t *gt_ghostty_app_new_with_config(const char *path);

/*
 * 把配置文件重新加载进 app（设置页「保存」后用）。
 * 返回 0 失败（错误串给出原因），已加载的 app 保持原配置不变。
 *
 * 已存在的 surface **不需要**另外通知：libghostty 的
 * ghostty_app_update_config 会遍历 app 的所有 surface 逐个下发
 * （vendor/ghostty/src/App.zig: updateConfig → handleMessage(.change_config)）。
 * 早期版本这里另外调了一次 ghostty_surface_update_config(NULL)，
 * 而那个 C 入口的签名是 `*const Config`（非可选）——直接解引用空指针崩，
 * 实测在测试里被 sentry 抓到。已删除。
 */
int gt_ghostty_app_reload_config(gt_ghostty_app_t *app, const char *path);

/*
 * 读当前生效配置里的一个键（如 "font-size"、"background-opacity"）。
 * 键不存在或类型不符返回 false（**不写 out**，调用方据此报「读不到」）。
 *
 * 为什么是强类型入口而不是 `void *out`（两个都踩过）：
 *   1. 键名长度由本函数 strlen 得出——libghostty 内部按**键的字节数**
 *      精确匹配（`std.meta.stringToEnum(Key, key_str[0..len])`），把缓冲区
 *      大小当长度传会让任何键都查不到；
 *   2. libghostty 对 out 做 `@alignCast`（src/config/c_get.zig），而 Swift
 *      往 `UnsafeMutableRawPointer` 传 `&x` 会经过一个**对齐不保证**的临时
 *      缓冲（实测 panic: incorrect alignment）。强类型的指针参数才有对齐
 *      保证，所以这里一个类型一个入口。
 */
bool gt_ghostty_config_get_float(gt_ghostty_app_t *app, const char *key,
                                 float *out);
bool gt_ghostty_config_get_bool(gt_ghostty_app_t *app, const char *key,
                                bool *out);
/* f64 键（如 background-opacity）用这个——类型不符会得到 0 或 false。 */
bool gt_ghostty_config_get_double(gt_ghostty_app_t *app, const char *key,
                                  double *out);

/*
 * 颜色键（如 `background`、`unfocused-split-fill`）用这个。分离入口的理由同
 * 上面：libghostty 的 out 参数按类型做 @alignCast，且键名长度按字节精确匹配。
 * 键不存在或类型不符返回 false（**不写 out**）。
 */
bool gt_ghostty_config_get_color(gt_ghostty_app_t *app, const char *key,
                                 uint8_t *r, uint8_t *g, uint8_t *b);

/*
 * 配置诊断条数（0 = 配置干净）。**这才是判断配置是否有效的口径**：
 * libghostty 对未知键/语法错误不丢弃配置，只记一条诊断
 * （实测：`bogus-key = 1` → diagnostics=1，而 font-size 仍能取到值）。
 * 所以「能否取到 font-size」证明不了配置干净——我们先前那版探针是错的。
 */
uint32_t gt_ghostty_config_diagnostics_count(gt_ghostty_app_t *app);

/* 第 idx 条诊断的可读描述（失败返回 NULL）。宿主把它原样给用户看。 */
const char *gt_ghostty_config_diagnostic(gt_ghostty_app_t *app, uint32_t idx);

/* 最后一次错误（线程局部）。 */
const char *gt_ghostty_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOSTTY_HOST_H */