/*
 * ghosterm/ssh.h — SSH 会话（libssh2 + mbedTLS/OpenSSL）
 *
 * 线程模型：一个 gt_ssh_t 绑一个 worker 线程；libssh2 是阻塞 API，
 * 绝不把它的调用放进 UI 线程。连接状态经 gt_core_poll_events 上报。
 */
#ifndef GHOSTERM_SSH_H
#define GHOSTERM_SSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ghosterm/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gt_ssh gt_ssh_t;
typedef struct gt_sftp gt_sftp_t;

/* 主机密钥策略：刻意三态，禁止把 UNKNOWN 折叠成 REJECT（铁律 #6）。 */
typedef enum {
  GT_HOSTKEY_ACCEPT = 0,
  GT_HOSTKEY_REJECT = 1,
  GT_HOSTKEY_UNKNOWN = 2, /* 需要上层确认 */
} gt_hostkey_decision_t;

typedef gt_hostkey_decision_t (*gt_hostkey_decider_fn)(
    void *userdata,
    const char *host,
    uint16_t port,
    const char *key_type,
    const char *fingerprint_sha256);

typedef struct {
  const char *host;     /* 必填 */
  uint16_t port;        /* 0 → 22 */
  const char *username; /* 必填 */
  const char *known_hosts_path; /* NULL → $HOME/.ssh/known_hosts */
  int connect_timeout_ms; /* TCP + 握手的总预算；0 → 默认 15000 */
} gt_ssh_opts_t;

/*
 * 服务器允许的认证方式（握手后、认证前才有值；未连接时为 0）。
 * 位与 gt_ssh_auth_method_t 对应。
 *
 * 为什么要有它：只带私钥去连一台**只收密码**的服务器时，认证必然失败，
 * 而失败原因（"publickey 不被接受"）从 libssh2 的错误串里读不出来——
 * 上层于是只能猜，最容易猜成「口令错了」再问一次口令，用户就看到
 * 「明明输了登录密码还是连不上」。有了这张方法表，上层能据此要密码。
 */
typedef enum {
  GT_SSH_AUTH_PUBLICKEY = 1 << 0,
  GT_SSH_AUTH_PASSWORD = 1 << 1,
  GT_SSH_AUTH_KEYBOARD = 1 << 2, /* keyboard-interactive（常与密码同义） */
  GT_SSH_AUTH_AGENT = 1 << 3,
  GT_SSH_AUTH_HOSTBASED = 1 << 4,
  GT_SSH_AUTH_NONE = 1 << 5,
} gt_ssh_auth_method_t;

int gt_ssh_auth_methods(gt_ssh_t *ssh);

gt_ssh_t *gt_ssh_new(const gt_ssh_opts_t *opts);

/*
 * 认证凭据。密钥只进平台安全存储；这里接收的是**已解锁**的材料，
 * 核心从不把明文落盘（铁律 #5）。
 */
int gt_ssh_auth_password(gt_ssh_t *ssh, const char *password);
int gt_ssh_auth_private_key(gt_ssh_t *ssh, const char *pem, const char *passphrase);

int gt_ssh_set_hostkey_policy(gt_ssh_t *ssh,
                              gt_hostkey_decider_fn decide,
                              void *userdata);

/*
 * 私钥是否需要口令（**只看文件头，不解密**）。
 *
 * 为什么单列一条：上层在认证前要决定「问不问用户私钥口令」。先前是
 * 「文件读得出来就先弹口令框」——没口令的钥匙（实测 `~/.ssh/id_rsa`）
 * 也被问了，用户以为自己的钥匙出了问题。问错比不问更糟。
 *
 * 返回值（三态，别折叠）：
 *   1  = 需要口令（PEM 的 Proc-Type: 4,ENCRYPTED / PKCS#8 ENCRYPTED /
 *        OpenSSH 新格式的 ciphername != "none"）
 *   0  = 不需要口令
 *   -1 = 认不出来（调用方**不要**据此弹口令框：libssh2 本来就用不了这种
 *        文件，弹了也解不开，应当把认证失败如实报给用户）
 */
