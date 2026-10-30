/*
 * ssh.c — SSH 会话（libssh2 封装）+ 交互式 shell 通道。
 *
 * 线程模型（铁律 #1/#2）：一个 gt_ssh_t 绑一个会话线程；libssh2 是阻塞
 * API，绝不放 UI 线程。connect/auth/handshake 在调用线程阻塞完成；shell
 * 通道的 read/write/resize 由会话线程独占调用。
 *
 * 三态（铁律 #6）：主机密钥 UNKNOWN 绝不折叠成 REJECT。核对顺序：
 *   1) known_hosts 命中且不一致 → 直接 REJECT（密钥已变更，不可自动接受）；
 *   2) 命中且一致 → 直接 ACCEPT（已知主机无需再裁决，decider 不被调用）；
 *   3) 未知（无记录/解析不了）→ 有 decider 则由上层裁决（TOFU 提示），
 *      ACCEPT 且原本未知 → 落盘；无 decider → UNKNOWN（返回 GT_ERR_HOST_KEY）。
 *
 * 未编译进 libssh2 时（GT_HAVE_LIBSSH2 未定义）所有入口返回
 * GT_ERR_UNSUPPORTED——能力缺失显式报错，绝不静默降级。
 */
#include "ghosterm.h"
#include "internal.h"
#include "tmux/command.h"
#include "ssh/connector.h"
#include "base/secure-random.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 私钥是否需要口令（只看文件头，不解密；与 libssh2 无关的纯文本判断）  */
/* ------------------------------------------------------------------ */

static int b64_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1; /* 换行、空白、'=' 都按「跳过」处理——前缀解码够用 */
}

/* 解出 base64 文本的前 cap 个字节；返回实际解出的字节数。 */
static size_t b64_prefix(const char *s, unsigned char *out, size_t cap) {
  unsigned acc = 0;
  int bits = 0;
  size_t n = 0;
  for (; *s && n < cap; s++) {
    int v = b64_val((unsigned char)*s);
    if (v < 0) continue;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[n++] = (unsigned char)((acc >> bits) & 0xffu);
    }
  }
  return n;
}

int gt_ssh_key_needs_passphrase(const char *pem) {
  if (!pem) return -1;

  /* PKCS#8 加密：头一行就写着。 */
  if (strstr(pem, "-----BEGIN ENCRYPTED PRIVATE KEY-----")) return 1;

  /* 传统 PEM（PKCS#1/EC）加密：Proc-Type: 4,ENCRYPTED（DEK-Info 必随行）。 */
  if (strstr(pem, "Proc-Type: 4,ENCRYPTED")) return 1;
  if (strstr(pem, "DEK-Info:")) return 1;

  /* OpenSSH 新格式：正文第一个字段是 ciphername，"none" = 没加密。
   * 布局： "openssh-key-v1\0" + uint32(长度) + ciphername + ... */
  const char *begin = strstr(pem, "-----BEGIN OPENSSH PRIVATE KEY-----");
  if (begin) {
    static const char magic[] = "openssh-key-v1";
    unsigned char buf[64];
    const char *body = begin + strlen("-----BEGIN OPENSSH PRIVATE KEY-----");
    size_t n = b64_prefix(body, buf, sizeof buf);
    if (n < 19) return -1;                       /* 头都读不全 → 认不出来 */
    if (memcmp(buf, magic, sizeof magic - 1) != 0) return -1;
    unsigned len = ((unsigned)buf[15] << 24) | ((unsigned)buf[16] << 16) |
                   ((unsigned)buf[17] << 8) | (unsigned)buf[18];
    if (len == 0 || 19 + len > n) return -1;
    if (len == 4 && memcmp(buf + 19, "none", 4) == 0) return 0;
    return 1;                                    /* aes256-ctr / chacha20… */
  }

  /* 其余 PEM 私钥（RSA/EC/PKCS#8 明文）→ 不需要口令。 */
  if (strstr(pem, "PRIVATE KEY-----")) return 0;

  return -1; /* 认不出来：不是 libssh2 能用的私钥文件，别据此弹口令框 */
}

#ifdef GT_HAVE_LIBSSH2
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <libssh2.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct gt_ssh {
  /* 自有拷贝：opts 里的指针来自调用方，可能在 new 返回后失效（Swift 传
   * 临时 C 串），故此处全部 strdup 拥有。 */
  char *host;
  char *username;
  uint16_t port;
  gt_hostkey_decider_fn decide;
  void *decide_ud;
  char *password;
  char *pem;
  char *passphrase;
  char *kh_path; /* 解析后的 known_hosts 路径（拥有） */
  int connect_timeout_ms; /* TCP + 握手预算 */
  int auth_methods;       /* 服务器允许的认证方式（握手后才有值） */

  int sock;
  LIBSSH2_SESSION *session;
  bool connected,handshake_verified;
  pthread_mutex_t io_lock; /* 每会话 I/O 锁（见 internal.h 说明） */
  bool io_lock_init;
};

/* ------------------------------------------------------------------ */
/* 工具：base64（指纹）                                                 */
/* ------------------------------------------------------------------ */

static char *b64_encode(const unsigned char *in, size_t n) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t olen = ((n + 2) / 3) * 4;
  char *out = gt__alloc(olen + 1);
  if (!out) return NULL;
  size_t i = 0, o = 0;
  while (i + 2 < n) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = tbl[(v >> 6) & 63];
    out[o++] = tbl[v & 63];
    i += 3;
  }
  if (i < n) {
    unsigned v = in[i] << 16;
    if (i + 1 < n) v |= in[i + 1] << 8;
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
    out[o++] = '=';
  }
  out[o] = '\0';
  return out;
}

static char *fingerprint_base64(const unsigned char *hash) {
  char *text=b64_encode(hash,32);if(text){size_t n=strlen(text);while(n && text[n-1]=='=')text[--n]=0;}return text;
}

