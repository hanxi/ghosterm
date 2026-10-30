
/*
 * config.c — ssh_config 解析 / 无损写回 / 求值
 *
 * 语义锚点：core/tests/fixtures/golden.*（真 `ssh -G` 同源再生，勿手改）。
 * 值渲染约定：布尔统一输出 yes/no（`ssh -G` 混用 yes/no 与 true/false），
 * 对拍时做归一化比较（见 core/tests/README.md）。
 */
#include "ghosterm.h"
#include "ghosterm/config.h"
#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 关键字表：支持集 = 对拍集 = UI「生效配置」可见集                     */
/* ------------------------------------------------------------------ */

typedef enum { K_SCALAR, K_LIST, K_LIST_FIRST } key_kind_t;

typedef struct {
  const char *name;
  key_kind_t kind;
  const char *def; /* 默认值（NULL = 由上下文决定） */
} keydef_t;

static const keydef_t k_keys[] = {
    {"addressfamily", K_SCALAR, "any"},
    {"addkeystoagent", K_SCALAR, "no"},
    {"batchmode", K_SCALAR, "no"},
    {"canonicalizehostname", K_SCALAR, "no"},
    {"canonicalizemaxdots", K_SCALAR, "1"},
    {"checkhostip", K_SCALAR, "no"},
    {"clearallforwardings", K_SCALAR, "no"},
    {"compression", K_SCALAR, "no"},
    {"connectionattempts", K_SCALAR, "1"},
    {"connecttimeout", K_SCALAR, "none"},
    {"escapechar", K_SCALAR, "~"},
    {"exitonforwardfailure", K_SCALAR, "no"},
    {"forwardagent", K_SCALAR, "no"},
    {"forwardx11", K_SCALAR, "no"},
    {"forwardx11timeout", K_SCALAR, "1200"},
    {"forwardx11trusted", K_SCALAR, "no"},
    {"gssapiauthentication", K_SCALAR, "no"},
    {"hashknownhosts", K_SCALAR, "no"},
    {"hostbasedauthentication", K_SCALAR, "no"},
    {"hostname", K_SCALAR, NULL}, /* 默认 = 别名本身 */
    {"identitiesonly", K_SCALAR, "no"},
    {"identityfile", K_LIST, NULL}, /* 默认 = identity_policy */
    {"kbdinteractiveauthentication", K_SCALAR, "yes"},
    {"localforward", K_LIST, NULL},
    {"loglevel", K_SCALAR, "INFO"},
    {"numberofpasswordprompts", K_SCALAR, "3"},
    {"passwordauthentication", K_SCALAR, "yes"},
    {"permitlocalcommand", K_SCALAR, "no"},
    {"port", K_SCALAR, "22"},
    {"proxyjump", K_SCALAR, NULL},
    {"pubkeyauthentication", K_SCALAR, "yes"},
    {"remoteforward", K_LIST, NULL},
    {"requesttty", K_SCALAR, "auto"},
    {"serveralivecountmax", K_SCALAR, "3"},
    {"serveraliveinterval", K_SCALAR, "0"},
    {"sessiontype", K_SCALAR, "default"},
    {"setenv", K_LIST_FIRST, NULL},
    {"stricthostkeychecking", K_SCALAR, "ask"},
    {"tcpkeepalive", K_SCALAR, "yes"},
    {"user", K_SCALAR, NULL}, /* 默认 = default_user */
    {"visualhostkey", K_SCALAR, "no"},
    {"warnweakcrypto", K_SCALAR, "yes"},
};

#define K_COUNT ((int)(sizeof k_keys / sizeof k_keys[0]))

const char *const *gt_config_supported_keys(size_t *count) {
  static const char *names[K_COUNT];
  static int built = 0;
  if (!built) {
    for (int i = 0; i < K_COUNT; i++) names[i] = k_keys[i].name;
    built = 1;
  }
  if (count) *count = (size_t)K_COUNT;
  return names;
}

static int key_index(const char *kw) {
  for (int i = 0; i < K_COUNT; i++)
    if (strcmp(k_keys[i].name, kw) == 0) return i;
  return -1;
}

/* OpenSSH 默认 IdentityFile 候选链（源码 readconf.c fill_default_options） */
static const char *const k_id_openssh_9_6[] = {
    "~/.ssh/id_rsa",     "~/.ssh/id_ecdsa",      "~/.ssh/id_ecdsa_sk",
    "~/.ssh/id_ed25519", "~/.ssh/id_ed25519_sk", "~/.ssh/id_xmss",
    "~/.ssh/id_dsa",     NULL,
};
/* 本机 OpenSSH_10.3p1 `ssh -G -F /dev/null` 实测 */
static const char *const k_id_openssh_10_3[] = {
    "~/.ssh/id_rsa",     "~/.ssh/id_ecdsa",      "~/.ssh/id_ecdsa_sk",
    "~/.ssh/id_ed25519", "~/.ssh/id_ed25519_sk", NULL,
};
/* 上游 master readconf.c fill_default_options()（xmss/dsa 已删，换抗量子混合密钥） */
static const char *const k_id_openssh_master[] = {
    "~/.ssh/id_rsa",     "~/.ssh/id_ecdsa",      "~/.ssh/id_ecdsa_sk",
    "~/.ssh/id_ed25519", "~/.ssh/id_ed25519_sk", "~/.ssh/id_mldsa44_ed25519",
    NULL,
};

/* ------------------------------------------------------------------ */
/* 节点：保留原始行（无损写回）+ 解析出的关键字/参数                     */
/* ------------------------------------------------------------------ */

typedef struct {
  char *raw;
  char *keyword;
  char **args;
  int nargs;
  int block;
  int included;
  char *source_path;
  size_t source_line;
} gt_node_t;

typedef struct {
  char **patterns;
  int npatterns;
  size_t header_node;
} gt_block_t;

struct gt_config {
  gt_node_t *nodes;
  size_t nnodes, cap;
  gt_block_t *blocks;
  size_t nblocks, bcap;
  char *source_path;
  int include_error; /* Include 展开失败（深度超限/具名文件缺失）→ parse 失败 */
};

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
  if (need <= *cap) return p;
  size_t nc = *cap ? *cap * 2 : 8;
  while (nc < need) nc *= 2;
  void *q = gt__realloc(p, nc * elem);
  if (q) *cap = nc;
  return q;
}

static char *lower_dup(const char *s, size_t n) {
  char *p = gt__alloc(n + 1);
  if (!p) return NULL;
  for (size_t i = 0; i < n; i++) p[i] = (char)tolower((unsigned char)s[i]);
  p[n] = '\0';
  return p;
}

typedef struct { int index; const char *start, *end; } token_range_t;

/* Optional raw range uses the same quote/escape scanner as parsing. */
static int tokenize(const char *p, const char *end, char ***out_args, int *out_n,
                    token_range_t *range) {
  char **args = NULL;
  int n = 0;
  size_t cap = 0;
  while (p < end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) break;
    /* 行内注释：token 开头的未引号 # 注释到行尾。对齐 OpenSSH——ssh -G
     * 实证：`RemoteForward 9001 localhost:9001 # note` 输出不含 note；
     * token 中间的 # 不是注释（`HostName foo#bar` 原样保留）。不剥离会
     * 把注释当 token 进 args：normalize_forward 取到的「最后一个 token」
     * 不含冒号 → 负差值算长度 → memchr 越界读（SIGBUS，闸门实证）。 */
    if (*p == '#') break;
    const char *token_start = p;
    char buf[1024];
    size_t bl = 0;
    int quoted = 0;
    while (p < end) {
      char c = *p;
      if (c == '\\' && p + 1 < end) { p++; c = *p; }
      else if (c == '"') { quoted = !quoted; p++; continue; }
      else if (!quoted && isspace((unsigned char)c)) break;
      if (bl + 1 < sizeof buf) buf[bl++] = c;
      p++;
    }
    buf[bl] = '\0';
    if (range && range->index == n) { range->start = token_start; range->end = p; }
    args = grow(args, &cap, (size_t)n + 2, sizeof *args);
    if (!args) return -1;
    args[n++] = gt__strdup(buf);
    args[n] = NULL;
  }
  *out_args = args;
  *out_n = n;
  return 0;
}

