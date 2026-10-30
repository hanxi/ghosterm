#include "CGhostty.h"

#include <ghostty.h>

#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 错误串（线程局部）                                                   */
/* ------------------------------------------------------------------ */

static _Thread_local char g_err[512];

static void set_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof g_err, fmt, ap);
  va_end(ap);
}

const char *gt_ghostty_last_error(void) { return g_err; }

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

struct gt_ghostty_surface;

struct gt_ghostty_app {
  ghostty_app_t app;
  ghostty_config_t config;
  /* config 保留条件主题源；这里保存 CONFIG_CHANGE 提供的已解析颜色。 */
  bool has_resolved_colors;
  bool color_valid[3];
  ghostty_config_color_s colors[3];
  /* 已创建的 surface 清单（动态数组）。
   * 为什么 app 要记着它们：浅/深色切换不只是 app 级条件状态的事——**每个
   * surface 各自持有一份 `config_conditional_state`（创建时从 app 拷贝）**，
   * 只喂 app 级事件的话，已有 surface 不会换色（新开的才对，因为它创建时
   * 拷到的是新状态）。上游 GTK apprt 也是这么逐个下发的
   * （`application.zig` handleStyleManagerDark 里 colorSchemeEvent + 遍历
   * surfaces 调 colorSchemeCallback）。 */
  struct gt_ghostty_surface **surfaces;
  size_t surface_count;
  size_t surface_cap;
};

struct gt_ghostty_surface {
  ghostty_surface_t surface;
  gt_host_callbacks_t cbs;
  /* 从 app 的清单里摘除时用；NULL = 还没登记（创建失败路径）。 */
  struct gt_ghostty_app *owner;
  /* libghostty 保存的是 userdata 指针，回调里要找回本结构，故自存。 */
};

static void cb_wakeup(void *ud) {
  (void)ud;
  /* tick 由宿主在主线程驱动；此处不跨线程调度，避免隐式线程模型。 */
}

static bool cb_action(ghostty_app_t app, ghostty_target_s target,
                      ghostty_action_s action) {
  if (action.tag == GHOSTTY_ACTION_CONFIG_CHANGE && target.tag == GHOSTTY_TARGET_APP) {
    struct gt_ghostty_app *a = ghostty_app_userdata(app);
    if (!a) return false;
    const char *keys[] = {"background", "foreground", "unfocused-split-fill"};
    for (size_t i = 0; i < 3; i++) {
      a->color_valid[i] = ghostty_config_get(action.action.config_change.config,
          &a->colors[i], keys[i], (uintptr_t)strlen(keys[i]));
    }
    a->has_resolved_colors = true;
    return true;
  }
  /* `reload_config`：libghostty 改完条件状态（浅/深色）后发的动作，把
   * 「该重读配置」这件事**交给嵌入侧**（embedded apprt 自己不做；上游 macOS
   * 在 `Ghostty.App.swift` 的 `configReload` 里落成 `ghostty_app_update_config`）。
   * 不消费它 = 条件状态改了、配置却没重下发 → 已有 surface 不换色。 */
  if (action.tag == GHOSTTY_ACTION_RELOAD_CONFIG) {
    struct gt_ghostty_app *a = ghostty_app_userdata(app);
    if (a && a->app && a->config) {
      if (target.tag == GHOSTTY_TARGET_APP) {
        ghostty_app_update_config(a->app, a->config);
        return true;
      }
      if (target.tag == GHOSTTY_TARGET_SURFACE && target.target.surface) {
        ghostty_surface_update_config(target.target.surface, a->config);
        return true;
      }
    }
    return false;
  }
  /* M1：只把「标题变化」透传出去，其余动作一律不消费（返回 false 让
   * libghostty 走默认路径）。 */
  if (target.tag == GHOSTTY_TARGET_SURFACE && target.target.surface) {
    struct gt_ghostty_surface *s = ghostty_surface_userdata(target.target.surface);
    if (s && action.tag == GHOSTTY_ACTION_SET_TITLE) {
      if (s->cbs.title && action.action.set_title.title)
        s->cbs.title(s->cbs.userdata, action.action.set_title.title);
    } else if (s && action.tag == GHOSTTY_ACTION_PWD) {
      /* OSC 7（由 shell 集成发出）→ 宿主。没有集成就不会到这儿。 */
      if (s->cbs.pwd && action.action.pwd.pwd)
        s->cbs.pwd(s->cbs.userdata, action.action.pwd.pwd);
    }
  }
  return false;
}