static const char *hostkey_type_name(int t) {
  switch (t) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_DSS: return "ssh-dss";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ssh-ed25519";
    default: return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/* known_hosts：明文模式、撤销优先；hashed/未支持规则显式拒绝。          */
/* ------------------------------------------------------------------ */

typedef enum { KH_MATCH, KH_MISMATCH, KH_UNKNOWN, KH_REVOKED, KH_UNSUPPORTED, KH_IO } kh_result_t;

/*
 * 逐行枚举 known_hosts 的公钥条目（供读侧与对拍闸门共用）。
 *
 * **没有「挑出公钥条目」的过滤**：早先按 `ssh` 前缀过滤是照抄 OpenSSH 对
 * ssh-rsa 的写法，而 ECDSA 的 key type 是 `ecdsa-sha2-nistp256`——根本不以
 * `ssh` 开头，于是 ECDSA 条目被整条跳过。真机上的 SSH 服务器常给 ECDSA，
 * 症状就是「记过的主机每次重连都再问一遍用户」（本机 e2e 用 ed25519，
 * 所以一直是绿的）。
 *
 * 判断一条是不是公钥条目，靠**字段形态**而不是前缀：
 *   `[marker] hosts keytype base64 [comment]`，marker 是 @cert-authority /
 *   @revoked；公钥条目的第 2 个字段是密钥算法名、第 3 个是 base64。
 * 前两个字段里都不会出现 '/'（base64 里有），也不会有 '='。
 */
gt_status_t gt__kh_visit(const char *path, gt__kh_visit_fn cb, void *ud) {
  if (!path || !cb) return GT_ERR_INVALID_ARG;
  FILE *f = fopen(path, "rb");
  if (!f) return errno == ENOENT ? GT_OK : GT_ERR_IO;
  char *line = NULL;
  size_t cap = 0;
  ssize_t got;
  while ((got = getline(&line, &cap, f)) > 0) {
    gt_known_host_entry_t entry;
    if (!gt__kh_split_line(line, &entry) || !entry.recognized) continue;
    if (!cb(ud, entry.hosts, entry.key_type, entry.key_base64, entry.marker)) break;
  }
  bool failed = ferror(f) != 0 || (got < 0 && !feof(f));
  free(line);
  if (fclose(f) != 0) failed = true;
  return failed ? GT_ERR_IO : GT_OK;
}

typedef struct {
  const char *want;    /* 期望的 host 字段（含端口时的 [host]:port 形态） */
  const char *host;    /* 裸主机名（OpenSSH 允许裸名条目匹配任意端口） */
  const char *key_type;
  const char *key_b64;
  bool saw_host;       /* 主机命中过（用于区分「没见过」与「密钥不同」） */
  bool matched;        /* 主机命中且密钥一致 */
  bool revoked, authority, unsupported;
} kh_lookup_t;

/* OpenSSH host patterns use only * and ?, not fnmatch bracket classes.
 * Work on spans so long comma-separated host lists are never truncated. */
static bool kh_pattern(const char *pattern, size_t n, const char *host) {
  size_t p=0, h=0, star=SIZE_MAX, retry=0;
  while (host[h]) {
    if (p<n && (pattern[p]=='?' || tolower((unsigned char)pattern[p])==tolower((unsigned char)host[h]))) { p++; h++; }
    else if (p<n && pattern[p]=='*') { star=p++; retry=h; }
    else if (star!=SIZE_MAX) { p=star+1; h=++retry; }
    else return false;
  }
  while (p<n && pattern[p]=='*') p++;
  return p==n;
}
static bool kh_host_matches(const char *hosts, const kh_lookup_t *q) {
  bool positive=false;
  for (const char *p=hosts; *p;) {
    const char *end=strchr(p,','); size_t n=end?(size_t)(end-p):strlen(p);
    bool negated=n && *p=='!'; const char *pattern=p+negated; n-=negated;
    bool match=kh_pattern(pattern,n,q->want) || kh_pattern(pattern,n,q->host);
    if (match && negated) return false;
    if (match) positive=true;
    if (!end) break;
    p=end+1;
  }
  return positive;
}
static bool kh_lookup_cb(void *ud, const char *hosts, const char *key_type,
                         const char *key_b64, const char *marker) {
  kh_lookup_t *q = ud;
  /* Hash matching is not implemented. We cannot prove this is unrelated;
   * especially a hashed revocation must not be bypassed by ordinary trust. */
  if (hosts[0]=='|') { q->unsupported=true; return true; }
  if (!kh_host_matches(hosts,q)) return true;
  bool same=strcmp(key_type,q->key_type)==0 && strcmp(key_b64,q->key_b64)==0;
  if (!strcmp(marker,"@revoked")) { if (same) q->revoked=true; return true; }
  if (!strcmp(marker,"@cert-authority")) { q->authority=true; return true; }
  if (*marker) { q->unsupported=true; return true; }
  q->saw_host=true;
  if (same) q->matched=true;
  return true; /* A later revocation always overrides ordinary trust. */
}

static kh_result_t kh_decision(kh_lookup_t q) {
  if (q.revoked) return KH_REVOKED;
  if (q.unsupported) return KH_UNSUPPORTED;
  if (q.matched) return KH_MATCH;
  if (q.authority) return KH_UNSUPPORTED;
  if (q.saw_host) return KH_MISMATCH;
  return KH_UNKNOWN;
}
static kh_result_t kh_check(const char *path, const char *host, uint16_t port,
                            const char *key_type, const char *key_b64) {
  if (!path) return KH_UNKNOWN;
  char want[512];
  if (port == 22) snprintf(want, sizeof want, "%s", host);
  else snprintf(want, sizeof want, "[%s]:%u", host, (unsigned)port);
  kh_lookup_t q = {.want = want, .host = host, .key_type = key_type, .key_b64 = key_b64};
  if (gt__kh_visit(path, kh_lookup_cb, &q) != GT_OK) return KH_IO;
  return kh_decision(q);
}

typedef struct { kh_lookup_t lookup; kh_result_t observed; } kh_append_context;
static gt_status_t kh_prepare_append(const char *bytes, size_t length, char **candidate,
                                     size_t *size, void *context) {
  kh_append_context *c = context;
  gt_known_hosts_t *snapshot = gt_known_hosts_parse(bytes, length);
  if (!snapshot) return GT_ERR_IO;
  size_t records = gt_known_hosts_count(snapshot);
  for (size_t i = 0; i < records; i++) {
    gt_known_host_entry_t entry;
    if (gt_known_hosts_entry_at(snapshot, i, &entry) != GT_OK) {
      gt_known_hosts_free(snapshot); return GT_ERR_IO;
    }
    if (entry.recognized) kh_lookup_cb(&c->lookup, entry.hosts, entry.key_type, entry.key_base64, entry.marker);
  }
  gt_known_hosts_free(snapshot);
  c->observed = kh_decision(c->lookup);
  if (c->observed == KH_MATCH) return GT_OK; /* Concurrent identical consent: no duplicate. */
  if (c->observed != KH_UNKNOWN) {
    gt__set_error("known_hosts changed while awaiting consent: %s",
        c->observed == KH_REVOKED ? "revoked" : c->observed == KH_MISMATCH ? "key mismatch" : "unsupported trust rule");
    return c->observed == KH_UNSUPPORTED ? GT_ERR_UNSUPPORTED : GT_ERR_HOST_KEY;
  }
  if (records >= 4096) {
    gt__set_error("known_hosts append exceeds record limit"); return GT_ERR_IO;
  }
  const kh_lookup_t *q = &c->lookup;
  size_t record_size = strlen(q->want) + strlen(q->key_type) + strlen(q->key_b64) + 4;
  bool newline = length && bytes[length - 1] != '\n';
  if (length + newline >= 1024 * 1024 || record_size > 1024 * 1024 - (length + newline)) {
    gt__set_error("known_hosts append exceeds size limit"); return GT_ERR_IO;
  }
  *candidate = gt__alloc(length + newline + record_size);
  if (!*candidate) return GT_ERR_INTERNAL;
  memcpy(*candidate, bytes, length);
  if (newline) (*candidate)[length] = '\n';
  int n = snprintf(*candidate + length + newline, record_size, "%s %s %s\n", q->want, q->key_type, q->key_b64);
  *size = length + newline + (size_t)n;
  return GT_OK;
}
static int kh_append(const char *path, const char *host, uint16_t port,
                     const char *key_type, const char *key_b64, kh_result_t *observed) {
  *observed = KH_UNKNOWN;
  /* Public host strings must not inject a second record or wildcard identity. */
  if (!path || path[0] != '/' || !*host || strpbrk(host, " \t\r\n,*!?|@") ||
      strpbrk(key_type, " \t\r\n") || strpbrk(key_b64, " \t\r\n")) {
    gt__set_error("known_hosts requires an absolute path and a literal host identity");
    return GT_ERR_INVALID_ARG;
  }
  size_t capacity = strlen(host) + 16;
  char *want = gt__alloc(capacity);
  if (!want) return GT_ERR_INTERNAL;
  if (port == 22) snprintf(want, capacity, "%s", host);
  else snprintf(want, capacity, "[%s]:%u", host, (unsigned)port);
  kh_append_context context = {.lookup = {.want = want, .host = host,
      .key_type = key_type, .key_b64 = key_b64}, .observed = KH_UNKNOWN};
  gt_known_hosts_update_result_t result;
  gt_status_t status = gt__kh_transaction(path, NULL, 0, kh_prepare_append, &context, &result);
  *observed = context.observed;
  free(want);
  return status;
}

/* ------------------------------------------------------------------ */
/* TCP 连接                                                            */
/* ------------------------------------------------------------------ */

/*
 * TCP 连接，带超时。
 *
 * 为什么必须带（实测）：阻塞的 `connect()` 在防火墙丢包时**不返回**——
 * 用户点连接之后界面就一直「连接中…」，既没有失败也没有超时，只能强退。
 * 这里用非阻塞 connect + poll，超时按 opts.connect_timeout_ms。
 */
static int tcp_connect(const char *host, uint16_t port, int timeout_ms) {
  char pstr[16];
  snprintf(pstr, sizeof pstr, "%u", (unsigned)port);
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  if (getaddrinfo(host, pstr, &hints, &res) != 0) {
    gt__set_error("ssh: 无法解析主机 %s", host);
    return -1;
  }
  if (timeout_ms <= 0) timeout_ms = 15000;

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
      if (flags >= 0) fcntl(fd, F_SETFL, flags);
      break; /* 立刻连通（环回多半如此） */
    }
    if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }

    struct pollfd p = {.fd = fd, .events = POLLOUT};
    rc = poll(&p, 1, timeout_ms);
    if (rc <= 0) {
      gt__set_error(rc == 0 ? "ssh: 连接 %s:%u 超时（%d ms）"
                            : "ssh: 连接 %s:%u 等待可写失败",
                    host, (unsigned)port, timeout_ms);
      close(fd);
      fd = -1;
      /* 超时不必再试别的地址（多半是同一条路） */
      if (rc == 0) break;
      continue;
    }
    int err = 0;
    socklen_t elen = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
      close(fd);
      fd = -1;
      continue;
    }
    if (flags >= 0) fcntl(fd, F_SETFL, flags);
    break;
  }
  freeaddrinfo(res);
  if (fd < 0 && gt_last_error()[0] == '\0')
    gt__set_error("ssh: 连接 %s:%u 失败: %s", host, (unsigned)port,
                  strerror(errno));
  return fd;
}

/* ------------------------------------------------------------------ */
/* 对外 API                                                            */
/* ------------------------------------------------------------------ */

gt_ssh_t *gt_ssh_new(const gt_ssh_opts_t *opts) {
  if (!opts || !opts->host || !opts->username) {
    gt__set_error("gt_ssh_new: host/username required");
    return NULL;
  }
  gt_ssh_t *s = gt__alloc(sizeof *s);
  if (!s) return NULL;
  s->host = gt__strdup(opts->host);
  s->username = gt__strdup(opts->username);
  s->port = opts->port ? opts->port : 22;
  s->connect_timeout_ms = opts->connect_timeout_ms;
  s->sock = -1;
  if (pthread_mutex_init(&s->io_lock, NULL) == 0) s->io_lock_init = true;
  if (opts->known_hosts_path) {
    s->kh_path = gt__strdup(opts->known_hosts_path);
  } else {
    const char *home = getenv("HOME");
    if (home && *home) {
      size_t n = strlen(home) + sizeof "/.ssh/known_hosts";
      s->kh_path = gt__alloc(n);
      if (s->kh_path) snprintf(s->kh_path, n, "%s/.ssh/known_hosts", home);
    }
  }
  return s;
}

int gt_ssh_auth_methods(gt_ssh_t *ssh) {
  return ssh ? ssh->auth_methods : 0;
}

static const char *auth_methods_str(int m) {
  static _Thread_local char buf[128];
  buf[0] = '\0';
  if (m & GT_SSH_AUTH_PUBLICKEY) strncat(buf, "publickey ", 11);
  if (m & GT_SSH_AUTH_PASSWORD) strncat(buf, "password ", 10);
  if (m & GT_SSH_AUTH_KEYBOARD) strncat(buf, "keyboard-interactive ", 22);
  if (m & GT_SSH_AUTH_HOSTBASED) strncat(buf, "hostbased ", 11);
  size_t n = strlen(buf);
  if (n) buf[n - 1] = '\0';
  return buf;
}

static void secret_free(char *value){if(value){gt__secure_erase(value,strlen(value));free(value);}}

int gt_ssh_auth_password(gt_ssh_t *ssh, const char *password) {
  if (!ssh) return GT_ERR_INVALID_ARG;
  secret_free(ssh->password);
  ssh->password = gt__strdup(password);
  return GT_OK;
}

