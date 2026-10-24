/*
 * test_ssh.c — libssh2 SSH 后端的行为断言（真 sshd，离线/环回）。
 *
 * 覆盖（每条对应一条不变量）：
 *   1. 未知主机密钥 → GT_ERR_HOST_KEY（三态 UNKNOWN 不折叠成 ACCEPT）
 *   2. decider ACCEPT → 连接成功，并把密钥落盘（TOFU）
 *   3. 已知且一致 → 无 decider 也接受，且 **decider 不被调用**
 *      （已确认主机的静默通过，不是把 TOFU 当一次性动作）
 *   4. known_hosts 被篡改（密钥变了）→ GT_ERR_HOST_KEY（绝不自动接受）
 *   5. 私钥认证 + 交互式 shell 通道：写命令→读回显
 *   5b. shell 通道写有界 + 输入不丢（远端 sleep 5，大 payload）
 *   6. 运行中 resize 生效（stty size 读控制终端）
 *   7. 空凭据 → GT_ERR_AUTH（显式失败，不假装成功）
 *
 * 用法：test_ssh <host> <port> <user> <pem> <workdir>
 */
#include "ghosterm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* walk 的访客：计数，并支持「见够 N 个就停」（验取消路径）。 */
static int g_walk_seen = 0;
static int g_walk_stop_after = 0;
static int walk_count_cb(void *ud, const gt_sftp_walk_ent_t *e) {
  (void)ud; (void)e;
  g_walk_seen++;
  if (g_walk_stop_after > 0 && g_walk_seen >= g_walk_stop_after) return 1;
  return 0;
}

static int pass = 0, fail = 0;
#define CHECK(c, msg) do { if (c) pass++; else { fail++; printf("FAIL: %s\n", msg); } } while (0)

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = calloc(1, (size_t)sz + 1);
  if (b) { size_t n = fread(b, 1, (size_t)sz, f); b[n] = '\0'; }
  fclose(f);
  return b;
}

static gt_hostkey_decision_t decide_accept(void *ud, const char *host, uint16_t port,
                         const char *kt, const char *fp) {
  (void)host; (void)port; (void)kt; (void)fp;
  int *called = ud;
  if (called) (*called)++;
  return GT_HOSTKEY_ACCEPT;
}

static gt_ssh_t *mk(const char *host, int port, const char *user,
                    const char *pem, const char *kh, gt_hostkey_decider_fn d) {
  gt_ssh_opts_t o;
  memset(&o, 0, sizeof o);
  o.host = host;
  o.port = (uint16_t)port;
  o.username = user;
  o.known_hosts_path = kh;
  gt_ssh_t *s = gt_ssh_new(&o);
  if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
  if (s && d) gt_ssh_set_hostkey_policy(s, d, NULL);
  return s;
}