static ghostty_clipboard_read_result_e cb_read_clipboard(
    void *ud, ghostty_clipboard_e clipboard, void *state,
    const char *const *mimes, size_t mime_count, bool listing) {
  (void)ud; (void)clipboard; (void)state; (void)mimes; (void)mime_count; (void)listing;
  /* M1 不做剪贴板；显式报「不可用」，不假装成功（铁律 #6）。 */
  return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
}

static void cb_confirm_read_clipboard(void *ud,
                                      const ghostty_clipboard_confirm_s *req,
                                      void *state,
                                      ghostty_clipboard_request_e type) {
  (void)ud; (void)req; (void)state; (void)type;
}
static void cb_write_clipboard(void *ud, ghostty_clipboard_e clipboard,
                               const ghostty_clipboard_content_s *contents,
                               size_t count, bool confirm) {
  (void)ud; (void)clipboard; (void)contents; (void)count; (void)confirm;
}

static void host_write(void *ud, const unsigned char *data, size_t len) {
  gt_ghostty_surface_t *s = ud;
  if (s && s->cbs.write) s->cbs.write(s->cbs.userdata, data, len);
}

static void host_resize(void *ud, uint16_t cols, uint16_t rows) {
  gt_ghostty_surface_t *s = ud;
  if (s && s->cbs.resize) s->cbs.resize(s->cbs.userdata, cols, rows);
}

/* OSC 7501（Program Status Protocol）：把报告转给宿主。
 * report 只在本次调用期间有效（见 ghostty_host.h）。 */
static void host_program_status(void *ud, const ghostty_program_status_s *report) {
  gt_ghostty_surface_t *s = ud;
  if (s && s->cbs.program_status) s->cbs.program_status(s->cbs.userdata, report);
}

/* OSC 133（shell integration）：把事件转给宿主（只在本次调用期间有效）。 */
static void host_semantic_prompt(void *ud, const ghostty_semantic_prompt_s *event) {
  gt_ghostty_surface_t *s = ud;
  if (s && s->cbs.semantic_prompt) s->cbs.semantic_prompt(s->cbs.userdata, event);
}

/* ------------------------------------------------------------------ */
/* App                                                                 */
/* ------------------------------------------------------------------ */

static ghostty_config_t load_config(const char *path);
static gt_ghostty_app_t *gt_ghostty_app_create(const char *path);

gt_ghostty_app_t *gt_ghostty_app_new_with_config(const char *path) {
  return gt_ghostty_app_create(path);
}

gt_ghostty_app_t *gt_ghostty_app_new(void) { return gt_ghostty_app_create(NULL); }

/*
 * 把 Ghostty 的资源目录指到 app bundle 内。
 *
 * 为什么需要（上游的搜索逻辑见 src/os/resourcesdir.zig）：libghostty 找
 * 主题等资源时会**沿可执行文件路径向上爬**，看有没有
 * `Contents/Resources/terminfo/78/xterm-ghostty` 这个哨兵文件——
 * 那是官方 app 随包带的 terminfo。我们不做终端 terminfo 随包（我们的
 * shell 用 xterm-256color），于是这个哨兵不存在 → 资源目录判为空 →
 * `theme = "..."` 找不到配色。
 *
 * 自己设 `GHOSTTY_RESOURCES_DIR` 是上游支持的入口（release 构建优先读它，
 * debug 构建在爬哨兵失败后也会读它），且这是**嵌入者的正当权利**：
 * 我们自己出的包，知道自己的资源放在哪，不该靠猜。
 * 只在自己没被显式设置时兜底，不覆盖调用方（也不覆盖测试）。
 */