int gt_ssh_auth_private_key(gt_ssh_t *ssh, const char *pem, const char *passphrase) {
  if (!ssh) return GT_ERR_INVALID_ARG;
  secret_free(ssh->pem);
  secret_free(ssh->passphrase);
  ssh->pem = gt__strdup(pem);
  ssh->passphrase = passphrase ? gt__strdup(passphrase) : NULL;
  return GT_OK;
}

int gt_ssh_set_hostkey_policy(gt_ssh_t *ssh, gt_hostkey_decider_fn decide, void *ud) {
  if (!ssh) return GT_ERR_INVALID_ARG;
  ssh->decide = decide;
  ssh->decide_ud = ud;
  return GT_OK;
}

/*
 * 握手：TCP + 密钥交换 + 主机密钥裁决 + 认证方式发现。幂等。
 *
 * 与认证拆开的理由见 ssh.h：上层要知道**服务器允许什么**才能决定问什么
 * 凭据。先前是「设凭据 → connect（内含握手+认证）」一步走，方法表在
 * 认证前恒为空，于是「服务器只收密码」永远判不出来，只能一律猜成
 * 「问私钥口令」——用户看到的就是「明明一把没口令的钥匙，却让我输口令」。
 */
int gt_ssh_handshake(gt_ssh_t *ssh) {
  if (!ssh) return GT_ERR_INVALID_ARG;
  if (ssh->session) return ssh->handshake_verified?GT_OK:GT_ERR_PROTOCOL; /* 失败握手不能冒充已核验 */
  uint16_t port = ssh->port;

  int fd = tcp_connect(ssh->host, port, ssh->connect_timeout_ms);
  if (fd < 0) return GT_ERR_IO;
  ssh->sock = fd;

  LIBSSH2_SESSION *sess = libssh2_session_init();
  if (!sess) {
    gt__set_error("ssh: libssh2_session_init 失败");
    return GT_ERR_INTERNAL;
  }
  ssh->session = sess;
  libssh2_session_set_blocking(sess, 1);

  if (libssh2_session_handshake(sess, fd) != 0) {
    char *msg = NULL;
    libssh2_session_last_error(sess, &msg, NULL, 0);
    gt__set_error("ssh: 握手失败: %s", msg ? msg : "unknown");
    return GT_ERR_PROTOCOL;
  }

  /* ---- 主机密钥（三态） ---- */
  size_t hklen = 0;
  int hkt = 0;
  const char *hk = libssh2_session_hostkey(sess, &hklen, &hkt);
  const unsigned char *raw_fp =
      (const unsigned char *)libssh2_hostkey_hash(sess, LIBSSH2_HOSTKEY_HASH_SHA256);
  char *b64 = NULL, *fingerprint = NULL;
  if (hk && raw_fp) {
    b64 = b64_encode((const unsigned char *)hk, hklen);
    char *fp = fingerprint_base64(raw_fp);
    if (fp) {
      size_t n = strlen(fp) + 8;
      fingerprint = gt__alloc(n);
      if (fingerprint) snprintf(fingerprint, n, "SHA256:%s", fp);
      free(fp);
    }
  }
  const char *ktype = hostkey_type_name(hkt);
  kh_result_t kh = kh_check(ssh->kh_path, ssh->host, port, ktype,
                            b64 ? b64 : "");

  if (kh == KH_REVOKED || kh == KH_UNSUPPORTED || kh == KH_IO) {
    gt__set_error("ssh: %s", kh == KH_REVOKED ? "主机密钥已撤销，连接已拒绝" :
                  kh == KH_UNSUPPORTED ? "known_hosts 包含尚不支持的信任规则" : "known_hosts 读取失败");
    free(b64); free(fingerprint);
    return kh == KH_REVOKED ? GT_ERR_HOST_KEY : kh == KH_UNSUPPORTED ? GT_ERR_UNSUPPORTED : GT_ERR_IO;
  }
  int decision;
  if (kh == KH_MISMATCH) {
    decision = GT_HOSTKEY_REJECT; /* 密钥已变更：绝不自动接受 */
  } else if (kh == KH_MATCH) {
    /* 已知且一致：无需上层裁决。UI 的确认对话框只对 UNKNOWN 弹出——
     * 「见过并接受过」的主机再弹一遍，等于把 TOFU 落盘当成一次性动作。 */
    decision = GT_HOSTKEY_ACCEPT;
  } else if (ssh->decide) {
    decision = ssh->decide(ssh->decide_ud, ssh->host, port, ktype,
                           fingerprint ? fingerprint : "");
    if (decision == GT_HOSTKEY_ACCEPT && kh == KH_UNKNOWN && b64) {
      int saved = kh_append(ssh->kh_path, ssh->host, port, ktype, b64, &kh);
      if (saved != GT_OK) { free(b64); free(fingerprint); return saved; }
    }
  } else {
    decision = GT_HOSTKEY_UNKNOWN;
  }
  if (decision != GT_HOSTKEY_ACCEPT) {
    gt__set_error("ssh: 主机密钥未接受（%s，%s）",
                  decision == GT_HOSTKEY_REJECT ? "reject" : "unknown",
                  fingerprint ? fingerprint : "no fingerprint");
    free(b64);
    free(fingerprint);
    return GT_ERR_HOST_KEY;
  }
  free(b64);
  free(fingerprint);

  /* ---- 服务器允许的认证方式（握手后才有值） ---- */
  ssh->auth_methods = 0;
  {
    char *m = libssh2_userauth_list(sess, ssh->username,
                                    (unsigned)strlen(ssh->username));
    if (m) {
      if (strstr(m, "publickey")) ssh->auth_methods |= GT_SSH_AUTH_PUBLICKEY;
      if (strstr(m, "password")) ssh->auth_methods |= GT_SSH_AUTH_PASSWORD;
      if (strstr(m, "keyboard-interactive"))
        ssh->auth_methods |= GT_SSH_AUTH_KEYBOARD;
      if (strstr(m, "hostbased")) ssh->auth_methods |= GT_SSH_AUTH_HOSTBASED;
      if (strstr(m, "none")) ssh->auth_methods |= GT_SSH_AUTH_NONE;
    }
  }

  ssh->handshake_verified=true;
  return GT_OK;
}

/*
 * 认证：用存下的凭据做一次 userauth。可在同一会话上重试（换钥匙）。
 *
 * 失败时把 libssh2 的原始错误串**原样**带出来：调用方要靠它区分
 * 「钥匙解不开（缺口令/口令错）」与「服务器不收这把钥匙」——两者的
 * 补救完全不同（问口令 vs 换一把/走密码），猜错就是让用户白输一遍。
 */
int gt_ssh_authenticate(gt_ssh_t *ssh) {
  if (!ssh) return GT_ERR_INVALID_ARG;
  if (!ssh->session || !ssh->handshake_verified) {
    gt__set_error("gt_ssh_authenticate: 尚未握手（先 gt_ssh_handshake）");
    return GT_ERR_PROTOCOL;
  }
  if (ssh->connected) return GT_OK;

  int rc = -1;
  if (ssh->pem) {
    rc = libssh2_userauth_publickey_frommemory(
        ssh->session, ssh->username, strlen(ssh->username), NULL, 0, ssh->pem,
        strlen(ssh->pem), ssh->passphrase);
  } else if (ssh->password) {
    rc = libssh2_userauth_password(ssh->session, ssh->username, ssh->password);
  } else {
    gt__set_error("ssh: 未提供认证凭据（password/private key）");
    return GT_ERR_AUTH;
  }
  if (rc != 0) {
    char *msg = NULL;
    libssh2_session_last_error(ssh->session, &msg, NULL, 0);
    /* 带上「服务器允许的方式」：只带私钥去连只收密码的服务器时，
     * libssh2 的原始错误串读不出这个区别，上层只能猜（最容易猜成
     * 「口令错了」再问一次口令）。 */
    const char *used = ssh->pem ? "publickey" : "password";
    gt__set_error("ssh: 认证失败（用了 %s；服务器允许：%s）: %s", used,
                  ssh->auth_methods ? auth_methods_str(ssh->auth_methods)
                                    : "未知",
                  msg ? msg : "unknown");
    return GT_ERR_AUTH;
  }
  ssh->connected = true;
  gt__emit_status(0, "ssh connected");
  return GT_OK;
}

int gt_ssh_connect(gt_ssh_t *ssh) {
  int rc = gt_ssh_handshake(ssh);
  if (rc != GT_OK) return rc;
  return gt_ssh_authenticate(ssh);
}

/* Nonblocking connector owns fresh SSH throughout TCP/hostkey/auth. DNS and
 * UI decisions are supplied separately; no worker may outlive a cancellation. */