static int glob_match(const char *pat, const char *str) {
  for (;;) {
    char pc = *pat, sc = *str;
    if (pc == '\0') return sc == '\0';
    if (pc == '*') {
      while (*pat == '*') pat++;
      if (*pat == '\0') return 1;
      for (const char *t = str;; t++) {
        if (glob_match(pat, t)) return 1;
        if (*t == '\0') return 0;
      }
    }
    if (sc == '\0') return 0;
    if (pc != '?' && tolower((unsigned char)pc) != tolower((unsigned char)sc)) return 0;
    pat++; str++;
  }
}

/* ------------------------------------------------------------------ */
/* 解析                                                               */
/* ------------------------------------------------------------------ */

/* 预约一个新节点并**立即计入 nnodes**（返回的槽位已归属调用者）。
 * 计数先行是关键：递归 parse（Include 展开）会在外层节点填完之前
 * 追加新节点——若槽位靠 nnodes 推导而外层尚未 ++，递归会**复用同一
 * 槽位**覆盖外层节点（表现为 raw 被清 NULL、to_string 崩溃）。 */
static gt_node_t *node_new(gt_config_t *cfg) {
  cfg->nodes = grow(cfg->nodes, &cfg->cap, cfg->nnodes + 1, sizeof(gt_node_t));
  if (!cfg->nodes) return NULL;
  gt_node_t *n = &cfg->nodes[cfg->nnodes];
  memset(n, 0, sizeof *n);
  n->block = -1;
  cfg->nnodes++;
  return n;
}

static char *read_file(const char *path, size_t *len_out);
static void parse_depth(gt_config_t *cfg, const char *text, size_t len,
                        int included, int depth, const char *source_path);
static void process_include(gt_config_t *cfg, const char *const *paths, int npaths,
                             int depth);

static void parse_text(gt_config_t *cfg, const char *text, size_t len, int included) {
  parse_depth(cfg, text, len, included, 0, cfg->source_path ? cfg->source_path : "");
}

/* ------------------------------------------------------------------ */
/* Include 展开语义（对齐 OpenSSH readconf.c）                          */
/* ------------------------------------------------------------------ */

/* OpenSSH 的 Include 递归深度上限（readconf.c "too many recursive "
 * "configuration includes"）。 */
#define GT_CONFIG_MAX_INCLUDE_DEPTH 16

/* 展开 ~ 为家目录（OpenSSH 用户级语义）。返回堆串（调用方 free）。 */
static char *expand_tilde(const char *path) {
  if (!path) return NULL;
  if (path[0] != '~') return gt__strdup(path);
  if (path[1] == '\0' || path[1] == '/') {
    const char *home = getenv("HOME");
    if (!home || !*home) {
      struct passwd *pw = getpwuid(getuid());
      home = pw ? pw->pw_name : NULL;
      /* 无家目录时回落 /etc 下按用户名（与 OpenSSH 同式），再不行裸返回。 */
      if (home) {
        char buf[256];
        snprintf(buf, sizeof buf, "/etc/ssh/users/%s", home);
        return gt__strdup(buf);
      }
      return gt__strdup(path);
    }
    const char *rest = path[1] == '/' ? path + 2 : path + 1;
    size_t n = strlen(home) + 1 + strlen(rest) + 1;
    char *out = gt__alloc(n);
    if (out) snprintf(out, n, "%s/%s", home, rest);
    return out;
  }
  /* ~user 前缀：查 passwd（getpwnam 失败按原样返回，OpenSSH 亦静默）。 */
  const char *slash = strchr(path, '/');
  size_t name_len = slash ? (size_t)(slash - path - 1) : strlen(path + 1);
  char name[128];
  if (name_len >= sizeof name) return gt__strdup(path);
  memcpy(name, path + 1, name_len);
  name[name_len] = '\0';
  struct passwd *pw = getpwnam(name);
  if (!pw || !pw->pw_dir) return gt__strdup(path);
  const char *rest = slash ? slash + 1 : "";
  size_t n = strlen(pw->pw_dir) + 1 + strlen(rest) + 1;
  char *out = gt__alloc(n);
  if (out) snprintf(out, n, "%s/%s", pw->pw_dir, rest);
  return out;
}

/*
 * 处理一条 Include 指令的每个参数：glob 展开 → 逐文件递归 parse。
 * 语义对齐 OpenSSH（实测 OpenSSH_10.3p1 验证）：
 *   - 绝对路径直接用；**相对路径相对 ~/.ssh**（与包含者所在目录无关）；
 *   - ~ / ~user 展开；
 *   - glob 无匹配 → 静默跳过（与 ssh 一致）；
 *   - 具名（无通配符）文件读不了 → 记错误（顶层时 parse 失败）；
 *   - 深度超限 → 报错（cfg->include_error 置位，gt_config_parse 转失败）。
 * included 节点不参与无损写回（to_string 跳过）——Include 行本身保留。
 */
static void process_include(gt_config_t *cfg, const char *const *paths, int npaths,
                             int depth) {
  if (depth >= GT_CONFIG_MAX_INCLUDE_DEPTH) {
    gt__set_error("include: 递归深度超限（>%d）", GT_CONFIG_MAX_INCLUDE_DEPTH);
    cfg->include_error = 1;
    return;
  }
  /* 相对 include 的基准目录：~/.ssh（OpenSSH 用户级语义；实测
   * OpenSSH_10.3p1：同目录 inner.conf 不生效、~/.ssh/inner.conf 生效）。 */
  const char *home = getenv("HOME");
  char sshdir_buf[512];
  if (home && *home) {
    snprintf(sshdir_buf, sizeof sshdir_buf, "%s/.ssh", home);
  } else {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
      snprintf(sshdir_buf, sizeof sshdir_buf, "%s/.ssh", pw->pw_dir);
    else
      snprintf(sshdir_buf, sizeof sshdir_buf, ".ssh");
  }
  const char *base_dir = sshdir_buf;

  for (int i = 0; i < npaths; i++) {
    char *expanded = expand_tilde(paths[i]);
    if (!expanded) continue;

    /* 相对路径 → 相对 ~/.ssh（实测 OpenSSH_10.3p1 对拍）。 */
    char *resolved = expanded;
    if (expanded[0] != '/') {
      size_t n = strlen(base_dir) + 1 + strlen(expanded) + 1;
      char *joined = gt__alloc(n);
      if (joined) {
        snprintf(joined, n, "%s/%s", base_dir, expanded);
        resolved = joined;
      }
    }

    glob_t g;
    memset(&g, 0, sizeof g);
    /* expand_tilde already resolved ~ / ~user above. GLOB_TILDE is a
     * non-POSIX extension absent from Android Bionic. */
    int gr = glob(resolved, 0, NULL, &g);
    if (gr == 0) {
      for (size_t k = 0; k < g.gl_pathc; k++) {
        size_t flen = 0;
        char *ftext = read_file(g.gl_pathv[k], &flen);
        if (ftext) {
          parse_depth(cfg, ftext, flen, 1, depth + 1, g.gl_pathv[k]);
          free(ftext);
        } else if (!strpbrk(resolved, "*?")) {
          /* 具名文件读不了（glob 成功但 open 失败不会走到这——read_file
           * 失败即报）。 */
          gt__set_error("include: 无法读取 %s", g.gl_pathv[k]);
          cfg->include_error = 1;
        }
      }
      globfree(&g);
    } else if (gr == GLOB_NOMATCH) {
      if (strpbrk(resolved, "*?") == NULL) {
        /* 无通配符但文件不存在 → 报错（OpenSSH 对用户级具名 include 报错）。 */
        gt__set_error("include: 文件不存在 %s", resolved);
        cfg->include_error = 1;
      }
      /* 通配无匹配 → 静默（与 ssh 一致）。 */
    } else {
      gt__set_error("include: glob 失败 %s", resolved);
      cfg->include_error = 1;
    }

    if (resolved != expanded) free(resolved);
    free(expanded);
  }
}

