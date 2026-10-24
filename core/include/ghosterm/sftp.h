/*
 * ghosterm/sftp.h — SFTP 客户端（libssh2_sftp，外包一层稳的 API）
 *
 * 注：libssh2 的 SFTP 历史上有过堆越界漏洞，故一律经此薄封装，
 * 不让上层直接碰 libssh2 句柄；越界/边界参数在本层拦。
 *
 * 线程纪律（铁律 #1/#2）：一个 gt_sftp_t 的全部 libssh2 调用经**一把
 * 每会话 I/O 锁**串行化——请求/响应操作（list/stat/... ）由调用线程在锁内
 * 完成，传输 job 由**单条 I/O worker 线程**分块推进，二者不并发触同一
 * 会话。上层仍应在**同一条会话线程**上发起这些调用（阻塞语义止步于 core）。
 *
 * 三态（铁律 #6）：job 结果 OK / FAILED / CANCELLED 不折叠；能力缺失
 * （libssh2 未编入）一律显式 GT_ERR_UNSUPPORTED，绝不静默降级。
 */
#ifndef GHOSTERM_SFTP_H
#define GHOSTERM_SFTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ghosterm/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gt_sftp gt_sftp_t;

typedef struct {
  uint64_t size;
  uint32_t mode;       /* POSIX mode；0 = 未知 */
  bool is_dir;
  bool is_symlink;
  int64_t mtime_unix;  /* <0 = 未知 */
  uint32_t uid;        /* has_owner=false 时无意义 */
  uint32_t gid;        /* has_owner=false 时无意义 */
  bool has_owner;      /* 服务器给了 UIDGID 属性 */
} gt_sftp_stat_t;

typedef struct {
  const char *name;
  char *full_path;     /* 堆分配，随 gt_sftp_entry_t 一起释放 */
  gt_sftp_stat_t stat;
} gt_sftp_entry_t;

/* 列目录。成功返回 0；目录条目写入 out（调用方 gt_sftp_list_free）。 */
int gt_sftp_list(gt_sftp_t *sftp, const char *path,
                 gt_sftp_entry_t **out, size_t *out_count);
void gt_sftp_list_free(gt_sftp_entry_t *entries, size_t count);

int gt_sftp_stat(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out);

/* lstat：**不跟随**符号链接（报链接自身的属性）。 */
int gt_sftp_lstat(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out);

/* readlink：读符号链接的目标路径（NUL 结尾写入 buf）。 */
int gt_sftp_readlink(gt_sftp_t *sftp, const char *path, char *buf, size_t cap);

/* realpath：规范化路径（服务器端 SSH_FXP_REALPATH）。 */
int gt_sftp_realpath(gt_sftp_t *sftp, const char *path, char *buf, size_t cap);

/* ---- 写侧原语（三态：失败如实返回，不折叠成 OK） ---- */

int gt_sftp_mkdir(gt_sftp_t *sftp, const char *path, uint32_t mode);
int gt_sftp_rmdir(gt_sftp_t *sftp, const char *path);   /* 仅空目录 */
int gt_sftp_remove(gt_sftp_t *sftp, const char *path);  /* 文件 / 符号链接本身 */
int gt_sftp_remove_all(gt_sftp_t *sftp, const char *path); /* 递归删除（深度护栏） */
int gt_sftp_rename(gt_sftp_t *sftp, const char *from, const char *to);
int gt_sftp_chmod(gt_sftp_t *sftp, const char *path, uint32_t mode);

/* ---- 递归枚举（调用线程同步执行；visitor 返回非 0 即停止） ---- */

typedef struct {
  const char *path;          /* 完整路径（visitor 调用期间有效） */
  const char *name;          /* 基本名（同上） */
  const gt_sftp_stat_t *stat; /* 该条目自身属性（不跟随链接） */
} gt_sftp_walk_ent_t;

/* 返回 0 = 继续；非 0 = 请求停止（walk 返回 GT_ERR_CANCELLED）。 */
typedef int (*gt_sftp_walk_fn)(void *ud, const gt_sftp_walk_ent_t *ent);

/* 深度优先枚举 root 下的所有条目（含目录本身的第一层孩子；不含 root）。
 * max_depth < 0 → 无限制（仍有编译期护栏）。 */
int gt_sftp_walk(gt_sftp_t *sftp, const char *root, int max_depth,
                 gt_sftp_walk_fn fn, void *ud);

/* ---- 传输 job ---- */

/* 下载/上传：返回 job id（0 = 失败），进度经 GT_EVENT_PROGRESS 上报。 */
uint64_t gt_sftp_download(gt_sftp_t *sftp, const char *remote, const char *local);
uint64_t gt_sftp_upload(gt_sftp_t *sftp, const char *local, const char *remote);

/* 取消一个进行中的 job。 */
int gt_sftp_cancel(gt_sftp_t *sftp, uint64_t job_id);

/* 暂停/继续一个进行中的 job（协作式：分块之间检查标志）。 */
int gt_sftp_pause(gt_sftp_t *sftp, uint64_t job_id, bool paused);

void gt_sftp_close(gt_sftp_t *sftp);

#ifdef __cplusplus
}
#endif

#endif /* GHOSTERM_SFTP_H */