typedef enum {CONNECT_SOCKET,CONNECT_POLL,CONNECT_HANDSHAKE,CONNECT_HOSTKEY,CONNECT_METHODS,CONNECT_AUTH} connect_stage;
struct gt_ssh_connector {
  gt_allocator_t allocator;gt_ssh_t *ssh;gt_ssh_connector_config_t config;
  gt_ssh_connector_info_t info;connect_stage stage;uint64_t started,now;
  char *host_key; /* Public hostkey base64, never private authentication key. */
};
static void *connector_allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void connector_release(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static void connector_drop(gt_ssh_connector_t *c) {
  if(c->ssh) {
    gt_ssh_t *s=c->ssh;
    if(s->sock>=0)(void)shutdown(s->sock,SHUT_RDWR);
    if(s->session){libssh2_session_set_blocking(s->session,1);libssh2_session_set_timeout(s->session,1);}
    s->connected=false;gt_ssh_close(s);c->ssh=NULL;
  }
  if(c->host_key){gt__secure_erase(c->host_key,strlen(c->host_key));free(c->host_key);c->host_key=NULL;}
}
static void connector_fail(gt_ssh_connector_t *c,gt_status_t error){c->info.phase=GT_SSH_CONNECT_FAILED;c->info.error=error;connector_drop(c);}
static bool connector_tick(gt_ssh_connector_t *c) {
  uint64_t now;if(!c->config.clock(c->config.clock_context,&now) || now<c->now){connector_fail(c,GT_ERR_INTERNAL);return false;}c->now=now;
  if(now-c->started>=c->config.timeout_ms){c->info.deadline=true;connector_fail(c,GT_ERR_IO);return false;}return true;
}
static gt_status_t connector_error(int code) {
  switch(code){case LIBSSH2_ERROR_SOCKET_SEND:case LIBSSH2_ERROR_SOCKET_RECV:case LIBSSH2_ERROR_SOCKET_DISCONNECT:case LIBSSH2_ERROR_TIMEOUT:return GT_ERR_IO;
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED:case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED:case LIBSSH2_ERROR_KEYFILE_AUTH_FAILED:return GT_ERR_AUTH;
    case LIBSSH2_ERROR_ALLOC:return GT_ERR_INTERNAL;default:return GT_ERR_PROTOCOL;}
}
gt_status_t gt__ssh_connector_open(gt_ssh_t **owned,const gt_ssh_connector_config_t *config,const gt_allocator_t *allocator,gt_ssh_connector_t **out) {
  if(out)*out=NULL;
  if(!owned || !*owned || !out || !config || (config->family!=4 && config->family!=6) || !config->clock || !config->timeout_ms || config->timeout_ms>60000 || (*owned)->session || (*owned)->sock>=0)return GT_ERR_INVALID_ARG;
  if(!config->defer_credentials && !(*owned)->pem && !(*owned)->password)return GT_ERR_AUTH;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,connector_allocate,connector_release};if(!a.allocate || !a.release)return GT_ERR_INVALID_ARG;
  uint64_t now;if(!config->clock(config->clock_context,&now))return GT_ERR_IO;
  gt_ssh_connector_t *c=a.allocate(a.context,sizeof(*c));if(!c)return GT_ERR_IO;
  memset(c,0,sizeof(*c));c->allocator=a;c->config=*config;c->started=c->now=now;c->ssh=*owned;*owned=NULL;*out=c;return GT_OK;
}
static void connector_socket(gt_ssh_connector_t *c) {
  gt_ssh_t *s=c->ssh;int family=c->config.family==4?AF_INET:AF_INET6;int fd=socket(family,SOCK_STREAM,0);
  if(fd<0){connector_fail(c,GT_ERR_IO);return;}s->sock=fd;
  int flags=fcntl(fd,F_GETFL),descriptor=fcntl(fd,F_GETFD);
  if(flags<0 || descriptor<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK) || fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC)){connector_fail(c,GT_ERR_IO);return;}
#ifdef SO_NOSIGPIPE
  int one=1;if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one))){connector_fail(c,GT_ERR_IO);return;}
#endif
  int rc;
  if(family==AF_INET){struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(s->port)};memcpy(&address.sin_addr,c->config.address,4);rc=connect(fd,(struct sockaddr *)&address,sizeof(address));}
  else{struct sockaddr_in6 address={.sin6_family=AF_INET6,.sin6_port=htons(s->port),.sin6_scope_id=c->config.scope};memcpy(&address.sin6_addr,c->config.address,16);rc=connect(fd,(struct sockaddr *)&address,sizeof(address));}
  if(rc && errno!=EINPROGRESS){connector_fail(c,GT_ERR_IO);return;}c->stage=CONNECT_POLL;
}
static void connector_hostkey(gt_ssh_connector_t *c) {
  gt_ssh_t *s=c->ssh;size_t n=0;int type=0;const char *key=libssh2_session_hostkey(s->session,&n,&type);
  const unsigned char *hash=(const unsigned char *)libssh2_hostkey_hash(s->session,LIBSSH2_HOSTKEY_HASH_SHA256);
  if(!key || !n || n>65536 || !hash){connector_fail(c,GT_ERR_PROTOCOL);return;}
  const char *name=hostkey_type_name(type);if(!strcmp(name,"unknown")){connector_fail(c,GT_ERR_UNSUPPORTED);return;}
  char *fingerprint=fingerprint_base64(hash);c->host_key=b64_encode((const unsigned char *)key,n);
  if(!fingerprint || !c->host_key){free(fingerprint);connector_fail(c,GT_ERR_INTERNAL);return;}
  snprintf(c->info.key_type,sizeof(c->info.key_type),"%s",name);snprintf(c->info.fingerprint,sizeof(c->info.fingerprint),"SHA256:%s",fingerprint);free(fingerprint);
  kh_result_t result=kh_check(s->kh_path,s->host,s->port,name,c->host_key);
  if(result==KH_REVOKED){c->info.host=GT_SSH_HOST_REVOKED;connector_fail(c,GT_ERR_HOST_KEY);return;}
  if(result==KH_UNSUPPORTED || result==KH_IO){connector_fail(c,result==KH_UNSUPPORTED?GT_ERR_UNSUPPORTED:GT_ERR_IO);return;}
  c->info.host=result==KH_MATCH?GT_SSH_HOST_KNOWN:result==KH_MISMATCH?GT_SSH_HOST_MISMATCH:GT_SSH_HOST_UNKNOWN;
  if(result==KH_MISMATCH){connector_fail(c,GT_ERR_HOST_KEY);return;}
  if(result==KH_MATCH){c->stage=CONNECT_METHODS;c->info.phase=GT_SSH_CONNECT_AUTHENTICATING;}
  else c->info.phase=GT_SSH_CONNECT_HOSTKEY;
}
gt_status_t gt__ssh_connector_decide(gt_ssh_connector_t *c,gt_hostkey_decision_t decision) {
  if(!c || c->info.phase!=GT_SSH_CONNECT_HOSTKEY || c->info.host!=GT_SSH_HOST_UNKNOWN)return GT_ERR_INVALID_ARG;
  if(!connector_tick(c))return c->info.error;
  if(decision==GT_HOSTKEY_UNKNOWN)return GT_OK;
  if(decision==GT_HOSTKEY_REJECT){connector_fail(c,GT_ERR_HOST_KEY);return GT_ERR_HOST_KEY;}
  if(decision!=GT_HOSTKEY_ACCEPT)return GT_ERR_INVALID_ARG;
  kh_result_t refreshed = KH_UNKNOWN;
  gt_status_t rc=kh_append(c->ssh->kh_path,c->ssh->host,c->ssh->port,c->info.key_type,c->host_key,&refreshed);
  if (refreshed == KH_REVOKED) c->info.host = GT_SSH_HOST_REVOKED;
  else if (refreshed == KH_MISMATCH) c->info.host = GT_SSH_HOST_MISMATCH;
  if(rc!=GT_OK){connector_fail(c,rc);return rc;}
  c->stage=CONNECT_METHODS;c->info.phase=GT_SSH_CONNECT_AUTHENTICATING;return GT_OK;
}
gt_ssh_connector_phase_t gt__ssh_connector_step(gt_ssh_connector_t *c) {
  if(!c)return GT_SSH_CONNECT_FAILED;
  if((c->info.phase>=GT_SSH_CONNECT_READY && c->info.phase!=GT_SSH_CONNECT_CREDENTIALS) || !connector_tick(c))return c->info.phase;
  if(c->info.phase==GT_SSH_CONNECT_CREDENTIALS)return c->info.phase;
  gt_ssh_t *s=c->ssh;int rc=0;
  switch(c->stage) {
    case CONNECT_SOCKET:connector_socket(c);break;
    case CONNECT_POLL: {
      struct pollfd p={.fd=s->sock,.events=POLLOUT};rc=poll(&p,1,0);
      if(rc<0){if(errno!=EINTR)connector_fail(c,GT_ERR_IO);break;}if(!rc)break;
      int error=0;socklen_t n=sizeof(error);if((p.revents&(POLLERR|POLLHUP|POLLNVAL)) || getsockopt(s->sock,SOL_SOCKET,SO_ERROR,&error,&n) || error){connector_fail(c,GT_ERR_IO);break;}
      s->session=libssh2_session_init();if(!s->session){connector_fail(c,GT_ERR_INTERNAL);break;}
      libssh2_session_set_blocking(s->session,0);c->stage=CONNECT_HANDSHAKE;c->info.phase=GT_SSH_CONNECT_NEGOTIATING;break;
    }
    case CONNECT_HANDSHAKE:rc=libssh2_session_handshake(s->session,s->sock);if(!rc)c->stage=CONNECT_HOSTKEY;else if(rc!=LIBSSH2_ERROR_EAGAIN)connector_fail(c,connector_error(rc));break;
    case CONNECT_HOSTKEY:if(c->info.phase!=GT_SSH_CONNECT_HOSTKEY)connector_hostkey(c);break;
    case CONNECT_METHODS: {
      char *methods=libssh2_userauth_list(s->session,s->username,(unsigned)strlen(s->username));
      if(!methods){rc=libssh2_session_last_errno(s->session);if(rc!=LIBSSH2_ERROR_EAGAIN)connector_fail(c,rc?connector_error(rc):GT_ERR_UNSUPPORTED);break;}
      s->auth_methods=0;
      if(strstr(methods,"publickey"))s->auth_methods|=GT_SSH_AUTH_PUBLICKEY;
      if(strstr(methods,"password"))s->auth_methods|=GT_SSH_AUTH_PASSWORD;
      if(strstr(methods,"keyboard-interactive"))s->auth_methods|=GT_SSH_AUTH_KEYBOARD;
      if(strstr(methods,"hostbased"))s->auth_methods|=GT_SSH_AUTH_HOSTBASED;
      c->info.auth_methods=s->auth_methods;s->handshake_verified=true;c->stage=CONNECT_AUTH;
      if(c->config.defer_credentials && !s->pem && !s->password)c->info.phase=GT_SSH_CONNECT_CREDENTIALS;
      break;
    }
    case CONNECT_AUTH:
      if(s->pem)rc=libssh2_userauth_publickey_frommemory(s->session,s->username,strlen(s->username),NULL,0,s->pem,strlen(s->pem),s->passphrase);
      else rc=libssh2_userauth_password(s->session,s->username,s->password);
      if(!rc){s->connected=true;c->info.phase=GT_SSH_CONNECT_READY;}
      else if(rc!=LIBSSH2_ERROR_EAGAIN) {
        gt_status_t error=connector_error(rc);
        /* libssh2's OpenSSL backend reports FILE when extracting the public
         * key from an encrypted OpenSSH key with a wrong passphrase. This
         * from-memory key failure permits replacement credentials; it is not
         * a transport/file-system failure. Keep protocol/decrypt errors and
         * password/handshake errors on their existing paths. */
        if(s->pem && rc==LIBSSH2_ERROR_FILE)error=GT_ERR_AUTH;
        if(c->config.defer_credentials && error==GT_ERR_AUTH) {
          secret_free(s->pem);s->pem=NULL;secret_free(s->password);s->password=NULL;secret_free(s->passphrase);s->passphrase=NULL;
          c->info.error=GT_ERR_AUTH;c->stage=CONNECT_METHODS;c->info.phase=GT_SSH_CONNECT_AUTHENTICATING;
        } else connector_fail(c,error);
      }
      break;
  }
  if(c->info.phase<=GT_SSH_CONNECT_READY || c->info.phase==GT_SSH_CONNECT_CREDENTIALS)(void)connector_tick(c);
  return c->info.phase;
}
bool gt__ssh_connector_info(const gt_ssh_connector_t *c,gt_ssh_connector_info_t *out){if(out)memset(out,0,sizeof(*out));if(!c || !out)return false;*out=c->info;return true;}
gt_status_t gt__ssh_connector_supply(gt_ssh_connector_t *c,gt_ssh_t **owned) {
  if(!c || !owned || !*owned || c->info.phase!=GT_SSH_CONNECT_CREDENTIALS || !c->config.defer_credentials)return GT_ERR_INVALID_ARG;
  if(!connector_tick(c))return c->info.error;
  gt_ssh_t *target=c->ssh,*material=*owned;
  if(!target || material==target || material->session || material->sock>=0 || material->connected ||
     (!material->pem && !material->password) || strcmp(target->host,material->host) ||
     strcmp(target->username,material->username) || target->port!=material->port || strcmp(target->kh_path,material->kh_path))return GT_ERR_INVALID_ARG;
  if((material->pem && !(target->auth_methods&GT_SSH_AUTH_PUBLICKEY)) ||
     (!material->pem && !(target->auth_methods&GT_SSH_AUTH_PASSWORD)))return GT_ERR_UNSUPPORTED;
  secret_free(target->pem);secret_free(target->passphrase);secret_free(target->password);
  target->pem=material->pem;target->passphrase=material->passphrase;target->password=material->password;
  material->pem=material->passphrase=material->password=NULL;*owned=NULL;gt_ssh_close(material);
  c->stage=CONNECT_AUTH;c->info.phase=GT_SSH_CONNECT_AUTHENTICATING;c->info.error=GT_OK;
  return GT_OK;
}
bool gt__ssh_connector_take(gt_ssh_connector_t *c,gt_ssh_t **out) {
  if(out)*out=NULL;
  if(!c || !out || c->info.phase!=GT_SSH_CONNECT_READY)return false;
  /* libssh2 blocking mode implements bounded waits around nonblocking recv.
   * Making the OS socket blocking here bypasses its timeout machinery. */
  libssh2_session_set_blocking(c->ssh->session,1);libssh2_session_set_timeout(c->ssh->session,5000);*out=c->ssh;c->ssh=NULL;c->info.phase=GT_SSH_CONNECT_TAKEN;
  free(c->host_key);c->host_key=NULL;return true;
}
void gt__ssh_connector_cancel(gt_ssh_connector_t *c){if(!c || (c->info.phase>=GT_SSH_CONNECT_TAKEN && c->info.phase!=GT_SSH_CONNECT_CREDENTIALS))return;c->info.phase=GT_SSH_CONNECT_CANCELLED;c->info.error=GT_ERR_CANCELLED;connector_drop(c);}
void gt__ssh_connector_close(gt_ssh_connector_t *c){if(!c)return;connector_drop(c);gt_allocator_t a=c->allocator;gt__secure_erase(c,sizeof(*c));a.release(a.context,c,sizeof(*c));}