static char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) { fclose(f); return NULL; }
  char *buf = gt__alloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[got] = '\0';
  if (len_out) *len_out = got;
  return buf;
}

/*
 * 解析主体（带深度与基准目录）：include 指令在行处即展开——
 * 展开出的节点带 included=1（不参与无损写回），Include 行本身保留。
 */
static void parse_depth(gt_config_t *cfg, const char *text, size_t len,
                        int included, int depth, const char *source_path) {
  const char *p = text, *end = text + len;
  size_t source_line = 0;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *le = nl ? nl + 1 : end;
    size_t raw_len = (size_t)(le - p);

    gt_node_t *node = node_new(cfg);
    if (!node) return;
    node->source_path = gt__strdup(source_path);
    node->source_line = ++source_line;
    if (!node->source_path) {
      cfg->include_error = 1;
      gt__set_error("config source metadata: out of memory");
      return;
    }
    node->raw = gt__alloc(raw_len + 1);
    memcpy(node->raw, p, raw_len);
    node->raw[raw_len] = '\0';
    node->included = included;
    node->block = cfg->nblocks ? (int)cfg->nblocks - 1 : -1;

    /* 注意：本循环里的 node 指针在**任何**可能追加节点的调用
     * （process_include → parse_depth → node_new → realloc nodes）后都会
     * 悬垂。故 include 的展开推迟到本节点全部填完、且后续不再解引用 node。 */
    const char *q = p;
    char *kw = NULL;
    char **args = NULL;
    int nargs = 0;
    while (q < le && (*q == ' ' || *q == '\t')) q++;
    if (q < le && *q != '#' && *q != '\n' && *q != '\r') {
      const char *ks = q;
      /* 关键字在空白或首个 '=' 处结束——OpenSSH 同时接受 `Keyword value`
       * 与 `Keyword=value`（后者见 writeback 夹具 bastion 的 IdentityFile=）；
       * 不切开 '=' 会把整行都当成关键字名。 */
      while (q < le && !isspace((unsigned char)*q) && *q != '=') q++;
      kw = lower_dup(ks, (size_t)(q - ks));
      if (q < le && *q == '=') q++; /* 跳过 '='，值从其后 tokenize */
      if (tokenize(q, le, &args, &nargs, NULL) != 0) {
        free(kw);
        return;
      }
    }

    size_t idx = (size_t)(node - cfg->nodes); /* realloc 后重取 */
    if (kw && strcmp(kw, "host") == 0 && nargs > 0) {
      cfg->blocks = grow(cfg->blocks, &cfg->bcap, cfg->nblocks + 1, sizeof(gt_block_t));
      if (!cfg->blocks) { free(kw); return; }
      gt_block_t *b = &cfg->blocks[cfg->nblocks];
      memset(b, 0, sizeof *b);
      b->patterns = args;
      b->npatterns = nargs;
      b->header_node = idx;
      cfg->nodes[idx].keyword = kw;
      cfg->nodes[idx].args = args;
      cfg->nodes[idx].nargs = nargs;
      cfg->nodes[idx].block = (int)cfg->nblocks;
      cfg->nblocks++;
    } else if (kw && strcmp(kw, "include") == 0 && nargs > 0) {
      /* Include 展开会追加节点（realloc nodes！）——先固化本节点再展开。
       * 嵌套 include 允许（OpenSSH 语义），递归深度上限挡循环。 */
      cfg->nodes[idx].keyword = kw;
      cfg->nodes[idx].args = args;
      cfg->nodes[idx].nargs = nargs;
      const char *const *inc_args = (const char *const *)(void *)args;
      process_include(cfg, inc_args, nargs, depth);
    } else if (kw) {
      cfg->nodes[idx].keyword = kw;
      cfg->nodes[idx].args = args;
      cfg->nodes[idx].nargs = nargs;
    }
    p = le;
  }
}

gt_config_t *gt_config_parse(const char *text, size_t len) {
  if (!text) { gt__set_error("parse: null text"); return NULL; }
  gt_config_t *cfg = gt__alloc(sizeof *cfg);
  if (!cfg) return NULL;
  parse_text(cfg, text, len, 0);
  if (cfg->include_error) {
    gt_config_free(cfg);
    return NULL; /* 错误串已由 include 层设置 */
  }
  return cfg;
}

gt_config_t *gt_config_parse_file(const char *path) {
  if (!path) { gt__set_error("parse_file: null path"); return NULL; }
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (errno == ENOENT) return gt_config_parse("", 0);
    gt__set_error("open %s: %s", path, strerror(errno));
    return NULL;
  }
  size_t cap = 4096, len = 0;
  char *buf = gt__alloc(cap);
  if (!buf) { fclose(f); return NULL; }
  for (;;) {
    if (len + 4096 > cap) {
      buf = gt__realloc(buf, cap *= 2);
      if (!buf) { fclose(f); return NULL; }
    }
    size_t got = fread(buf + len, 1, cap - len - 1, f);
    len += got;
    if (got == 0) break;
  }
  if (ferror(f)) {
    int read_error = errno ? errno : EIO;
    fclose(f);
    free(buf);
    gt__set_error("read %s: %s", path, strerror(read_error));
    return NULL;
  }
  fclose(f);
  buf[len] = '\0';
  /* 相对 include 解析相对 ~/.ssh（OpenSSH 用户级语义，见 process_include）。 */
  gt_config_t *cfg = gt__alloc(sizeof *cfg);
  if (cfg) {
    cfg->source_path = gt__strdup(path);
    if (cfg->source_path) parse_depth(cfg, buf, len, 0, 0, cfg->source_path);
    else cfg->include_error = 1;
    if (cfg->include_error) {
      gt_config_free(cfg);
      cfg = NULL; /* 错误串已由 include 层设置 */
    }
  }
  free(buf);
  return cfg;
}

void gt_config_free(gt_config_t *cfg) {
  if (!cfg) return;
  for (size_t i = 0; i < cfg->nnodes; i++) {
    gt_node_t *n = &cfg->nodes[i];
    free(n->raw);
    free(n->keyword);
    free(n->source_path);
    if (n->args) {
      for (int a = 0; a < n->nargs; a++) free(n->args[a]);
      free(n->args);
    }
  }
  free(cfg->nodes);
  free(cfg->blocks); /* patterns 与 node->args 共享所有权 */
  free(cfg->source_path);
  free(cfg);
}

char *gt_config_to_string(const gt_config_t *cfg) {
  if (!cfg) return NULL;
  size_t total = 0;
  for (size_t i = 0; i < cfg->nnodes; i++)
    if (!cfg->nodes[i].included) total += strlen(cfg->nodes[i].raw);
  char *out = gt__alloc(total + 1);
  if (!out) return NULL;
  size_t at = 0;
  for (size_t i = 0; i < cfg->nnodes; i++) {
    const gt_node_t *n = &cfg->nodes[i];
    if (n->included) continue;
    size_t l = strlen(n->raw);
    memcpy(out + at, n->raw, l);
    at += l;
  }
  out[at] = '\0';
  return out;
}

size_t gt_config_host_count(const gt_config_t *cfg) {
  if (!cfg) return 0;
  size_t n = 0;
  for (size_t b = 0; b < cfg->nblocks; b++) n += (size_t)cfg->blocks[b].npatterns;
  return n;
}

