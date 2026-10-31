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
  bool has_size;       /* false is unknown, not an empty file */
  bool is_regular;     /* false for unknown type, links and special files */
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

/* Resolve a real directory and read its complete immediate children under one
 * budget, including closing the directory handle. Dedicated SSH/SFTP IO owner
 * only: no other channels, concurrent callers or transfer jobs on this owner.
 * timeout_ms and max_entries must be nonzero; max_entries <= 65536. Paths and
 * result buffers are limited to 65536 bytes; names must be single components.
 * Success publishes resolved_path and entries together; free with list_free.
 * Every failure clears all outputs; never publishes a partial/empty success.
 * Preflight rejection leaves the connection usable. Failure after IO aborts
 * the SSH connection: close SFTP, then SSH; retry requires a fresh connection.
 * Cancellation is CANCELLED; timeout/server/protocol/limit failure is IO.
 * The callback runs on the owner and must not reenter Core. Existing list,
 * transfer and close contracts are unchanged; no lifetime ownership transfer. */
int gt_sftp_browse_controlled(gt_sftp_t *sftp, const char *path,
    char *resolved_path, size_t resolved_cap,
    gt_sftp_entry_t **out, size_t *out_count,
    uint32_t timeout_ms, size_t max_entries,
    bool (*cancelled)(void *), void *context);

/* Read-only, bounded regular-file preview. Dedicated owner rules and failure
 * invalidation are identical to browse_controlled. One budget covers lstat,
 * read-only open, fstat, reads through EOF and successful close. Symlinks,
 * special files and unknown types are UNSUPPORTED; SFTP v3 cannot guarantee
 * atomic no-follow against a concurrent path replacement. Open-handle fstat
 * checks the actual file type and size again. Unknown size is allowed, but
 * reads never publish more than capacity bytes; known size must match EOF.
 * capacity is 1..1048576 bytes, timeout_ms is nonzero. Caller owns buffer.
 * No partial result: valid-sized buffer is zeroed and out_count is zero on
 * failure; bytes are published only after close succeeds. Empty file is OK.
 * Core returns raw bytes; decoding, binary rejection and preview lifetime
 * belong to the platform. No local file or transfer job is created. */
int gt_sftp_preview_controlled(gt_sftp_t *sftp, const char *path,
    uint8_t *buffer, size_t capacity, size_t *out_count, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context);

/* Synchronous IO observations, never completion/publication acknowledgements.
 * RECEIVE counts bytes written into the caller's staging fd. CREATE counts
 * libssh2-reported server acknowledgements: a lower bound because writes may
 * be pipelined. total_known=false means total_bytes is 0, not an empty file.
 * A peer may send more bytes than advertised; preserve observed bytes and let
 * the operation's existing consistency/limit checks reject it. Reaching total
 * does not acknowledge CLOSE, final verification or destination publication.
 * Called on the dedicated IO owner after OPEN/FSTAT (initial zero) and after
 * positive IO only. The POD pointer is valid only during this call; copy it.
 * Observer must return promptly and must not reenter Core. Its time consumes
 * the same operation budget. No callback after the observed function returns.
 * NULL observer is supported; no global observer or queued event is retained. */
typedef enum {
  GT_SFTP_PROGRESS_RECEIVE = 1,
  GT_SFTP_PROGRESS_CREATE = 2,
} gt_sftp_progress_kind_t;
typedef struct {
  gt_sftp_progress_kind_t kind;
  uint64_t bytes;
  uint64_t total_bytes;
  bool total_known;
} gt_sftp_progress_t;
typedef void (*gt_sftp_progress_fn)(const gt_sftp_progress_t *progress, void *context);