int gt__ssh_borrow(gt_ssh_t *ssh,void **session,int *socket) {
  if(!ssh || !ssh->connected || !session || !socket)return GT_ERR_INVALID_ARG;
  *session=ssh->session;*socket=ssh->sock;return GT_OK;
}
int gt_ssh_abort(gt_ssh_t *ssh) {
  if (!ssh) return GT_ERR_INVALID_ARG;
  int result = 0;
  if (ssh->sock >= 0) {
    do { result = shutdown(ssh->sock, SHUT_RDWR); } while (result < 0 && errno == EINTR);
  }
  ssh->connected = false;
  if (result < 0 && errno != ENOTCONN) {
    gt__set_error("ssh abort: socket shutdown failed"); return GT_ERR_IO;
  }
  return GT_OK;
}
void gt__ssh_abort(gt_ssh_t *ssh) { (void)gt_ssh_abort(ssh); }



void gt_ssh_close(gt_ssh_t *ssh) {
  if (!ssh) return;
  if (ssh->session) {
    libssh2_session_set_timeout(ssh->session,100);
    if (ssh->connected) libssh2_session_disconnect(ssh->session, "bye");
    libssh2_session_free(ssh->session);
  }
  if (ssh->sock >= 0) close(ssh->sock);
  if (ssh->io_lock_init) pthread_mutex_destroy(&ssh->io_lock);
  secret_free(ssh->password);
  secret_free(ssh->pem);
  secret_free(ssh->passphrase);
  free(ssh->kh_path);
  free(ssh->host);
  free(ssh->username);
  free(ssh);
}

void gt__ssh_io_lock(gt_ssh_t *ssh) {
  if (ssh && ssh->io_lock_init) pthread_mutex_lock(&ssh->io_lock);
}
void gt__ssh_io_unlock(gt_ssh_t *ssh) {
  if (ssh && ssh->io_lock_init) pthread_mutex_unlock(&ssh->io_lock);
}

/* SFTP 子系统（M4）：gt_sftp_t 由 sftp.c 定义（LIBSSH2_SFTP 的薄封装）。
 * 契约：libssh2 句柄不穿透——上层只见 gt_sftp_* / gt_sftp_entry_t。
 * 线程：与 shell 通道同规则，全部由**同一会话线程**独占调用。 */
gt_sftp_t *gt_ssh_sftp(gt_ssh_t *ssh) {
  if (!ssh || !ssh->connected || !ssh->session) {
    gt__set_error("gt_ssh_sftp: 会话未连接");
    return NULL;
  }
  return gt__sftp_from_session(ssh->session, ssh);
}

/* ------------------------------------------------------------------ */
/* 交互式 shell 通道                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  gt_ssh_t *ssh;
  LIBSSH2_SESSION *sess;
  LIBSSH2_CHANNEL *chan;
  uint8_t *wtail;    /* 没送出去的尾巴（见 gt__ssh_shell_write） */
  size_t wtail_len;
} ssh_shell_t;

/* Nonblocking command/channel admission on a dedicated SSH owner. Every wait
 * shares one monotonic budget; cancellation never borrows a terminal/SFTP link.
 */