const char *gt_config_host_pattern(const gt_config_t *cfg, size_t index) {
  if (!cfg) return NULL;
  for (size_t b = 0; b < cfg->nblocks; b++)
    for (int i = 0; i < cfg->blocks[b].npatterns; i++)
      if (index-- == 0) return cfg->blocks[b].patterns[i];
  return NULL;
}

/* ------------------------------------------------------------------ */
/* 求值引擎（typed 与通用视图共用同一遍扫描）                           */
/* ------------------------------------------------------------------ */

typedef struct {
  char **vals;
  size_t n, cap;
  int set;
  const gt_node_t *origin;
} effslot_t;

typedef struct {
  effslot_t slot[K_COUNT];
} effmap_t;

static void slot_push(effslot_t *s, const char *v) {
  s->vals = grow(s->vals, &s->cap, s->n + 2, sizeof(char *));
  if (!s->vals) return;
  s->vals[s->n++] = gt__strdup(v);
  s->vals[s->n] = NULL;
}

static void effmap_free(effmap_t *m) {
  for (int i = 0; i < K_COUNT; i++) {
    if (!m->slot[i].vals) continue;
    for (size_t k = 0; m->slot[i].vals[k]; k++) free(m->slot[i].vals[k]);
    free(m->slot[i].vals);
  }
}

static int block_matches(const gt_block_t *b, const char *host) {
  int positive = 0;
  for (int i = 0; i < b->npatterns; i++) {
    const char *p = b->patterns[i];
    if (*p == '!') {
      if (glob_match(p + 1, host)) return 0;
    } else if (glob_match(p, host)) {
      positive = 1;
    }
  }
  return positive;
}

/* The profile describes what BOTH native connection consumers actually execute.
 * Do not infer support from k_keys (which describes evaluation, not transport). */
static bool connection_value_supported(const gt_node_t *n) {
  const char *key=n->keyword;
  if (n->nargs != 1 || !*n->args[0]) return false;
  const char *value=n->args[0];
  if (!strcmp(key,"hostname") || !strcmp(key,"user") || !strcmp(key,"identityfile"))
    return strchr(value,'%') == NULL && strstr(value,"${") == NULL;
  if (!strcmp(key,"port")) {
    unsigned port=0;
    for (const unsigned char *p=(const unsigned char *)value; *p; p++) {
      if (*p<'0' || *p>'9' || port>6553) return false;
      port=port*10+(*p-'0');
    }
    return port>0 && port<=65535;
  }
  if (!strcmp(key,"proxyjump")) return !strcasecmp(value,"none");
  if (!strcmp(key,"serveraliveinterval")) return !strcmp(value,"0");
  if (!strcmp(key,"addressfamily")) return !strcasecmp(value,"any");
  if (!strcmp(key,"stricthostkeychecking")) return !strcasecmp(value,"ask");
  static const char *const disabled[]={"forwardagent","forwardx11","compression",
    "addkeystoagent","canonicalizehostname","checkhostip","hashknownhosts",
    "gssapiauthentication","hostbasedauthentication","permitlocalcommand",NULL};
  for (size_t i=0;disabled[i];i++)
    if (!strcmp(key,disabled[i])) return !strcasecmp(value,"no");
  return false;
}

int gt_config_check_connection(const gt_config_t *cfg, const char *host,
                               size_t index, gt_config_connection_issue_t *out) {
  if (!cfg || !host || !*host || !out) return GT_ERR_INVALID_ARG;
  memset(out,0,sizeof *out);
  bool seen[K_COUNT]={false};
  for (size_t i=0;i<cfg->nnodes;i++) {
    const gt_node_t *n=&cfg->nodes[i];
    if (!n->keyword || !strcmp(n->keyword,"host") || !strcmp(n->keyword,"include")) continue;
    bool conditional=!strcmp(n->keyword,"match");
    if (!conditional && n->block>=0 && !block_matches(&cfg->blocks[n->block],host)) continue;
    int key=key_index(n->keyword);
    bool shadowed=false;
    if (key>=0 && k_keys[key].kind!=K_LIST && n->nargs>0) {
      shadowed=seen[key]; seen[key]=true;
    }
    if (shadowed || (!conditional && connection_value_supported(n))) continue;
    if (index-- != 0) continue;
    out->keyword=n->keyword; out->source_path=n->source_path;
    out->source_line=n->source_line; out->included=n->included;
    gt__set_error("SSH connection: unsupported directive %s at line %zu",n->keyword,n->source_line);
    return GT_ERR_UNSUPPORTED;
  }
  return GT_OK;
}

/* "8080 localhost:80" → "8080 [localhost]:80"（对齐 ssh -G 归一化） */
static char *normalize_forward(char **args, int n) {
  size_t need = 1;
  for (int i = 0; i < n; i++) need += strlen(args[i]) + 1;
  char *joined = gt__alloc(need);
  if (!joined) return NULL;
  size_t at = 0;
  for (int i = 0; i < n; i++) {
    if (i) joined[at++] = ' ';
    size_t l = strlen(args[i]);
    memcpy(joined + at, args[i], l);
    at += l;
  }
  joined[at] = '\0';

  char *colon = strrchr(joined, ':');
  if (!colon) return joined;
  char *tok = joined;
  for (char *q = joined; *q; q++)
    if (*q == ' ') tok = q + 1;
  char *hstart = tok, *hend = colon;
  /* 最后一个冒号可能在最后一个 token 之前（`LocalForward 8080:lh 9090`）：
   * hend < hstart 时 (size_t)(hend - hstart) 回绕成巨大长度，memchr 越界
   * 读到未映射页即 SIGBUS（闸门全量跑实证过）。该形状不加括号，原样返回
   * （OpenSSH 对 bind:port 的进一步解析不在本次对齐范围）。 */
  if (hend <= hstart) return joined;
  if (memchr(hstart, ':', (size_t)(hend - hstart))) return joined;
  if (memchr(hstart, '[', (size_t)(hend - hstart))) return joined;
  size_t hlen = (size_t)(hend - hstart);
  char *out = gt__alloc(at + hlen + 3);
  if (!out) { free(joined); return NULL; }
  size_t prefix = (size_t)(hstart - joined);
  memcpy(out, joined, prefix);
  out[prefix] = '[';
  memcpy(out + prefix + 1, hstart, hlen);
  out[prefix + 1 + hlen] = ']';
  strcpy(out + prefix + 2 + hlen, hend);
  free(joined);
  return out;
}

