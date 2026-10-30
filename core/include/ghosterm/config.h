/*
 * ghosterm/config.h — ssh_config 的解析 / 无损写回 / 求值
 *
 * 铁律 #4「文件即真相」：解析→编辑→写回必须无损往返，保留注释、未知指令、
 * 块顺序、缩进与行尾。语义以真 `ssh -G` 对拍为准，锚点是
 * core/tests/fixtures/golden.{conf,json,effective.tsv}
 * （由 core/tests/scripts/regen-golden.sh 同源再生，**勿手改**）。
 */
#ifndef GHOSTERM_CONFIG_H
#define GHOSTERM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gt_config gt_config_t;

/* 解析。失败返回 NULL，错误见 gt_last_error()。path 不存在时按空配置处理。 */
gt_config_t *gt_config_parse_file(const char *path);
gt_config_t *gt_config_parse(const char *text, size_t len);
void gt_config_free(gt_config_t *cfg);

/*
 * 无损写回。返回堆分配的 NUL 结尾字符串（调用方 free）。
 * 未做任何编辑时，输出与输入**逐字节相同**——本模块的头号不变量。
 */
char *gt_config_to_string(const gt_config_t *cfg);

/* 文件中出现的 Host 模式，按声明顺序（UI 列表用）。 */
size_t gt_config_host_count(const gt_config_t *cfg);
const char *gt_config_host_pattern(const gt_config_t *cfg, size_t index);

/* Borrowed metadata: strings remain valid until cfg is edited/freed.
 * source_path is empty for in-memory main text; source_line is 1-based,
 * or 0 for a default. source_header is empty for global/default values.
 * explicit_value is NULL unless the target main-file block sets this field;
 * an earlier rule/Include may still win over that explicit value. */
typedef struct {
  const char *explicit_value;
  const char *source_path;
  const char *source_header;
  size_t source_line;
  bool source_included;
  bool effective_is_explicit;
} gt_config_field_info_t;

typedef struct {
  const char *header;
  const char *source_path;
  size_t source_line;
  size_t pattern_count;
  size_t matching_blocks; /* literal alias occurrences by block, case-insensitive */
  bool included;
  gt_config_field_info_t hostname;
  gt_config_field_info_t user;
  gt_config_field_info_t port;
} gt_config_host_info_t;

/* Inspect the first literal Host block for alias, including its shared patterns
 * and the winning source for HostName/User/Port. Does not modify cfg.
 * GT_ERR_NOT_FOUND for no literal block, GT_ERR_INVALID_ARG for invalid input.
 * Only a single main-file block is editable by the alias-based edit API. */
int gt_config_inspect_host(const gt_config_t *cfg, const char *alias,
                           gt_config_host_info_t *out);

/* IdentityFile is cumulative, not first-wins. Enumerate parsed entries from
 * matching rules and from the first literal Host block, in source order.
 * Values are unquoted parser tokens, without tilde/token expansion or defaults.
 * explicit_value means a main-file directive in that literal block; effective
 * means its Host patterns match alias. An explicit entry may be ineffective
 * (e.g. "Host demo !demo"); an inline Include is never an editable explicit entry.
 * Host ownership/editability still comes from gt_config_inspect_host.
 * All strings are borrowed until cfg is edited/freed. No IO or evaluation of Match.
 * OK supplies one entry; NOT_FOUND means end or no literal Host block.
 * Any Match, or a relevant IdentityFile with other than one argument, returns
 * UNSUPPORTED before any entry is returned. INVALID_ARG for invalid input.
 * out is zeroed on every non-OK result. */
typedef struct {
  const char *value;
  const char *source_path;
  const char *source_header;
  size_t source_line;
  bool included;
  bool explicit_value;
  bool effective;
} gt_config_identity_info_t;
int gt_config_identity_at(const gt_config_t *cfg, const char *alias,
                          size_t index, gt_config_identity_info_t *out);

/* Current ordinary SSH client profile, shared by native hosts. Enumerate from
 * index 0: UNSUPPORTED supplies one borrowed issue; OK means end (zeroed out).
 * No IO, DNS, commands, defaults or mutation. Strings expire on edit/free.
 * Only explicit winning directives are checked; known list directives accumulate.
 * Unknown applicable directives and any Match block fail conservatively, since
 * their execution/scope cannot be established. This is not an OpenSSH validator.
 * INVALID_ARG for null/empty host or null output/config. A clear result does not
 * establish platform availability, credentials, or remote capabilities. */
typedef struct {
  const char *keyword;
  const char *source_path;
  size_t source_line;
  bool included;
} gt_config_connection_issue_t;
int gt_config_check_connection(const gt_config_t *cfg, const char *host,
                               size_t index, gt_config_connection_issue_t *out);

/* ------------------------------------------------------------------ */
/* 默认 IdentityFile 候选链策略                                        */
/* ------------------------------------------------------------------ */

/*
 * OpenSSH 只在「配置里一条 IdentityFile 都没有」时才追加默认候选链，
 * 且该链**随版本/构建变化**（实测证据见 core/tests/README.md）：
 *   9.6p1 : rsa ecdsa ecdsa_sk ed25519 ed25519_sk xmss dsa            （7 条）
 *   10.3p1: rsa ecdsa ecdsa_sk ed25519 ed25519_sk                       （5 条，本机实测）
 *   master: rsa ecdsa ecdsa_sk ed25519 ed25519_sk mldsa44_ed25519       （6 条，上游源码）
 * 故此处做成显式策略，而不是写死的常量。每条链都有出处，见 const 定义处注释。
 */
