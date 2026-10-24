/*
 * test_config.c — ssh_config 语义对拍（fixtures 即规范）
 *
 * 断言：
 *   1. 无损往返：parse(golden.conf) → to_string 与原文逐字节相同
 *   2. Host 模式列表按声明顺序
 *   3. 支持集双向一致：C 表(gt_config_supported_keys) == 夹具元数据 supported_key
 *   4. 全表语义对拍：42 键 × 6 host，与真 `ssh -G` 逐键比较（布尔归一化）
 *   5. IdentityFile 回落机制：显式条目不追加默认链；无显式时按策略回落
 *
 * 运行：test_config <fixtures-dir>
 */
#include "ghosterm.h"
#include "ghosterm/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (cond) { g_pass++; }                                       \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

static char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0'; fclose(f);
  if (len_out) *len_out = got;
  return buf;
}

/* ---------------- TSV 解析 ---------------- */

#define MAXREC 8192
typedef struct { char *host, *key, *val; } rec_t;
static rec_t g_rec[MAXREC];
static int g_nrec = 0;

static char *g_default_user = NULL;
static char **g_fallback = NULL; static int g_nfallback = 0;
static char **g_meta_keys = NULL; static int g_nmetakeys = 0;

static char *dupn(const char *s, size_t n) {
  char *p = malloc(n + 1); memcpy(p, s, n); p[n] = '\0'; return p;
}

static void split3(const char *line, char **a, char **b, char **c) {
  *a = *b = *c = NULL;
  const char *t1 = strchr(line, '\t'); if (!t1) return;
  *a = dupn(line, (size_t)(t1 - line));
  const char *t2 = strchr(t1 + 1, '\t');
  if (!t2) { *b = dupn(t1 + 1, strlen(t1 + 1)); return; }
  *b = dupn(t1 + 1, (size_t)(t2 - t1 - 1));
  size_t vlen = strlen(t2 + 1);
  while (vlen && (t2[1 + vlen - 1] == '\n' || t2[1 + vlen - 1] == '\r')) vlen--;
  *c = dupn(t2 + 1, vlen);
}

static void parse_tsv(const char *text) {
  char *copy = strdup(text);
  for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
    if (line[0] == '#') {
      char *body = (line[1] == ' ') ? line + 2 : line + 1;
      char *a, *b, *c;
      split3(body, &a, &b, &c);
      if (!a) continue;
      if (b && strcmp(a, "default_user") == 0) { free(g_default_user); g_default_user = strdup(b); }
      else if (b && strcmp(a, "identity_fallback") == 0) {
        g_fallback = realloc(g_fallback, sizeof(char*) * (g_nfallback + 2));
        g_fallback[g_nfallback++] = strdup(b); g_fallback[g_nfallback] = NULL;
      } else if (b && strcmp(a, "supported_key") == 0) {
        g_meta_keys = realloc(g_meta_keys, sizeof(char*) * (g_nmetakeys + 2));
        g_meta_keys[g_nmetakeys++] = strdup(b); g_meta_keys[g_nmetakeys] = NULL;
      }
      free(a); free(b); free(c);
      continue;
    }
    char *a, *b, *c;
    split3(line, &a, &b, &c);
    if (a && b && c && g_nrec < MAXREC) { g_rec[g_nrec].host = a; g_rec[g_nrec].key = b; g_rec[g_nrec].val = c; g_nrec++; }
    else { free(a); free(b); free(c); }
  }
  free(copy);
}

/* 取该 host/key 的期望值（多值以 \n 连接）；无记录则返回空串 */
static char *expected_for(const char *host, const char *key) {
  size_t need = 1;
  for (int i = 0; i < g_nrec; i++)
    if (strcmp(g_rec[i].host, host) == 0 && strcmp(g_rec[i].key, key) == 0)
      need += strlen(g_rec[i].val) + 1;
  char *out = malloc(need); out[0] = '\0';
  size_t at = 0; int first = 1;
  for (int i = 0; i < g_nrec; i++) {
    if (strcmp(g_rec[i].host, host) || strcmp(g_rec[i].key, key)) continue;
    if (!first) out[at++] = '\n';
    first = 0;
    size_t l = strlen(g_rec[i].val);
    memcpy(out + at, g_rec[i].val, l); at += l;
  }
  out[at] = '\0';
  return out;
}