static void ensure_resources_dir(void) {
  if (getenv("GHOSTTY_RESOURCES_DIR") != NULL) return;
  char exe[4096];
  uint32_t size = (uint32_t)sizeof exe;
  if (_NSGetExecutablePath(exe, &size) != 0) return;
  char real[4096];
  if (realpath(exe, real) == NULL) return;
  /* <App>/Contents/MacOS/GhostermApp → 上两级到 Contents，再进 Resources */
  char *slash = strrchr(real, '/');
  if (!slash) return;
  *slash = '\0';                      /* .../Contents/MacOS */
  slash = strrchr(real, '/');
  if (!slash) return;
  *slash = '\0';                      /* .../Contents */
  char res[4096];
  snprintf(res, sizeof res, "%s/Resources/ghostty", real);
  struct stat st;
  if (stat(res, &st) != 0 || !S_ISDIR(st.st_mode)) return;
  setenv("GHOSTTY_RESOURCES_DIR", res, 1);
}

gt_ghostty_app_t *gt_ghostty_app_create(const char *path) {
  /* libghostty 必须先 init 全局状态（分配器、xev 后端、字体后端…）。
   * 但 ghostty_init **不是幂等的**：内部 sentry 初始化带
   * `assert(init_thread == null)`，第二次调用直接 panic。上游约定由 main()
   * 调一次；这里用一次性闸门兜住，让 Swift 侧可以安全地「每个 app 调一次」。 */
  static bool inited = false;
  if (!inited) {
    ensure_resources_dir();
    if (ghostty_init(0, NULL) != 0) {
      set_err("ghostty_init 失败");
      return NULL;
    }
    inited = true;
  }

  ghostty_config_t config = load_config(path);
  if (!config) return NULL;

  /* 先分配 wrapper：cb_action 要靠 `ghostty_app_userdata` 找回它拿 config。 */
  gt_ghostty_app_t *out = calloc(1, sizeof *out);
  if (!out) { set_err("app 分配失败"); ghostty_config_free(config); return NULL; }
  out->config = config;

  ghostty_runtime_config_s rt;
  memset(&rt, 0, sizeof rt);
  rt.userdata = out;
  rt.supports_selection_clipboard = false;
  rt.wakeup_cb = cb_wakeup;
  rt.action_cb = cb_action;
  rt.read_clipboard_cb = cb_read_clipboard;
  rt.confirm_read_clipboard_cb = cb_confirm_read_clipboard;
  rt.write_clipboard_cb = cb_write_clipboard;
  rt.close_surface_cb = NULL;

  ghostty_app_t app = ghostty_app_new(&rt, config);
  if (!app) {
    set_err("ghostty_app_new 失败（配置无效？）");
    free(out);
    ghostty_config_free(config);
    return NULL;
  }
  out->app = app;
  return out;
}

void gt_ghostty_app_free(gt_ghostty_app_t *a) {
  if (!a) return;
  /* 先解开反向引用再释放：surface 的 `owner` 指向本结构，若不置空，
   * 之后 surface 释放时 `app_untrack_surface` 会写已释放内存。 */
  for (size_t i = 0; i < a->surface_count; i++) {
    if (a->surfaces[i]) a->surfaces[i]->owner = NULL;
  }
  if (a->app) ghostty_app_free(a->app);
  if (a->config) ghostty_config_free(a->config);
  free(a->surfaces);
  free(a);
}

static ghostty_config_t load_config(const char *path) {
  ghostty_config_t config = ghostty_config_new();
  if (!config) {
    set_err("ghostty_config_new 失败");
    return NULL;
  }
  if (path && *path) {
    /* 加载 + finalize 后看**诊断**：libghostty 对未知键 / 语法错误不丢弃
     * 配置，只记一条诊断（实测 `bogus-key = 1` → 1 条诊断，font-size
     * 照样取得到）。所以判有效的口径是诊断条数，不是「取不取得到某个键」。
     * 有诊断就显式报错——不静默当成「用户设置没生效」（铁律 #6）。 */
    ghostty_config_load_file(config, path);
    ghostty_config_finalize(config);
    uint32_t n = ghostty_config_diagnostics_count(config);
    if (n > 0) {
      ghostty_diagnostic_s d = ghostty_config_get_diagnostic(config, 0);
      set_err("配置无效：%s", d.message ? d.message : path);
      ghostty_config_free(config);
      return NULL;
    }
    return config;
  }
  ghostty_config_finalize(config);
  return config;
}

