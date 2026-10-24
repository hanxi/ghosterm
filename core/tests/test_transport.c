/*
 * test_transport.c — 本地 PTY transport 的行为断言（离线可跑，真 forkpty）。
 *
 * 覆盖点（每一条都对应一条不变量，而非「跑通就算」）：
 *   1. EOF 前的最后一批数据不丢（printf 后立即退出，逐字节不截断）
 *   2. 子进程退出码经 waitpid 如实上报
 *   3. 写 → 从端回显（字节往返，含换行 CR 归一化）
 *   4. 超时不折叠成 EOF（三态）
 *   5. 打开时的 winsize 生效（stty size 真读控制终端）
 *   6. 运行中 resize 生效（TIOCSWINSZ）
 *   7. TERM 由 opts 注入（默认 xterm-256color，可覆盖）
 *   8. close(NULL)/exited(NULL) 安全
 *   9. 写队列满时**有界返回**、且积压字节一个不丢（写绝不无限等——
 *      旧实现的 poll(-1) 会把会话线程钉死，进而 stop() 的 queue.sync 把
 *      主线程也钉死：整个 app 卡死）
 *  10. cmd_kind 决定登录语义：SHELL 起登录 shell（PATH 经 path_helper 补全），
 *      DIRECT 一点登录语义都不加；判据是 tag 而非「给没给 executable」
 */
#include "ghosterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int pass = 0, fail = 0;
#define CHECK(c, msg)                          \
  do {                                         \
    if (c) {                                   \
      pass++;                                  \
    } else {                                   \
      fail++;                                  \
      printf("FAIL: %s\n", msg);               \
    }                                          \
  } while (0)

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000 * 1000;
  nanosleep(&ts, NULL);
}

/* 读到 EOF 或 max_ms 用尽；总是 NUL 结尾。返回收到的字节数。 */
static size_t read_until_eof(gt_transport_t *t, char *buf, size_t cap, int max_ms) {
  size_t total = 0;
  int elapsed = 0;
  const int per = 100;
  while (total + 1 < cap && elapsed < max_ms) {
    int n = gt_transport_read(t, (uint8_t *)buf + total, cap - total - 1, per);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n == GT_TRANSPORT_READ_EOF) break;
    if (n == GT_TRANSPORT_READ_TIMEOUT) {
      elapsed += per;
      continue;
    }
    break; /* error */
  }
  buf[total] = '\0';
  return total;
}

/* 读到命中 needle / EOF / 超时；返回是否命中。 */
static int read_until_contains(gt_transport_t *t, const char *needle, char *buf,
                               size_t cap, int max_ms) {
  size_t total = 0;
  int elapsed = 0;
  const int per = 100;
  while (total + 1 < cap && elapsed < max_ms) {
    int n = gt_transport_read(t, (uint8_t *)buf + total, cap - total - 1, per);
    if (n > 0) {
      total += (size_t)n;
      buf[total] = '\0';
      if (strstr(buf, needle)) return 1;
      continue;
    }
    if (n == GT_TRANSPORT_READ_EOF) break;
    if (n == GT_TRANSPORT_READ_TIMEOUT) {
      elapsed += per;
      continue;
    }
    break;
  }
  buf[total] = '\0';
  return strstr(buf, needle) != NULL;
}

/* 起一个**默认** shell（=登录 shell），等它进入交互状态后关掉，返回 close
 * 耗时（秒）。opened / saw_output 把「起不来」与「起来了但没输出」分开报，
 * 折叠成一种就看不出是哪种故障。
 *
 * 为什么必须「等到有输出再关」：固定 sleep 不可靠——shell 在读 rc 文件期间
 * 还没装 SIGHUP 处理器，那时关它会「很配合地」秒退，于是变异（把宽限写长）
 * 也能蒙混过关（实测踩到：同一变异手动跑红、脚本里跑绿）。等到有输出时
 * shell 已在正常交互状态，SIGHUP 才真的被忽略。
 */
static double open_interactive_then_close(bool *opened, bool *saw_output) {
  gt_pty_opts_t o;
  memset(&o, 0, sizeof o);
  o.cols = 80; o.rows = 24; o.term = "xterm-256color";
  gt_transport_t *t = gt_transport_open_pty(&o);
  *opened = (t != NULL);
  if (!t) return -1;

  char rb[4096];
  int got = 0;
  for (int i = 0; i < 200 && got == 0; i++) { /* 最多 20s */
    int n = gt_transport_read(t, (uint8_t *)rb, sizeof rb, 100);
    if (n > 0) got = n;
    else if (n == GT_TRANSPORT_READ_EOF) break;
  }
  *saw_output = (got > 0);
  if (!*saw_output) {
    gt_transport_close(t);
    return -1;
  }

  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  gt_transport_close(t);
  clock_gettime(CLOCK_MONOTONIC, &b);
  return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

/* 量一遍「pty_close 里那段宽限轮询」在**当前负载下**的真实耗时。
 *
 * 实现等 SIGHUP 生效的方式是 20 次 10ms 的 nanosleep 轮询（见
 * core/src/io/pty_posix.c 的 pty_close），交互式 shell 忽略 SIGHUP，所以
 * close 的 wall-clock 里**必然含整段宽限**——名义 0.2s。共享 CI runner 会把
 * 每次唤醒往后挤（实测三次 close = 1.07/1.53/1.09s），于是「拿固定 1 秒当
 * 预算」测的就不是功能，是调度抖动。标定用同一个循环，放大就是双向的：
 * 真回归（把宽限写长、或 close 只发单个 child 收不掉整组）只有 close 一侧
 * 变慢，照样红。
 */
static double measure_grace_poll_cost(void) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < 20; i++) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10 * 1000 * 1000; /* 与 pty_close 同一个步长 */
    nanosleep(&ts, NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &b);
  return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