/* Bounded remote regular file -> task-owned local staging fd. Dedicated owner
 * only, with the same invalidation rules as preview_controlled. Caller retains
 * an empty writable regular fd (no O_APPEND), exclusively owns it until return,
 * then closes/discards it on failure. pwrite preserves offset; Core never closes
 * or truncates the descriptor. Partial bytes may remain on failure; out_count
 * stays zero. max_bytes is 0..INT64_MAX (zero accepts only an empty source).
 * All network operations share timeout_ms:
 * LSTAT/READ OPEN/FSTAT/READ through EOF/final FSTAT/CLOSE. Observed size/mtime
 * changes, missing previously known attributes, premature EOF, limit and local
 * write errors fail explicitly. Unknown size remains bounded by an extra byte
 * probe. SFTP v3 cannot provide atomic no-follow or an atomic content snapshot.
 * Successful CLOSE and final cancellation check precede count publication.
 * No job/event, destination commit or fsync guarantee. Local syscalls are not
 * hard-preemptible. Caller must not let native fd duplicates outlive its lease. */
int gt_sftp_receive_controlled(gt_sftp_t *sftp, const char *path, int destination_fd,
    uint64_t max_bytes, uint64_t *out_count, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context);
/* Same contract and results as receive_controlled, with per-call IO observations. */
int gt_sftp_receive_observed(gt_sftp_t *sftp, const char *path, int destination_fd,
    uint64_t max_bytes, uint64_t *out_count, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context,
    gt_sftp_progress_fn observer, void *observer_context);

/* Exclusive creation for task-owned remote staging paths. Does not overwrite,
 * rename, remove or publish a final destination. Dedicated owner only; no jobs
 * or other channels. Caller retains a readable regular fd and must keep its
 * identity/content stable until return. pread preserves its offset; Core never
 * closes it. expected_size must match fstat, checked before OPEN and after IO.
 * Network OPEN/WRITE/CLOSE share timeout_ms; local filesystem syscalls cannot
 * be preempted. The cancellation callback must not reenter Core.
 * Rejected means an explicit server rejection, never "target absent"; UNKNOWN
 * means OPEN may have succeeded without a usable reply. CREATED may contain
 * partial data. COMPLETE requires all bytes and successful CLOSE; it wins over
 * cancellation arriving after that acknowledgement. No fsync/durability claim.
 * Any error after attempted IO invalidates this owner. No automatic unlink:
 * path replacement races make deletion unsafe. Caller must retain outcome even
 * on error. bytes_acked is a lower bound, not a partial-file size or progress
 * guarantee (libssh2 may pipeline writes). No terminal/job events are emitted. */
typedef enum {
  GT_SFTP_CREATE_NOT_ATTEMPTED = 0,
  GT_SFTP_CREATE_UNKNOWN = 1,
  GT_SFTP_CREATE_REJECTED = 2,
  GT_SFTP_CREATE_CREATED = 3,
  GT_SFTP_CREATE_COMPLETE = 4,
} gt_sftp_create_state_t;
typedef struct {
  gt_sftp_create_state_t state;
  uint64_t bytes_acked;
} gt_sftp_create_result_t;
int gt_sftp_create_controlled(gt_sftp_t *sftp, const char *path, int source_fd,
    uint64_t expected_size, gt_sftp_create_result_t *out, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context);
/* Same contract and results as create_controlled, with per-call IO observations. */
int gt_sftp_create_observed(gt_sftp_t *sftp, const char *path, int source_fd,
    uint64_t expected_size, gt_sftp_create_result_t *out, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context,
    gt_sftp_progress_fn observer, void *observer_context);

/* Publish a completed, caller-owned remote staging path WITHOUT overwrite.
 * Dedicated owner only, no jobs/other channels. Source LSTAT, destination
 * LSTAT, source recheck and RENAME share one positive budget. A regular source
 * must have known expected_size (0..INT64_MAX); directory requires size 0 and
 * a known directory type. Caller owns a complete tree and keeps it unchanged;
 * this call neither scans directory contents nor provides an atomic snapshot.
 * Only explicit NO_SUCH_FILE admits RENAME; query errors are not absence.
 * CONFLICT means destination LSTAT succeeded; REJECTED means RENAME received
 * a defined operation-error status (2..5 or 8..21). EOF, connection-loss and
 * unknown status codes leave UNKNOWN, as does a missing usable acknowledgement. NOT_ATTEMPTED says only that RENAME was not attempted, not
 * that destination is absent. remote_status is 0 unless a decoded server error
 * is available; generic FAILURE must not be classified as "already exists".
 * Successful RENAME wins over subsequent cancellation. No overwrite fallback,
 * automatic retry, unlink, fsync or portable atomicity/durability promise.
 * Metadata recheck cannot prevent source/path replacement races in SFTP v3.
 * Any failure after attempted IO invalidates this owner; preserve the result
 * even on error and reconnect before inspecting partial/unknown side effects. */