int gt_ghostty_app_reload_config(gt_ghostty_app_t *a, const char *path) {
  if (!a || !a->app) { set_err("reload_config: app 为空"); return -1; }
  ghostty_config_t fresh = load_config(path);
  if (!fresh) return -1; /* 错误串已置；旧配置原样保留（不半途换配置） */
  ghostty_app_update_config(a->app, fresh);
  if (a->config) ghostty_config_free(a->config);
  a->config = fresh;
  return 0;
}

bool gt_ghostty_config_get_float(gt_ghostty_app_t *a, const char *key,
                                 float *out) {
  if (!a || !a->config || !key || !out) return false;
  return ghostty_config_get(a->config, out, key, (uintptr_t)strlen(key));
}

bool gt_ghostty_config_get_bool(gt_ghostty_app_t *a, const char *key,
                                bool *out) {
  if (!a || !a->config || !key || !out) return false;
  return ghostty_config_get(a->config, out, key, (uintptr_t)strlen(key));
}

bool gt_ghostty_config_get_double(gt_ghostty_app_t *a, const char *key,
                                  double *out) {
  if (!a || !a->config || !key || !out) return false;
  return ghostty_config_get(a->config, out, key, (uintptr_t)strlen(key));
}

bool gt_ghostty_config_get_color(gt_ghostty_app_t *a, const char *key,
                                 uint8_t *r, uint8_t *g, uint8_t *b) {
  if (!a || !a->config || !key || !r || !g || !b) return false;
  if (a->has_resolved_colors) {
    const char *keys[] = {"background", "foreground", "unfocused-split-fill"};
    for (size_t i = 0; i < 3; i++) {
      if (strcmp(key, keys[i]) != 0) continue;
      if (!a->color_valid[i]) return false;
      *r = a->colors[i].r; *g = a->colors[i].g; *b = a->colors[i].b;
      return true;
    }
  }
  ghostty_config_color_s c = {0, 0, 0};
  if (!ghostty_config_get(a->config, &c, key, (uintptr_t)strlen(key)))
    return false;
  *r = c.r;
  *g = c.g;
  *b = c.b;
  return true;
}

uint32_t gt_ghostty_config_diagnostics_count(gt_ghostty_app_t *a) {
  if (!a || !a->config) return 0;
  return ghostty_config_diagnostics_count(a->config);
}

const char *gt_ghostty_config_diagnostic(gt_ghostty_app_t *a, uint32_t idx) {
  if (!a || !a->config) return NULL;
  if (idx >= ghostty_config_diagnostics_count(a->config)) return NULL;
  ghostty_diagnostic_s d = ghostty_config_get_diagnostic(a->config, idx);
  return d.message;
}

void gt_ghostty_app_tick(gt_ghostty_app_t *a) {
  if (a && a->app) ghostty_app_tick(a->app);
}

void gt_ghostty_app_set_focus(gt_ghostty_app_t *a, bool focused) {
  if (!a || !a->app) return;
  /* 只是把「App 是否活动」记进 libghostty（App.zig:focusEvent），不碰任何
   * surface——surface 级焦点由 gt_ghostty_surface_set_focus 单独报。 */
  ghostty_app_set_focus(a->app, focused);
}

/*
 * 浅色 / 深色：app 级色板事件。`theme = light:X,dark:Y` 靠它选边
 * （见 ghostty_host.h 里这条函数的说明）。libghostty 自己判幂等。
 */
