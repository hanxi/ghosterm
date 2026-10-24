/*
 * pty_posix.c — 私有 POSIX PTY 后端；公共 Transport 不依赖平台细节。
 *
 * 契约（铁律 #1/#2）：
 *   - 字节流由会话线程直接 read → surface，**不进事件队列、不过 UI 线程**；
 *   - read/write/resize 由会话线程独占调用，不跨线程；
 *   - 后台仅有一个「收尸」线程（waitpid 子进程）：它只发低频退出事件
 *     （GT_EVENT_STATUS），绝不搬运字节流。
 *
 * 线程与生命周期：
 *   gt__pty_open  fork 子进程 + 起 waiter 线程（等子进程退出）；
 *   pty_read      轮询 master + 自管道；**EOF 前不丢最后一批数据**；
 *   pty_close     唤醒 reader → 必要时 SIGHUP/SIGKILL 子进程 →
 *                 关 master → join waiter → 关 wake fd → 释放句柄。
 *                          close(NULL) 安全；关闭后不得再用该句柄。
 *
 * 平台：POSIX（macOS/Linux）forkpty。Windows 走 ConPTY（后续）；
 *       iOS/HarmonyOS 无本地 PTY（能力缺失应显式报错，见铁律 #6）。
 */
#include "ghosterm.h"
#include "internal.h"
#include "backend.h"
#include "pty-size.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#include <libproc.h>
#include <sys/proc_info.h>
#elif defined(__linux__)
#include <pty.h>
#endif

extern char **environ;

typedef struct {
  gt_io_backend_t base;
  gt_allocator_t allocator;

  int master_fd;
  pid_t child;
  pthread_t waiter;
  bool has_waiter;

  int wake_r, wake_w; /* 自管道：唤醒 read 的 poll（子进程退出 / 关闭） */

  atomic_bool exited;
  atomic_bool closing;
  atomic_int exit_code;
  atomic_int observed_kind,observed_code;

  bool eof;

  /*
   * 本地 PTY：子进程暂时不读输入时，写不进去的**尾巴**暂存于此。
   *
   * 为什么必须攒着而不是等：pty 的输入队列很小（macOS 实测约 1 KiB），
   * 前台程序一旦不读 stdin（卡住的程序 / 只输出的程序），write 立刻 EAGAIN。
   * 旧实现在 EAGAIN 上 `poll(...,-1)` **无限等**——那个子进程永不读，于是
   * 会话线程被永久钉死：1) 读循环再也出不来，终端画面冻住；2) stop() 的
   * `queue.sync` 恰好在等这条线程，**主线程跟着卡死（整个 app 无响应）**。
   * 故写**绝不无限等**：留下的尾巴由后续 read/write 顺带冲掉（与真实终端的
   * 流控同语义——输入不会丢，只是晚一点送到）。
   *
   * 只在会话线程上被读写，无需加锁。 */
  uint8_t *wtail;
  size_t wtail_len;
} gt_pty_backend_t;

/* Exact-sized, maximally aligned header keeps launch temporaries and backlog
 * in the same domain without calling an allocator after fork in the child. */
typedef union {
  max_align_t alignment;
  struct {gt_allocator_t allocator;size_t bytes;} meta;
} pty_allocation_t;
static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
static void *pty_allocate(const gt_allocator_t *a,size_t bytes) {
  if(!bytes || bytes>SIZE_MAX-sizeof(pty_allocation_t))return NULL;
  pty_allocation_t *h=a->allocate(a->context,sizeof(*h)+bytes);if(!h)return NULL;
  h->meta.allocator=*a;h->meta.bytes=bytes;return h+1;
}
static void pty_free(void *p) {
  if(!p)return;
  pty_allocation_t *h=(pty_allocation_t *)p-1;gt_allocator_t a=h->meta.allocator;
  a.release(a.context,h,sizeof(*h)+h->meta.bytes);
}
static void *pty_calloc(const gt_allocator_t *a,size_t count,size_t bytes) {
  if(!bytes || count>SIZE_MAX/bytes)return NULL;
  void *p=pty_allocate(a,count*bytes);if(p)memset(p,0,count*bytes);return p;
}
static char *pty_strdup(const gt_allocator_t *a,const char *s) {
  size_t n=strlen(s);if(n==SIZE_MAX)return NULL;
  char *p=pty_allocate(a,n+1);if(p)memcpy(p,s,n+1);return p;
}
static void *pty_reallocate(const gt_allocator_t *a,void *p,size_t bytes) {
  if(!p)return pty_allocate(a,bytes);
  if(!bytes || bytes>SIZE_MAX-sizeof(pty_allocation_t))return NULL;
  pty_allocation_t *h=(pty_allocation_t *)p-1;
  if(h->meta.allocator.allocate==default_allocate) {
    h=realloc(h,sizeof(*h)+bytes);if(!h)return NULL;
    h->meta.bytes=bytes;return h+1;
  }
  void *next=pty_allocate(a,bytes);if(!next)return NULL;
  memcpy(next,p,h->meta.bytes<bytes?h->meta.bytes:bytes);pty_free(p);return next;
}

