/* 内部共享声明（不对外发布） */
#ifndef GHOSTERM_INTERNAL_H
#define GHOSTERM_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ghosterm.h" /* gt_ssh_t / gt_transport_t 等 opaque 类型 */

/* 设置线程局部错误（printf 风格）。 */
void gt__set_error(const char *fmt, ...);

/* 分配失败即记录错误并返回 NULL 的包装。 */
void *gt__alloc(size_t n);
void *gt__realloc(void *p, size_t n);
char *gt__strdup(const char *s);
char *gt__strndup(const char *s, size_t n);

/* 事件入队（线程安全）。message 可为 NULL。 */
void gt__emit_status(int code, const char *message);

/* 通用事件入队：SFTP job（PROGRESS / JOB_DONE）等带 job_id 的低频事件。 */
void gt__emit_event(gt_event_kind_t kind, uint64_t job_id, int code,
                    const char *message);

/* ------------------------------------------------------------------ */
/* known_hosts 的枚举（真值来源是**文件**，不是 libssh2 的会话缓存）    */
/* ------------------------------------------------------------------ */

/*
 * 逐行枚举一个 known_hosts 文件里的公钥条目，对每一条调用 `cb`。
 *
 * 为什么要暴露它（两个都踩过）：
 *   1. **对拍闸门需要**：M1 起 fixture 的 known_hosts 只有 ssh-ed25519，
 *      而真机的 SSH 服务器常给 ECDSA——读侧曾经按 `ssh` 前缀过滤
 *      （照抄 OpenSSH 对 ssh-rsa 的写法），ECDSA 条目整条被跳过，于是
 *      「记过的主机每次重连都再问一遍用户」；
 *   2. **遍历必须覆盖所有密钥类型**：过滤键型和只用第一行都会漏。
 *
 * `cb` 收到的是**已切好的字段**（host 模式串、key type、base64 blob），
 * 返回 false 表示停止遍历。返回遍历的条目数。
 */
typedef bool (*gt__kh_visit_fn)(void *ud, const char *hosts, const char *key_type,
                                const char *key_b64, uint16_t port_scope);
size_t gt__kh_visit(const char *path, gt__kh_visit_fn cb, void *ud);

/* ------------------------------------------------------------------ */
/* SSH shell 通道（libssh2 封装在 ssh.c，transport.c 只经此接缝）       */
/* ------------------------------------------------------------------ */

/* 打开交互式 shell 通道（pty-req + shell）。失败返回 NULL。 */
void *gt__ssh_shell_open(gt_ssh_t *ssh, const char *term, uint16_t cols,
                         uint16_t rows, uint32_t px_w, uint32_t px_h);
/* 返回 >0 字节数 / GT_TRANSPORT_READ_TIMEOUT / GT_TRANSPORT_READ_EOF /
 * GT_TRANSPORT_READ_ERROR；语义同 gt_transport_read。 */
int gt__ssh_shell_read(void *ch, uint8_t *buf, size_t cap, int timeout_ms);
int gt__ssh_shell_write(void *ch, const uint8_t *data, size_t len);
int gt__ssh_shell_resize(void *ch, uint16_t cols, uint16_t rows, uint32_t px_w,
                         uint32_t px_h);
bool gt__ssh_shell_exited(void *ch, int *exit_code);
void gt__ssh_shell_close(void *ch);

/* ------------------------------------------------------------------ */
/* SFTP（libssh2_sftp 封装在 sftp.c，ssh.c 只经此接缝）                 */
/* ------------------------------------------------------------------ */

/* 在已连接会话上打开 SFTP 子系统。session 为 libssh2 会话指针（void*
 * 传递：internal.h 不暴露 libssh2 类型）。失败返回 NULL（错误已设置）。 */
gt_sftp_t *gt__sftp_from_session(void *session, gt_ssh_t *ssh);

/* ------------------------------------------------------------------ */
/* 每会话 I/O 锁（libssh2「一会话一线程」的执行体）                     */
/* ------------------------------------------------------------------ */

/*
 * 一把每个 gt_ssh_t 一把的互斥锁，**所有**触同一 libssh2 会话的操作
 * （SFTP 请求/响应、SFTP job 分块、exec 通道读写）都经它串行化——libssh2
 * 的会话不是线程安全的，先前「每 job 一条线程共用同一会话」是潜伏竞态。
 * 锁粒度刻意小：长任务（job/exec）分块持锁，块间释放，故浏览与传输可交替
 * 推进，而不是一方把另一方锁死。
 * 未编入 libssh2 时是 no-op。
 */
void gt__ssh_io_lock(gt_ssh_t *ssh);
void gt__ssh_io_unlock(gt_ssh_t *ssh);

/* Transfer a dedicated authenticated session to the single-owner IPC link.
 * No shell/SFTP/exec handles or aliases may remain. Clears *ssh on success. */
int gt__ssh_take(gt_ssh_t **ssh, void **session, int *socket);
int gt__ssh_borrow(gt_ssh_t *,void **session,int *socket);
void gt__ssh_abort(gt_ssh_t *);

#endif /* GHOSTERM_INTERNAL_H */