static void effmap_build(effmap_t *m, const gt_config_t *cfg, const char *host,
                         const gt_resolve_opts_t *opts) {
  memset(m, 0, sizeof *m);
  for (size_t i = 0; i < cfg->nnodes; i++) {
    const gt_node_t *n = &cfg->nodes[i];
    if (!n->keyword) continue;
    if (strcmp(n->keyword, "host") == 0) continue;
    int idx = key_index(n->keyword);
    if (idx < 0) continue; /* 未支持的指令：不影响支持集 */
    int applies = (n->block < 0) ? 1 : block_matches(&cfg->blocks[n->block], host);
    if (!applies) continue;

    effslot_t *s = &m->slot[idx];
    switch (k_keys[idx].kind) {
      case K_SCALAR:
        if (s->set) break; /* first-wins */
        if (n->nargs > 0) { slot_push(s, n->args[0]); s->set = 1; s->origin = n; }
        break;
      case K_LIST:
        if (strcmp(k_keys[idx].name, "localforward") == 0 ||
            strcmp(k_keys[idx].name, "remoteforward") == 0) {
          if (n->nargs > 0) {
            char *norm = normalize_forward(n->args, n->nargs);
            if (norm) { slot_push(s, norm); free(norm); }
          }
        } else {
          for (int a = 0; a < n->nargs; a++) slot_push(s, n->args[a]);
        }
        break;
      case K_LIST_FIRST:
        if (s->set) break; /* 只取首个匹配指令 */
        for (int a = 0; a < n->nargs; a++) slot_push(s, n->args[a]);
        s->set = 1;
        break;
    }
  }

  /* 上下文相关默认 */
  effslot_t *user = &m->slot[key_index("user")];
  if (!user->set) {
    char buf[256];
    if (opts && opts->default_user) {
      snprintf(buf, sizeof buf, "%s", opts->default_user);
    } else {
      struct passwd *pw = getpwuid(getuid());
      snprintf(buf, sizeof buf, "%s", pw ? pw->pw_name : "root");
    }
    slot_push(user, buf);
    user->set = 1;
  }
  effslot_t *hostname = &m->slot[key_index("hostname")];
  if (!hostname->set) { slot_push(hostname, host); hostname->set = 1; }

  effslot_t *ids = &m->slot[key_index("identityfile")];
  if (ids->n == 0) {
    gt_identity_policy_t pol = opts ? opts->identity_policy : GT_IDENTITY_POLICY_OPENSSH_9_6;
    const char *const *chain = NULL;
    if (pol == GT_IDENTITY_POLICY_OPENSSH_9_6) chain = k_id_openssh_9_6;
    else if (pol == GT_IDENTITY_POLICY_OPENSSH_10_3) chain = k_id_openssh_10_3;
    else if (pol == GT_IDENTITY_POLICY_OPENSSH_MASTER) chain = k_id_openssh_master;
    else if (pol == GT_IDENTITY_POLICY_CUSTOM) chain = opts ? opts->custom_identity_files : NULL;
    for (int i = 0; chain && chain[i]; i++) slot_push(ids, chain[i]);
  }

  /* 无默认值的标量键：未设置时不产生输出（ssh -G 亦然，如 proxyjump） */
  for (int i = 0; i < K_COUNT; i++) {
    effslot_t *s = &m->slot[i];
    if (s->n == 0 && k_keys[i].def && k_keys[i].kind == K_SCALAR) {
      slot_push(s, k_keys[i].def);
      s->set = 1;
    }
  }
}

static size_t slot_join(const effslot_t *s, char *buf, size_t cap) {
  size_t need = 0;
  for (size_t i = 0; i < s->n; i++) need += strlen(s->vals[i]) + (i ? 1 : 0);
  if (!buf || cap == 0) return need;
  if (cap <= need) return need; /* 不足则不写半个值 */
  size_t at = 0;
  for (size_t i = 0; i < s->n; i++) {
    if (i) buf[at++] = '\n';
    size_t l = strlen(s->vals[i]);
    memcpy(buf + at, s->vals[i], l);
    at += l;
  }
  buf[at] = '\0';
  return need;
}

int gt_config_effective(const gt_config_t *cfg, const char *host, const char *key,
                        const gt_resolve_opts_t *opts, char *buf, size_t cap) {
  if (!cfg || !host || !key) { gt__set_error("effective: null arg"); return -1; }
  int idx = key_index(key);
  if (idx < 0) {
    gt__set_error("effective: 不支持的键 '%s'（支持集见 gt_config_supported_keys）", key);
    return -1;
  }
  effmap_t m;
  effmap_build(&m, cfg, host, opts);
  size_t need = slot_join(&m.slot[idx], buf, cap);
  effmap_free(&m);
  return (int)need;
}

/* ------------------------------------------------------------------ */
/* typed 视图                                                          */
/* ------------------------------------------------------------------ */

static char **slot_to_array(const effslot_t *s) {
  char **out = gt__alloc(sizeof(char *) * (s->n + 1));
  if (!out) return NULL;
  for (size_t i = 0; i < s->n; i++) out[i] = gt__strdup(s->vals[i]);
  out[s->n] = NULL;
  return out;
}

static char **split_commas(const effslot_t *s) {
  char **out = NULL;
  size_t n = 0, cap = 0;
  for (size_t i = 0; i < s->n; i++) {
    char *copy = gt__strdup(s->vals[i]);
    if (!copy) continue;
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
      out = grow(out, &cap, n + 2, sizeof(char *));
      if (!out) { free(copy); return NULL; }
      out[n++] = gt__strdup(t);
      out[n] = NULL;
    }
    free(copy);
  }
  if (!out) return slot_to_array(s); /* 空 */
  return out;
}

static int flag01(const effmap_t *m, const char *key) {
  const effslot_t *s = &m->slot[key_index(key)];
  if (s->n == 0) return -1;
  return (strcmp(s->vals[0], "yes") == 0 || strcmp(s->vals[0], "true") == 0) ? 1 : 0;
}

static int int_of(const effmap_t *m, const char *key, int fallback) {
  const effslot_t *s = &m->slot[key_index(key)];
  if (s->n == 0) return fallback;
  char *end = NULL;
  long v = strtol(s->vals[0], &end, 10);
  return (end && end != s->vals[0]) ? (int)v : fallback;
}

gt_resolved_t *gt_config_resolve(const gt_config_t *cfg, const char *host,
                                const gt_resolve_opts_t *opts) {
  if (!cfg || !host) { gt__set_error("resolve: null arg"); return NULL; }
  effmap_t m;
  effmap_build(&m, cfg, host, opts);

  gt_resolved_t *r = gt__alloc(sizeof *r);
  if (!r) { effmap_free(&m); return NULL; }
  const effslot_t *u = &m.slot[key_index("user")];
  const effslot_t *h = &m.slot[key_index("hostname")];
  r->user = u->n ? gt__strdup(u->vals[0]) : NULL;
  r->hostname = h->n ? gt__strdup(h->vals[0]) : NULL;
  r->port = int_of(&m, "port", 22);
  r->forward_agent = flag01(&m, "forwardagent");
  r->compression = flag01(&m, "compression");
  r->server_alive_interval = int_of(&m, "serveraliveinterval", 0);
  r->identity_files = slot_to_array(&m.slot[key_index("identityfile")]);
  r->proxy_jump = split_commas(&m.slot[key_index("proxyjump")]);
  r->setenv = slot_to_array(&m.slot[key_index("setenv")]);
  r->local_forward = slot_to_array(&m.slot[key_index("localforward")]);
  effmap_free(&m);
  return r;
}

static void free_strarray(char **v) {
  if (!v) return;
  for (size_t i = 0; v[i]; i++) free(v[i]);
  free(v);
}

void gt_resolved_free(gt_resolved_t *r) {
  if (!r) return;
  free(r->user);
  free(r->hostname);
  free_strarray(r->identity_files);
  free_strarray(r->proxy_jump);
  free_strarray(r->setenv);
  free_strarray(r->local_forward);
  free(r);
}

/* ------------------------------------------------------------------ */
/* 编辑：稀疏补丁（铁律 #4——除目标行外逐字节不变）                     */
/*                                                                     */
/* 语义锚点：core/tests/fixtures/writeback.json（9 用例，真 ssh -G    */
/* 验证来源，勿手改）。实现策略：把节点数组摊平为「行表」，在行表上做  */
/* 替换 / 删除 / 追加，最后按新文本**重新解析**成 gt_config_t——这样   */
/* 块编号、缓冲区等派生状态天然自洽，不必手工改索引。                  */
/* ------------------------------------------------------------------ */

/* 指令行（与 node->raw 一一对应）。 */
typedef struct {
  char *raw;
  char *kw; /* 小写关键字；非指令行/注释为 NULL */
  int block;
} eline_t;

typedef struct {
  eline_t *v;
  size_t n, cap;
} elist_t;