static const gt_io_ops_t pty_ops;

static int nonblocking_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  int descriptor_flags = fcntl(fd, F_GETFD);
  if (descriptor_flags < 0 ||
      fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) return -1;
  return 0;
}

/* Only async-signal-safe calls between fork and exec. CLOEXEC on owned fds
 * alone cannot cover caller fds or another thread's open-before-fcntl race. */
static void close_child_fds(long limit) {
  for (long fd = 3; fd < limit; fd++) {
    while (close((int)fd) < 0 && errno == EINTR) {}
  }
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static size_t arr_len(const char *const *a) {
  size_t n = 0;
  if (a) {
    while (a[n]) n++;
  }
  return n;
}

/* 写路径的**有界**等待上限（毫秒）：队列满时最多等这么久就给后面攒着。
 * 必须远小于「人点一下关标签页」的容忍度——stop() 会等会话线程收工。 */
#define GT_PTY_WRITE_GRACE_MS 100
/* 未送达尾巴的上限：子进程长时间不读输入时，避免内存无限增长。 */
#define GT_PTY_WTAIL_MAX (4u * 1024u * 1024u)

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *default_shell(const gt_allocator_t *allocator) {
  const char *sh = getenv("SHELL");
  if (!sh || !*sh) {
    /* SHELL 缺失时问 passwd：GUI 启动（Finder/Dock）的进程环境里常常没有
     * SHELL，而用户真实登录 shell 在 passwd 里。回落到 /bin/sh 会让差异
     * 以「命令找不到」的形式暴露，且难以归因。 */
    struct passwd *pw = getpwuid(getuid());
    sh = (pw && pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh";
  }
  return pty_strdup(allocator,sh);
}

/* 该 environ 条目是否被 overrides 覆盖，或被 TERM 覆盖（TERM 由本模块接管）。 */
static bool env_key_is(const char *entry, const char *const *overrides) {
  size_t klen = strcspn(entry, "=");
  if (klen == 4 && strncmp(entry, "TERM", 4) == 0) return true;
  for (size_t i = 0; overrides && overrides[i]; i++) {
    size_t olen = strcspn(overrides[i], "=");
    if (olen == klen && strncmp(entry, overrides[i], klen) == 0) return true;
  }
  return false;
}

/* 由父进程 environ + overrides 构造子进程环境；TERM 恒被注入。
 * 返回的数组与 *out_term_kv 均由调用方 pty_free（fork 后即可释放）。 */
static char **build_child_env(const char *term, const char *const *overrides,
                              char **out_term_kv,const gt_allocator_t *allocator) {
  *out_term_kv = NULL;
  const char *t = (term && *term) ? term : "xterm-256color";
  size_t tlen = strlen(t);
  if(tlen>SIZE_MAX-6){gt__set_error("pty: TERM size overflow");return NULL;}
  char *term_kv = pty_allocate(allocator,5 + tlen + 1);
  if (!term_kv) {
    gt__set_error("pty: out of memory");
    return NULL;
  }
  memcpy(term_kv, "TERM=", 5);
  memcpy(term_kv + 5, t, tlen + 1);

  size_t kept = 0;
  for (size_t i = 0; environ[i]; i++) {
    if (!env_key_is(environ[i], overrides)) kept++;
  }
  size_t nov = arr_len(overrides);
  char **env = pty_calloc(allocator,kept + nov + 2, sizeof(char *));
  if (!env) {
    pty_free(term_kv);
    gt__set_error("pty: out of memory");
    return NULL;
  }
  size_t k = 0;
  for (size_t i = 0; environ[i]; i++) {
    if (!env_key_is(environ[i], overrides)) env[k++] = environ[i];
  }
  for (size_t i = 0; overrides && overrides[i]; i++) env[k++] = (char *)overrides[i];
  env[k++] = term_kv;
  env[k] = NULL;
  *out_term_kv = term_kv;
  return env;
}

static struct winsize make_winsize(uint16_t cols, uint16_t rows, uint32_t pw,
                                   uint32_t ph) {
  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_col = cols ? cols : 80;
  ws.ws_row = rows ? rows : 24;
  ws.ws_xpixel = (unsigned short)pw;
  ws.ws_ypixel = (unsigned short)ph;
  return ws;
}

/*
 * macOS：把 shell 起成**登录 shell**（argv[0] 前缀 `-`）。
 *
 * 为什么需要：GUI（Finder/Dock）启动的进程环境里 PATH 往往只有 launchd 给的
 * `/usr/bin:/bin:/usr/sbin:/sbin`，而 macOS 的 PATH 由 `/etc/zprofile` 里的
 * `/usr/libexec/path_helper` 补全（读 /etc/paths 与 /etc/paths.d）——zprofile
 * **只对登录 shell** 生效。于是 Homebrew 装的 brew / go / gls 全是
 * 「command not found」，用户在 ~/.zshrc 里看到一片报错。
 *
 * 为什么**不**用 `login(1)`（上游 Ghostty 的做法，我照着做过一版）：
 * `/usr/bin/login` 是 setuid root 的（要写 utmp）——exec 之后它短暂以 root
 * 身份存在，而**非 root 的父进程不能给它发信号**：`kill()` 返回 0 但进程不
 * 死。后果是 `gt_transport_close` 里的 `pthread_join(waiter)` 永远等不到
 * （waiter 卡在 waitpid），**关标签页/分屏就直接卡死**。而且它是间歇性的
 * ——只在子进程恰好处于 root 阶段时命中，表现为「有时瞬间关掉、有时卡住」
 * （实测本机因此堆了 52 个杀不掉的 login 进程）。
 * `login -f` 的 "-f" 只表示「跳过认证」，它照样 fork + setuid，救不了。
 *
 * argv[0] 前缀 `-` 就够了吗？**PATH 补全这一条成立**：实测
 * `execv("/bin/zsh", {"-zsh", ...})` 确实读了 /etc/zprofile，PATH 里有
 * /usr/sbin 与 Homebrew。而上游当初不用它的理由（getlogin() 不对、SHELL
 * 没设）在我们这里不成立：自绘终端不依赖 getlogin()，SHELL 由我们自己注入
 * 子进程环境（build_child_env 带上父进程的 SHELL，缺失时 default_shell
 * 会问 passwd）。拿「偶尔卡死」换「getlogin() 更漂亮」不划算。
 *
 * 只在 `o->cmd_kind == GT_PTY_CMD_SHELL` 时用。**判据不是「给没给
 * executable」**——用户在设置里挑的 shell 也带 executable，它依然是交互
 * shell，照样要登录语义。拿「给了 exe」当「不要登录 shell」的那一版，
 * 让分屏那格的 brew 全不见了（新建标签页不传 exe 所以是好的，分屏传了
 * 偏好里的 shell 所以是坏的——同一个 app 里两种行为）。
 * 非 macOS 平台不做：Linux/BSD 没有这套 zprofile 语义。
 */
static void macos_login_dash0(const char *shell, char **out_dash0,const gt_allocator_t *allocator) {
  *out_dash0 = NULL;
#if defined(__APPLE__)
  /* argv[0] = "-<basename>"：bash 与 zsh 都据此判定自己是登录 shell。 */
  const char *base = strrchr(shell, '/');
  base = base ? base + 1 : shell;
  size_t n = strlen(base) + 2;
  char *dash0 = pty_allocate(allocator,n);
  if (!dash0) return;
  snprintf(dash0, n, "-%s", base);
  *out_dash0 = dash0;
#else
  (void)shell;(void)allocator;
#endif
}

/*
 * 这个 shell 认不认显式的 `-l`（再补一道保险）。
 *
 * 为什么光靠 argv[0] 的 `-` 不够：那是 POSIX 系 shell 的约定，bash/zsh/
 * tcsh 都认，但 **fish 官方文档只写 `-l/--login`**，argv[0] 前缀没有任何
 * 文档保证；nushell / xonsh / elvish 更是基本不认。而我们的 shell 列表是
 * 解析 /etc/shells 来的，Homebrew 装的 fish 会把自己写进去——也就是说
 * 用户**选得到**一个只靠 dash 前缀保证不了的 shell。两道一起上，哪道成立
 * 都行。
 *
 * **只在调用方没给 argv 时补**，这不是偷懒：tcsh/csh 的 `-l` 规定「只能
 * 是唯一的标志」，`tcsh -l -c …` 直接报错退出——那比 PATH 不全糟得多
 * （用户看到的是个起不来的窗格）。本机实测：`tcsh -l` 单独跑 rc=0，
 * 叠上 `-c` 就 rc=1。
 *
 * 名单按「本机实测认 -l」取：bash / zsh / ksh / dash / sh 实测通过，
 * tcsh / csh 在单独使用时通过，fish 按官方文档收录。拿不准的一律不收——
 * 漏收只是回到 dash 前缀那一档，误收会让窗格起不来。
 */
static bool shell_takes_dash_l(const char *shell) {
  static const char *const ok[] = {"bash", "zsh",  "fish", "ksh",
                                   "tcsh", "csh",  "dash", "sh"};
  const char *base = strrchr(shell, '/');
  base = base ? base + 1 : shell;
  for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) {
    if (strcmp(base, ok[i]) == 0) return true;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* waiter：只收尸，不碰字节流                                          */
/* ------------------------------------------------------------------ */

static void *pty_waiter(void *arg) {
  gt_pty_backend_t *t = arg;
  int status = 0;
  pid_t r;
  do {
    r = waitpid(t->child, &status, 0);
  } while (r < 0 && errno == EINTR);

  int code = -1;
  if (r == t->child) {
    if (WIFEXITED(status)) code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
  }
  if(r==t->child && (WIFEXITED(status) || WIFSIGNALED(status))) {
    atomic_store(&t->observed_code,WIFEXITED(status)?WEXITSTATUS(status):WTERMSIG(status));
    atomic_store(&t->observed_kind,WIFEXITED(status)?GT_IO_EXIT_CODE:GT_IO_EXIT_SIGNAL);
  }
  atomic_store(&t->exit_code, code);
  atomic_store(&t->exited, true);
  if (t->wake_w >= 0) {
    char c = 1;
    ssize_t ig = write(t->wake_w, &c, 1);
    (void)ig;
  }
  char msg[64];
  snprintf(msg, sizeof msg, "pty child exited (code=%d)", code);
  gt__emit_status(code, msg);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* 打开 / 读写 / 关闭                                                  */
/* ------------------------------------------------------------------ */

gt_io_backend_t *gt__pty_open(const gt_pty_opts_t *opts) {
  return gt__pty_open_with_allocator(opts,NULL);
}
gt_io_backend_t *gt__pty_open_with_allocator(const gt_pty_opts_t *opts,const gt_allocator_t *allocator) {
  return gt__pty_open_with_options(opts,allocator,NULL);
}
gt_io_backend_t *gt__pty_open_with_options(const gt_pty_opts_t *opts,const gt_allocator_t *allocator,const gt_thread_options_t *thread_options) {
  if(thread_options) {
    pthread_attr_t attributes;
    if(gt__thread_attributes(&attributes,thread_options)){gt__set_error("pty: invalid thread options");return NULL;}
    (void)pthread_attr_destroy(&attributes);
  }
  gt_allocator_t domain=allocator?*allocator:(gt_allocator_t){NULL,default_allocate,default_release};
  if(!domain.allocate || !domain.release){gt__set_error("pty: invalid allocator");return NULL;}
  static const gt_pty_opts_t zero;
  const gt_pty_opts_t *o = opts ? opts : &zero;
  if(gt__pty_size_check(o->cols,o->rows,o->px_width,o->px_height,true)!=GT_OK) {
    gt__set_error("pty: pixel dimensions exceed POSIX winsize capability");return NULL;
  }

  long child_fd_limit = sysconf(_SC_OPEN_MAX);
  if (child_fd_limit < 3 || child_fd_limit > INT_MAX) {
    gt__set_error("pty: cannot determine child descriptor limit");
    return NULL;
  }

  gt_pty_backend_t *t = pty_calloc(&domain,1, sizeof *t);
  if (!t) {
    gt__set_error("pty: out of memory");
    return NULL;
  }
  t->base.ops = &pty_ops;t->allocator=domain;
  t->master_fd = -1;
  t->wake_r = t->wake_w = -1;
  atomic_init(&t->exited, false);
  atomic_init(&t->closing, false);
  atomic_init(&t->exit_code, -1);
  atomic_init(&t->observed_kind,GT_IO_EXIT_UNKNOWN);atomic_init(&t->observed_code,0);

  bool is_shell = (o->cmd_kind == GT_PTY_CMD_SHELL);
  char *exe = (o->executable && *o->executable) ? pty_strdup(&domain,o->executable)
                                                : default_shell(&domain);
  if (!exe) {
    gt__set_error("pty: executable allocation failed");
    pty_free(t);
    return NULL;
  }

  /* macOS：交互 shell 要起成**登录 shell**（argv[0] 前缀 `-`）。 */
  char *dash0 = NULL;
  if (is_shell) macos_login_dash0(exe, &dash0,&domain);
#if defined(__APPLE__)
  if(is_shell && !dash0){pty_free(exe);pty_free(t);gt__set_error("pty: login argv allocation failed");return NULL;}
#endif

  size_t extra = arr_len(o->argv);
  /* 调用方没给参数时，对认 `-l` 的 shell 再显式补一个（见
   * shell_takes_dash_l：给的 fish 之流留后路，而 tcsh 要求 `-l` 独占，
   * 所以只在 extra == 0 时补）。 */
  bool add_dash_l = is_shell && extra == 0 && shell_takes_dash_l(exe);
  size_t nargs = extra + (add_dash_l ? 1u : 0u);
  char **argv = pty_calloc(&domain,nargs + 2, sizeof(char *));
  if (!argv) {
    gt__set_error("pty: out of memory");
    pty_free(dash0);
    pty_free(exe);
    pty_free(t);
    return NULL;
  }
  /* 按约定 opts->argv 不含 argv[0]，此处补上——登录 shell 用 "-<shell>"，
   * 否则就是可执行文件路径本身。 */
  argv[0] = dash0 ? dash0 : exe;
  size_t k = 1;
  if (add_dash_l) argv[k++] = (char *)"-l";
  for (size_t i = 0; i < extra; i++) argv[k++] = (char *)o->argv[i];
  argv[k] = NULL;

  char *term_kv = NULL;
  char **env = build_child_env(o->term, o->env, &term_kv,&domain);
  if (!env) {
    pty_free(argv);
    pty_free(dash0);
    pty_free(exe);
    pty_free(t);
    return NULL;
  }

  struct winsize ws = make_winsize(o->cols, o->rows, o->px_width, o->px_height);

  int master = -1;
  pid_t pid = forkpty(&master, NULL, NULL, &ws);
  if (pid < 0) {
    gt__set_error("pty: forkpty failed: %s", strerror(errno));
    pty_free(env);
    pty_free(term_kv);
    pty_free(argv);
    pty_free(dash0);
    pty_free(exe);
    pty_free(t);
    return NULL;
  }
  if (pid == 0) {
    /* 子进程：自成一个进程组，使父进程可以整组信号（shell + 其后代），
     * 且绝不会误伤宿主进程组。forkpty 通常会 setsid，但它必须靠
     * ioctl(TIOCSCTTY) 才真正生效；某些 macOS 构建会静默失败，故显式补齐。 */
    setpgid(0, 0);
    close_child_fds(child_fd_limit);
    if (o->cwd && *o->cwd && chdir(o->cwd) != 0) _exit(127);
    execve(exe, argv, env);
    _exit(127);
  }

  /* 父进程：子进程已持有各自的内存副本，可安全释放临时数组。 */
  pty_free(env);
  pty_free(term_kv);
  pty_free(argv);
  pty_free(dash0);
  pty_free(exe);

  t->child = pid;
  /*
   * 这里**不能**再对子进程 setpgid(pid, pid)。
   *
   * forkpty 的子进程要在 login_tty 里 setsid() 才能取得会话与控制终端；
   * 而「父进程抢先把子进程设成进程组长」会让子进程的 setsid() 报 EPERM
   * （进程组长不能再 setsid）→ 子进程拿不到控制终端。实测后果：
   * tdev=NODEV、子进程开 /dev/tty 返回 ENXIO、tcgetpgrp(master) 恒为 0
   * ——作业控制（前台进程组）整套失效，而这一整套正是「关标签页时判断
   * 有没有前台作业」的依据。
   *
   * 自成进程组由**子进程自己**做（见 child 分支的 setpgid(0,0)）：那是在
   * forkpty 的 setsid() 之后执行的，不会踩这个坑。
   */
  t->master_fd = master;
  /* master 非阻塞：读路径完全由 poll 驱动（超时靠自管道/轮询），
   * 这样 EOF 探测与「谁在读」解耦，也不会因误判而阻塞。 */
  if (nonblocking_cloexec(t->master_fd) < 0) {
    gt__set_error("pty: cannot configure master descriptor: %s", strerror(errno));
    goto child_failed;
  }

  int wp[2];
  if (pipe(wp) < 0) {
    gt__set_error("pty: cannot create wakeup pipe: %s", strerror(errno));
    goto child_failed;
  }
  t->wake_r = wp[0];
  t->wake_w = wp[1];
  if (nonblocking_cloexec(t->wake_r) < 0 || nonblocking_cloexec(t->wake_w) < 0) {
    gt__set_error("pty: cannot configure wakeup descriptors: %s", strerror(errno));
    goto child_failed;
  }

  if (gt__thread_create(&t->waiter,pty_waiter,t,thread_options) != 0) {
    gt__set_error("pty: pthread_create failed");
    goto child_failed;
  }
  t->has_waiter = true;
  return &t->base;

child_failed:
  kill(pid, SIGKILL);
  /* Release the controlling PTY before waiting, just as in normal close. */
  if (t->master_fd >= 0) close(t->master_fd);
  if (t->wake_r >= 0) close(t->wake_r);
  if (t->wake_w >= 0) close(t->wake_w);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
  pty_free(t);
  return NULL;
}

gt_status_t gt__pty_waiter_thread(const gt_io_backend_t *backend,pthread_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!backend || !out)return GT_ERR_INVALID_ARG;
  if(backend->ops!=&pty_ops)return GT_ERR_UNSUPPORTED;
  const gt_pty_backend_t *t=(const gt_pty_backend_t *)backend;
  if(!t->has_waiter)return GT_ERR_UNSUPPORTED;
  *out=t->waiter;return GT_OK;
}

/* 把积压的输入尾巴尽力送进 master（**非阻塞**，EAGAIN 即止，下次再冲）。 */
static void pty_flush_wtail(gt_pty_backend_t *t) {
  while (t->wtail_len > 0 && t->master_fd >= 0) {
    ssize_t n = write(t->master_fd, t->wtail, t->wtail_len);
    if (n > 0) {
      t->wtail_len -= (size_t)n;
      if (t->wtail_len > 0) memmove(t->wtail, t->wtail + n, t->wtail_len);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break; /* EAGAIN / 出错：留给下一次（不在这里等） */
  }
  if (t->wtail_len == 0) {
    pty_free(t->wtail);
    t->wtail = NULL;
  }
}

/* 把写不进去的尾巴接到待送缓冲尾部。返回 GT_OK，或超上限/OOM 时显式报错。 */
static int pty_wtail_append(gt_pty_backend_t *t, const uint8_t *data, size_t len) {
  if (len == 0) return GT_OK;
  if (len > GT_PTY_WTAIL_MAX - t->wtail_len) {
    gt__set_error("transport_write: 输入积压超过 %u 字节（子进程长时间不读输入）",
                  (unsigned)GT_PTY_WTAIL_MAX);
    return GT_ERR_IO;
  }
  uint8_t *grown = pty_reallocate(&t->allocator,t->wtail, t->wtail_len + len);
  if (!grown) {
    gt__set_error("transport_write: 输入积压分配失败");
    return GT_ERR_IO;
  }
  memcpy(grown + t->wtail_len, data, len);
  t->wtail = grown;
  t->wtail_len += len;
  return GT_OK;
}

static int pty_write(gt_io_backend_t *backend, const uint8_t *data, size_t len) {
  gt_pty_backend_t *t = (gt_pty_backend_t *)backend;
  if (!t || (!data && len)) {
    gt__set_error("transport_write: null argument");
    return GT_ERR_INVALID_ARG;
  }
  if (t->master_fd < 0) {
    gt__set_error("transport_write: pty 已关闭");
    return GT_ERR_IO;
  }

  /* 先把上次没送出去的尾巴补上（保持输入顺序）。 */
  pty_flush_wtail(t);

  size_t off = 0;
  /* 本来就有积压 = 子进程显然跟不上 → 不再等；否则给一小段宽限冲掉突发。
   * 有了这段宽限，正常交互与「子进程读得慢但在读」都不会无谓地攒尾巴。 */
  int64_t deadline = mono_ms() + (t->wtail_len > 0 ? 0 : GT_PTY_WRITE_GRACE_MS);
  while (off < len) {
    ssize_t n = write(t->master_fd, data + off, len - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* 队列满：**有界**地等一小段。到点就把剩下的攒起来——
       * 绝不 `poll(...,-1)` 无限等（那会把会话线程钉死，见结构体注释）。 */
      int64_t rem = deadline - mono_ms();
      if (rem <= 0) break;
      struct pollfd p;
      p.fd = t->master_fd;
      p.events = POLLOUT;
      p.revents = 0;
      int pr = poll(&p, 1, (int)rem);
      if (pr < 0 && errno != EINTR) {
        gt__set_error("transport_write: poll: %s", strerror(errno));
        return GT_ERR_IO;
      }
      continue;
    }
    gt__set_error("transport_write: %s", strerror(errno));
    return GT_ERR_IO;
  }
  if (off < len) return pty_wtail_append(t, data + off, len - off);
  return GT_OK;
}

static int pty_resize(gt_io_backend_t *backend, uint16_t cols, uint16_t rows,
                      uint32_t px_w, uint32_t px_h) {
  gt_pty_backend_t *t = (gt_pty_backend_t *)backend;
  if (!t) return GT_ERR_INVALID_ARG;
  gt_status_t size_status=gt__pty_size_check(cols,rows,px_w,px_h,false);
  if(size_status!=GT_OK){gt__set_error("transport_resize: invalid cells or unsupported POSIX pixel dimensions");return size_status;}
  struct winsize ws = make_winsize(cols, rows, px_w, px_h);
  if (ioctl(t->master_fd, TIOCSWINSZ, &ws) < 0) {
    gt__set_error("transport_resize: %s", strerror(errno));
    return GT_ERR_IO;
  }
  return GT_OK;
}

static int pty_read_with_wake(gt_io_backend_t *backend, uint8_t *buf, size_t cap,
                    int timeout_ms, int command_fd) {
  gt_pty_backend_t *t = (gt_pty_backend_t *)backend;
  if (!t || !buf) {
    gt__set_error("transport_read: null argument");
    return GT_TRANSPORT_READ_ERROR;
  }
  if (cap == 0) {
    gt__set_error("transport_read: cap must be > 0");
    return GT_TRANSPORT_READ_ERROR;
  }
  if (t->eof) return GT_TRANSPORT_READ_EOF;

  /* 顺带把积压的输入尾巴冲出去：写路径满时留下的，此后每次读都重试，
   * 所以「子进程重新开始读」时输入会被及时补送（读循环本身从不被写阻塞）。 */
  pty_flush_wtail(t);

  bool bounded = timeout_ms >= 0;
  int64_t deadline = bounded ? mono_ms() + timeout_ms : 0;

  for (;;) {
    int wait_ms = -1;
    if (bounded) {
      int64_t rem = deadline - mono_ms();
      if (rem < 0) rem = 0;
      wait_ms = (int)rem;
    }

    struct pollfd pfds[3];
    pfds[0].fd = t->master_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = t->wake_r;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    pfds[2].fd = command_fd;
    pfds[2].events = POLLIN;
    pfds[2].revents = 0;
    int pr = poll(pfds, 3, wait_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      gt__set_error("transport_read: poll failed: %s", strerror(errno));
      return GT_TRANSPORT_READ_ERROR;
    }

    if (atomic_load(&t->closing)) {
      t->eof = true;
      return GT_TRANSPORT_READ_EOF;
    }
    /* The worker owns/drains this fd. Prefer one ready output chunk so a
     * continuous command producer cannot starve the PTY. The next worker
     * round handles another command before reading again. */
    if (pfds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) return GT_TRANSPORT_READ_ERROR;
    if ((pfds[2].revents & POLLIN) &&
        !(pfds[0].revents & (POLLIN | POLLHUP | POLLERR)))
      return GT_TRANSPORT_READ_TIMEOUT;
    if (pr == 0) {
      /* 超时：再做一次非阻塞探测。Darwin 上「对端已关闭」不一定报 POLLHUP，
       * EOF 的可靠识别不能只依赖 poll —— 这里读一手，0/EIO 即 EOF。 */
      ssize_t n = read(t->master_fd, buf, cap);
      if (n > 0) return (int)n;
      if (n == 0 || (n < 0 && errno == EIO)) {
        t->eof = true;
        return GT_TRANSPORT_READ_EOF;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return GT_TRANSPORT_READ_TIMEOUT;
      }
      if (n < 0 && errno == EINTR) continue;
      gt__set_error("transport_read: %s", strerror(errno));
      return GT_TRANSPORT_READ_ERROR;
    }

    if (pfds[1].revents & POLLIN) {
      char tmp[64];
      while (t->wake_r >= 0 && read(t->wake_r, tmp, sizeof tmp) > 0) {
      }
    }

    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = read(t->master_fd, buf, cap);
      if (n > 0) return (int)n;
      if (n == 0) {
        t->eof = true;
        return GT_TRANSPORT_READ_EOF;
      }
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      /* pty master 上的 EIO 表示从端（控制终端）已关闭 —— 即 EOF，
       * 不能折叠成错误（且必须保证此前读到/未读的数据已排空）。 */
      if (errno == EIO) {
        t->eof = true;
        return GT_TRANSPORT_READ_EOF;
      }
      gt__set_error("transport_read: %s", strerror(errno));
      return GT_TRANSPORT_READ_ERROR;
    }
  }
}

static int pty_read(gt_io_backend_t *backend,uint8_t *buf,size_t cap,int timeout_ms) {
  return pty_read_with_wake(backend,buf,cap,timeout_ms,-1);
}

static void pty_close(gt_io_backend_t *backend) {
  gt_pty_backend_t *t = (gt_pty_backend_t *)backend;
  if (!t) return;

  atomic_store(&t->closing, true);
  if (t->wake_w >= 0) {
    char c = 1;
    ssize_t ig = write(t->wake_w, &c, 1);
    (void)ig;
  }

  if (!atomic_load(&t->exited) && t->child > 0) {
    /* 目标是「整组」：shell 及其后代都要收掉，否则前台子进程留着会拖住
     * 从端、导致 EOF 永远不来（实测：登录 zsh 会把子进程放进**它自己的**
     * 组，单发给 shell 收不掉它们）。打整组的前提是那个组**不是我们自己的**
     * ——绝不能把信号打到自己所在的进程组（那会连带打死宿主）。child 的组
     * 既可能是它自己，也可能被 shell 换过，所以两个都发；重复发是安全的
     * （进程已死时 kill 只返回 ESRCH）。 */
    pid_t mypg = getpgid(0);
    pid_t pg = getpgid(t->child);
    bool group_is_ours = (pg == mypg);
    if (!group_is_ours && pg > 0) kill(-pg, SIGHUP);
    kill(t->child, SIGHUP);
    /*
     * 只给很短的宽限（200ms）就升级到 SIGKILL。
     *
     * 为什么不给足：**交互式 shell 会忽略 SIGHUP**（zsh/bash 的常规行为：
     * 免得关终端把用户正在跑的东西带走）。实测登录 zsh 上 SIGHUP 永远不
     * 生效，原先的 2 秒宽限就是白等——关一个标签页要卡 2.3 秒。真正生效的
     * 是 SIGKILL，宽限只用来给「响应 SIGHUP 的」普通子进程一个体面退场。
     */
    for (int i = 0; i < 20 && !atomic_load(&t->exited); i++) {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 10 * 1000 * 1000; /* 10ms */
      nanosleep(&ts, NULL);
    }
    if (!atomic_load(&t->exited)) {
      if (!group_is_ours && pg > 0) kill(-pg, SIGKILL);
      kill(t->child, SIGKILL);
    }
  }

  /*
   * 让子进程真的能退出：**必须先关掉 master 触发从端 hangup**。
   *
   * 会话首进程（我们的 shell）退出时，内核要撤销它的控制终端；而只要前台
   * 作业（如 `sleep`/`make`）还握着从端，这个撤销就一直挂着——实测「关闭
   * 一个正跑着作业的标签页」会**永久卡住**（子进程停在 trying-to-exit，
   * waitpid 永不返回）。关掉 master 即 hangup 从端：内核给会话/前台组送
   * SIGHUP，撤销得以完成，子进程随即退出。
   *
   * 这一步必须在 join 之前做：否则 waiter 的 waitpid 卡在子进程的 revoke 上。
   */
  if (t->master_fd >= 0) {
    close(t->master_fd);
    t->master_fd = -1;
  }
  if (t->has_waiter) pthread_join(t->waiter, NULL);
  if (t->wake_r >= 0) close(t->wake_r);
  if (t->wake_w >= 0) close(t->wake_w);
  pty_free(t->wtail);
  pty_free(t);
}

static bool pty_exited(const gt_io_backend_t *backend, int *exit_code) {
  const gt_pty_backend_t *t = (const gt_pty_backend_t *)backend;
  if (!t) {
    if (exit_code) *exit_code = 0;
    return false;
  }
  bool e = atomic_load(&t->exited);
  if (e && exit_code) *exit_code = atomic_load(&t->exit_code);
  return e;
}

static gt_status_t pty_observe_exit(const gt_io_backend_t *backend,gt_io_exit_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!backend || !out)return GT_ERR_INVALID_ARG;
  const gt_pty_backend_t *t=(const gt_pty_backend_t *)backend;
  if(!atomic_load(&t->exited))return GT_ERR_NOT_FOUND;
  gt_io_exit_kind_t kind=(gt_io_exit_kind_t)atomic_load(&t->observed_kind);
  if(kind==GT_IO_EXIT_UNKNOWN)return GT_ERR_NOT_FOUND;
  *out=(gt_io_exit_t){kind,atomic_load(&t->observed_code)};return GT_OK;
}
static gt_transport_child_t pty_child_state(const gt_io_backend_t *backend) {
  const gt_pty_backend_t *t = (const gt_pty_backend_t *)backend;
  if (!t) return GT_TRANSPORT_CHILD_UNKNOWN;
  if (atomic_load(&t->exited)) return GT_TRANSPORT_CHILD_EXITED;
  if (t->master_fd < 0 || t->child <= 0) return GT_TRANSPORT_CHILD_UNKNOWN;
  /*
   * 前台进程组 == 子进程本身的组 → 停在提示符（空闲）；否则是别的组
   * （shell 把终端交给了某个作业）。child 在自身会话里既是组长也是本组
   * 唯一成员，故 getpgid(child) 就是「shell 自己那个组」。任一查询失败
   * 都报 UNKNOWN（**不**当空闲）。
   */
  pid_t shell_pgid = getpgid(t->child);
  pid_t fg = tcgetpgrp(t->master_fd);
  if (shell_pgid <= 0 || fg <= 0) return GT_TRANSPORT_CHILD_UNKNOWN;
  return fg == shell_pgid ? GT_TRANSPORT_CHILD_IDLE : GT_TRANSPORT_CHILD_JOB;
}

static bool pty_child_cwd(const gt_io_backend_t *backend, char *buf, size_t cap) {
  const gt_pty_backend_t *t = (const gt_pty_backend_t *)backend;
  if (!t || !buf || cap < 2) {
    gt__set_error("child_cwd: 参数非法");
    return false;
  }
  if (atomic_load(&t->exited)) {
    gt__set_error("child_cwd: 子进程已退出");
    return false;
  }
  if (t->child <= 0) {
    gt__set_error("child_cwd: 无子进程");
    return false;
  }
#if defined(__APPLE__)
  struct proc_vnodepathinfo vpi;
  memset(&vpi, 0, sizeof vpi);
  int n = proc_pidinfo(t->child, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi);
  if (n <= 0 || vpi.pvi_cdir.vip_path[0] == '\0') {
    gt__set_error("child_cwd: proc_pidinfo 没给出工作目录");
    return false;
  }
  snprintf(buf, cap, "%s", vpi.pvi_cdir.vip_path);
  return true;
#elif defined(__linux__)
  char link[64];
  snprintf(link, sizeof link, "/proc/%d/cwd", (int)t->child);
  ssize_t n = readlink(link, buf, cap - 1);
  if (n < 0) {
    gt__set_error("child_cwd: readlink 失败: %s", strerror(errno));
    return false;
  }
  buf[n] = '\0';
  return true;
#else
  gt__set_error("child_cwd: 该平台不支持");
  return false;
#endif
}

static const gt_io_ops_t pty_ops = {
  .read = pty_read,
  .write = pty_write,
  .resize = pty_resize,
  .exited = pty_exited,
  .child_state = pty_child_state,
  .child_cwd = pty_child_cwd,
  .close = pty_close,
  .observe_exit = pty_observe_exit,
  .read_with_wake = pty_read_with_wake,
};