static int channel_budget_status(const gt_command_budget_t *b, bool distinct_timeout) {
  /* Keep exec/tmux on their existing budget and diagnostic contract. */
  if (!distinct_timeout)
    return gt__command_expired(b) ? GT_ERR_CANCELLED : GT_OK;
  if (b->cancelled && b->cancelled(b->context)) return GT_ERR_CANCELLED;
  if (gt__command_now() >= b->deadline)
    return GT_ERR_IO;
  return GT_OK;
}
static int command_wait(gt_ssh_t *ssh, const gt_command_budget_t *b, bool distinct_timeout) {
  int status = channel_budget_status(b, distinct_timeout);
  if (status != GT_OK) return status;
  int direction = libssh2_session_block_directions(ssh->session);
  short events = 0;
  if (direction & LIBSSH2_SESSION_BLOCK_INBOUND)
    events |= POLLIN;
  if (direction & LIBSSH2_SESSION_BLOCK_OUTBOUND)
    events |= POLLOUT;
  struct pollfd p = {ssh->sock, events ? events : POLLIN, 0};
  int rc = poll(&p, 1, 10);
  if (rc < 0 && errno != EINTR)
    return GT_ERR_IO;
  if (rc > 0 && (p.revents & (POLLERR | POLLNVAL)))
    return GT_ERR_IO;
  return GT_OK;
}
static LIBSSH2_CHANNEL *command_channel(gt_ssh_t *ssh, const char *command,
                                        const gt_shell_opts_t *pty,
                                        const gt_command_budget_t *b,
                                        int *status, bool distinct_timeout) {
  LIBSSH2_CHANNEL *channel = NULL;
  *status = GT_ERR_IO;
  if (!ssh || !ssh->connected || !ssh->session) {
    gt__set_error("SSH: connection unavailable");
    return NULL;
  }
  if ((*status = channel_budget_status(b, distinct_timeout)) != GT_OK) goto failed;
  libssh2_session_set_blocking(ssh->session, 0);
  while (!(channel = libssh2_channel_open_session(ssh->session))) {
    if (libssh2_session_last_errno(ssh->session) != LIBSSH2_ERROR_EAGAIN)
      goto failed;
    if ((*status = command_wait(ssh, b, distinct_timeout)) != GT_OK)
      goto failed;
  }
  int rc;
  if ((*status = channel_budget_status(b, distinct_timeout)) != GT_OK) goto failed;
  if (pty) {
    const char *term = pty->term ? pty->term : "xterm-256color";
    do {
      rc = libssh2_channel_request_pty_ex(channel, term, (unsigned)strlen(term),
                                          NULL, 0, pty->cols ? pty->cols : 80,
                                          pty->rows ? pty->rows : 24,
                                          pty->px_width, pty->px_height);
      if (rc == LIBSSH2_ERROR_EAGAIN &&
          (*status = command_wait(ssh, b, distinct_timeout)) != GT_OK)
        goto failed;
    } while (rc == LIBSSH2_ERROR_EAGAIN);
    if (rc) {
      *status = rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED ? GT_ERR_UNSUPPORTED : GT_ERR_IO;
      gt__set_error("SSH: server refused PTY");
      goto failed;
    }
  }
  if ((*status = channel_budget_status(b, distinct_timeout)) != GT_OK) goto failed;
  do {
    rc = command ? libssh2_channel_exec(channel, command)
                 : libssh2_channel_shell(channel);
    if (rc == LIBSSH2_ERROR_EAGAIN && (*status = command_wait(ssh, b, distinct_timeout)) != GT_OK)
      goto failed;
  } while (rc == LIBSSH2_ERROR_EAGAIN);
  if (rc) {
    *status = rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED ? GT_ERR_UNSUPPORTED : GT_ERR_IO;
    gt__set_error("SSH: server refused exec/shell");
    goto failed;
  }
  if ((*status = channel_budget_status(b, distinct_timeout)) != GT_OK) goto failed;
  *status = GT_OK;
  return channel;
failed:
  if (*status == GT_OK)
    *status = GT_ERR_IO;
  if (*status != GT_ERR_UNSUPPORTED)
    gt__set_error("SSH: channel timed out, cancelled or disconnected");
  /* A partly admitted request is unsafe to retry on this owner. */
  if (*status != GT_ERR_UNSUPPORTED) gt__ssh_abort(ssh);
  libssh2_session_set_blocking(ssh->session, 1);
  libssh2_session_set_timeout(ssh->session, 50);
  if (channel)
    libssh2_channel_free(channel);
  return NULL;
}
int gt__command_ssh(gt_ssh_t *ssh, const char *command,
                    const gt_command_budget_t *b, gt_command_result_t *r) {
  memset(r, 0, sizeof *r);
  if (gt__command_expired(b))
    return GT_ERR_CANCELLED;
  if (!ssh || !ssh->session || !ssh->connected)
    return GT_ERR_IO;
  gt__ssh_io_lock(ssh);
  long previous = libssh2_session_get_timeout(ssh->session);
  int blocking = libssh2_session_get_blocking(ssh->session), rc = GT_OK;
  /* Only the trailing shell completion record proves the command returned.
   * SSH EOF/get_exit_status alone can report zero after a reset. This is a
   * bounded one-shot command, not a resident protocol or recovery token. */
  size_t n = strlen(command) + 100;
  char *wrapped = malloc(n);
  if (!wrapped) {
    gt__ssh_io_unlock(ssh);
    return GT_ERR_INTERNAL;
  }
  snprintf(
      wrapped, n,
      "set +e; %s; gt_tmux_rc=$?; printf '\\nGHOSTERM_TMUX_RC=%%d\\n' \"$gt_tmux_rc\"",
      command);
  r->started = true;
  LIBSSH2_CHANNEL *ch = command_channel(ssh, wrapped, NULL, b, &rc, false);
  free(wrapped);
  if (!ch)
    goto end;
  for (;;) {
    bool data = false;
    for (int stream = 0; stream < 2; stream++) {
      char buffer[8192];
      ssize_t got = libssh2_channel_read_ex(ch, stream, buffer, sizeof buffer);
      if (got > 0) {
        data = true;
        rc = gt__command_append(stream ? &r->err : &r->out,
                                stream ? &r->err_len : &r->out_len, buffer,
                                (size_t)got);
        if (rc)
          goto close;
      } else if (got < 0 && got != LIBSSH2_ERROR_EAGAIN) {
        rc = GT_ERR_IO;
        goto close;
      }
    }
    if (libssh2_channel_eof(ch) && !data)
      break;
    if ((rc = command_wait(ssh, b, false)) != GT_OK)
      goto close;
  }
  if (r->out) {
    char *marker = NULL, *p = r->out;
    while ((p = strstr(p, "\nGHOSTERM_TMUX_RC="))) {
      marker = p;
      p++;
    }
    if (marker) {
      char *after = NULL;
      long code = strtol(marker + 18, &after, 10);
      if (after && after[0] == '\n' && after[1] == 0 && code >= 0 &&
          code <= 255) {
        r->exit_code = (int)code;
        r->completed = true;
        r->out_len = (size_t)(marker - r->out);
        *marker = 0;
      }
    }
  }
  if (!r->completed) {
    gt__set_error("tmux: command result unknown (missing SSH completion)");
    rc = GT_ERR_IO;
  }
close:
  if (rc != GT_OK)
    gt__ssh_abort(ssh);
  libssh2_session_set_blocking(ssh->session, 1);
  libssh2_session_set_timeout(ssh->session, 50);
  libssh2_channel_free(ch);
end:
  libssh2_session_set_blocking(ssh->session, blocking);
  libssh2_session_set_timeout(ssh->session, previous);
  gt__ssh_io_unlock(ssh);
  return rc;
}
static void *ssh_interactive_open(gt_ssh_t *ssh, const char *command,
                           const gt_shell_opts_t *opts, uint32_t timeout_ms,
                           bool (*cancelled)(void *), void *context, gt_status_t *out_status, bool distinct_timeout) {
  if (out_status) *out_status = GT_ERR_INVALID_ARG;
  if (!ssh || !ssh->session)
    return NULL;
  gt_command_budget_t budget = {
      gt__command_now() + (timeout_ms ? timeout_ms : 5000), cancelled, context};
  gt__ssh_io_lock(ssh);
  long previous = libssh2_session_get_timeout(ssh->session);
  int status = GT_OK;
  LIBSSH2_CHANNEL *channel =
      command_channel(ssh, command, opts, &budget, &status, distinct_timeout);
  libssh2_session_set_blocking(ssh->session, 1);
  libssh2_session_set_timeout(ssh->session, previous);
  ssh_shell_t *sh = NULL;
  if (channel) {
    sh = gt__alloc(sizeof *sh);
    if (sh) {
      sh->ssh = ssh;
      sh->sess = ssh->session;
      sh->chan = channel;
    } else {
      status = GT_ERR_INTERNAL;
      libssh2_channel_free(channel);
    }
  }
  gt__ssh_io_unlock(ssh);
  if (out_status) *out_status = (gt_status_t)status;
  return sh;
}

/* 写最多等这么久，然后把剩下的排队；不是丢，是晚到。 */
#define GT_SSH_WRITE_GRACE_MS 100
/* 补送时每次只做一次「尽量不等」的尝试（0 在 libssh2 里是**无限**，故取 1）。 */
#define GT_SSH_FLUSH_MS 1
/* 尾队列上限：超过就显式报错，绝不静默丢输入。 */
#define GT_SSH_WTAIL_MAX (4u * 1024u * 1024u)

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * 有界写：在 budget_ms 内尽量送，送到哪算哪（*out_off）。
 *
 * 超时值**自己设、自己还**：先前这里一个超时都不设，有界性全靠上一次
 * gt__ssh_shell_read 残留在 session 上的值（实测 20ms）——于是「第一次写
 * 发生在任何读之前」就退化成 0 = 无限等。还原是为了不打扰共用同一
 * session 的 SFTP（它不设超时，靠继承）。
 */
static int ssh_write_bounded(ssh_shell_t *sh, const uint8_t *data, size_t len,
                             int budget_ms, size_t *out_off) {
  long prev = libssh2_session_get_timeout(sh->sess);
  int64_t deadline = mono_ms() + budget_ms;
  size_t off = 0;
  int rc = GT_OK;
  while (off < len) {
    int64_t rem = deadline - mono_ms();
    if (rem <= 0) break;
    libssh2_session_set_timeout(sh->sess, (long)rem);
    ssize_t n =
        libssh2_channel_write(sh->chan, (const char *)data + off, len - off);
    if (n > 0) { off += (size_t)n; continue; }
    /* 远端不收（窗口满 / 对端不读）：不是错误，把剩下的留给调用方排队。
     * 先前把 TIMEOUT 一并算成 GT_ERR_IO —— 键入的字节就那么没了。 */
    if (n == 0 || n == LIBSSH2_ERROR_TIMEOUT || n == LIBSSH2_ERROR_EAGAIN) break;
    gt__set_error("ssh_shell write: %ld", (long)n);
    rc = GT_ERR_IO;
    break;
  }
  libssh2_session_set_timeout(sh->sess, prev);
  *out_off = off;
  return rc;
}

/* 把尾队列往外推一把（能推多少推多少）。 */
static int ssh_flush_wtail(ssh_shell_t *sh) {
  if (!sh->wtail_len) return GT_OK;
  size_t sent = 0;
  int rc = ssh_write_bounded(sh, sh->wtail, sh->wtail_len, GT_SSH_FLUSH_MS, &sent);
  if (sent) {
    sh->wtail_len -= sent;
    if (sh->wtail_len) memmove(sh->wtail, sh->wtail + sent, sh->wtail_len);
    else { free(sh->wtail); sh->wtail = NULL; }
  }
  return rc;
}