static bool kw_eq(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool line_is_blank(const char *raw) {
  for (const char *p = raw; *p; p++)
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
  return true;
}

static void elist_free(elist_t *l) {
  for (size_t i = 0; i < l->n; i++) {
    free(l->v[i].raw);
    free(l->v[i].kw);
  }
  free(l->v);
}

/* 在 pos 处删除 rm 行、插入 ins（ins 为 NULL 结尾的字符串数组），
 * 新行关键字与块号由参数给定。返回 0 成功。 */
static int elist_splice(elist_t *l, size_t pos, size_t rm,
                        char *const *ins, size_t nins, const char *kwname,
                        int block) {
  if (pos > l->n || pos + rm > l->n) return -1;
  size_t nn = l->n - rm + nins;
  if (nn > l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    while (nc < nn) nc *= 2;
    eline_t *q = gt__realloc(l->v, nc * sizeof *q);
    if (!q) return -1;
    l->v = q;
    l->cap = nc;
  }
  /* 先把被删行的 raw/kw 所有权移出，再 memmove 腾位置。 */
  for (size_t i = 0; i < rm; i++) {
    free(l->v[pos + i].raw);
    free(l->v[pos + i].kw);
  }
  memmove(l->v + pos + nins, l->v + pos + rm,
          (l->n - pos - rm) * sizeof *l->v);
  for (size_t i = 0; i < nins; i++) {
    l->v[pos + i].raw = ins[i];
    l->v[pos + i].kw = kwname ? lower_dup(kwname, strlen(kwname)) : NULL;
    l->v[pos + i].block = block;
  }
  l->n = nn;
  return 0;
}

/* Remove actual matching rows, not a contiguous range sized by match count.
 * Compact once so large blocks with interleaved directives remain linear.
 * The caller owns this editable copy; cfg is not changed until serialization. */
static void elist_remove_matches(elist_t *l, size_t start, size_t end,
                                 const char *keyword) {
  size_t out = start;
  for (size_t k = start; k < l->n; k++) {
    if (k < end && kw_eq(l->v[k].kw, keyword)) {
      free(l->v[k].raw);
      free(l->v[k].kw);
    } else {
      l->v[out++] = l->v[k];
    }
  }
  l->n = out;
}

/* 累积型指令：一行一条目（与 ssh -G 的重复指令语义一致）。 */
static bool render_per_line(const char *kw) {
  static const char *const names[] = {
      "identityfile", "localforward", "remoteforward",
      "dynamicforward", "certificatefile", "sendenv", NULL};
  for (int i = 0; names[i]; i++)
    if (kw_eq(kw, names[i])) return true;
  return false;
}

static size_t vcount(const char *const *v) {
  size_t n = 0;
  if (v)
    while (v[n]) n++;
  return n;
}

static void free_lines(char **lines, size_t n);

/* 把 tokens 以 sep_c 连接成一条值串（NULL = 单值）。 */
static char *join_tokens(const char *const *vals, size_t nv, char sep_c) {
  if (nv == 0) return gt__strdup("");
  size_t need = 1;
  for (size_t i = 0; i < nv; i++) need += strlen(vals[i]) + (i ? 1 : 0);
  char *s = gt__alloc(need);
  if (!s) return NULL;
  size_t at = 0;
  for (size_t i = 0; i < nv; i++) {
    if (i) s[at++] = sep_c;
    size_t l = strlen(vals[i]);
    memcpy(s + at, vals[i], l);
    at += l;
  }
  s[at] = '\0';
  return s;
}

/* 组装渲染行：ins[0]=indent, ins[1]=keyword, ins[2]=sep。 */
static char *mkline(const char *indent, const char *kw, const char *sep,
                    const char *value) {
  size_t n = strlen(indent) + strlen(kw) + strlen(sep) + strlen(value) + 2;
  char *s = gt__alloc(n);
  if (!s) return NULL;
  snprintf(s, n, "%s%s%s%s\n", indent, kw, sep, value);
  return s;
}

/* 从既有行拆出缩进 / 关键字原样拼写 / 分隔符（`=` 或空白，见
 * restyle_keep_equals_separator_bastion）。 */
static void split_line(const char *raw, char **indent, char **kw, char **sep) {
  const char *p = raw;
  const char *is = p;
  while (*p == ' ' || *p == '\t') p++;
  *indent = gt__strndup(is, (size_t)(p - is));
  const char *ks = p;
  while (*p && !isspace((unsigned char)*p) && *p != '=') p++;
  *kw = gt__strndup(ks, (size_t)(p - ks));
  const char *ss = p;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '=') {
    p++;
    while (*p == ' ' || *p == '\t') p++;
  }
  *sep = gt__strndup(ss, (size_t)(p - ss));
}

/* 渲染一条指令为若干行；kwspell 决定关键字原样拼写（保留大小写，
 * 见 restyle_keep_equals_separator_bastion），sep 决定 `=`/空格分隔。
 * 返回 NULL 结尾数组（调用方 free_lines）。values 为空时返回 NULL=NULL 删除。 */
static char **render_edit(const gt_config_edit_t *e, const char *indent,
                          const char *kwspell, const char *sep, size_t *out_n) {
  size_t nv = vcount(e->values);
  *out_n = 0;
  if (nv == 0) return NULL; /* 交给调用方按「删除」处理 */
  char **out;
  size_t n = 0;
  if (render_per_line(e->keyword)) {
    out = gt__alloc(sizeof(char *) * (nv + 1));
    if (!out) return NULL;
    for (size_t i = 0; i < nv; i++) out[n++] = mkline(indent, kwspell, sep, e->values[i]);
  } else {
    char *joined = join_tokens(e->values, nv, ' ');
    if (!joined) return NULL;
    out = gt__alloc(sizeof(char *) * 2);
    if (!out) { free(joined); return NULL; }
    out[n++] = mkline(indent, kwspell, sep, joined);
    free(joined);
  }
  if (!out[n - 1]) { free_lines(out, n); return NULL; }
  out[n] = NULL;
  *out_n = n;
  return out;
}

static void free_lines(char **lines, size_t n) {
  if (!lines) return;
  for (size_t i = 0; i < n; i++) free(lines[i]);
  free(lines);
}

static int find_block_for_alias(const gt_config_t *cfg, const char *alias) {
  for (size_t b = 0; b < cfg->nblocks; b++)
    for (int i = 0; i < cfg->blocks[b].npatterns; i++) {
      const char *p = cfg->blocks[b].patterns[i];
      if (p[0] == '!') continue; /* 否定模式不是别名 */
      if (kw_eq(p, alias)) return (int)b;
    }
  return -1;
}

int gt_config_inspect_host(const gt_config_t *cfg, const char *alias,
                           gt_config_host_info_t *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!cfg || !alias || !*alias || !out) {
    gt__set_error("config_inspect_host: invalid arguments");
    return GT_ERR_INVALID_ARG;
  }
  int block = find_block_for_alias(cfg, alias);
  if (block < 0) {
    gt__set_error("config_inspect_host: no literal Host block");
    return GT_ERR_NOT_FOUND;
  }
  const gt_node_t *header = &cfg->nodes[cfg->blocks[block].header_node];
  out->header = header->raw;
  out->source_path = header->source_path;
  out->source_line = header->source_line;
  out->included = header->included != 0;
  out->pattern_count = (size_t)cfg->blocks[block].npatterns;
  for (size_t b = 0; b < cfg->nblocks; b++) {
    for (int i = 0; i < cfg->blocks[b].npatterns; i++) {
      const char *pattern = cfg->blocks[b].patterns[i];
      if (*pattern != '!' && kw_eq(pattern, alias)) { out->matching_blocks++; break; }
    }
  }
  /* Provenance comes from the evaluator's actual first-wins selection. */
  effmap_t values;
  gt_resolve_opts_t opts = {.default_user = "", .identity_policy = GT_IDENTITY_POLICY_NONE};
  effmap_build(&values, cfg, alias, &opts);
  const char *keys[] = {"hostname", "user", "port"};
  gt_config_field_info_t *fields[] = {&out->hostname, &out->user, &out->port};
  for (size_t f = 0; f < 3; f++) {
    gt_config_field_info_t *field = fields[f];
    const gt_node_t *explicit_node = NULL;
    if (!out->included) for (size_t i = 0; i < cfg->nnodes; i++) {
      const gt_node_t *node = &cfg->nodes[i];
      if (node->block == block && !node->included && node->nargs > 0 && kw_eq(node->keyword, keys[f])) {
        explicit_node = node; field->explicit_value = node->args[0]; break;
      }
    }
    const gt_node_t *origin = values.slot[key_index(keys[f])].origin;
    field->source_path = origin ? origin->source_path : "";
    field->source_header = origin && origin->block >= 0 ? cfg->nodes[cfg->blocks[origin->block].header_node].raw : "";
    field->source_line = origin ? origin->source_line : 0;
    field->source_included = origin && origin->included;
    field->effective_is_explicit = origin && origin == explicit_node;
  }
  effmap_free(&values);
  return GT_OK;
}