typedef enum {
  GT_IDENTITY_POLICY_OPENSSH_9_6 = 0,   /* 7 条（上游 V_9_6_P1 readconf.c） */
  GT_IDENTITY_POLICY_OPENSSH_10_3 = 1,  /* 5 条（本机 `ssh -G -F /dev/null` 实测） */
  GT_IDENTITY_POLICY_OPENSSH_MASTER = 2,/* 6 条（上游 master，含抗量子混合密钥） */
  GT_IDENTITY_POLICY_NONE = 3,          /* 不回落：只返回显式条目 */
  GT_IDENTITY_POLICY_CUSTOM = 4,        /* 使用 custom_identity_files */
} gt_identity_policy_t;

/* 求值选项：NULL 时 default_user 取当前进程用户，policy 取 OPENSSH_9_6。 */
typedef struct {
  const char *default_user;              /* NULL → 当前进程用户 */
  gt_identity_policy_t identity_policy;  /* 默认候选链策略 */
  const char *const *custom_identity_files; /* POLICY_CUSTOM 时的链（NULL 结尾） */
} gt_resolve_opts_t;

/* ------------------------------------------------------------------ */
/* 求值（typed 视图）                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
  char *user;
  char *hostname;
  int port;
  int forward_agent;        /* -1 = 未知；0/1 */
  int compression;          /* -1 = 未知；0/1 */
  int server_alive_interval;
  char **identity_files;    /* NULL 结尾 */
  char **proxy_jump;        /* NULL 结尾（已按 ',' 拆分） */
  char **setenv;            /* NULL 结尾（形如 "K=V"） */
  char **local_forward;     /* NULL 结尾（已按 ssh -G 归一化） */
} gt_resolved_t;

/*
 * 语义：单值字段（user/hostname/port/…）按出现位置 first-wins；
 * IdentityFile 跨匹配块累积，无显式条目时按 identity_policy 回落；
 * SetEnv 只取首个匹配指令；ProxyJump 按 ',' 拆分；LocalForward 归一化。
 */
gt_resolved_t *gt_config_resolve(const gt_config_t *cfg, const char *host,
                                 const gt_resolve_opts_t *opts);
void gt_resolved_free(gt_resolved_t *r);

/* ------------------------------------------------------------------ */
/* 求值（通用视图，等价 `ssh -G <host>`）                              */
/* ------------------------------------------------------------------ */

/*
 * 取生效值，等价于 `ssh -G` 的对应行：
 *   - 多值键（identityfile / localforward / setenv）以 '\n' 连接；
 *   - 键不在支持集内 → 返回 -1（不静默编造）；
 *   - 缓冲区不足 → 返回所需长度（> cap），不写入半个值。
 * 返回值：写入字节数（不含结尾 NUL），或 -1。
 */
int gt_config_effective(const gt_config_t *cfg, const char *host,
                        const char *key, const gt_resolve_opts_t *opts,
                        char *buf, size_t cap);

/* 支持集（用于对拍与 UI 的「生效配置」面板）。key_count 写入 *count。 */
const char *const *gt_config_supported_keys(size_t *count);

/* ------------------------------------------------------------------ */
/* 编辑（稀疏补丁，铁律 #4：除目标行外必须逐字保持不变）               */
/* ------------------------------------------------------------------ */

/*
 * 一条编辑指令，按 ssh_config 指令名（大小写不敏感）：
 *   values == NULL 或 values[0] == NULL → 删除该指令在目标块内的所有行；
 *   否则按该指令的多值语义渲染：
 *     · 累积型（IdentityFile / LocalForward / RemoteForward / DynamicForward /
 *       CertificateFile / SendEnv）：一行一条目；
 *     · SetEnv：一行，token 以空格连接；
 *     · 其余（标量 / extra）：一行，token 以空格连接。
 * 渲染时**沿用块内既有行的缩进与分隔符**（`=` 或空格，见
 * writeback `restyle_keep_equals_separator_*`）；块内没有该指令时用
 * 2 空格缩进 + 空格分隔，追加到块尾。
 */
typedef struct {
  const char *keyword;
  const char *const *values; /* NULL 结尾 */
} gt_config_edit_t;

/*
 * 对 alias 对应的 Host 块做稀疏编辑：
 *   - reset=false：原地补丁；找不到 alias 对应块 → GT_ERR_NOT_FOUND；
 *   - reset=true ：先删除该 alias 的整块，再按 edits 在文件末尾新建块。
 *   - alias 属于包含文件或多个同名块 → GT_ERR_UNSUPPORTED，不修改配置。
 * 除点名的目标行外，其余行**逐字节不变**（本函数的头号不变量）。
 * 成功返回 GT_OK，失败返回 GT_ERR_*（错误见 gt_last_error）。
 */
int gt_config_edit(gt_config_t *cfg, const char *alias,
                   const gt_config_edit_t *edits, size_t nedits, bool reset);

/* Rename one positive literal token in one main-file Host header. New names:
 * 1..128 ASCII letters/digits/._-, first character a letter/digit.
 * Preserves every byte outside that token, including sibling patterns, comments,
 * separators and line endings. Case-only changes are allowed. Other literal
 * aliases (including included files) collide case-insensitively: INVALID_ARG.
 * Missing alias: NOT_FOUND. Included blocks, duplicate blocks or repeated source
 * tokens in one header: UNSUPPORTED. Invalid input/pattern: INVALID_ARG.
 * A failed reparse (e.g. vanished Include) returns IO without changing cfg.
 * Does not write files or update external references/active sessions. A new name
 * may match different wildcard rules or change the default HostName.
 * Success invalidates borrowed metadata, just like edit. */
int gt_config_rename_host(gt_config_t *cfg, const char *alias, const char *new_alias);

#ifdef __cplusplus
}
#endif

#endif /* GHOSTERM_CONFIG_H */