static int ssh_wtail_append(ssh_shell_t *sh, const uint8_t *data, size_t len) {
  if (sh->wtail_len + len > GT_SSH_WTAIL_MAX) {
    gt__set_error("ssh_shell write: 远端久不收，待发队列超过 %u 字节",
                  (unsigned)GT_SSH_WTAIL_MAX);
    return GT_ERR_IO;
  }
  uint8_t *p = realloc(sh->wtail, sh->wtail_len + len);
  if (!p) {
    gt__set_error("ssh_shell write: 待发队列分配失败");
    return GT_ERR_IO;
  }
  memcpy(p + sh->wtail_len, data, len);
  sh->wtail = p;
  sh->wtail_len += len;
  return GT_OK;
}

void *gt__ssh_command_open(gt_ssh_t *ssh, const char *command,
    const gt_shell_opts_t *opts, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context, gt_status_t *out_status) {
  return ssh_interactive_open(ssh, command, opts, timeout_ms, cancelled, context, out_status, false);
}
void *gt__ssh_shell_open_controlled(gt_ssh_t *ssh, const gt_shell_opts_t *opts,
    uint32_t timeout_ms, bool (*cancelled)(void *), void *context, gt_status_t *out_status) {
  return ssh_interactive_open(ssh, NULL, opts, timeout_ms, cancelled, context, out_status, true);
}

void *gt__ssh_shell_open(gt_ssh_t *ssh, const char *term, uint16_t cols,
                         uint16_t rows, uint32_t px_w, uint32_t px_h) {
  gt_shell_opts_t o={term,cols,rows,px_w,px_h};
  return gt__ssh_command_open(ssh,NULL,&o,5000,NULL,NULL,NULL);
}

int gt__ssh_shell_read(void *ch, uint8_t *buf, size_t cap, int timeout_ms) {
  ssh_shell_t *sh = ch;
  if (!sh) return GT_TRANSPORT_READ_ERROR;
  ssh_flush_wtail(sh); /* 读是尾队列的泵：没有它，排队的输入只能等下一次写 */
  /* Transport zero is a poll; libssh2 zero is an unlimited blocking wait.
   * Restore both settings before another operation borrows this session. */
  long previous_timeout = libssh2_session_get_timeout(sh->sess);
  int previous_blocking = libssh2_session_get_blocking(sh->sess);
  if (timeout_ms == 0) libssh2_session_set_blocking(sh->sess, 0);
  else libssh2_session_set_timeout(sh->sess, timeout_ms < 0 ? 0 : (long)timeout_ms);
  ssize_t n = libssh2_channel_read(sh->chan, (char *)buf, cap);
  libssh2_session_set_blocking(sh->sess, previous_blocking);
  libssh2_session_set_timeout(sh->sess, previous_timeout);
  if (n > 0) return (int)n;
  if (n == 0) {
    if (libssh2_channel_eof(sh->chan)) return GT_TRANSPORT_READ_EOF;
    return GT_TRANSPORT_READ_TIMEOUT;
  }
  if (n == LIBSSH2_ERROR_TIMEOUT || n == LIBSSH2_ERROR_EAGAIN)
    return GT_TRANSPORT_READ_TIMEOUT;
  char *msg = NULL;
  libssh2_session_last_error(sh->sess, &msg, NULL, 0);
  gt__set_error("ssh_shell read: %s", msg ? msg : "unknown");
  return GT_TRANSPORT_READ_ERROR;
}

/*
 * 写：绝不无限等（契约见 transport.h）。
 *
 * 远端不收时在 GT_SSH_WRITE_GRACE_MS 内尽量送，剩下的进尾队列、由后续
 * read/write 补送——输入不丢，只是晚到。先前这里是 `EAGAIN → continue`
 * 死循环 + 超时当 IO 错误（丢字节），和本地 PTY 路径是同一类毛病。
 */
int gt__ssh_shell_write(void *ch, const uint8_t *data, size_t len) {
  ssh_shell_t *sh = ch;
  if (!sh) return GT_ERR_INVALID_ARG;
  if (!len) return GT_OK;
  int rc = ssh_flush_wtail(sh);
  if (rc != GT_OK) return rc;
  /* 队列里还有旧字节 → 新字节必须排在后面，否则就乱序了。 */
  if (sh->wtail_len) return ssh_wtail_append(sh, data, len);
  size_t off = 0;
  rc = ssh_write_bounded(sh, data, len, GT_SSH_WRITE_GRACE_MS, &off);
  if (rc != GT_OK) return rc;
  if (off < len) {
    return ssh_wtail_append(sh, data + off, len - off);
  }
  return GT_OK;
}

int gt__ssh_shell_resize(void *ch, uint16_t cols, uint16_t rows, uint32_t px_w,
                         uint32_t px_h) {
  ssh_shell_t *sh = ch;
  if (!sh) return GT_ERR_INVALID_ARG;
  (void)px_w; (void)px_h;
  if (libssh2_channel_request_pty_size(sh->chan, cols ? cols : 80,
                                       rows ? rows : 24) != 0) {
    gt__set_error("ssh_shell resize 失败");
    return GT_ERR_IO;
  }
  return GT_OK;
}

bool gt__ssh_shell_exited(void *ch, int *exit_code) {
  /* libssh2 defaults its status getter to zero if no exit-status arrived. Its
   * public API has no availability bit. EOF alone cannot prove an exit code. */
  (void)ch; (void)exit_code; return false;
}

void gt__ssh_shell_close(void *ch) {
  ssh_shell_t *sh = ch;
  if (!sh) return;
  long previous=libssh2_session_get_timeout(sh->sess);
  libssh2_session_set_timeout(sh->sess,100);
  libssh2_channel_send_eof(sh->chan);
  libssh2_channel_close(sh->chan);
  libssh2_channel_free(sh->chan);
  libssh2_session_set_timeout(sh->sess,previous);
  free(sh->wtail);
  free(sh);
}

/* ------------------------------------------------------------------ */
/* exec 通道（远端单命令；非交互）                                      */
/* ------------------------------------------------------------------ */

typedef struct gt_ssh_exec {
  gt_ssh_t *ssh;
  LIBSSH2_SESSION *sess;
  LIBSSH2_CHANNEL *chan;
  bool closed;
  bool stdout_eof;
  bool stderr_eof;
} gt_ssh_exec_t;

gt_ssh_exec_t *gt_ssh_exec(gt_ssh_t *ssh, const char *command,
                           gt_status_t *out_status) {
  if (out_status)
    *out_status = GT_ERR_INTERNAL;
  if (!ssh || !ssh->connected || !ssh->session) {
    gt__set_error("ssh_exec: 会话未连接");
    if (out_status)
      *out_status = GT_ERR_INVALID_ARG;
    return NULL;
  }
  if (!command || !*command) {
    gt__set_error("ssh_exec: 命令为空");
    if (out_status)
      *out_status = GT_ERR_INVALID_ARG;
    return NULL;
  }
  gt_ssh_exec_t *e = gt__alloc(sizeof *e);
  if (!e)
    return NULL;
  e->ssh = ssh;
  e->sess = ssh->session;

  /* Both streaming exec and bounded management use the same channel admission.
   * Streaming reads retain their existing stdout/stderr interface. */
  size_t length = strlen(command);
  char *cmd = gt__strdup(command);
  if (!cmd) {
    free(e);
    return NULL;
  }
  while (length && (cmd[length - 1] == '\n' || cmd[length - 1] == '\r'))
    cmd[--length] = 0;
  gt_command_budget_t budget = {gt__command_now() + 15000, NULL, NULL};
  gt__ssh_io_lock(ssh);
  long previous = libssh2_session_get_timeout(ssh->session);
  int blocking = libssh2_session_get_blocking(ssh->session), status = GT_OK;
  LIBSSH2_CHANNEL *chan = command_channel(ssh, cmd, NULL, &budget, &status, false);
  libssh2_session_set_blocking(ssh->session, blocking);
  libssh2_session_set_timeout(ssh->session, previous);
  gt__ssh_io_unlock(ssh);
  free(cmd);
  if (!chan) {
    free(e);
    if (out_status)
      *out_status = (gt_status_t)status;
    return NULL;
  }
  e->chan = chan;
  e->closed = false;
  e->stdout_eof = false;
  e->stderr_eof = false;
  if (out_status)
    *out_status = GT_OK;
  return e;
}

/*
 * 读一路输出。stdout 先读、stderr 其次——两路都用**短时片**（100ms）读，
 * 分片之间释放 I/O 锁，故长命令（远端 find）期间浏览/传输仍能推进。
 * 只有两路都 EOF 才报 EOF（铁律 #6：不把超时折叠成 EOF）。
 */
static int64_t exec_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int gt_ssh_exec_read(gt_ssh_exec_t *e, uint8_t *buf, size_t cap,
                     int timeout_ms, bool *is_stderr) {
  if (is_stderr) *is_stderr = false;
  if (!e || !buf || cap == 0) return GT_EXEC_READ_ERROR;
  int64_t start = exec_mono_ms();
  for (;;) {
    gt__ssh_io_lock(e->ssh);
    long previous_timeout = libssh2_session_get_timeout(e->sess);
    libssh2_session_set_timeout(e->sess, 100);
    ssize_t n = -1;
    bool fromerr = false;
    bool hard = false;
    if (!e->stdout_eof) {
      ssize_t a = libssh2_channel_read(e->chan, (char *)buf, cap);
      if (a > 0) n = a;
      else if (a == 0) e->stdout_eof = true;
      else if (a != LIBSSH2_ERROR_TIMEOUT && a != LIBSSH2_ERROR_EAGAIN) hard = true;
    }
    if (n < 0 && !hard && !e->stderr_eof) {
      ssize_t b = libssh2_channel_read_stderr(e->chan, (char *)buf, cap);
      if (b > 0) { n = b; fromerr = true; }
      else if (b == 0) e->stderr_eof = true;
      else if (b != LIBSSH2_ERROR_TIMEOUT && b != LIBSSH2_ERROR_EAGAIN) hard = true;
    }
    bool both_eof = e->stdout_eof && e->stderr_eof;
    char *emsg = NULL;
    if (hard) libssh2_session_last_error(e->sess, &emsg, NULL, 0);
    /* The polling slice belongs to this read, not sibling SFTP channels. */
    libssh2_session_set_timeout(e->sess, previous_timeout);
    gt__ssh_io_unlock(e->ssh);

    if (n > 0) { if (is_stderr) *is_stderr = fromerr; return (int)n; }
    if (both_eof) return GT_EXEC_READ_EOF;
    if (hard) {
      gt__set_error("ssh_exec read: %s", emsg ? emsg : "unknown");
      return GT_EXEC_READ_ERROR;
    }
    if (timeout_ms >= 0 && exec_mono_ms() - start >= timeout_ms)
      return GT_EXEC_READ_TIMEOUT;
  }
}