int gt_config_identity_at(const gt_config_t *cfg, const char *alias,
                          size_t index, gt_config_identity_info_t *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!cfg || !alias || !*alias || !out) {
    gt__set_error("config_identity_at: invalid arguments");
    return GT_ERR_INVALID_ARG;
  }
  int block = find_block_for_alias(cfg, alias);
  if (block < 0) return GT_ERR_NOT_FOUND;
  /* Validate the entire relevant view first: never publish a plausible prefix
   * when later input needs a conditional evaluator or has invalid arity. */
  for (size_t i = 0; i < cfg->nnodes; i++) {
    const gt_node_t *node = &cfg->nodes[i];
    if (kw_eq(node->keyword, "match")) {
      gt__set_error("config_identity_at: Match is unsupported");
      return GT_ERR_UNSUPPORTED;
    }
    if (!kw_eq(node->keyword, "identityfile")) continue;
    bool applies = node->block < 0 || block_matches(&cfg->blocks[node->block], alias);
    if ((applies || node->block == block) && node->nargs != 1) {
      gt__set_error("config_identity_at: IdentityFile requires one argument");
      return GT_ERR_UNSUPPORTED;
    }
  }
  size_t found = 0;
  for (size_t i = 0; i < cfg->nnodes; i++) {
    const gt_node_t *node = &cfg->nodes[i];
    if (!kw_eq(node->keyword, "identityfile")) continue;
    bool applies = node->block < 0 || block_matches(&cfg->blocks[node->block], alias);
    if (!applies && node->block != block) continue;
    if (found++ != index) continue;
    out->value = node->args[0];
    out->source_path = node->source_path;
    out->source_header = node->block < 0 ? "" : cfg->nodes[cfg->blocks[node->block].header_node].raw;
    out->source_line = node->source_line;
    out->included = node->included != 0;
    out->explicit_value = node->block == block && !node->included;
    out->effective = applies;
    return GT_OK;
  }
  return GT_ERR_NOT_FOUND;
}

static void block_range(const elist_t *l, int b, size_t *start, size_t *end) {
  size_t s = l->n, e = l->n;
  for (size_t i = 0; i < l->n; i++)
    if (l->v[i].block == b) { s = i; break; }
  for (size_t i = s; i < l->n; i++)
    if (l->v[i].block != b) { e = i; break; }
  *start = s;
  *end = e;
}

/* 摊平节点数组为行表。 */
static int elist_from_config(const gt_config_t *cfg, elist_t *l) {
  memset(l, 0, sizeof *l);
  l->v = gt__alloc(sizeof(eline_t) * (cfg->nnodes + 1));
  if (!l->v) return -1;
  l->cap = cfg->nnodes + 1;
  for (size_t i = 0; i < cfg->nnodes; i++) {
    const gt_node_t *nd = &cfg->nodes[i];
    if (nd->included) continue; /* 目前无 Include，保险起见仍跳过 */
    l->v[l->n].raw = gt__strdup(nd->raw);
    l->v[l->n].kw = nd->keyword ? gt__strdup(nd->keyword) : NULL;
    l->v[l->n].block = nd->block;
    l->n++;
  }
  return 0;
}

static char *elist_join(const elist_t *l) {
  size_t total = 0;
  for (size_t i = 0; i < l->n; i++) total += strlen(l->v[i].raw);
  char *out = gt__alloc(total + 1);
  if (!out) return NULL;
  size_t at = 0;
  for (size_t i = 0; i < l->n; i++) {
    size_t sl = strlen(l->v[i].raw);
    memcpy(out + at, l->v[i].raw, sl);
    at += sl;
  }
  out[at] = '\0';
  return out;
}

/* 用新文本替换 cfg 内容（重新解析，派生状态自洽）。 */
static int config_reparse(gt_config_t *cfg, const char *text) {
  for (size_t i = 0; i < cfg->nnodes; i++) {
    gt_node_t *n = &cfg->nodes[i];
    free(n->raw);
    free(n->keyword);
    free(n->source_path);
    if (n->args) {
      for (int a = 0; a < n->nargs; a++) free(n->args[a]);
      free(n->args);
    }
  }
  free(cfg->nodes);
  free(cfg->blocks);
  cfg->nodes = NULL;
  cfg->blocks = NULL;
  cfg->nnodes = cfg->cap = cfg->nblocks = cfg->bcap = 0;
  parse_text(cfg, text, strlen(text), 0);
  return 0;
}