/* hay[0..haylen) 是否含 needle。 */
static int mem_contains(const char *hay, size_t haylen, const char *needle) {
  size_t nl = strlen(needle);
  if (nl == 0 || haylen < nl) return 0;
  for (size_t i = 0; i + nl <= haylen; i++) {
    if (memcmp(hay + i, needle, nl) == 0) return 1;
  }
  return 0;
}

static int wait_exited(gt_transport_t *t, int *code, int max_ms) {
  for (int e = 0; e < max_ms; e += 10) {
    if (gt_transport_exited(t, code)) return 1;
    sleep_ms(10);
  }
  return gt_transport_exited(t, code);
}

static gt_transport_t *open_sh(const char *script, uint16_t cols, uint16_t rows,
                               const char *term) {
  const char *argv[] = {"-c", script, NULL};
  gt_pty_opts_t o;
  memset(&o, 0, sizeof o);
  o.executable = "/bin/sh";
  o.argv = argv;
  o.cmd_kind = GT_PTY_CMD_DIRECT; /* 跑一条命令，不是用户的交互 shell */
  o.cols = cols;
  o.rows = rows;
  o.term = term;
  return gt_transport_open_pty(&o);
}

int main(int argc, char **argv_main) {
  setvbuf(stdout, NULL, _IONBF, 0); /* 便于闸门失败时定位 */
  char buf[8192];
  /* 登录 shell 那节要一个干净的 HOME 与几个假 shell 脚本。按 AGENTS.md，
   * 临时文件只落工程根的 tmp/，且**不从系统 /tmp 执行**（那里起的进程会被
   * 判死）；路径由 CTest 传进来。 */
  char tmpdir[1024];
  snprintf(tmpdir, sizeof tmpdir, "%s/pty-login-XXXXXX",
           argc >= 2 ? argv_main[1] : ".");
  if (!mkdtemp(tmpdir)) {
    printf("FAIL: 建不出临时目录\n");
    return 1;
  }
  #define SEC(x) fprintf(stderr, "[section] %s\n", x)

  SEC("1+2"); /* 1 + 2 */
  {
    gt_transport_t *t = open_sh("printf abcdef", 80, 24, NULL);
    CHECK(t != NULL, "open pty(sh printf) 成功");
    if (t) {
      size_t n = read_until_eof(t, buf, sizeof buf, 5000);
      CHECK(n == 6 && strcmp(buf, "abcdef") == 0,
            "EOF 前最后一批数据不丢（应为 abcdef）");
      int code = -123;
      CHECK(wait_exited(t, &code, 1000) && code == 0, "退出码 0 如实上报");
      gt_transport_close(t);
    }
  }

  SEC("2b");
  {
    gt_transport_t *t = open_sh("exit 7", 80, 24, NULL);
    if (t) {
      read_until_eof(t, buf, sizeof buf, 5000);
      int code = -123;
      CHECK(wait_exited(t, &code, 1000) && code == 7, "退出码 7 如实上报");
      gt_transport_close(t);
    }
  }

  SEC("3");
  {
    gt_transport_t *t = open_sh("cat", 80, 24, NULL);
    CHECK(t != NULL, "open pty(cat) 成功");
    if (t) {
      int w = gt_transport_write(t, (const uint8_t *)"ping\n", 5);
      CHECK(w == GT_OK, "gt_transport_write 返回 GT_OK");
      CHECK(read_until_contains(t, "ping", buf, sizeof buf, 3000),
            "写入数据经从端回显往返");
      gt_transport_close(t);
    }
  }

  SEC("4");
  {
    gt_transport_t *t = open_sh("cat", 80, 24, NULL);
    if (t) {
      int n = gt_transport_read(t, (uint8_t *)buf, sizeof buf, 150);
      CHECK(n == GT_TRANSPORT_READ_TIMEOUT, "无数据超时返回 GT_TRANSPORT_READ_TIMEOUT");
      CHECK(!gt_transport_exited(t, NULL), "超时不得被当作进程已退出");
      gt_transport_close(t);
    }
  }

  SEC("5");
  {
    gt_transport_t *t = open_sh("stty size", 100, 40, NULL);
    CHECK(t != NULL, "open pty(stty size) 成功");
    if (t) {
      CHECK(read_until_contains(t, "40 100", buf, sizeof buf, 3000),
            "打开时 winsize(cols=100,rows=40) 生效");
      gt_transport_close(t);
    }
  }

  SEC("6");
  {
    gt_pty_opts_t o;
    memset(&o, 0, sizeof o);
    o.executable = "/bin/sh";
    o.cmd_kind = GT_PTY_CMD_DIRECT; /* 本节验 resize，不要登录 shell 的额外行为 */
    o.cols = 80;
    o.rows = 24;
    gt_transport_t *t = gt_transport_open_pty(&o);
    CHECK(t != NULL, "open 交互式 sh 成功");
    if (t) {
      CHECK(gt_transport_resize(t, 120, 33, 0, 0) == GT_OK, "resize 返回 GT_OK");
      gt_transport_write(t, (const uint8_t *)"stty size\n", 10);
      CHECK(read_until_contains(t, "33 120", buf, sizeof buf, 3000),
            "resize 后 stty size 反映新尺寸");
      gt_transport_close(t);
    }
  }

  SEC("7");
  {
    gt_transport_t *t = open_sh("printf %s \"$TERM\"", 80, 24, NULL);
    if (t) {
      read_until_contains(t, "xterm-256color", buf, sizeof buf, 3000);
      CHECK(strstr(buf, "xterm-256color") != NULL, "默认 TERM=xterm-256color");
      gt_transport_close(t);
    }
    gt_transport_t *t2 = open_sh("printf %s \"$TERM\"", 80, 24, "screen-256color");
    if (t2) {
      read_until_contains(t2, "screen-256color", buf, sizeof buf, 3000);
      CHECK(strstr(buf, "screen-256color") != NULL, "opts.term 覆盖 TERM");
      gt_transport_close(t2);
    }
  }

  SEC("9 login-shell");
  {
    /* 不指定 executable → 走「登录 shell」路径（macOS 经 login(1) 包装）。
     *
     * **必须注入最小 PATH** 才能验到东西：测试进程继承的 PATH 里本来就有
     * /usr/sbin（自己的 shell 已经补过），不注入的话「非登录 shell」也能
     * 通过——第一版断言就栽在这里（变异不红）。
     * 注入的这套正是 GUI（Finder/Dock）启动时的真实环境（launchd 给的）。
     *
     * 判据用**行为**而不是实现细节：登录 shell 会读 /etc/zprofile，那里有
     * /usr/libexec/path_helper——它把 /etc/paths 的条目（含 /usr/sbin）
     * 补进 PATH。非登录 shell 不读 zprofile，PATH 就还是那三个目录。
     *
     * 为什么这条重要：PATH 只有 /usr/bin:/bin 时，Homebrew 装的 brew / go /
     * gls 全是「command not found」——用户在 ~/.zshrc 里看到一片报错。
     * 根因不是 PATH 注错了，是没给登录 shell。
     */
#if defined(__APPLE__)
    /* HOME 指到空目录（与 9b/9c 同一口径）：判据只落在 /etc 那套启动文件上。
     * 用真实 HOME 时结果取决于跑测试那个账号自己的 dotfile——CI 的 runner
     * 账号里就有一条把 PATH 补没了，红得无法归因（实测）。 */
    char home_kv[1024];
    snprintf(home_kv, sizeof home_kv, "HOME=%s", tmpdir);
    const char *minenv[] = {"PATH=/usr/bin:/bin", home_kv, NULL};

    const char *argv[] = {"-c", "echo PATH_IS:$PATH", NULL};
    gt_pty_opts_t o;
    memset(&o, 0, sizeof o);
    o.argv = argv;               /* 注意：**不**设 executable */
    o.env = minenv;              /* 注入 GUI 启动时那套最小 PATH */
    o.cols = 80; o.rows = 24;
    o.term = "xterm-256color";
    gt_transport_t *t = gt_transport_open_pty(&o);
    CHECK(t != NULL, "无 executable 时能起默认 shell");
    if (t) {
      read_until_contains(t, "PATH_IS:", buf, sizeof buf, 20000);
      char *line = strstr(buf, "PATH_IS:");
      bool has_sbin = line && strstr(line, "/usr/sbin") != NULL;
      if (!has_sbin) {
        /* 默认 shell 由 getenv(SHELL) → passwd 解析（见 pty_posix.c 的
         * default_shell）：不印出来就没法区分「我们没起成登录 shell」与
         * 「这台机器的账号 shell 压根不认登录语义」。 */
        struct passwd *pw = getpwuid(getuid());
        const char *sh = getenv("SHELL");
        printf("  归因: $SHELL=%s passwd=%s 期望登录 shell 补出 /usr/sbin，实际 %s\n",
               (sh && *sh) ? sh : "(未设)",
               (pw && pw->pw_shell) ? pw->pw_shell : "(无)",
               line ? line : "(没读到 PATH_IS)");
      }
      CHECK(line != NULL, "默认 shell 输出了 PATH");
      CHECK(has_sbin,
            "默认 shell 必须是**登录 shell**（注入最小 PATH 后仍能经 "
            "path_helper 补出 /usr/sbin）——否则 ~/.zshrc 里的 brew/go "
            "会 command not found");
      gt_transport_close(t);
    }

    /*
     * 9b/9c：**给了 executable 也照样按 cmd_kind 判**。
     *
     * 这两条守的是用户实测那个 bug：「新建标签页的 brew 在、分屏那格的
     * brew 不在」。根因是上一版拿「给没给 executable」当「要不要登录
     * shell」——而设置里挑的 shell 也带 executable，于是分屏（传偏好）
     * 和新建标签页（不传）在同一个 app 里给出两种 PATH。
     *
     * HOME 指到一个空目录：排掉用户自己的 ~/.zshenv 等 dotfile，让判据
     * 只落在 /etc/zprofile 的 path_helper 上（否则 9c 的「没有 /usr/sbin」
     * 会随谁的机器而变）。本机实测过双向：登录态有 /usr/sbin，非登录态没有。
     */
    char tmphome_kv[1024];
    snprintf(tmphome_kv, sizeof tmphome_kv, "HOME=%s", tmpdir);
    const char *cleanenv[] = {"PATH=/usr/bin:/bin", tmphome_kv, NULL};
    const char *zargv[] = {"-c", "echo PATH_IS:$PATH", NULL};

    for (int direct = 0; direct <= 1; direct++) {
      gt_pty_opts_t o2;
      memset(&o2, 0, sizeof o2);
      o2.executable = "/bin/zsh";   /* **显式**给出 —— 同设置里挑了 shell */
      o2.argv = zargv;
      o2.env = cleanenv;
      o2.cmd_kind = direct ? GT_PTY_CMD_DIRECT : GT_PTY_CMD_SHELL;
      o2.cols = 80; o2.rows = 24;
      o2.term = "xterm-256color";
      gt_transport_t *t2 = gt_transport_open_pty(&o2);
      CHECK(t2 != NULL, "显式 executable 起得来");
      if (!t2) continue;
      read_until_contains(t2, "PATH_IS:", buf, sizeof buf, 20000);
      char *line = strstr(buf, "PATH_IS:");
      bool has_sbin2 = line && strstr(line, "/usr/sbin") != NULL;
      if (direct) {
        CHECK(!has_sbin2,
              "GT_PTY_CMD_DIRECT：跑一条命令不该被包成登录 shell"
              "（PATH 里不该冒出 /usr/sbin）");
      } else {
        CHECK(has_sbin2,
              "GT_PTY_CMD_SHELL：**给了 executable 也要**起成登录 shell"
              "——这条红了就是「分屏那格 brew command not found」");
      }
      gt_transport_close(t2);
    }

    /*
     * 9d：认 `-l` 的 shell 要额外拿到 `-l`（见 transport.c 的
     * shell_takes_dash_l）。fish 官方只文档化 `-l`，argv[0] 前缀没保证；
     * 名单外的（nu 之流）不许乱加，加了就是个起不来的窗格。
     *
     * 观测用**同名假 shell 脚本**把自己的参数打出来：shebang 脚本被 exec
     * 时内核会丢掉原 argv[0]（换成解释器），所以这里看不见 dash 前缀，
     * 正好只隔离出 `-l` 这一维——dash 前缀那一维由 9a/9b 的 PATH 守。
     */
    /* 两维：{假 shell 名单} × {调用方给不给 argv}。
     * 期望补 -l 的那一档见 pty_posix.c 的 shell_takes_dash_l：
     * fish/bash 两档都要（bash 给了 argv 就必须补——见 9e），nu 一律不补，
     * tcsh 只认**独占**的 -l（给了 argv 还不补就是起不来的窗格）。 */
    const char *fakes[] = {"fish", "nu", "tcsh", "bash"};
    const bool expect_l[4][2] = {{1, 1}, {0, 0}, {1, 0}, {1, 1}};
    for (int i = 0; i < 4; i++) {
      for (int with_argv = 0; with_argv <= 1; with_argv++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", tmpdir, fakes[i]);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL, "假 shell 脚本写得出");
        if (!f) continue;
        fputs("#!/bin/sh\necho \"ARGV:[$*]\"\n", f);
        fclose(f);
        chmod(path, 0755);

        const char *argv_fake[] = {"-c", "true", NULL};
        gt_pty_opts_t o3;
        memset(&o3, 0, sizeof o3);
        o3.executable = path;
        o3.cmd_kind = GT_PTY_CMD_SHELL;
        o3.argv = with_argv ? argv_fake : NULL;  /* 两档各验一次 */
        o3.cols = 80; o3.rows = 24;
        gt_transport_t *t3 = gt_transport_open_pty(&o3);
        CHECK(t3 != NULL, "假 shell 起得来");
        if (!t3) { unlink(path); continue; }
        read_until_contains(t3, "ARGV:[", buf, sizeof buf, 10000);
        char *line = strstr(buf, "ARGV:[");
        bool got_l = line && strncmp(line, "ARGV:[-l", 8) == 0;
        char msg[200];
        snprintf(msg, sizeof msg, "%s（%s argv）的 -l = %s"
                 "——见 pty_posix.c 的 shell_takes_dash_l 两档名单",
                 fakes[i], with_argv ? "给了" : "没给",
                 expect_l[i][with_argv] ? "必须补" : "不许补");
        CHECK(got_l == expect_l[i][with_argv], msg);
        gt_transport_close(t3);
        unlink(path);
      }
    }

    /*
     * 9e：**bash 给了 argv 也要真的登录**（CI 的 macos runner 就是这条红出来的）。
     *
     * argv[0] 的 `-` 前缀对 bash 的 `-c` 不生效：实测 `-bash -c 'echo $PATH'`
     * 拿到的还是注入的最小 PATH（zsh 不是这样，所以本机一直是绿的）。
     * 9a 走的是「默认 shell」这一维，落在谁的 shell 上由机器决定；这里**钉住
     * /bin/bash**，把「补 -l」这一环单独锁死，免得换台机器就看不见这个缺口。
     */
    const char *bash_argv[] = {"-c", "echo PATH_IS:$PATH", NULL};
    gt_pty_opts_t o5;
    memset(&o5, 0, sizeof o5);
    o5.executable = "/bin/bash";
    o5.argv = bash_argv;
    o5.env = cleanenv;
    o5.cmd_kind = GT_PTY_CMD_SHELL;
    o5.cols = 80; o5.rows = 24;
    o5.term = "xterm-256color";
    gt_transport_t *t5 = gt_transport_open_pty(&o5);
    CHECK(t5 != NULL, "/bin/bash 带 argv 起得来");
    if (t5) {
      read_until_contains(t5, "PATH_IS:", buf, sizeof buf, 20000);
      char *line5 = strstr(buf, "PATH_IS:");
      CHECK(line5 && strstr(line5, "/usr/sbin") != NULL,
            "bash + argv 也必须是登录 shell（显式 -l）——否则这一格"
            "的 brew/go 全成 command not found，且不报错");
      gt_transport_close(t5);
    }

    /*
     * 9f：argv[0] 的 `-` 前缀仍然要**画得出**（补 `-l` 之外的另一条机制）。
     *
     * 为什么补 `-l` 之后还要守这条：`-l` 只覆盖名单内的 shell，而名单外的
     * （nu / xonsh / elvish 与任何自填的 shell 路径）只剩 dash 前缀这一道。
     * 观测用 `$0`：shell 在 `-c` 模式下把 `$0` 就是 argv[0] 打出来，
     * 实测 `-sh` → "ARGV0:-sh"、`sh` → "ARGV0:sh"，所以这条不依赖登录语义、
     * 也不被 `-l` 掩盖（变异 L2「照抄 basename 忘了前导 -」只有它能抓到）。
     */
    const char *sh_argv[] = {"-c", "echo ARGV0:$0", NULL};
    gt_pty_opts_t o6;
    memset(&o6, 0, sizeof o6);
    o6.executable = "/bin/sh";
    o6.argv = sh_argv;
    o6.env = cleanenv;
    o6.cmd_kind = GT_PTY_CMD_SHELL;
    o6.cols = 80; o6.rows = 24;
    o6.term = "xterm-256color";
    gt_transport_t *t6 = gt_transport_open_pty(&o6);
    CHECK(t6 != NULL, "/bin/sh 带 argv 起得来");
    if (t6) {
      read_until_contains(t6, "ARGV0:", buf, sizeof buf, 20000);
      char *line6 = strstr(buf, "ARGV0:");
      CHECK(line6 && strncmp(line6, "ARGV0:-", 7) == 0,
            "登录 shell 的 argv[0] 必须带前导 `-`（名单外的 shell 只剩这一道）");
      gt_transport_close(t6);
    }
#else
    printf("  （跳过登录 shell 断言：非 macOS 平台没有这套 login(1) 语义）\n");
#endif
  }

  SEC("10 close-speed");
  {
    /* 关（close）必须**快**：交互式 shell 忽略 SIGHUP（这是它的常规行为），
     * 所以关闭路径的宽限必须短并果断升级 SIGKILL——实测原先 2 秒宽限让
     * 「关一个标签页」卡 2.3 秒（不报错，就是慢，用户能感觉出来）。
     *
     * 判据是「**最多量 3 次、取最小值，并与同一次量到的宽限标定比**」，
     * 不是单次 wall-clock：共享 CI runner 会把线程唤醒整体往后挤（full 轨实测
     * 连续三次 close = 1.07/1.53/1.09s，而同一 commit 在 dev 轮空机器上
     * 0.2s 就过）。固定 1 秒预算测到的其实是调度抖动，于是预算改为
     * `max(1.0s, 1.5×宽限标定 + 0.3s)`——标定跑的是实现里那段一模一样的
     * 20×10ms 轮询，放大对两侧同向，真回归（宽限写长、或 close 只发单个
     * child 收不掉整组）只有 close 一侧变慢，照样红；而 1.0s 的绝对下限
     * 不放宽，空机器上的判据与历史上拦住 2.3 秒那次完全一致。
     */
    double secs = -1, budget = 1.0, calib = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
      double c = measure_grace_poll_cost();
      bool opened = false, saw_output = false;
      double s = open_interactive_then_close(&opened, &saw_output);
      if (attempt == 0) {
        CHECK(opened, "默认 shell 起得来");
        CHECK(saw_output, "默认 shell 有输出（说明它已进入交互状态）");
      }
      if (s < 0) break;
      /* 预算 = max(1.0 秒, 1.5×宽限标定 + 0.3)。绝对下限**不放宽**：空机器上
       * 判据仍是 close<1.0s，历史上那次「2 秒宽限 → 关标签页卡 2.3 秒」就是
       * 被它拦下的；标定只用来容忍 runner 的调度放大。 */
      double b = 1.0;
      if (c > 0 && 1.5 * c + 0.3 > b) b = 1.5 * c + 0.3;
      if (secs < 0 || s < secs) { secs = s; budget = b; calib = c; }
      if (secs < budget) break;
      printf("  归因: 第 %d 次 close=%.2fs（预算 %.2fs，宽限标定 %.2fs），复量\n",
             attempt + 1, s, budget, c);
    }
    printf("  close=%.2fs / 预算 %.2fs（宽限标定 %.2fs）\n", secs, budget, calib);
    CHECK(secs >= 0 && secs < budget,
          "close 要快——交互式 shell 忽略 SIGHUP，宽限必须短并果断升级 SIGKILL");
  }

  SEC("11 child-state");
  {
    /* 前台作业判定（关闭确认的判据）。四态里三条本地可验：
     *   IDLE（停在提示符）/ JOB（前台跑着 sleep）/ EXITED（收尸后）；
     *   UNKNOWN 见 NULL 与 SSH（SSH 在 test_ssh）。
     *
     * **这条断言有牙**：只要本地 PTY 的子进程拿不到控制终端（例如父进程
     * 抢跑 setpgid 把子进程的 setsid 憋成 EPERM），tcgetpgrp(master) 恒 0
     * → 这里只会得到 UNKNOWN → IDLE/JOB 两条变红。它守的就是那个坑。
     */
    CHECK(gt_transport_child_state(NULL) == GT_TRANSPORT_CHILD_UNKNOWN,
          "child_state(NULL) = UNKNOWN（不折叠）");

    struct timespec ts = {0, 50 * 1000 * 1000};
    char sink[1024];

    /* (a) 空闲 → IDLE；空闲的交互式 shell 默认就是前台组。 */
    gt_pty_opts_t o;
    memset(&o, 0, sizeof o);
    o.cols = 80; o.rows = 24; o.term = "xterm-256color";
    gt_transport_t *t = gt_transport_open_pty(&o);
    CHECK(t != NULL, "child-state: 默认 shell 起得来");
    if (t) {
      bool idle = false;
      for (int i = 0; i < 400 && !idle; i++) { /* 最多 20s */
        gt_transport_child_t st = gt_transport_child_state(t);
        if (st == GT_TRANSPORT_CHILD_IDLE) { idle = true; break; }
        if (st == GT_TRANSPORT_CHILD_EXITED) break;
        if (gt_transport_read(t, (uint8_t *)sink, sizeof sink, 0) == GT_TRANSPORT_READ_EOF) break;
        nanosleep(&ts, NULL);
      }
      CHECK(idle, "空闲的本地 shell 应判为 IDLE（无控制终端时会退化成 UNKNOWN）");

      /* (b) 前台跑一个**长**作业 → JOB（不是 IDLE）。
       * 用长作业（而非 sleep 5）是刻意的：这样「关得掉吗」的断言才有牙——
       * 若关闭路径不挂断从端，close 会卡到作业自己结束（30s），断言即红。 */
      bool closed = false;
      if (idle) {
        const char *cmd = "sleep 30\n";
        gt_transport_write(t, (const uint8_t *)cmd, strlen(cmd));
        bool job = false;
        for (int i = 0; i < 200 && !job; i++) { /* 最多 10s */
          if (gt_transport_child_state(t) == GT_TRANSPORT_CHILD_JOB) { job = true; break; }
          if (gt_transport_read(t, (uint8_t *)sink, sizeof sink, 0) == GT_TRANSPORT_READ_EOF) break;
          nanosleep(&ts, NULL);
        }
        CHECK(job, "前台跑 sleep 时应判为 JOB（不是 IDLE）");

        /* (b2) 有前台作业时也必须**关得掉、关得快**：会话首进程退出要撤销
         * 控制终端，而后台/前台作业还握着从端时这个撤销会挂住——实测不挂断
         * master 就会卡到作业自己结束（「关一个正跑作业的标签页」直接卡死）。 */
        struct timespec c0, c1;
        clock_gettime(CLOCK_MONOTONIC, &c0);
        gt_transport_close(t);
        clock_gettime(CLOCK_MONOTONIC, &c1);
        closed = true;
        double secs = (c1.tv_sec - c0.tv_sec) + (c1.tv_nsec - c0.tv_nsec) / 1e9;
        CHECK(secs < 2.0,
              "有前台作业时 close 也要秒关（否则会话首进程卡在撤销控制终端上）");
      }
      if (!closed) gt_transport_close(t);
    }

    /* (c) 子进程退出 → EXITED。用确定的短命令，不依赖 Ctrl-C 时序。
     * 显式给 executable（不走登录 shell 的 argv[0] 包装），命令跑完即退。 */
    const char *argv[] = {"-c", "exit 7", NULL};
    gt_pty_opts_t o2;
    memset(&o2, 0, sizeof o2);
    o2.executable = "/bin/sh";
    o2.argv = argv;
    o2.cmd_kind = GT_PTY_CMD_DIRECT;
    o2.cols = 80; o2.rows = 24;
    gt_transport_t *t2 = gt_transport_open_pty(&o2);
    CHECK(t2 != NULL, "child-state: 一次性命令起得来");
    if (t2) {
      bool exited = false;
      for (int i = 0; i < 200 && !exited; i++) { /* 最多 10s */
        if (gt_transport_child_state(t2) == GT_TRANSPORT_CHILD_EXITED) { exited = true; break; }
        if (gt_transport_read(t2, (uint8_t *)sink, sizeof sink, 0) == GT_TRANSPORT_READ_EOF) continue;
        nanosleep(&ts, NULL);
      }
      CHECK(exited, "子进程退出后应判为 EXITED");
      gt_transport_close(t2);
    }
  }

  SEC("12 child-cwd");
  {
    /* 本地 PTY 的工作目录走**内核**（不依赖 shell 集成）：子进程继承我们的
     * cwd，`cd` 之后内核看到的 cwd 也跟着变。SSH / NULL 一律「不可用」。 */
    CHECK(!gt_transport_child_cwd(NULL, (char[8]){0}, 8), "NULL 取不到 cwd");

    char here[PATH_MAX];
    CHECK(getcwd(here, sizeof here) != NULL, "拿到测试进程的 cwd");

    char out[PATH_MAX];
    gt_pty_opts_t o;
    memset(&o, 0, sizeof o);
    o.cols = 80; o.rows = 24; o.term = "xterm-256color";
    gt_transport_t *t = gt_transport_open_pty(&o);
    CHECK(t != NULL, "child-cwd: 默认 shell 起得来");
    if (t) {
      /* 等它到空闲提示符再问（进程刚起时 cwd 已可读，这里多等一步稳妥）。 */
      struct timespec ts = {0, 50 * 1000 * 1000};
      char sink[1024];
      for (int i = 0; i < 400; i++) {
        if (gt_transport_child_state(t) == GT_TRANSPORT_CHILD_IDLE) break;
        if (gt_transport_read(t, (uint8_t *)sink, sizeof sink, 0) == GT_TRANSPORT_READ_EOF) break;
        nanosleep(&ts, NULL);
      }
      memset(out, 0, sizeof out);
      bool ok = gt_transport_child_cwd(t, out, sizeof out);
      CHECK(ok, "本地 PTY 应能取到 cwd（内核查询）");
      CHECK(strcmp(out, here) == 0,
            "子进程继承我们的 cwd（子进程 cwd vs 进程 cwd）");

      /* cd 到 /usr → 内核里的 cwd 应随之变化。 */
      const char *cmd = "cd /usr\n";
      gt_transport_write(t, (const uint8_t *)cmd, strlen(cmd));
      bool moved = false;
      for (int i = 0; i < 200; i++) {
        memset(out, 0, sizeof out);
        if (gt_transport_child_cwd(t, out, sizeof out) && strcmp(out, "/usr") == 0) {
          moved = true; break;
        }
        if (gt_transport_read(t, (uint8_t *)sink, sizeof sink, 0) == GT_TRANSPORT_READ_EOF) break;
        nanosleep(&ts, NULL);
      }
      CHECK(moved, "shell `cd /usr` 之后内核 cwd 应为 /usr");
      gt_transport_close(t);
    }
  }

  SEC("13 write-flow"); /* 写不阻塞 + 积压不丢 */
  {
    /* 背景（用户实测根因）：pty 输入队列很小（macOS 实测约 1 KiB）。一个
     * **不读 stdin** 的前台程序（卡住的进程 / 只输出的程序）会把队列撑满。
     * 旧实现在队满时 `poll(POLLOUT, -1)` **无限等** → 会话线程被永久钉死：
     *   ① 读循环再也出不来（终端画面冻住）；
     *   ② 关标签页时 SessionController.stop() 的 `queue.sync` 恰好在等这条
     *      线程 → **主线程一起卡死 = 整个 app 无响应**。
     * 修法：写有界——队满最多等一小段，余量攒进 transport，随后每次 read
     * 顺带补送（流控语义，输入不丢只是晚到）。下面分两档钉住它。 */

    /* ---- (a) 子进程**永不读** stdin → 写必须**有界**返回，绝不无限等 ----
     * 这条是决定性的：改了写法（回到 poll(-1)）时它会挂死在 write 上，整套
     * 断言跑不到终点（ctest 超时 → 显式失败），而不是靠时序凑一个红。 */
    gt_transport_t *noread =
        open_sh("stty -icanon -echo; sleep 60", 80, 24, NULL);
    CHECK(noread != NULL, "write-flow(a): 起得来");
    if (noread) {
      struct timespec warm = {0, 300 * 1000 * 1000}; /* 等 stty 生效 */
      nanosleep(&warm, NULL);
      char pre[512];
      for (int i = 0; i < 4; i++)
        gt_transport_read(noread, (uint8_t *)pre, sizeof pre, 50);

      const size_t N = 300 * 1000; /* ≫ 输入队列 */
      uint8_t *payload = malloc(N);
      CHECK(payload != NULL, "write-flow(a): payload 分配");
      if (payload) {
        memset(payload, 'x', N);
        struct timespec w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        int rc = GT_OK;
        for (size_t off = 0; off < N && rc == GT_OK; off += 4096) {
          size_t chunk = (N - off < 4096) ? (N - off) : 4096;
          rc = gt_transport_write(noread, payload + off, chunk);
        }
        clock_gettime(CLOCK_MONOTONIC, &w1);
        double wsec = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) / 1e9;
        CHECK(rc == GT_OK, "write-flow(a): 子进程不读时写也返回 GT_OK");
        CHECK(wsec < 3.0,
              "子进程不读 stdin 时写也要**有界**返回（旧实现的 poll(-1) 会永久钉死会话线程 → app 卡死）");
        free(payload);
      }
      gt_transport_close(noread); /* 也必须能秒关（sleep 60 会被收掉） */
    }

    /* ---- (b) 子进程先不读、后开始读 → 积压字节**一个不丢**（流控而非丢数据） ---- */
    gt_transport_t *t =
        open_sh("stty -icanon min 1 time 0 -echo; sleep 2; cat", 80, 24, NULL);
    CHECK(t != NULL, "write-flow(b): 起得来");
    if (t) {
      struct timespec warm = {0, 300 * 1000 * 1000}; /* 等 stty 生效 */
      nanosleep(&warm, NULL);
      char pre[512];
      for (int i = 0; i < 4; i++)
        gt_transport_read(t, (uint8_t *)pre, sizeof pre, 50);

      const size_t N = 300 * 1000; /* ≫ 输入队列 */
      uint8_t *payload = malloc(N);
      char *acc = malloc(N + 8192);
      CHECK(payload && acc, "write-flow(b): payload/acc 分配");
      if (payload && acc) {
        memset(payload, 'x', N);
        memcpy(payload, "FLOWMARK", 8);        /* 头标记 */
        memcpy(payload + N - 7, "ENDMARK", 7); /* 尾标记 */

        struct timespec w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        int rc = GT_OK;
        for (size_t off = 0; off < N && rc == GT_OK; off += 4096) {
          size_t chunk = (N - off < 4096) ? (N - off) : 4096;
          rc = gt_transport_write(t, payload + off, chunk);
        }
        clock_gettime(CLOCK_MONOTONIC, &w1);
        double wsec = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) / 1e9;
        CHECK(rc == GT_OK, "write-flow(b): 全部接受（GT_OK）");
        CHECK(wsec < 1.0, "write-flow(b): 有积压时不再等（立即攒起来）");

        /* 子进程睡醒开始读 → 积压应被补送回来（cat 原样回显）。 */
        size_t got = 0;
        int hit_end = 0, hit_start = 0, elapsed = 0;
        while (elapsed < 8000 && !hit_end) {
          int n = gt_transport_read(t, (uint8_t *)acc + got, 8192, 100);
          if (n > 0) {
            got += (size_t)n;
            if (mem_contains(acc, got, "FLOWMARK")) hit_start = 1;
            if (mem_contains(acc, got, "ENDMARK")) hit_end = 1;
            if (got + 8192 > N + 8192) break;
            continue;
          }
          if (n == GT_TRANSPORT_READ_EOF) break;
          if (n == GT_TRANSPORT_READ_TIMEOUT) { elapsed += 100; continue; }
          break;
        }
        CHECK(hit_start, "积压输入的**开头**送达了");
        CHECK(hit_end, "积压输入的**结尾**送达了（流控不丢数据）");
        CHECK(got >= N, "回显字节数不少于写入量（无截断）");
        free(payload);
        free(acc);
      }
      gt_transport_close(t);
    }
  }

  SEC("8");
  {
    gt_transport_close(NULL);
    CHECK(!gt_transport_exited(NULL, NULL), "exited(NULL) 返回 false");
    CHECK(gt_transport_kind(NULL) == GT_TRANSPORT_LOCAL_PTY, "kind(NULL) 回退为 LOCAL_PTY");
  }

  rmdir(tmpdir);
  printf("test_transport: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