static int read_until(gt_transport_t *t, const char *needle, char *buf,
                      size_t cap, int max_ms) {
  size_t total = 0;
  int elapsed = 0;
  while (total + 1 < cap && elapsed < max_ms) {
    int n = gt_transport_read(t, (uint8_t *)buf + total, cap - total - 1, 100);
    if (n > 0) {
      total += (size_t)n;
      buf[total] = '\0';
      if (strstr(buf, needle)) return 1;
      continue;
    }
    if (n == GT_TRANSPORT_READ_EOF) break;
    if (n == GT_TRANSPORT_READ_TIMEOUT) { elapsed += 100; continue; }
    break;
  }
  buf[total] = '\0';
  return strstr(buf, needle) != NULL;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc != 6) { fprintf(stderr, "usage: %s host port user pem workdir\n", argv[0]); return 2; }
  const char *host = argv[1];
  int port = atoi(argv[2]);
  const char *user = argv[3];
  char *pem = read_file(argv[4]);
  char kh[1024], kh2[1024];
  snprintf(kh, sizeof kh, "%s/known_hosts", argv[5]);
  snprintf(kh2, sizeof kh2, "%s/known_hosts2", argv[5]);
  remove(kh);
  remove(kh2);

  /* 1. 未知主机密钥 + 无 decider → UNKNOWN（GT_ERR_HOST_KEY） */
  {
    gt_ssh_t *s = mk(host, port, user, pem, kh, NULL);
    CHECK(gt_ssh_connect(s) == GT_ERR_HOST_KEY, "未知主机密钥返回 GT_ERR_HOST_KEY");
    gt_ssh_close(s);
  }

  /* 2. decider ACCEPT → 成功并 TOFU 落盘 */
  {
    gt_ssh_t *s = mk(host, port, user, pem, kh, decide_accept);
    CHECK(gt_ssh_connect(s) == GT_OK, "decider ACCEPT 后连接成功");
    gt_ssh_close(s);
    FILE *f = fopen(kh, "rb");
    CHECK(f != NULL, "TOFU 已把主机密钥写入 known_hosts");
    if (f) fclose(f);
  }

  /* 3. 已知且一致 → 无 decider 也接受；且 decider **不被调用**
   *    （一致主机的接受来自 known_hosts 命中，不靠把 TOFU 一次性化） */
  {
    int decider_called = 0;
    gt_ssh_t *s = mk(host, port, user, pem, kh, NULL);
    gt_ssh_set_hostkey_policy(s, decide_accept, &decider_called);
    CHECK(gt_ssh_connect(s) == GT_OK, "known_hosts 命中后无需 decider 也接受");
    CHECK(decider_called == 0, "密钥已知且一致时 decider 不得被调用");
    gt_ssh_close(s);
  }

  /* 4. 篡改 known_hosts（密钥变了）→ REJECT */
  {
    FILE *f = fopen(kh2, "wb");
    fprintf(f, "%s ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBogusBogusBogusBogus\n", host);
    fclose(f);
    gt_ssh_t *s = mk(host, port, user, pem, kh2, decide_accept);
    CHECK(gt_ssh_connect(s) == GT_ERR_HOST_KEY, "密钥变更（篡改）返回 GT_ERR_HOST_KEY");
    gt_ssh_close(s);
  }

  /* 5+6. 私钥认证 + shell 通道：回显与 resize */
  {
    gt_ssh_t *s = mk(host, port, user, pem, kh, NULL);
    if (s && gt_ssh_connect(s) == GT_OK) {
      gt_shell_opts_t so;
      memset(&so, 0, sizeof so);
      so.term = "xterm-256color";
      so.cols = 80; so.rows = 24;
      gt_transport_t *t = gt_transport_open_ssh_shell(s, &so);
      CHECK(t != NULL, "打开 SSH shell 通道成功");
      if (t) {
        CHECK(gt_transport_kind(t) == GT_TRANSPORT_SSH_SHELL, "kind 为 SSH_SHELL");
        /* 先握手到「shell 真在跑」再断言命令回显，别靠 5s 读超时硬等：
         * 远端 shell 起来要时间（负载高时更久），直接 write 可能落在
         * shell 还没读 stdin 的那一段。**不能用提示符当握手信号**——
         * 非交互 shell 未必打印提示符（实测这里就没有，等到超时也等不到）。
         * 用一条 `echo READY` 当握手：它不依赖提示符，回显到了就说明
         * shell 已在处理命令，后面两条关键断言（命令回显 / resize）才稳。 */
        char buf[4096];
        gt_transport_write(t, (const uint8_t *)"echo READY\n", 11);
        CHECK(read_until(t, "READY", buf, sizeof buf, 15000),
              "远端 shell 已就绪（echo READY 有回显）");
        gt_transport_write(t, (const uint8_t *)"echo MARK_$((6*7))\n", 19);
        CHECK(read_until(t, "MARK_42", buf, sizeof buf, 15000),
              "shell 回显命令输出（42）");
        CHECK(gt_transport_resize(t, 100, 40, 0, 0) == GT_OK, "ssh shell resize 返回 OK");
        gt_transport_write(t, (const uint8_t *)"stty size\n", 10);
        CHECK(read_until(t, "40 100", buf, sizeof buf, 15000),
              "resize 后 stty size 反映新尺寸（40 100）");

        /* §5b. 写有界 + 输入不丢（契约见 transport.h）。
         *   远端跑 sleep 5 但 stdin 不重定向 → sleep 占着 shell 的 stdin、
         *   自己却不读 → pty 输入队列满 → sshd 无法消费 channel → channel
         *   窗口满。写必须在 GT_SSH_WRITE_GRACE_MS 内返回（不是永远等、
         *   不是丢字节报 IO 错）。
         *   注：断言**到此为止**——不验证「队列被送达」。原因：sshd 的排空
         *   速率远高于 channel 窗口，3 MiB payload 写完时 sshd 已经把窗口
         *   排空，队列从未形成，flush 机制无法被 e2e 触发。队列机制本身由
         *   代码审阅 + 与 PTY wtail 的结构对称性保证。 */
        gt_transport_write(t, (const uint8_t *)"sleep 5\n", 8);
        read_until(t, "sleep 5", buf, sizeof buf, 5000);
        usleep(100 * 1000); /* 让 sleep 进入前台进程组、shell 暂停 stdin 读取 */
        size_t n = 3u * 1024u * 1024u;
        char *huge = malloc(n);
        if (huge) {
          memset(huge, '#', n);
          struct timespec t0;
          clock_gettime(CLOCK_MONOTONIC, &t0);
          int rc = gt_transport_write(t, (const uint8_t *)huge, n);
          struct timespec t1;
          clock_gettime(CLOCK_MONOTONIC, &t1);
          int64_t elapsed_ms =
              (int64_t)(t1.tv_sec - t0.tv_sec) * 1000 +
              (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000;
          CHECK(elapsed_ms < 3000, "写有界：远端不读 stdin 时 3s 内返回");
          CHECK(rc == GT_OK, "远端不读 stdin 时不丢字节（GT_OK，不是 GT_ERR_IO）");
          free(huge);
        } else {
          CHECK(0, "写有界：3 MiB 分配失败");
        }

        gt_transport_close(t);
      }
    } else {
      CHECK(0, "认证/shell 前置条件：连接成功");
    }
    gt_ssh_close(s);
  }

  /* 7. 无凭据 → GT_ERR_AUTH */
  {
    gt_ssh_t *s = mk(host, port, user, NULL, kh, NULL);
    CHECK(gt_ssh_connect(s) == GT_ERR_AUTH, "无凭据返回 GT_ERR_AUTH");
    gt_ssh_close(s);
  }

  /* 8. SFTP e2e（M4）：list / stat / upload / download 全链路（真 sshd）。
   * 工作目录在 sshd 侧就是发起目录（无 chroot），本地与远端同一文件系统——
   * 语义仍是真的 SFTP 协议往返（不是本地拷贝的假绿）。 */
  {
    char up_remote[1024], up_local[1024], down_local[1024], remote_home[512];
    snprintf(up_remote, sizeof up_remote, "%s/gt-sftp-e2e-remote.bin", argv[5]);
    snprintf(up_local, sizeof up_local, "%s/gt-sftp-e2e-local.bin", argv[5]);
    snprintf(down_local, sizeof down_local, "%s/gt-sftp-e2e-downloaded.bin", argv[5]);
    snprintf(remote_home, sizeof remote_home, "%s", argv[5]);

    /* 上传源：带可校验内容的固定缓冲。 */
    unsigned char payload[4096];
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (unsigned char)(i * 31 + 7);
    FILE *f = fopen(up_local, "wb");
    CHECK(f != NULL, "写上传源文件");
    if (f) { fwrite(payload, 1, sizeof payload, f); fclose(f); }

    gt_ssh_t *s = mk(host, port, user, pem, kh, NULL);
    if (s && gt_ssh_connect(s) == GT_OK) {
      gt_sftp_t *sftp = gt_ssh_sftp(s);
      CHECK(sftp != NULL, "SFTP 子系统初始化");
      if (sftp) {
        /* list：至少能看到 home 下我们建的文件（先上传再列）。 */
        uint64_t job = gt_sftp_upload(sftp, up_local, up_remote);
        CHECK(job != 0, "upload job 启动（返回非 0 job id）");
        /* 等 job 完成（事件队列低频轮询）。 */
        int done = 0;
        for (int i = 0; i < 300 && !done; i++) {
          gt_event_t evs[8];
          size_t n = gt_core_poll_events(evs, 8);
          for (size_t k = 0; k < n; k++) {
            if (evs[k].kind == GT_EVENT_JOB_DONE && evs[k].job_id == job &&
                evs[k].code == GT_JOB_OK) {
              done = 1;
              break;
            }
          }
          if (!done) usleep(10 * 1000);
        }
        CHECK(done, "upload job 完成且结果为 GT_JOB_OK（三态不折叠）");

        /* stat：远端文件尺寸正确。 */
        gt_sftp_stat_t st;
        memset(&st, 0, sizeof st);
        CHECK(gt_sftp_stat(sftp, up_remote, &st) == GT_OK, "远端 stat 成功");
        CHECK(st.size == sizeof payload, "远端尺寸等于上传尺寸");

        /* list：目录里能找到刚上传的文件名。 */
        gt_sftp_entry_t *entries = NULL;
        size_t ecount = 0;
        CHECK(gt_sftp_list(sftp, remote_home, &entries, &ecount) == GT_OK,
              "列目录成功");
        int found = 0;
        for (size_t k = 0; k < ecount; k++)
          if (strstr(entries[k].full_path, "gt-sftp-e2e-remote.bin")) { found = 1; break; }
        CHECK(found, "列目录应包含刚上传的文件");
        CHECK(ecount > 0, "目录条目数 > 0");
        gt_sftp_list_free(entries, ecount);

        /* download：内容逐字节一致。 */
        job = gt_sftp_download(sftp, up_remote, down_local);
        CHECK(job != 0, "download job 启动");
        int ddone = 0;
        for (int i = 0; i < 300 && !ddone; i++) {
          gt_event_t evs[8];
          size_t n = gt_core_poll_events(evs, 8);
          for (size_t k = 0; k < n; k++) {
            if (evs[k].kind == GT_EVENT_JOB_DONE && evs[k].job_id == job &&
                evs[k].code == GT_JOB_OK) {
              ddone = 1;
              break;
            }
          }
          if (!ddone) usleep(10 * 1000);
        }
        CHECK(ddone, "download job 完成且结果为 GT_JOB_OK");
        int same = 0;
        FILE *df = fopen(down_local, "rb");
        if (df) {
          unsigned char back[4096];
          size_t got = fread(back, 1, sizeof back, df);
          fclose(df);
          same = (got == sizeof payload) &&
                 (memcmp(back, payload, sizeof payload) == 0);
        }
        CHECK(same, "下载内容与上传源逐字节一致");

        /* 9. 失败 job 的三态：不存在的远端文件 → GT_EVENT_JOB_DONE 且
         * code == GT_JOB_FAILED（失败不得被折叠成 OK——铁律 #6）。 */
        uint64_t bad = gt_sftp_download(sftp, "/no/such/file/gt-missing.bin",
                                        down_local);
        CHECK(bad != 0, "对不存在远端文件的 download 也应给出 job id");
        int saw_failed = 0;
        for (int i = 0; i < 300 && !saw_failed; i++) {
          gt_event_t evs[8];
          size_t n = gt_core_poll_events(evs, 8);
          for (size_t k = 0; k < n; k++) {
            if (evs[k].kind == GT_EVENT_JOB_DONE && evs[k].job_id == bad &&
                evs[k].code == GT_JOB_FAILED) {
              saw_failed = 1;
              break;
            }
          }
          if (!saw_failed) usleep(10 * 1000);
        }
        CHECK(saw_failed, "失败 job 必须以 GT_JOB_FAILED 上报（不得折叠成 OK）");

        /* 10. 写侧原语：mkdir / chmod / rename / realpath / remove / rmdir */
        {
          char dir[1024], sub[1024], ren[1024], file[1024], real[1024];
          snprintf(dir, sizeof dir, "%s/gt-ops-dir", remote_home);
          snprintf(sub, sizeof sub, "%s/gt-ops-dir/sub", remote_home);
          snprintf(ren, sizeof ren, "%s/gt-ops-dir/renamed", remote_home);
          snprintf(file, sizeof file, "%s/gt-ops-dir/a.txt", remote_home);

          gt_sftp_remove_all(sftp, dir); /* 清残留 */
          CHECK(gt_sftp_mkdir(sftp, dir, 0755) == GT_OK, "mkdir 成功");
          CHECK(gt_sftp_mkdir(sftp, dir, 0755) != GT_OK, "重复 mkdir 必须失败（不折叠成 OK）");
          gt_sftp_stat_t st;
          CHECK(gt_sftp_stat(sftp, dir, &st) == GT_OK && st.is_dir, "stat 报该路径是目录");

          FILE *pf = fopen(file, "wb");
          CHECK(pf != NULL, "写本地测试文件");
          if (pf) { fwrite("hello", 1, 5, pf); fclose(pf); }

          CHECK(gt_sftp_chmod(sftp, file, 0600) == GT_OK, "chmod 成功");
          CHECK(gt_sftp_stat(sftp, file, &st) == GT_OK, "chmod 后 stat 成功");
          CHECK((st.mode & 0777) == 0600, "chmod 真的改了模式位（0600）");

          CHECK(gt_sftp_rename(sftp, file, ren) == GT_OK, "rename 成功");
          CHECK(gt_sftp_stat(sftp, ren, &st) == GT_OK, "rename 后新路径存在");
          CHECK(gt_sftp_stat(sftp, file, &st) != GT_OK, "rename 后旧路径不存在");

          CHECK(gt_sftp_realpath(sftp, ren, real, sizeof real) == GT_OK, "realpath 成功");
          CHECK(strstr(real, "renamed") != NULL, "realpath 指向真实路径");

          CHECK(gt_sftp_remove(sftp, ren) == GT_OK, "remove 删除文件成功");
          CHECK(gt_sftp_remove(sftp, ren) != GT_OK, "重复 remove 必须失败（不折叠成 OK）");

          CHECK(gt_sftp_mkdir(sftp, sub, 0755) == GT_OK, "mkdir 子目录");
          CHECK(gt_sftp_rmdir(sftp, dir) != GT_OK, "非空目录 rmdir 必须失败");
          CHECK(gt_sftp_rmdir(sftp, sub) == GT_OK, "空目录 rmdir 成功");
          CHECK(gt_sftp_rmdir(sftp, dir) == GT_OK, "清空后 rmdir 成功");
        }

        /* 11. 符号链接：lstat 不跟随 / readlink / 删链接不删目标 */
        {
          char target[1024], link[1024], got[1024];
          snprintf(target, sizeof target, "%s/gt-sym-target.txt", remote_home);
          snprintf(link, sizeof link, "%s/gt-sym-link", remote_home);
          FILE *tf = fopen(target, "wb");
          if (tf) { fputs("target-content", tf); fclose(tf); }
          unlink(link);
          if (symlink("gt-sym-target.txt", link) == 0) {
            gt_sftp_stat_t lst, fst, chk;
            CHECK(gt_sftp_lstat(sftp, link, &lst) == GT_OK, "lstat 成功");
            CHECK(lst.is_symlink, "lstat 报这是符号链接（不跟随）");
            CHECK(gt_sftp_readlink(sftp, link, got, sizeof got) == GT_OK, "readlink 成功");
            CHECK(strcmp(got, "gt-sym-target.txt") == 0, "readlink 返回链接目标");
            CHECK(gt_sftp_stat(sftp, link, &fst) == GT_OK && !fst.is_symlink,
                  "stat 跟随链接（报目标自身属性）");
            CHECK(gt_sftp_remove(sftp, link) == GT_OK, "remove 删链接");
            CHECK(gt_sftp_stat(sftp, target, &chk) == GT_OK,
                  "删链接不删目标（目标仍在）");
          } else {
            printf("note: 环境不允许创建符号链接，跳过 symlink 断言\n");
          }
          remove(target);
        }

        /* 12. 递归删除 remove_all */
        {
          char root[1024], a[1152], b[1152], c[1152];
          snprintf(root, sizeof root, "%s/gt-rmall", remote_home);
          gt_sftp_remove_all(sftp, root);
          CHECK(gt_sftp_mkdir(sftp, root, 0755) == GT_OK, "建递归删除根目录");
          snprintf(a, sizeof a, "%s/a", root);
          snprintf(b, sizeof b, "%s/a/b", root);
          snprintf(c, sizeof c, "%s/a/b/c.txt", root);
          CHECK(gt_sftp_mkdir(sftp, a, 0755) == GT_OK, "建 a");
          CHECK(gt_sftp_mkdir(sftp, b, 0755) == GT_OK, "建 a/b");
          FILE *cf = fopen(c, "wb");
          if (cf) { fputs("deep", cf); fclose(cf); }
          CHECK(gt_sftp_remove_all(sftp, root) == GT_OK, "递归删除整棵树成功");
          gt_sftp_stat_t st;
          CHECK(gt_sftp_stat(sftp, root, &st) != GT_OK, "递归删除后根不存在");
        }

        /* 13. walk：计数 / 取消 / max_depth */
        {
          char root[1024], d1[1152], f1[1152], f2[1152];
          snprintf(root, sizeof root, "%s/gt-walk", remote_home);
          gt_sftp_remove_all(sftp, root);
          gt_sftp_mkdir(sftp, root, 0755);
          snprintf(d1, sizeof d1, "%s/d1", root);
          snprintf(f1, sizeof f1, "%s/d1/f1", root);
          snprintf(f2, sizeof f2, "%s/top.txt", root);
          gt_sftp_mkdir(sftp, d1, 0755);
          FILE *x = fopen(f1, "wb"); if (x) { fputs("1", x); fclose(x); }
          x = fopen(f2, "wb"); if (x) { fputs("2", x); fclose(x); }

          g_walk_seen = 0; g_walk_stop_after = 0;
          CHECK(gt_sftp_walk(sftp, root, -1, walk_count_cb, NULL) == GT_OK, "walk 完成");
          CHECK(g_walk_seen == 3, "walk 见到 3 个条目（d1 / top.txt / d1/f1）");

          g_walk_seen = 0; g_walk_stop_after = 1;
          CHECK(gt_sftp_walk(sftp, root, -1, walk_count_cb, NULL) == GT_ERR_CANCELLED,
                "visitor 请求停止 → walk 返回 CANCELLED");

          g_walk_seen = 0; g_walk_stop_after = 0;
          CHECK(gt_sftp_walk(sftp, root, 1, walk_count_cb, NULL) == GT_OK && g_walk_seen == 2,
                "max_depth=1 只走一层（2 个条目）");
          gt_sftp_remove_all(sftp, root);
        }

        /* 14. 暂停 / 继续（协作式；暂停期间不得完成） */
        {
          char big[1024], dl[1024];
          snprintf(big, sizeof big, "%s/gt-big.bin", remote_home);
          snprintf(dl, sizeof dl, "%s/gt-big-dl.bin", remote_home);
          FILE *bf = fopen(big, "wb");
          if (bf) {
            char chunk[65536];
            memset(chunk, 0x5a, sizeof chunk);
            for (int i = 0; i < 256; i++) fwrite(chunk, 1, sizeof chunk, bf); /* 16MB */
            fclose(bf);
          }
          uint64_t pj = gt_sftp_download(sftp, big, dl);
          CHECK(pj != 0, "大文件下载 job 启动");
          CHECK(gt_sftp_pause(sftp, pj, true) == GT_OK, "暂停 job 成功");
          CHECK(gt_sftp_pause(sftp, 9999999ULL, true) == GT_ERR_NOT_FOUND,
                "暂停未知 job → NOT_FOUND");
          int donecs = 0;
          for (int i = 0; i < 30; i++) {
            gt_event_t evs[8];
            size_t n = gt_core_poll_events(evs, 8);
            for (size_t k = 0; k < n; k++)
              if (evs[k].kind == GT_EVENT_JOB_DONE && evs[k].job_id == pj) donecs = 1;
            usleep(10 * 1000);
          }
          CHECK(!donecs, "暂停期间 job 不得完成（进度真的停住）");
          CHECK(gt_sftp_pause(sftp, pj, false) == GT_OK, "继续 job 成功");
          int pdone = 0;
          for (int i = 0; i < 2000 && !pdone; i++) {
            gt_event_t evs[8];
            size_t n = gt_core_poll_events(evs, 8);
            for (size_t k = 0; k < n; k++)
              if (evs[k].kind == GT_EVENT_JOB_DONE && evs[k].job_id == pj &&
                  evs[k].code == GT_JOB_OK) pdone = 1;
            if (!pdone) usleep(5 * 1000);
          }
          CHECK(pdone, "继续后 job 完成且结果为 OK");
          remove(big); remove(dl);
        }

        /* 15. exec 通道：stdout / stderr 分离 + 退出码 */
        {
          gt_status_t es = GT_ERR_INTERNAL;
          gt_ssh_exec_t *bad = gt_ssh_exec(NULL, "x", &es);
          CHECK(bad == NULL && es == GT_ERR_INVALID_ARG, "exec(NULL) → NULL + INVALID_ARG");

          gt_ssh_exec_t *ex = gt_ssh_exec(s, "echo GT_EXEC_OK", &es);
          CHECK(ex != NULL, "exec 打开成功（服务器允许 exec）");
          if (ex) {
            char ebuf[512]; size_t tot = 0; int ec = -1;
            while (tot + 1 < sizeof ebuf) {
              bool iserr = false;
              int n = gt_ssh_exec_read(ex, (uint8_t *)ebuf + tot,
                                       sizeof ebuf - tot - 1, 3000, &iserr);
              if (n > 0) { tot += (size_t)n; continue; }
              if (n == GT_EXEC_READ_TIMEOUT) continue;
              break;
            }
            ebuf[tot] = '\0';
            CHECK(strstr(ebuf, "GT_EXEC_OK") != NULL, "exec stdout 含命令输出");
            CHECK(gt_ssh_exec_exit_status(ex, &ec) && ec == 0, "exec 退出码为 0");
            gt_ssh_exec_close(ex);
          } else {
            printf("note: exec 不可用（status=%d）\n", (int)es);
          }

          /* 命令带尾部换行也必须照跑（防御性剥离；本机 libssh2 本就容得下）。 */
          gt_ssh_exec_t *exn = gt_ssh_exec(s, "echo GT_NL_OK\n", &es);
          if (exn) {
            char nb[256]; size_t nt = 0;
            for (;;) {
              bool ie = false;
              int n = gt_ssh_exec_read(exn, (uint8_t *)nb + nt, sizeof nb - nt - 1, 3000, &ie);
              if (n > 0) { nt += (size_t)n; if (nt >= sizeof nb - 1) break; continue; }
              if (n == GT_EXEC_READ_TIMEOUT) continue;
              break;
            }
            nb[nt] = '\0';
            CHECK(strstr(nb, "GT_NL_OK") != NULL, "带尾部换行的命令被剥换行后照跑");
            gt_ssh_exec_close(exn);
          }

          gt_ssh_exec_t *ex2 = gt_ssh_exec(s, "echo OUT; echo ERR 1>&2; exit 3", &es);
          if (ex2) {
            char ob[512] = {0}, er[512] = {0};
            size_t no = 0, ne = 0;
            for (;;) {
              uint8_t tmp[256]; bool iserr = false;
              int n = gt_ssh_exec_read(ex2, tmp, sizeof tmp, 3000, &iserr);
              if (n > 0) {
                size_t un = (size_t)n;
                if (iserr) { size_t c = sizeof er - 1 - ne; if (un > c) un = c;
                             memcpy(er + ne, tmp, un); ne += un; }
                else { size_t c = sizeof ob - 1 - no; if (un > c) un = c;
                       memcpy(ob + no, tmp, un); no += un; }
                continue;
              }
              if (n == GT_EXEC_READ_TIMEOUT) continue;
              break;
            }
            ob[no] = '\0'; er[ne] = '\0';
            int ec2 = -1;
            CHECK(strstr(ob, "OUT") != NULL, "exec stdout 有 OUT");
            CHECK(strstr(ob, "ERR") == NULL, "stderr 未混入 stdout");
            CHECK(strstr(er, "ERR") != NULL, "exec stderr 有 ERR");
            CHECK(gt_ssh_exec_exit_status(ex2, &ec2) && ec2 == 3, "exec 退出码 3 被读出");
            gt_ssh_exec_close(ex2);
          }
        }

        /* 16. 性能证据：远端 `find`（exec，一次往返）vs SFTP 逐层 walk（每条目
         * 一次往返）。造一个宽目录（300 文件），两条路都要找全；打印耗时对比。
         *
         * **诚实口径**：环回（RTT≈0）上 exec **反而慢**（~40ms vs ~3ms）——
         * 差异来自**起一个远端进程**的开销；SFTP walk 的每条目往返在环回上几乎
         * 免费。exec 的优势只在**真实 RTT** 链路显现（每 readdir 一次 RTT ×
         * 条目数，300 文件在 50ms RTT 下 walk 要多花约 15s，exec 仍是 ~1 RTT）。
         * 故这里**只断言正确性**（两条路都找全），耗时仅打印——不做易抖的速度
         * 断言（环回上它会得出「exec 更慢」的误导结论）。 */
        {
          char wdir[1024];
          snprintf(wdir, sizeof wdir, "%s/gt-perf-wide", remote_home);
          gt_sftp_remove_all(sftp, wdir);
          gt_sftp_mkdir(sftp, wdir, 0755);
          const int NFILES = 300;
          for (int i = 0; i < NFILES; i++) {
            char fp[1200];
            snprintf(fp, sizeof fp, "%s/f%03d.txt", wdir, i);
            FILE *pf2 = fopen(fp, "wb");
            if (pf2) { fputs("x", pf2); fclose(pf2); }
          }

          struct timespec t0, t1;

          /* 路径 A：exec find（一次往返）。 */
          clock_gettime(CLOCK_MONOTONIC, &t0);
          int find_n = 0;
          gt_status_t es2 = GT_ERR_INTERNAL;
          char cmd[1400];
          snprintf(cmd, sizeof cmd, "find '%s' -type f -print0", wdir);
          gt_ssh_exec_t *fx = gt_ssh_exec(s, cmd, &es2);
          if (fx) {
            char fbuf[8192];
            for (;;) {
              bool ie = false;
              int n = gt_ssh_exec_read(fx, (uint8_t *)fbuf, sizeof fbuf, 3000, &ie);
              if (n > 0) {
                if (!ie) for (int k = 0; k < n; k++) if (fbuf[k] == '\0') find_n++;
                continue;
              }
              if (n == GT_EXEC_READ_TIMEOUT) continue;
              break;
            }
            gt_ssh_exec_close(fx);
          }
          clock_gettime(CLOCK_MONOTONIC, &t1);
          double find_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                         + (t1.tv_nsec - t0.tv_nsec) / 1e6;

          /* 路径 B：SFTP walk（每条目一次往返）。 */
          clock_gettime(CLOCK_MONOTONIC, &t0);
          g_walk_seen = 0; g_walk_stop_after = 0;
          int wrc = gt_sftp_walk(sftp, wdir, -1, walk_count_cb, NULL);
          clock_gettime(CLOCK_MONOTONIC, &t1);
          double walk_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                         + (t1.tv_nsec - t0.tv_nsec) / 1e6;

          CHECK(wrc == GT_OK && g_walk_seen == NFILES, "SFTP walk 找全 300 个文件");
          if (fx) {
            CHECK(find_n == NFILES, "exec find 找全 300 个文件");
            printf("perf: exec-find %d 文件 %.1fms | sftp-walk %d 文件 %.1fms\n",
                   find_n, find_ms, g_walk_seen, walk_ms);
          } else {
            printf("note: exec 不可用，跳过 find 性能对比\n");
          }
          gt_sftp_remove_all(sftp, wdir);
        }

        remove(up_remote);
        gt_sftp_close(sftp);
      }
    } else {
      CHECK(0, "SFTP e2e 前置条件：连接成功");
    }
    remove(up_local);
    remove(down_local);
    gt_ssh_close(s);
  }

  free(pem);
  printf("test_ssh: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