typedef enum {
  GT_SFTP_PUBLISH_NOT_ATTEMPTED = 0,
  GT_SFTP_PUBLISH_CONFLICT = 1,
  GT_SFTP_PUBLISH_UNKNOWN = 2,
  GT_SFTP_PUBLISH_REJECTED = 3,
  GT_SFTP_PUBLISH_PUBLISHED = 4,
} gt_sftp_publish_state_t;
typedef struct {
  gt_sftp_publish_state_t state;
  uint32_t remote_status;
} gt_sftp_publish_result_t;
int gt_sftp_publish_controlled(gt_sftp_t *sftp, const char *source, const char *destination,
    bool directory, uint64_t expected_size, gt_sftp_publish_result_t *out,
    uint32_t timeout_ms, bool (*cancelled)(void *), void *context);

/* Create one empty directory with requested mode 0700, never merge or retry.
 * Dedicated SSH/SFTP owner, no jobs/other channels. parent must be the frozen
 * canonical absolute path (no empty/dot/dot-dot components or trailing slash,
 * except root); name is one non-dot component, 1..255 bytes. Joined path <64KiB.
 * Parent LSTAT/REALPATH, target absence, MKDIR, child LSTAT/REALPATH and empty
 * directory OPEN/READ/CLOSE share one positive budget and cancellation signal.
 * Only explicit NO_SUCH_FILE admits MKDIR. CONFLICT means target LSTAT succeeded;
 * REJECTED means MKDIR received a defined operation error, not inferred absence.
 * UNKNOWN preserves a possible side effect without a usable acknowledgement;
 * CREATED preserves acknowledged creation even if verification fails/cancels.
 * COMPLETE means directory/observed path/emptiness and CLOSE were verified; a
 * subsequent cancellation does not undo it. remote_status retains decoded
 * server errors; generic FAILURE is not "already exists". No automatic remove,
 * rollback, fsync, durable journal or protection against arbitrary path writers:
 * SFTP v3 metadata/REALPATH checks cannot pin a parent inode. Preserve out even
 * on error; any failure after IO invalidates this owner, reconnect to inspect. */
typedef enum {
  GT_SFTP_MKDIR_NOT_ATTEMPTED = 0,
  GT_SFTP_MKDIR_CONFLICT = 1,
  GT_SFTP_MKDIR_UNKNOWN = 2,
  GT_SFTP_MKDIR_REJECTED = 3,
  GT_SFTP_MKDIR_CREATED = 4,
  GT_SFTP_MKDIR_COMPLETE = 5,
} gt_sftp_mkdir_state_t;
typedef struct {
  gt_sftp_mkdir_state_t state;
  uint32_t remote_status;
} gt_sftp_mkdir_result_t;
int gt_sftp_mkdir_controlled(gt_sftp_t *sftp, const char *parent, const char *name,
    gt_sftp_mkdir_result_t *out, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context);

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

/* 下载/上传：返回 job id（0 = 失败），进度经 GT_EVENT_PROGRESS 上报。
 * Legacy overwrite API: upload uses CREATE|TRUNC; download opens the remote
 * source before truncating the local target. Neither is an atomic/safe-copy
 * commit: failure/cancellation can leave partial data. OK requires successful
 * IO and both remote/local close; no fsync/durability guarantee. Queued jobs
 * cancelled before execution open neither file. Event delivery can overflow;
 * a missing completion event must never be inferred as success. */
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