void gt_ghostty_app_set_color_scheme(gt_ghostty_app_t *a, bool dark) {
  if (!a || !a->app) return;
  ghostty_color_scheme_e scheme = dark ? GHOSTTY_COLOR_SCHEME_DARK
                                       : GHOSTTY_COLOR_SCHEME_LIGHT;
  /* app 级：管新 surface（它在创建时从 app 拷条件状态）。 */
  ghostty_app_set_color_scheme(a->app, scheme);
  /* surface 级：**已有** surface 各自的条件状态是私有的，得逐个报，
   * 否则「切深色只有新开的终端变色、已开的不变」（用户实测）。 */
  for (size_t i = 0; i < a->surface_count; i++) {
    struct gt_ghostty_surface *s = a->surfaces[i];
    if (s && s->surface) ghostty_surface_set_color_scheme(s->surface, scheme);
  }
}

/* ------------------------------------------------------------------ */
/* Surface                                                             */
/* ------------------------------------------------------------------ */

static void app_track_surface(gt_ghostty_app_t *a, gt_ghostty_surface_t *s) {
  if (!a || !s) return;
  if (a->surface_count == a->surface_cap) {
    size_t cap = a->surface_cap ? a->surface_cap * 2 : 4;
    struct gt_ghostty_surface **grown =
        realloc(a->surfaces, cap * sizeof *grown);
    if (!grown) return; /* 记不下就记不下：色板切换会漏掉这一个面，不崩。 */
    a->surfaces = grown;
    a->surface_cap = cap;
  }
  a->surfaces[a->surface_count++] = s;
}

static void app_untrack_surface(gt_ghostty_surface_t *s) {
  gt_ghostty_app_t *a = s ? s->owner : NULL;
  if (!a) return;
  for (size_t i = 0; i < a->surface_count; i++) {
    if (a->surfaces[i] == s) {
      a->surfaces[i] = a->surfaces[--a->surface_count];
      break;
    }
  }
  s->owner = NULL;
}

gt_ghostty_surface_t *gt_ghostty_surface_new(gt_ghostty_app_t *a,void *nsview,double scale,
  const gt_host_callbacks_t *cbs) {
  if (!a || !a->app) { set_err("surface_new: app 为空"); return NULL; }
  if (!nsview) { set_err("surface_new: nsview 为空（需要渲染锚点）"); return NULL; }
  if (!cbs || !cbs->write || !cbs->resize) {
    set_err("surface_new: host 后端需要 write 与 resize 回调");
    return NULL;
  }

  gt_ghostty_surface_t *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->cbs = *cbs;

  ghostty_surface_config_s sc = ghostty_surface_config_new();
  sc.platform_tag = GHOSTTY_PLATFORM_MACOS;
  sc.platform.macos.nsview = nsview;
  sc.scale_factor = scale > 0 ? scale : 1.0;
  sc.userdata = s;
  /* host 后端：不 fork、不分配 pty，字节流由宿主喂入。 */
  sc.host = true;
  sc.host_write = host_write;
  sc.host_resize = host_resize;
  /* 注册即声明支持 OSC 7501（终端会回答 `OSC 7501 ; ?`）。 */
  sc.host_program_status = host_program_status;
  sc.host_semantic_prompt = host_semantic_prompt;

  s->surface = ghostty_surface_new(a->app, &sc);
  if (!s->surface) {
    set_err("ghostty_surface_new 失败");
    free(s);
    return NULL;
  }
  s->owner = a;
  app_track_surface(a, s);
  return s;
}


void gt_ghostty_surface_free(gt_ghostty_surface_t *s) {
  if (!s) return;
  app_untrack_surface(s);
  if (s->surface) ghostty_surface_free(s->surface);
  free(s);
}

void gt_ghostty_surface_set_focus(gt_ghostty_surface_t *s, bool focused) {
  if (!s || !s->surface) return;
  /* 只报焦点，不猜语义：聚焦/非聚焦怎么画交给 libghostty（非聚焦 = 空心
   * 块 + 停止闪烁，见 renderer/cursor.zig）。 */
  ghostty_surface_set_focus(s->surface, focused);
}

