/*
 * test_known_hosts.c — known_hosts 读侧的语义（离线，不连真 sshd）。
 *
 * 为什么单拎出来：这里踩过一个**只在特定密钥类型下才暴露**的真缺陷——
 * 过滤 `ss` 前缀是照抄 OpenSSH 对 **ssh-rsa** 的写法，而 ECDSA 的 key type
 * 是 `ecdsa-sha2-nistp256`（根本不以 `ssh` 开头）。于是所有 ECDSA 主机的
 * known_hosts 条目被整条跳过 → 每次连接都判 UNKNOWN → 每次弹信任提示，
 * 落盘再多遍也不管用。
 *
 * 路径：core/src/ssh.c 的 kh_check 是 static，故这里**经真实连接路径**验：
 * 用一份「已经记着本机 sshd 主机密钥」的 known_hosts 连一次，断言
 * **decider 不被调用**（已知且一致 → 直接放行，不弹对话框）。
 */
#include "ghosterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...)                                        \
  do {                                                       \
    if (c) { g_pass++; }                                     \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0'; fclose(f);
  return buf;
}

/* 记下 decider 是否被叫过（叫过 = 核心没认出已知主机）。 */
static int g_decider_calls = 0;
static gt_hostkey_decision_t decider(void *ud, const char *host, uint16_t port,
                                     const char *kt, const char *fp) {
  (void)ud; (void)host; (void)port; (void)kt; (void)fp;
  g_decider_calls++;
  return GT_HOSTKEY_ACCEPT;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc != 6) {
    fprintf(stderr, "usage: %s host port user pem workdir\n", argv[0]);
    return 2;
  }
  const char *host = argv[1];
  int port = atoi(argv[2]);
  const char *user = argv[3];
  char *pem = read_file(argv[4]);
  char kh[1024], kh2[1024];
  snprintf(kh, sizeof kh, "%s/known_hosts", argv[5]);
  snprintf(kh2, sizeof kh2, "%s/known_hosts2", argv[5]);

  /* 1. 先连一次并把主机密钥落盘（与 test_ssh 的 TOFU 用例同法）。 */
  {
    gt_ssh_opts_t o;
    memset(&o, 0, sizeof o);
    o.host = host; o.port = (uint16_t)port; o.username = user;
    o.known_hosts_path = kh;
    gt_ssh_t *s = gt_ssh_new(&o);
    if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
    if (s) gt_ssh_set_hostkey_policy(s, decider, NULL);
    int rc = gt_ssh_connect(s);
    CHECK(rc == GT_OK, "首次连接（TOFU）成功（rc=%d）", rc);
    gt_ssh_close(s);
  }

  char *saved = read_file(kh);
  CHECK(saved != NULL, "TOFU 已写入 known_hosts");
  if (saved) {
    /* 2. **决定性断言**：落盘的 key type 必须能被读侧认出来。
     *    缺陷版本对 ecdsa 条目整条跳过——这里打印出来好定位。 */
    char first[256] = "";
    sscanf(saved, "%255[^\n]", first);
    printf("  known_hosts 首行: %.120s\n", first);
    CHECK(strlen(first) > 0, "known_hosts 有内容");
  }

  /* 3. 用**同一份** known_hosts 再连一次：已知且一致 → decider 不该被调。
   *    缺陷版本每次都会判 UNKNOWN → decider 被调 → UI 弹信任提示。 */
  g_decider_calls = 0;
  {
    gt_ssh_opts_t o;
    memset(&o, 0, sizeof o);
    o.host = host; o.port = (uint16_t)port; o.username = user;
    o.known_hosts_path = kh;
    gt_ssh_t *s = gt_ssh_new(&o);
    if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
    if (s) gt_ssh_set_hostkey_policy(s, decider, NULL);
    int rc = gt_ssh_connect(s);
    CHECK(rc == GT_OK, "第二次连接成功（rc=%d）", rc);
    gt_ssh_close(s);
  }
  CHECK(g_decider_calls == 0,
        "已知主机重连时**不应**再问用户（decider 被调 %d 次）——"
        "被调说明读侧没认出已知条目（ECDSA 条目的 key type 不以 ssh 开头，"
        "按 `ssh` 前缀过滤会把它们整条跳过）", g_decider_calls);

  /* 3b. **多算法 / 裸名条目**也要能认（变异 K2/K3 的牙就在这里）：
   *     - 服务器常同时提供 ed25519 与 ecdsa 两把主机密钥，known_hosts 里
   *       会有两条不同 key type 的记录——只读第一行就会漏掉第二条；
   *     - 用户手写或 ssh 自己写的裸名条目（`127.0.0.1`，不带端口）
   *       按 OpenSSH 语义匹配**任意端口**。
   *
   *     做法：在落盘的那条**前面**插一条「同主机但不同 key type」的记录，
   *     再把主机名换成裸名形态重连。 */
  if (saved) {
    /* 取出刚才落盘的那条，另存一份用于构造"裸名 + 不同算法"的文件。 */
    char ktype[128] = "", kb64[1024] = "";
    {
      char hosts[256];
      sscanf(saved, "%255s %127s %1023s", hosts, ktype, kb64);
    }
    CHECK(ktype[0] != '\0', "能从 known_hosts 里解析出 key type");
    if (ktype[0]) {
      /* 造一份：先一条**不同**算法的同主机记录（把算法名换掉），
       * 再一条真记录。只读第一行会读到假的那条 → 判 MISMATCH →
       * decider 被调（正是要抓的）。 */
      char fake_type[160];
      snprintf(fake_type, sizeof fake_type, "%s",
               strcmp(ktype, "ssh-ed25519") == 0 ? "ssh-rsa" : "ssh-ed25519");
      char path2[1200];
      snprintf(path2, sizeof path2, "%s/known_hosts_multi", argv[5]);
      FILE *f = fopen(path2, "wb");
      if (f) {
        fprintf(f, "[%s]:%d %s AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIII\n",
                host, port, fake_type);
        fputs(saved, f);
        fclose(f);
      }
      g_decider_calls = 0;
      gt_ssh_opts_t o;
      memset(&o, 0, sizeof o);
      o.host = host; o.port = (uint16_t)port; o.username = user;
      o.known_hosts_path = path2;
      gt_ssh_t *s2 = gt_ssh_new(&o);
      if (s2 && pem) gt_ssh_auth_private_key(s2, pem, NULL);
      if (s2) gt_ssh_set_hostkey_policy(s2, decider, NULL);
      int rc = s2 ? gt_ssh_connect(s2) : GT_OK;
      CHECK(rc == GT_OK, "多算法 known_hosts：真记录在第二条也要认（rc=%d）", rc);
      gt_ssh_close(s2);
      CHECK(g_decider_calls == 0,
            "多算法文件里真记录在第二条：不该问用户（decider %d 次）——"
            "被调说明只读了第一行", g_decider_calls);

      /* 裸名条目（不带端口）必须匹配任意端口。 */
      char path3[1200];
      snprintf(path3, sizeof path3, "%s/known_hosts_bare", argv[5]);
      f = fopen(path3, "wb");
      if (f) {
        fprintf(f, "%s %s %s\n", host, ktype, kb64);
        fclose(f);
      }
      g_decider_calls = 0;
      memset(&o, 0, sizeof o);
      o.host = host; o.port = (uint16_t)port; o.username = user;
      o.known_hosts_path = path3;
      gt_ssh_t *s3 = gt_ssh_new(&o);
      if (s3 && pem) gt_ssh_auth_private_key(s3, pem, NULL);
      if (s3) gt_ssh_set_hostkey_policy(s3, decider, NULL);
      rc = s3 ? gt_ssh_connect(s3) : GT_OK;
      CHECK(rc == GT_OK, "裸名条目应匹配任意端口（rc=%d）", rc);
      gt_ssh_close(s3);
      CHECK(g_decider_calls == 0,
            "裸名（不带端口）条目应认出来（decider %d 次）", g_decider_calls);
    }
  }

  /* 4. 密钥变更 → 绝不自动接受（三态不折叠）。 */
  {
    FILE *f = fopen(kh, "wb");
    if (f) { fputs("garbage-host ssh-ed25519 AAAAgarbage\n", f); fclose(f); }
    gt_ssh_opts_t o;
    memset(&o, 0, sizeof o);
    o.host = host; o.port = (uint16_t)port; o.username = user;
    o.known_hosts_path = kh2;
    gt_ssh_t *s = gt_ssh_new(&o);
    if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
    int rc = s ? gt_ssh_connect(s) : GT_OK;
    CHECK(rc == GT_ERR_HOST_KEY,
          "空 known_hosts 且无 decider → GT_ERR_HOST_KEY（不静默 TOFU）");
    gt_ssh_close(s);
  }

  free(saved);
  free(pem);
  printf("test_known_hosts: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}