bool gt_ssh_exec_exit_status(gt_ssh_exec_t *e, int *exit_code) {
  if (!e) return false;
  gt__ssh_io_lock(e->ssh);
  /* 必须先 close：libssh2 要在通道关闭时才把 exit-status 收全。 */
  if (!e->closed) {
    libssh2_channel_close(e->chan);
    e->closed = true;
  }
  int rc = libssh2_channel_get_exit_status(e->chan);
  gt__ssh_io_unlock(e->ssh);
  if (exit_code) *exit_code = rc;
  return true;
}

void gt_ssh_exec_close(gt_ssh_exec_t *e) {
  if (!e) return;
  gt__ssh_io_lock(e->ssh);
  if (!e->closed) {
    libssh2_channel_send_eof(e->chan);
    libssh2_channel_close(e->chan);
    e->closed = true;
  }
  libssh2_channel_free(e->chan);
  gt__ssh_io_unlock(e->ssh);
  free(e);
}

#else /* !GT_HAVE_LIBSSH2 —— 显式 UNSUPPORTED */
int gt__command_ssh(gt_ssh_t *s,const char *c,const gt_command_budget_t *b,gt_command_result_t *r){
  (void)s;(void)c;(void)b;memset(r,0,sizeof *r);gt__set_error("SSH: libssh2 unavailable");return GT_ERR_UNSUPPORTED;
}
void *gt__ssh_command_open(gt_ssh_t *s,const char *c,const gt_shell_opts_t *o,uint32_t t,bool (*f)(void *),void *u,gt_status_t *out_status){
  if(out_status)*out_status=GT_ERR_UNSUPPORTED;
  (void)s;(void)c;(void)o;(void)t;(void)f;(void)u;gt__set_error("SSH: libssh2 unavailable");return NULL;
}


struct gt_ssh_connector {int unused;};
gt_status_t gt__ssh_connector_open(gt_ssh_t **s,const gt_ssh_connector_config_t *c,const gt_allocator_t *a,gt_ssh_connector_t **o){(void)s;(void)c;(void)a;if(o)*o=NULL;return GT_ERR_UNSUPPORTED;}
gt_ssh_connector_phase_t gt__ssh_connector_step(gt_ssh_connector_t *c){(void)c;return GT_SSH_CONNECT_FAILED;}
bool gt__ssh_connector_info(const gt_ssh_connector_t *c,gt_ssh_connector_info_t *o){(void)c;if(o)memset(o,0,sizeof(*o));return false;}
gt_status_t gt__ssh_connector_decide(gt_ssh_connector_t *c,gt_hostkey_decision_t d){(void)c;(void)d;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_connector_supply(gt_ssh_connector_t *c,gt_ssh_t **s){(void)c;(void)s;return GT_ERR_UNSUPPORTED;}
bool gt__ssh_connector_take(gt_ssh_connector_t *c,gt_ssh_t **o){(void)c;if(o)*o=NULL;return false;}
void gt__ssh_connector_cancel(gt_ssh_connector_t *c){(void)c;}
void gt__ssh_connector_close(gt_ssh_connector_t *c){(void)c;}


int gt__ssh_borrow(gt_ssh_t *s,void **p,int *fd){(void)s;if(p)*p=NULL;if(fd)*fd=-1;return GT_ERR_UNSUPPORTED;}
void gt__ssh_abort(gt_ssh_t *s){(void)s;}
int gt_ssh_abort(gt_ssh_t *ssh) { return ssh ? GT_ERR_UNSUPPORTED : GT_ERR_INVALID_ARG; }



struct gt_ssh {
  /* 自有拷贝：opts 里的指针来自调用方，可能在 new 返回后失效（Swift 传
   * 临时 C 串），故此处全部 strdup 拥有。 */
  char *host;
  char *username;
  uint16_t port;
  gt_hostkey_decider_fn decide;
  void *decide_ud;
};

gt_ssh_t *gt_ssh_new(const gt_ssh_opts_t *opts) {
  if (!opts || !opts->host || !opts->username) {
    gt__set_error("gt_ssh_new: host/username required");
    return NULL;
  }
  gt_ssh_t *s = gt__alloc(sizeof *s);
  if (s) {
    s->host = gt__strdup(opts->host);
    s->username = gt__strdup(opts->username);
    s->port = opts->port ? opts->port : 22;
  }
  return s;
}
int gt_ssh_auth_password(gt_ssh_t *ssh, const char *password) {
  (void)ssh; (void)password;
  return GT_ERR_UNSUPPORTED;
}
int gt_ssh_auth_private_key(gt_ssh_t *ssh, const char *pem, const char *passphrase) {
  (void)ssh; (void)pem; (void)passphrase;
  return GT_ERR_UNSUPPORTED;
}
int gt_ssh_set_hostkey_policy(gt_ssh_t *ssh, gt_hostkey_decider_fn decide, void *ud) {
  (void)ssh; (void)decide; (void)ud;
  return GT_ERR_UNSUPPORTED;
}
int gt_ssh_connect(gt_ssh_t *ssh) {
  (void)ssh;
  gt__set_error("gt_ssh_connect: libssh2 未编译进核心（显式 UNSUPPORTED）");
  return GT_ERR_UNSUPPORTED;
}
int gt_ssh_handshake(gt_ssh_t *ssh) {
  (void)ssh;
  gt__set_error("gt_ssh_handshake: libssh2 未编译进核心（显式 UNSUPPORTED）");
  return GT_ERR_UNSUPPORTED;
}
int gt_ssh_authenticate(gt_ssh_t *ssh) {
  (void)ssh;
  gt__set_error("gt_ssh_authenticate: libssh2 未编译进核心（显式 UNSUPPORTED）");
  return GT_ERR_UNSUPPORTED;
}
int gt_ssh_auth_methods(gt_ssh_t *ssh) {
  (void)ssh;
  return 0; /* 未握手，没有服务器认证方式信息。 */
}
void gt_ssh_close(gt_ssh_t *ssh) {
  if (!ssh) return;
  free(ssh->host);
  free(ssh->username);
  free(ssh);
}
gt_sftp_t *gt_ssh_sftp(gt_ssh_t *ssh) {
  (void)ssh;
  gt__set_error("gt_ssh_sftp: libssh2 未编译进核心（显式 UNSUPPORTED）");
  return NULL;
}

void *gt__ssh_shell_open_controlled(gt_ssh_t *s, const gt_shell_opts_t *o,
    uint32_t t, bool (*f)(void *), void *u, gt_status_t *status) {
  (void)s; (void)o; (void)t; (void)f; (void)u;
  if (status) *status = GT_ERR_UNSUPPORTED;
  gt__set_error("SSH shell: libssh2 unavailable");
  return NULL;
}
void *gt__ssh_shell_open(gt_ssh_t *ssh, const char *term, uint16_t cols,
                         uint16_t rows, uint32_t px_w, uint32_t px_h) {
  (void)ssh; (void)term; (void)cols; (void)rows; (void)px_w; (void)px_h;
  gt__set_error("gt_transport_open_ssh_shell: libssh2 未编译进核心");
  return NULL;
}
int gt__ssh_shell_read(void *ch, uint8_t *b, size_t c, int t) {
  (void)ch; (void)b; (void)c; (void)t; return GT_TRANSPORT_READ_ERROR;
}
int gt__ssh_shell_write(void *ch, const uint8_t *d, size_t l) {
  (void)ch; (void)d; (void)l; return GT_ERR_UNSUPPORTED;
}
int gt__ssh_shell_resize(void *ch, uint16_t c, uint16_t r, uint32_t w, uint32_t h) {
  (void)ch; (void)c; (void)r; (void)w; (void)h; return GT_ERR_UNSUPPORTED;
}
bool gt__ssh_shell_exited(void *ch, int *e) { (void)ch; if (e) *e = 0; return false; }
void gt__ssh_shell_close(void *ch) { (void)ch; }

void gt__ssh_io_lock(gt_ssh_t *ssh) { (void)ssh; }
void gt__ssh_io_unlock(gt_ssh_t *ssh) { (void)ssh; }

gt_ssh_exec_t *gt_ssh_exec(gt_ssh_t *ssh, const char *command,
                           gt_status_t *out_status) {
  (void)ssh; (void)command;
  gt__set_error("gt_ssh_exec: libssh2 未编译进核心（显式 UNSUPPORTED）");
  if (out_status) *out_status = GT_ERR_UNSUPPORTED;
  return NULL;
}
int gt_ssh_exec_read(gt_ssh_exec_t *e, uint8_t *b, size_t c, int t, bool *is_err) {
  (void)e; (void)b; (void)c; (void)t;
  if (is_err) *is_err = false;
  return GT_EXEC_READ_ERROR;
}
bool gt_ssh_exec_exit_status(gt_ssh_exec_t *e, int *code) {
  (void)e; if (code) *code = 0; return false;
}
void gt_ssh_exec_close(gt_ssh_exec_t *e) { (void)e; }

#endif /* GT_HAVE_LIBSSH2 */