void gt_ghostty_surface_set_occlusion(gt_ghostty_surface_t *s, bool visible) {
  if (!s || !s->surface) return;
  /* 只是把「这个面在不在屏幕上」告诉引擎：不可见时它自己停掉绘制
   * （renderer/Thread.zig 的 drawFrame 短路 + QoS 降档 + Metal 停 DisplayLink），
   * 变回可见时它会重建 cells 再画。见 ghostty_host.h 的说明。 */
  ghostty_surface_set_occlusion(s->surface, visible);
}

void gt_ghostty_surface_process_output(gt_ghostty_surface_t *s,
                                       const uint8_t *data, size_t len) {
  if (!s || !s->surface || !data || len == 0) return;
  ghostty_surface_process_output(s->surface, data, len);
}

void gt_ghostty_surface_set_size(gt_ghostty_surface_t *s, uint32_t w, uint32_t h) {
  if (!s || !s->surface) return;
  ghostty_surface_set_size(s->surface, w, h);
}

void gt_ghostty_surface_draw(gt_ghostty_surface_t *s) {
  if (!s || !s->surface) return;
  ghostty_surface_draw(s->surface);
}

void gt_ghostty_surface_grid_size(gt_ghostty_surface_t *s, uint16_t *cols,
                                  uint16_t *rows) {
  if (!s || !s->surface) return;
  ghostty_surface_size_s sz = ghostty_surface_size(s->surface);
  if (cols) *cols = sz.columns;
  if (rows) *rows = sz.rows;
}

void gt_ghostty_surface_text(gt_ghostty_surface_t *s, const char *text, size_t len) {
  if (!s || !s->surface || !text || len == 0) return;
  ghostty_surface_text(s->surface, text, len);
}


void gt_ghostty_surface_key_text(gt_ghostty_surface_t *s, uint32_t native_keycode,
                                 uint32_t codepoint, const char *text, int mods) {
  if (!s || !s->surface) return;
  ghostty_input_key_s ev;
  memset(&ev, 0, sizeof ev);
  ev.action = GHOSTTY_ACTION_PRESS;
  ev.mods = (ghostty_input_mods_e)mods;
  ev.keycode = native_keycode;
  ev.text = (text && *text) ? text : NULL;
  ev.unshifted_codepoint = codepoint;
  ghostty_surface_key(s->surface, ev);
  ev.action = GHOSTTY_ACTION_RELEASE;
  ghostty_surface_key(s->surface, ev);
}

void gt_ghostty_surface_key_press(gt_ghostty_surface_t *s, uint32_t native_keycode,
                                  uint32_t codepoint, int mods) {
  if (!s || !s->surface) return;
  ghostty_input_key_s ev;
  memset(&ev, 0, sizeof ev);
  ev.action = GHOSTTY_ACTION_PRESS;
  ev.mods = (ghostty_input_mods_e)mods;
  ev.consumed_mods = 0;
  ev.keycode = native_keycode;
  ev.text = NULL; /* 让 libghostty 自己编码（回车即 CR） */
  ev.unshifted_codepoint = codepoint;
  ev.composing = false;
  ghostty_surface_key(s->surface, ev);
}

void gt_ghostty_surface_preedit(gt_ghostty_surface_t *s, const char *text, size_t len) {
  if (!s || !s->surface) return;
  /* len == 0（或 text == NULL）= 清除组合态。ghostty 侧按 null 处理。 */
  ghostty_surface_preedit(s->surface, (text && len > 0) ? text : NULL,
                          (text && len > 0) ? len : 0);
}

void gt_ghostty_surface_ime_point(gt_ghostty_surface_t *s, double *x, double *y,
                                  double *width, double *height) {
  /* 先把 out 全部置 0：libghostty 的 C 入口对四个指针都无条件解引用，故一律传
   * 内部变量，避免调用方传 NULL 时崩。 */
  double lx = 0, ly = 0, lw = 0, lh = 0;
  if (s && s->surface) ghostty_surface_ime_point(s->surface, &lx, &ly, &lw, &lh);
  if (x) *x = lx;
  if (y) *y = ly;
  if (width) *width = lw;
  if (height) *height = lh;
}