int gt_ssh_key_needs_passphrase(const char *pem);

/*
 * 握手：TCP + 密钥交换 + 主机密钥裁决 + **服务器认证方式发现**。
 * 幂等（已握手直接返回 GT_OK）。与 gt_ssh_authenticate 拆开，是为了让上层
 * 在认证前就能读到 gt_ssh_auth_methods——只带私钥去连一台只收密码的服务器
 * 时，这张表是「该问密码还是该问口令」的唯一依据。
 */
int gt_ssh_handshake(gt_ssh_t *ssh);

/*
 * 认证：用 gt_ssh_auth_* 存下的凭据做一次 userauth。
 * **可在同一会话上多次调用**（换一把钥匙重试；libssh2 支持失败后重试）。
 * 必须先 gt_ssh_handshake。成功返回 GT_OK 且会话可用。
 */
int gt_ssh_authenticate(gt_ssh_t *ssh);

/* 一步到位（= 握手 + 认证），保持既有契约；阻塞。 */
int gt_ssh_connect(gt_ssh_t *ssh);
void gt_ssh_close(gt_ssh_t *ssh);

gt_sftp_t *gt_ssh_sftp(gt_ssh_t *ssh);

/* ------------------------------------------------------------------ */
/* exec 通道（远端单命令，用于远端加速：find / grep / du 等）            */
/* ------------------------------------------------------------------ */

/*
 * 在已连接会话上开一条 exec 通道跑一条命令（**非交互**：无 pty-req）。
 *
 * 用途：把「遍历大树 / 递归搜索」放到远端本地算，一次流式返回——
 * SFTP v3 的 readdir 是**每条目一次往返**，大目录上慢到不可用；远端
 * find/grep 只要一次往返。服务器禁 exec（纯 sftp 账号）时本函数返回
 * NULL 且错误为 GT_ERR_UNSUPPORTED —— **不静默降级**，由上层显式回退。
 *
 * 线程纪律与会话同规则：一条 exec 绑该会话；实现内部经每会话 I/O 锁
 * 与 SFTP 串行，不并发触同一 libssh2 会话。
 */
typedef struct gt_ssh_exec gt_ssh_exec_t;

/*
 * 返回句柄；失败返回 NULL，并把 *out_status（可空）设为具体错误码：
 *   GT_ERR_UNSUPPORTED = 服务器不允许 exec（纯 sftp 账号）——上层据此
 *                        显式回退 SFTP walk，**不是**静默降级；
 *   GT_ERR_IO          = 通道打不开；
 *   GT_ERR_INVALID_ARG = 会话未连接 / 命令为空。
 */
gt_ssh_exec_t *gt_ssh_exec(gt_ssh_t *ssh, const char *command,
                           gt_status_t *out_status);

/*
 * 读一路输出（stdout / stderr 择先到者）。三态返回，不折叠：
 *   >0  = 读到的字节数；*is_stderr 标示这批复自哪一路；
 *   0   = EOF（两路都到且排空）；
 *   -1  = 错误（详见 gt_last_error）；-2 = 超时（本次无数据，可重试）。
 */
enum {
  GT_EXEC_READ_TIMEOUT = -2,
  GT_EXEC_READ_ERROR = -1,
  GT_EXEC_READ_EOF = 0,
};
int gt_ssh_exec_read(gt_ssh_exec_t *e, uint8_t *buf, size_t cap,
                     int timeout_ms, bool *is_stderr);

/* 远端退出码（通道关闭后才有效）。成功写入返回 true。 */
bool gt_ssh_exec_exit_status(gt_ssh_exec_t *e, int *exit_code);

void gt_ssh_exec_close(gt_ssh_exec_t *e);

#ifdef __cplusplus
}
#endif

#endif /* GHOSTERM_SSH_H */