/* 布尔归一化：yes/true → yes；no/false → no；逐行处理 */
static char *normalize(const char *v) {
  size_t n = strlen(v);
  char *out = malloc(n + 1);
  size_t at = 0;
  const char *p = v;
  while (1) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    const char *rep = NULL;
    if (len == 2 && (strncmp(p, "no", 2) == 0)) rep = "no";
    else if (len == 3 && strncasecmp(p, "yes", 3) == 0) rep = "yes";
    else if (len == 4 && strncasecmp(p, "true", 4) == 0) rep = "yes";
    else if (len == 5 && strncasecmp(p, "false", 5) == 0) rep = "no";
    else if (len == 2 && strncasecmp(p, "on", 2) == 0) rep = "yes";
    else if (len == 3 && strncasecmp(p, "off", 3) == 0) rep = "no";
    if (rep) { memcpy(out + at, rep, strlen(rep)); at += strlen(rep); }
    else { memcpy(out + at, p, len); at += len; }
    if (!nl) break;
    out[at++] = '\n';
    p = nl + 1;
    if (!*p) break;
  }
  out[at] = '\0';
  return out;
}

/* ---------------- 主体 ---------------- */

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <fixtures-dir>\n", argv[0]); return 2; }
  char conf_path[512], tsv_path[512];
  snprintf(conf_path, sizeof conf_path, "%s/golden.conf", argv[1]);
  snprintf(tsv_path, sizeof tsv_path, "%s/golden.effective.tsv", argv[1]);

  size_t clen = 0, tlen = 0;
  char *conf_text = read_file(conf_path, &clen);
  char *tsv = read_file(tsv_path, &tlen);
  parse_tsv(tsv);

  gt_config_t *cfg = gt_config_parse(conf_text, clen);
  CHECK(cfg != NULL, "parse failed: %s", gt_last_error());
  if (!cfg) return 1;

  /* 1. 无损往返 */
  char *round = gt_config_to_string(cfg);
  CHECK(round && strcmp(round, conf_text) == 0, "lossless round-trip mismatch");

  /* 2. Host 模式列表 */
  CHECK(gt_config_host_count(cfg) == 6, "host pattern count != 6");
  const char *p0 = gt_config_host_pattern(cfg, 0);
  CHECK(p0 && strcmp(p0, "web") == 0, "pattern[0]");

  /* 3. 支持集双向一致 */
  size_t nsup = 0;
  const char *const *sup = gt_config_supported_keys(&nsup);
  CHECK((int)nsup == g_nmetakeys, "支持集数量：C=%zu 夹具=%d（改了 k_keys 必须重跑 regen-golden.sh）", nsup, g_nmetakeys);
  for (size_t i = 0; i < nsup; i++) {
    int found = 0;
    for (int j = 0; j < g_nmetakeys; j++) if (strcmp(sup[i], g_meta_keys[j]) == 0) { found = 1; break; }
    CHECK(found, "C 表键 '%s' 不在夹具元数据里", sup[i]);
  }

  /* 4. 全表对拍 */
  const gt_resolve_opts_t ropts = {
      .default_user = g_default_user ? g_default_user : "root",
      .identity_policy = GT_IDENTITY_POLICY_CUSTOM,
      .custom_identity_files = (const char *const *)g_fallback,
  };
  const char *hosts[] = {"web", "api", "db.prod", "secret", "jumpchain", "other"};
  int compared = 0;
  for (size_t hi = 0; hi < sizeof hosts / sizeof *hosts; hi++) {
    for (size_t ki = 0; ki < nsup; ki++) {
      char buf[4096];
      int n = gt_config_effective(cfg, hosts[hi], sup[ki], &ropts, buf, sizeof buf);
      if (n < 0) { CHECK(0, "%s.%s: gt_config_effective 返回 -1 (%s)", hosts[hi], sup[ki], gt_last_error()); continue; }
      char *want = expected_for(hosts[hi], sup[ki]);
      char *wn = normalize(want), *gn = normalize(buf);
      compared++;
      if (strcmp(wn, gn) != 0) {
        g_fail++;
        printf("FAIL: %s.%s\n  got  [%s]\n  want [%s]\n", hosts[hi], sup[ki], gn, wn);
      } else g_pass++;
      free(want); free(wn); free(gn);
    }
  }
  printf("  （全表对拍：%d 项 = %zu host × %zu key）\n", compared, sizeof hosts / sizeof *hosts, nsup);

  /* 5. IdentityFile 回落机制 */
  {
    gt_resolve_opts_t o_none = ropts; o_none.identity_policy = GT_IDENTITY_POLICY_NONE;
    char b1[1024], b2[1024];
    gt_config_effective(cfg, "secret", "identityfile", &o_none, b1, sizeof b1);
    gt_config_effective(cfg, "secret", "identityfile", &ropts, b2, sizeof b2);
    CHECK(b1[0] == '\0', "POLICY_NONE 时无显式条目应为空，得到 [%s]", b1);
    CHECK(g_nfallback > 0 && strncmp(b2, g_fallback[0], strlen(g_fallback[0])) == 0,
          "POLICY_CUSTOM 应回落到夹具记录的链（首条 %s），得到 [%s]", g_fallback ? g_fallback[0] : "(null)", b2);

    /* 显式条目存在时，任何策略都不得追加默认链 */
    char b3[1024];
    gt_config_effective(cfg, "web", "identityfile", &ropts, b3, sizeof b3);
    int lines = 1; for (const char *q = b3; *q; q++) if (*q == '\n') lines++;
    CHECK(lines == 3, "web 有 3 条显式 IdentityFile，不应追加默认链（得到 %d 行）", lines);
    CHECK(strstr(b3, g_fallback[0]) == NULL, "web 结果不应含默认链首条");
  }

  /* 6. 预设策略条数（值取自 OpenSSH 源码 readconf.c） */
  {
    gt_resolve_opts_t o = { .default_user = "u", .identity_policy = GT_IDENTITY_POLICY_OPENSSH_9_6, .custom_identity_files = NULL };
    gt_resolved_t *r = gt_config_resolve(cfg, "secret", &o);
    int n = 0; if (r && r->identity_files) for (; r->identity_files[n]; n++) {}
    CHECK(n == 7, "OPENSSH_9_6 策略应为 7 条，得到 %d", n);
    gt_resolved_free(r);
    o.identity_policy = GT_IDENTITY_POLICY_OPENSSH_10_3;
    r = gt_config_resolve(cfg, "secret", &o);
    n = 0; if (r && r->identity_files) for (; r->identity_files[n]; n++) {}
    CHECK(n == 5, "OPENSSH_10_3 策略应为 5 条，得到 %d", n);
    gt_resolved_free(r);
    o.identity_policy = GT_IDENTITY_POLICY_OPENSSH_MASTER;
    r = gt_config_resolve(cfg, "secret", &o);
    n = 0; if (r && r->identity_files) for (; r->identity_files[n]; n++) {}
    CHECK(n == 6, "OPENSSH_MASTER 策略应为 6 条，得到 %d", n);
    gt_resolved_free(r);
  }

  /* 7. Include 展开语义（M3 延后项落地）：相对路径相对**包含者所在目录**、
   * glob、嵌套 include、无损写回（Include 行保留 / 被含内容不进输出）、
   * 具名文件缺失显式报错——全部与真 ssh -G 同口径。
   * 布局必须用**绝对路径**：相对 include 的解析相对主文件目录，而主文件
   * 路径若相对 cwd，测试在不同 cwd（repo root / build）下行为会漂。 */
  {
    char fixtures_abs[512];
    if (!realpath(argv[1], fixtures_abs)) {
      snprintf(fixtures_abs, sizeof fixtures_abs, "%s", argv[1]);
    }
    /* 布局（全在临时目录，测试自建自清；include 用**绝对路径**——
     * 相对路径的 OpenSSH 语义是相对 ~/.ssh，测试不碰用户真实目录）：
     *   <t>/main.conf          : Include <t>/sub 通配（绝对）+ Host localhost
     *   <t>/sub/x.conf         : Include <t>/sub/inc_target.conf（绝对，嵌套）
     *   fixtures/inc_target.conf: Host incuser（User frominclude） */
    char t[512], sub[512], mainc[512], xconf[512], incsrc[512];
    snprintf(t, sizeof t, "%s/inc-e2e-XXXXXX", fixtures_abs);
    if (mkdtemp(t)) {
      snprintf(sub, sizeof sub, "%s/sub", t);
      snprintf(mainc, sizeof mainc, "%s/main.conf", t);
      snprintf(xconf, sizeof xconf, "%s/x.conf", sub);
      snprintf(incsrc, sizeof incsrc, "%s/inc_target.conf", fixtures_abs);
      mkdir(sub, 0755);
      FILE *f = fopen(mainc, "w");
      if (f) {
        fprintf(f, "Include %s/sub/*.conf\nHost localhost\n  User localuser\n", t);
        fclose(f);
      }
      f = fopen(xconf, "w");
      if (f) {
        fprintf(f, "Include %s/sub/inc_target.conf\nHost frominc\n  Port 2222\n", t);
        fclose(f);
      }
      /* 写入「被嵌套 include 的目标文件」（内容与 fixtures/inc_target.conf
       * 同构；复制源文件以保持夹具单一真相）。 */
      char incdst[512];
      snprintf(incdst, sizeof incdst, "%s/inc_target.conf", sub);
      {
        FILE *src = fopen(incsrc, "rb");
        FILE *dst = fopen(incdst, "wb");
        if (src && dst) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, src)) > 0) fwrite(b, 1, n, dst); }
        if (src) fclose(src);
        if (dst) fclose(dst);
      }

      gt_config_t *icfg = gt_config_parse_file(mainc);
      CHECK(icfg != NULL, "Include 配置解析成功: %s",
            icfg ? "" : gt_last_error());
      if (icfg) {
        /* a) Host 模式来自所有展开层级：incuser / frominc / localhost。 */
        size_t hn = gt_config_host_count(icfg);
        int have_incuser = 0, have_frominc = 0, have_localhost = 0;
        for (size_t i = 0; i < hn; i++) {
          const char *pat = gt_config_host_pattern(icfg, i);
          if (pat) {
            if (strcmp(pat, "incuser") == 0) have_incuser = 1;
            if (strcmp(pat, "frominc") == 0) have_frominc = 1;
            if (strcmp(pat, "localhost") == 0) have_localhost = 1;
          }
        }
        CHECK(have_incuser && have_frominc && have_localhost,
              "Include 展开应含三层 Host（incuser/frominc/localhost，得到 %zu 个）", hn);

        /* b) 语义断言（展开来自 include 链）。 */
        {
          gt_resolved_t *r = gt_config_resolve(icfg, "incuser", NULL);
          CHECK(r && r->user && strcmp(r->user, "frominclude") == 0,
                "incuser.user 应来自 include（want frominclude，got %s）",
                r && r->user ? r->user : "(null)");
          CHECK(r && r->hostname && strcmp(r->hostname, "10.7.7.7") == 0,
                "incuser.hostname 应为 10.7.7.7");
          gt_resolved_free(r);
        }

        /* c) 无损写回：Include 行保留，被含内容不重复出现。 */
        char *out = gt_config_to_string(icfg);
        CHECK(out && strstr(out, "Include") != NULL && strstr(out, "/sub/") != NULL,
              "写回应保留 Include 原行");
        CHECK(out && strstr(out, "frominclude") == NULL,
              "被含内容不得重复进入写回输出（铁律 #4 的 Include 口径）");
        free(out);
        gt_config_free(icfg);
      }

      /* d) 具名（无通配）include 缺文件 → parse 失败且错误可读（不静默）。 */
      {
        char bad[512];
        snprintf(bad, sizeof bad, "%s/bad.conf", t);
        FILE *f = fopen(bad, "w");
        if (f) { fprintf(f, "Include no-such-file.conf\n"); fclose(f); }
        gt_config_t *bc = gt_config_parse_file(bad);
        CHECK(bc == NULL, "具名 include 缺失必须 parse 失败");
        CHECK(strlen(gt_last_error()) > 0, "include 失败须留下可读错误（%s）",
              gt_last_error());
        remove(bad);
      }

      /* e0) 相对 include 的基准目录是 ~/.ssh（与真 ssh 同口径对拍；
       * HOME 重定向进临时目录，绝不碰真实 ~/.ssh）。 */
      {
        char fakehome[512], fkssh[512], fkconf[512];
        snprintf(fakehome, sizeof fakehome, "%s/fakehome", t);
        snprintf(fkssh, sizeof fkssh, "%s/.ssh", fakehome);
        snprintf(fkconf, sizeof fkconf, "%s/config", fkssh);
        mkdir(fakehome, 0755);
        mkdir(fkssh, 0755);
        FILE *f = fopen(fkconf, "w");
        if (f) { fprintf(f, "Include rel-*.conf\nHost relhost\n"); fclose(f); }
        char relsrc[512];
        snprintf(relsrc, sizeof relsrc, "%s/rel-a.conf", fkssh);
        f = fopen(relsrc, "w");
        if (f) { fprintf(f, "Host reltarget\n  User reluser\n"); fclose(f); }

        /* 我们：HOME 重定向后解析相对 include。 */
        char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
        setenv("HOME", fakehome, 1);
        gt_config_t *rc = gt_config_parse_file(fkconf);
        size_t reln = rc ? gt_config_host_count(rc) : 0;
        int have_reltarget = 0, have_relhost = 0;
        for (size_t i = 0; i < reln; i++) {
          const char *pat = gt_config_host_pattern(rc, i);
          if (pat && strcmp(pat, "reltarget") == 0) have_reltarget = 1;
          if (pat && strcmp(pat, "relhost") == 0) have_relhost = 1;
        }
        CHECK(have_reltarget && have_relhost,
              "相对 include 应相对 $HOME/.ssh 展开（reltarget/relhost）");

        /* 真 ssh 同口径对拍：-F 显式路径时相对 include 解析相对
         * $HOME/.ssh（实测 OpenSSH_10.3p1：HOME=getpwuid 家目录仅在无 -F
         * 时生效；-F 模式下 $HOME 参与 include 解析）。 */
        char cmd[1024];
        snprintf(cmd, sizeof cmd,
                 "HOME=%s ssh -G -F %s reltarget 2>/dev/null | grep -c '^user reluser$'",
                 fakehome, fkconf);
        int ssh_ok = system(cmd);
        CHECK(ssh_ok == 0,
              "真 ssh 同口径也应把相对 include 解析到 $HOME/.ssh（退出码 %d）",
              ssh_ok);
        if (rc) gt_config_free(rc);
        if (saved_home) { setenv("HOME", saved_home, 1); free(saved_home); }
        else unsetenv("HOME");
        remove(fkconf); remove(relsrc); rmdir(fkssh); rmdir(fakehome);
      }

      /* e) 通配无匹配 → 静默（与 ssh 一致，不是错误）。 */
      {
        char w[512];
        snprintf(w, sizeof w, "%s/wild.conf", t);
        FILE *f = fopen(w, "w");
        if (f) { fprintf(f, "Include nomatch-*.conf\nHost ok\n"); fclose(f); }
        gt_config_t *wc = gt_config_parse_file(w);
        CHECK(wc != NULL, "通配 include 无匹配应静默跳过（不是错误）");
        gt_config_free(wc);
        remove(w);
      }

      /* f) 自环 include（具名绝对路径指向自身）→ 深度上限必须拦下
       * （parse 失败，不无限递归）。上限抬高/失效时本断言变红。 */
      {
        char selfc[512];
        snprintf(selfc, sizeof selfc, "%s/self.conf", t);
        FILE *f = fopen(selfc, "w");
        if (f) { fprintf(f, "Include %s\nHost selfhost\n", selfc); fclose(f); }
        gt_config_t *sc = gt_config_parse_file(selfc);
        CHECK(sc == NULL, "自环 include 必须被深度上限拦下（parse 失败）");
        if (sc) gt_config_free(sc);
        remove(selfc);
      }

      /* 清理。 */
      remove(xconf); remove(incdst); remove(mainc); rmdir(sub); rmdir(t);
    }
  }

  /* 8. 行内注释剥离 + Forward 归一化防护（闸门 SIGBUS 的根因回归）。
   * 口径全部经真 ssh -G 实证：
   *   `RemoteForward 9001 localhost:9001 # 9001-gate` → `9001 [localhost]:9001`
   *   `HostName foo#bar`（token 中间 #）→ 原样 `foo#bar`
   *   `LocalForward 8080:lh 9090`（冒号在最后一个 token 之前）→ 我们原样
   *   返回不越界（OpenSSH 会解析成 `8080 [lh]:9090`，对齐它不在本次范围）。 */
  {
    const char *ftext =
      "Host fwd\n"
      "  HostName foo#bar\n"
      "  RemoteForward 9001 localhost:9001 # 9001-gate\n"
      "  LocalForward 8080:lh 9090\n";
    gt_config_t *fcfg = gt_config_parse(ftext, strlen(ftext));
    CHECK(fcfg != NULL, "forward 夹具解析失败");
    if (fcfg) {
      gt_resolve_opts_t o = { .default_user = "u",
                              .identity_policy = GT_IDENTITY_POLICY_NONE,
                              .custom_identity_files = NULL };
      char b[1024];
      gt_config_effective(fcfg, "fwd", "remoteforward", &o, b, sizeof b);
      CHECK(strcmp(b, "9001 [localhost]:9001") == 0,
            "RemoteForward 行内注释应剥离并归一化，期望 [9001 [localhost]:9001]，得到 [%s]", b);
      gt_config_effective(fcfg, "fwd", "hostname", &o, b, sizeof b);
      CHECK(strcmp(b, "foo#bar") == 0,
            "token 中间的 # 不是注释，期望 [foo#bar]，得到 [%s]", b);
      gt_config_effective(fcfg, "fwd", "localforward", &o, b, sizeof b);
      CHECK(strcmp(b, "8080:lh 9090") == 0,
            "冒号在最后一个 token 之前时应原样返回且不越界，得到 [%s]", b);
      gt_config_free(fcfg);
    }
  }

  gt_config_free(cfg);
  free(conf_text); free(tsv);
  printf("test_config: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