void gt_ghostty_surface_committed_text(gt_ghostty_surface_t *s, const char *text,
                                       size_t len) {
  if (!s || !s->surface || !text || len == 0) return;
  /* 键入语义（非粘贴）：keycode=0 → libghostty 侧视为不可识别物理键，携带 UTF-8
   * 文本时按文本原样编码。只发一次 press（release 不产生文本）。 */
  ghostty_input_key_s ev;
  memset(&ev, 0, sizeof ev);
  ev.action = GHOSTTY_ACTION_PRESS;
  ev.keycode = 0;
  ev.text = text;
  ev.unshifted_codepoint = 0;
  ev.composing = false;
  ghostty_surface_key(s->surface, ev);
}

/* ------------------------------------------------------------------ */
/* 鼠标事件                                                              */
/* ------------------------------------------------------------------ */

bool gt_ghostty_surface_mouse_button(gt_ghostty_surface_t *s, int state,
                                     int button, int mods) {
  if (!s || !s->surface) return false;
  return ghostty_surface_mouse_button(s->surface,
                                      (ghostty_input_mouse_state_e)state,
                                      (ghostty_input_mouse_button_e)button,
                                      (ghostty_input_mods_e)mods);
}

void gt_ghostty_surface_mouse_pos(gt_ghostty_surface_t *s, double x, double y,
                                  int mods) {
  if (!s || !s->surface) return;
  ghostty_surface_mouse_pos(s->surface, x, y, (ghostty_input_mods_e)mods);
}

void gt_ghostty_surface_mouse_scroll(gt_ghostty_surface_t *s, double dx,
                                     double dy, int scroll_mods) {
  if (!s || !s->surface) return;
  ghostty_surface_mouse_scroll(s->surface, dx, dy,
                               (ghostty_input_scroll_mods_t)scroll_mods);
}

bool gt_ghostty_surface_mouse_captured(gt_ghostty_surface_t *s) {
  if (!s || !s->surface) return false;
  return ghostty_surface_mouse_captured(s->surface);
}

void gt_ghostty_surface_mouse_pressure(gt_ghostty_surface_t *s, uint32_t stage,
                                       double pressure) {
  if (!s || !s->surface) return;
  ghostty_surface_mouse_pressure(s->surface, stage, pressure);
}

void gt_ghostty_surface_pixel_size(gt_ghostty_surface_t *s, uint32_t *width,
                                   uint32_t *height) {
  if (!s || !s->surface) return;
  ghostty_surface_size_s sz = ghostty_surface_size(s->surface);
  if (width) *width = sz.width_px;
  if (height) *height = sz.height_px;
}

char *gt_ghostty_surface_read_text(gt_ghostty_surface_t *s) {
  if (!s || !s->surface) return NULL;
  ghostty_selection_s sel;
  memset(&sel, 0, sizeof sel);
  sel.top_left.tag = GHOSTTY_POINT_SCREEN;
  sel.top_left.coord = GHOSTTY_POINT_COORD_TOP_LEFT;
  sel.bottom_right.tag = GHOSTTY_POINT_SCREEN;
  sel.bottom_right.coord = GHOSTTY_POINT_COORD_BOTTOM_RIGHT;

  ghostty_text_s txt;
  memset(&txt, 0, sizeof txt);
  if (!ghostty_surface_read_text(s->surface, sel, &txt)) return NULL;

  char *out = malloc(txt.text_len + 1);
  if (out) {
    memcpy(out, txt.text, txt.text_len);
    out[txt.text_len] = '\0';
  }
  ghostty_surface_free_text(s->surface, &txt);
  return out;
}

char *gt_ghostty_surface_read_selection(gt_ghostty_surface_t *s) {
  if (!s || !s->surface) return NULL;
  ghostty_text_s txt = {0};
  if (!ghostty_surface_read_selection(s->surface, &txt)) return NULL;
  char *out = malloc(txt.text_len + 1);
  if (out) {
    memcpy(out, txt.text, txt.text_len);
    out[txt.text_len] = '\0';
  }
  ghostty_surface_free_text(s->surface, &txt);
  return out;
}