int gt_config_edit(gt_config_t *cfg, const char *alias,
                   const gt_config_edit_t *edits, size_t nedits, bool reset) {
  if (!cfg || !alias || !*alias) {
    gt__set_error("config_edit: cfg/alias 不能为空");
    return GT_ERR_INVALID_ARG;
  }
  elist_t l;
  if (elist_from_config(cfg, &l) != 0) {
    gt__set_error("config_edit: out of memory");
    return GT_ERR_INTERNAL;
  }
  int b = find_block_for_alias(cfg, alias);
  if (b >= 0) {
    gt_config_host_info_t info;
    if (gt_config_inspect_host(cfg, alias, &info) != GT_OK || info.included || info.matching_blocks != 1) {
      elist_free(&l);
      gt__set_error("config_edit: alias belongs to an included file or multiple Host blocks");
      return GT_ERR_UNSUPPORTED;
    }
  }
  if (b < 0 && !reset) {
    elist_free(&l);
    gt__set_error("config_edit: 找不到 Host 块 '%s'", alias);
    return GT_ERR_NOT_FOUND;
  }

  int rc = GT_OK;
  if (reset) {
    /* 删除整块（含其尾随空行——空行归属前一块），再追加新块。 */
    if (b >= 0) {
      size_t s, e;
      block_range(&l, b, &s, &e);
      if (elist_splice(&l, s, e - s, NULL, 0, NULL, -1) != 0) rc = GT_ERR_INTERNAL;
    }
    if (rc == GT_OK) {
      /* 组装新块；先补「确保一个空行分隔」的换行。 */
      size_t need = 1;
      for (size_t i = 0; i < nedits; i++) need += vcount(edits[i].values) + 1;
      char **ins = gt__alloc(sizeof(char *) * (need + 2));
      size_t ni = 0;
      bool ok = ins != NULL;
      /* 顺序：[空行分隔, Host 头, 各指令行]。 */
      if (ok) {
        char *blank = gt__strdup("\n");
        char *host = blank ? mkline("", "Host", " ", alias) : NULL;
        if (!blank || !host) { free(blank); free(host); ok = false; }
        else { ins[ni++] = blank; ins[ni++] = host; }
      }
      for (size_t i = 0; ok && i < nedits; i++) {
        size_t rn = 0;
        char **rl = render_edit(&edits[i], "  ", edits[i].keyword, " ", &rn);
        if (!rl) continue; /* 删除指令：新块里无事可做 */
        for (size_t k = 0; k < rn; k++) ins[ni++] = rl[k];
        free(rl);
      }
      if (ok) {
        if (elist_splice(&l, l.n, 0, ins, ni, NULL, -1) != 0) {
          free_lines(ins, ni);
          rc = GT_ERR_INTERNAL;
        } else {
          free(ins); /* 字符串归 l */
        }
      } else {
        free_lines(ins, ni);
        rc = GT_ERR_INTERNAL;
      }
    }
  } else {
    for (size_t i = 0; i < nedits && rc == GT_OK; i++) {
      const gt_config_edit_t *e = &edits[i];
      size_t s, e2;
      block_range(&l, b, &s, &e2);
      /* 收集匹配行。 */
      size_t first = (size_t)-1, nfound = 0;
      for (size_t k = s; k < e2; k++)
        if (kw_eq(l.v[k].kw, e->keyword)) {
          if (first == (size_t)-1) first = k;
          nfound++;
        }
      size_t nv = vcount(e->values);
      if (nv == 0) {
        /* 仅删除匹配行，保留交错的其他指令和注释。 */
        if (nfound)
          elist_remove_matches(&l, first, e2, e->keyword);
        continue;
      }
      /* 缩进 / 关键字原样拼写 / 分隔符：沿用首个既有行，否则用默认。 */
      char *indent = NULL, *kwspell = NULL, *sep = NULL;
      if (nfound) split_line(l.v[first].raw, &indent, &kwspell, &sep);
      else {
        indent = gt__strdup("  ");
        kwspell = gt__strdup(e->keyword);
        sep = gt__strdup(" ");
      }
      size_t rn = 0;
      char **rl = render_edit(e, indent, kwspell ? kwspell : e->keyword, sep, &rn);
      free(indent);
      free(kwspell);
      free(sep);
      if (!rl) { rc = GT_ERR_INTERNAL; break; }

      if (nfound) {
        /* 首条原位替换，其他同关键字行按实际位置移除。
         * first 前没有移动；块号、未知指令、空白与其他块保持原样。 */
        int blk = l.v[first].block;
        elist_remove_matches(&l, first + 1, e2, e->keyword);
        if (elist_splice(&l, first, 1, rl, rn, e->keyword, blk) != 0) {
          free_lines(rl, rn); /* splice 失败：未接管所有权 */
          rc = GT_ERR_INTERNAL;
        } else {
          free(rl); /* 字符串已归 l，仅释放容器 */
        }
      } else {
        /* 追加到块尾（尾随空行之前）。 */
        size_t pos = e2;
        while (pos > s && line_is_blank(l.v[pos - 1].raw)) pos--;
        int blk = (s < l.n) ? l.v[s].block : -1;
        if (elist_splice(&l, pos, 0, rl, rn, e->keyword, blk) != 0) {
          free_lines(rl, rn);
          rc = GT_ERR_INTERNAL;
        } else {
          free(rl);
        }
      }
    }
  }

  if (rc == GT_OK) {
    char *text = elist_join(&l);
    if (!text) rc = GT_ERR_INTERNAL;
    else {
      config_reparse(cfg, text);
      free(text);
    }
  }
  elist_free(&l);
  if (rc != GT_OK) gt__set_error("config_edit: 失败（rc=%d）", rc);
  return rc;
}

/* New names deliberately use the same conservative literal subset as the GUI. */
static bool rename_name_valid(const char *name) {
  if (!name || !*name || strlen(name) > 128) return false;
  for (size_t i = 0; name[i]; i++) {
    unsigned char c = (unsigned char)name[i];
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && (i == 0 || (c != '.' && c != '-' && c != '_'))) return false;
  }
  return true;
}

int gt_config_rename_host(gt_config_t *cfg, const char *alias, const char *new_alias) {
  if (!cfg || !alias || !*alias || strpbrk(alias, "*?![]") || !rename_name_valid(new_alias)) {
    gt__set_error("config_rename_host: invalid literal alias");
    return GT_ERR_INVALID_ARG;
  }
  gt_config_host_info_t ownership;
  int rc = gt_config_inspect_host(cfg, alias, &ownership);
  if (rc != GT_OK) return rc;
  if (ownership.included || ownership.matching_blocks != 1) {
    gt__set_error("config_rename_host: included or ambiguous Host block");
    return GT_ERR_UNSUPPORTED;
  }
  int block = find_block_for_alias(cfg, alias), token = -1, matches = 0;
  const gt_node_t *header = &cfg->nodes[cfg->blocks[block].header_node];
  for (int i = 0; i < header->nargs; i++) {
    if (kw_eq(header->args[i], alias)) { token = i; matches++; }
  }
  if (matches != 1) {
    gt__set_error("config_rename_host: repeated alias within Host header");
    return GT_ERR_UNSUPPORTED;
  }
  for (size_t b = 0; b < cfg->nblocks; b++) {
    for (int i = 0; i < cfg->blocks[b].npatterns; i++) {
      if ((int)b == block && i == token) continue;
      if (kw_eq(cfg->blocks[b].patterns[i], new_alias)) {
        gt__set_error("config_rename_host: target alias already exists");
        return GT_ERR_INVALID_ARG;
      }
    }
  }
  if (strcmp(header->args[token], new_alias) == 0) return GT_OK;
  const char *q = header->raw, *end = q + strlen(q);
  while (q < end && (*q == ' ' || *q == '\t')) q++;
  while (q < end && !isspace((unsigned char)*q) && *q != '=') q++;
  if (q < end && *q == '=') q++;
  token_range_t range = {.index = token};
  char **args = NULL; int nargs = 0;
  if (tokenize(q, end, &args, &nargs, &range) != 0) return GT_ERR_INTERNAL;
  for (int i = 0; i < nargs; i++) free(args[i]);
  free(args);
  if (!range.start || !range.end) return GT_ERR_INTERNAL;
  size_t start = (size_t)(range.start - header->raw);
  for (size_t i = 0; i < cfg->blocks[block].header_node; i++)
    if (!cfg->nodes[i].included) start += strlen(cfg->nodes[i].raw);
  size_t removed = (size_t)(range.end - range.start);
  char *original = gt_config_to_string(cfg);
  if (!original) return GT_ERR_INTERNAL;
  size_t length = strlen(original), added = strlen(new_alias);
  char *text = gt__alloc(length - removed + added + 1);
  if (!text) { free(original); return GT_ERR_INTERNAL; }
  memcpy(text, original, start);
  memcpy(text + start, new_alias, added);
  memcpy(text + start + added, original + start + removed, length - start - removed + 1);
  free(original);

  /* Publish only after reparsing succeeds; missing Includes leave cfg intact. */
  gt_config_t *candidate = gt__alloc(sizeof *candidate);
  if (!candidate) { free(text); return GT_ERR_INTERNAL; }
  if (cfg->source_path) {
    candidate->source_path = gt__strdup(cfg->source_path);
    if (!candidate->source_path) { free(text); gt_config_free(candidate); return GT_ERR_INTERNAL; }
  }
  parse_text(candidate, text, strlen(text), 0);
  if (candidate->include_error) { free(text); gt_config_free(candidate); return GT_ERR_IO; }
  char *roundtrip = gt_config_to_string(candidate);
  bool complete = roundtrip && strcmp(roundtrip, text) == 0;
  free(roundtrip); free(text);
  if (!complete) { gt_config_free(candidate); return GT_ERR_INTERNAL; }
  gt_config_t old = *cfg;
  *cfg = *candidate;
  *candidate = old;
  gt_config_free(candidate);
  return GT_OK;
}
