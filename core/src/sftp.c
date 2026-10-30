/*
 * sftp.c — SFTP 客户端（libssh2_sftp 薄封装）。
 *
 * 契约（AGENTS.md「libssh2 的 SFTP 历史漏洞」条款）：
 *   - 上层**永不**接触 LIBSSH2_SFTP / LIBSSH2_SFTP_HANDLE；边界参数
 *     （路径长度、条目数上限、深度上限）在本层拦截；
 *   - **一会话一线程**（libssh2 会话非线程安全）：一个 gt_sftp_t 的**全部**
 *     libssh2 调用经每会话 I/O 锁（`gt__ssh_io_lock`）串行化。请求/响应操作
 *     （list/stat/mkdir/...）由调用线程在锁内完成；传输 job 由**单条 I/O
 *     worker 线程**分块推进，块间释放锁——故浏览与传输交替推进，不并发触
 *     同一会话（修掉先前「每 job 一条 detached 线程共用同一 LIBSSH2_SFTP」
 *     的潜伏竞态）。
 *
 * 三态（铁律 #6）：job 结果 OK / FAILED / CANCELLED 不折叠；取消与暂停都是
 * **协作式**（分块间检查标志，已写部分保留在磁盘）。未编译进 libssh2 时所有
 * 入口显式 GT_ERR_UNSUPPORTED——能力缺失显式报错，绝不静默降级。
 */
#include "ghosterm.h"
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GT_HAVE_LIBSSH2
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "internal.h"

#define SFTP_LOCK(s) gt__ssh_io_lock((s)->ssh)
#define SFTP_UNLOCK(s) gt__ssh_io_unlock((s)->ssh)

/* 单次 readdir/read 缓冲、条目上限、递归深度护栏。 */
#define GT_SFTP_MAX_ENTRIES 65536
#define GT_SFTP_CHUNK 32768
#define GT_SFTP_MAX_DEPTH 64

/* ------------------------------------------------------------------ */
/* job 与 worker 线程                                                   */
/* ------------------------------------------------------------------ */

typedef struct sftp_job sftp_job_t;
struct sftp_job {
  sftp_job_t *next_pending; /* pending 队列链 */
  sftp_job_t *next_all;     /* 全部在途 job 链（cancel/pause 查表用） */
  gt_sftp_t *sftp;
  char *remote;
  char *local;
  uint64_t job_id;
  bool upload; /* false = download */
  atomic_bool cancelled;
  atomic_bool paused;
};

struct gt_sftp {
  LIBSSH2_SFTP *sftp;
  gt_ssh_t *ssh;

  pthread_t worker;
  bool worker_started;
  pthread_mutex_t qlock;
  pthread_cond_t qcond;
  sftp_job_t *pending_head, *pending_tail; /* 待跑（FIFO） */
  sftp_job_t *all_head;                    /* 在途（含正在跑与排队） */
  bool quit;
};

static uint64_t g_next_job_id = 1;

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emit_progress(uint64_t job_id, uint64_t done, uint64_t total) {
  char msg[GT_EVENT_MESSAGE_MAX];
  snprintf(msg, sizeof msg, "sftp %llu/%llu",
           (unsigned long long)done, (unsigned long long)total);
  gt__emit_event(GT_EVENT_PROGRESS, job_id, 0, msg);
}

static void emit_job_done(uint64_t job_id, int result, const char *detail) {
  char msg[GT_EVENT_MESSAGE_MAX];
  snprintf(msg, sizeof msg, "sftp job %llu: %s",
           (unsigned long long)job_id, detail ? detail : "");
  gt__emit_event(GT_EVENT_JOB_DONE, job_id, result, msg);
}

/* ------------------------------------------------------------------ */
/* 打开 / 关闭                                                          */
/* ------------------------------------------------------------------ */

static void *worker_main(void *arg);

static gt_status_t open_status(int64_t deadline, bool (*cancelled)(void *), void *context) {
  if (cancelled && cancelled(context)) return GT_ERR_CANCELLED;
  if (mono_ms() >= deadline) return GT_ERR_IO;
  return GT_OK;
}

gt_sftp_t *gt_ssh_sftp_controlled(gt_ssh_t *ssh, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context, gt_status_t *status) {
  gt_status_t result = GT_ERR_INVALID_ARG;
  if (status) *status = result;
  if (!ssh || !timeout_ms) {
    gt__set_error("sftp open: connection and remaining budget required");
    return NULL;
  }
  const int64_t deadline = mono_ms() + timeout_ms;
  result = open_status(deadline, cancelled, context);
  if (result != GT_OK) {
    if (status) *status = result;
    gt__set_error("sftp open: cancelled or budget exhausted before IO");
    return NULL;
  }
  void *raw = NULL;
  int socket = -1;
  result = (gt_status_t)gt__ssh_borrow(ssh, &raw, &socket);
  if (result != GT_OK) {
    if (status) *status = result;
    gt__set_error("sftp open: authenticated connection unavailable");
    return NULL;
  }
  gt_sftp_t *out = gt__alloc(sizeof *out);
  if (!out) { if (status) *status = GT_ERR_INTERNAL; return NULL; }
  out->ssh = ssh;
  if (pthread_mutex_init(&out->qlock, NULL) != 0) {
    free(out); if (status) *status = GT_ERR_INTERNAL; return NULL;
  }
  if (pthread_cond_init(&out->qcond, NULL) != 0) {
    pthread_mutex_destroy(&out->qlock); free(out);
    if (status) *status = GT_ERR_INTERNAL;
    return NULL;
  }
  LIBSSH2_SESSION *session = raw;
  SFTP_LOCK(out);
  const int blocking = libssh2_session_get_blocking(session);
  const long timeout = libssh2_session_get_timeout(session);
  libssh2_session_set_blocking(session, 0);
  for (;;) {
    result = open_status(deadline, cancelled, context);
    if (result != GT_OK) break;
    out->sftp = libssh2_sftp_init(session);
    if (out->sftp) { result = open_status(deadline, cancelled, context); break; }
    if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
      result = GT_ERR_IO; break;
    }
    int direction = libssh2_session_block_directions(session);
    short events = 0;
    if (direction & LIBSSH2_SESSION_BLOCK_INBOUND) events |= POLLIN;
    if (direction & LIBSSH2_SESSION_BLOCK_OUTBOUND) events |= POLLOUT;
    struct pollfd wait = {socket, events ? events : POLLIN, 0};
    int rc = poll(&wait, 1, 10);
    if ((rc < 0 && errno != EINTR) || (rc > 0 && (wait.revents & (POLLERR | POLLNVAL)))) {
      result = GT_ERR_IO; break;
    }
  }
  if (result == GT_OK && pthread_create(&out->worker, NULL, worker_main, out) != 0)
    result = GT_ERR_INTERNAL;
  if (result == GT_OK) {
    out->worker_started = true;
    libssh2_session_set_blocking(session, blocking);
    libssh2_session_set_timeout(session, timeout);
    SFTP_UNLOCK(out);
    if (status) *status = GT_OK;
    return out;
  }
  /* An in-flight libssh2 state machine cannot be abandoned and then reused.
   * Abort before freeing a completed SFTP handle; unfinished init allocations
   * remain owned by the SSH session and are released by gt_ssh_close. */
  gt__ssh_abort(ssh);
  libssh2_session_set_blocking(session, 1);
  libssh2_session_set_timeout(session, 50);
  if (out->sftp) libssh2_sftp_shutdown(out->sftp);
  SFTP_UNLOCK(out);
  pthread_cond_destroy(&out->qcond); pthread_mutex_destroy(&out->qlock); free(out);
  if (status) *status = result;
  gt__set_error("sftp open: cancelled, timed out or protocol/transport failure");
  return NULL;
}

gt_sftp_t *gt__sftp_from_session(void *session, gt_ssh_t *ssh) {
  LIBSSH2_SESSION *sess = session;
  if (!sess) {
    gt__set_error("sftp: 会话为空");
    return NULL;
  }
  gt_sftp_t *out = gt__alloc(sizeof *out);
  if (!out) return NULL;
  out->ssh = ssh;
  if (pthread_mutex_init(&out->qlock, NULL) != 0 ||
      pthread_cond_init(&out->qcond, NULL) != 0) {
    gt__set_error("sftp: 初始化 worker 同步原语失败");
    free(out);
    return NULL;
  }
  gt__ssh_io_lock(ssh);
  LIBSSH2_SFTP *sf = libssh2_sftp_init(sess);
  gt__ssh_io_unlock(ssh);
  if (!sf) {
    gt__set_error("sftp: libssh2_sftp_init 失败（服务器不支持 SFTP？）");
    pthread_mutex_destroy(&out->qlock);
    pthread_cond_destroy(&out->qcond);
    free(out);
    return NULL;
  }
  out->sftp = sf;
  if (pthread_create(&out->worker, NULL, worker_main, out) == 0)
    out->worker_started = true;
  else
    gt__set_error("sftp: 起 I/O worker 失败（传输将不可用）");
  return out;
}

void gt_sftp_close(gt_sftp_t *sftp) {
  if (!sftp) return;
  if (sftp->worker_started) {
    pthread_mutex_lock(&sftp->qlock);
    sftp->quit = true;
    pthread_cond_broadcast(&sftp->qcond);
    pthread_mutex_unlock(&sftp->qlock);
    pthread_join(sftp->worker, NULL);
  }
  /* 关闭时丢弃剩余在途 job（面板/标签页已在关闭，UI 不再等它们）。 */
  sftp_job_t *j = sftp->all_head;
  while (j) {
    sftp_job_t *next = j->next_all;
    free(j->remote);
    free(j->local);
    free(j);
    j = next;
  }
  if (sftp->sftp) {
    SFTP_LOCK(sftp);
    libssh2_sftp_shutdown(sftp->sftp);
    SFTP_UNLOCK(sftp);
  }
  pthread_mutex_destroy(&sftp->qlock);
  pthread_cond_destroy(&sftp->qcond);
  free(sftp);
}

/* ------------------------------------------------------------------ */
/* stat / lstat / readlink / realpath / list                            */
/* ------------------------------------------------------------------ */

static void attrs_to_stat(const LIBSSH2_SFTP_ATTRIBUTES *a, gt_sftp_stat_t *out) {
  memset(out, 0, sizeof *out);
  if (!a) return;
  out->has_size = (a->flags & LIBSSH2_SFTP_ATTR_SIZE) != 0;
  out->size = out->has_size ? a->filesize : 0;
  out->mode = (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                  ? (uint32_t)(a->permissions & 07777) : 0;
  out->mtime_unix = (a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                        ? (int64_t)(long)a->mtime : -1;
  out->has_owner = (a->flags & LIBSSH2_SFTP_ATTR_UIDGID) != 0;
  out->uid = (a->flags & LIBSSH2_SFTP_ATTR_UIDGID) ? (uint32_t)a->uid : 0;
  out->gid = (a->flags & LIBSSH2_SFTP_ATTR_UIDGID) ? (uint32_t)a->gid : 0;
  unsigned long type = (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                           ? (a->permissions & LIBSSH2_SFTP_S_IFMT) : 0;
  out->is_dir = (type == LIBSSH2_SFTP_S_IFDIR);
  out->is_symlink = (type == LIBSSH2_SFTP_S_IFLNK);
  out->is_regular = (type == LIBSSH2_SFTP_S_IFREG);
}

/* Controlled read-only directory transaction for a dedicated IO owner. */
static int browse_wait(LIBSSH2_SESSION *session, int socket, int64_t deadline,
                       bool (*cancelled)(void *), void *context) {
  int status = open_status(deadline, cancelled, context);
  if (status != GT_OK) return status;
  int direction = libssh2_session_block_directions(session);
  short events = 0;
  if (direction & LIBSSH2_SESSION_BLOCK_INBOUND) events |= POLLIN;
  if (direction & LIBSSH2_SESSION_BLOCK_OUTBOUND) events |= POLLOUT;
  struct pollfd wait = {socket, events ? events : POLLIN, 0};
  int result = poll(&wait, 1, 10);
  if ((result < 0 && errno != EINTR) ||
      (result > 0 && (wait.revents & (POLLERR | POLLNVAL)))) return GT_ERR_IO;
  return open_status(deadline, cancelled, context);
}

static int browse_entry_compare(const void *a, const void *b) {
  return strcmp(((const gt_sftp_entry_t *)a)->full_path,
                ((const gt_sftp_entry_t *)b)->full_path);
}

int gt_sftp_browse_controlled(gt_sftp_t *sftp, const char *path,
    char *resolved_path, size_t resolved_cap,
    gt_sftp_entry_t **out, size_t *out_count,
    uint32_t timeout_ms, size_t max_entries,
    bool (*cancelled)(void *), void *context) {
  if (out) *out = NULL;
  if (out_count) *out_count = 0;
  if (resolved_path && resolved_cap) resolved_path[0] = '\0';
  if (!sftp || !path || !*path || !resolved_path || resolved_cap < 2 ||
      resolved_cap > 65536 || strlen(path) >= 65536 || !out || !out_count ||
      !timeout_ms || !max_entries || max_entries > GT_SFTP_MAX_ENTRIES) {
    gt__set_error("sftp browse: invalid arguments or limits");
    return GT_ERR_INVALID_ARG;
  }
  const int64_t deadline = mono_ms() + timeout_ms;
  int status = open_status(deadline, cancelled, context);
  if (status != GT_OK) return status;
  pthread_mutex_lock(&sftp->qlock);
  bool has_jobs = sftp->all_head != NULL;
  pthread_mutex_unlock(&sftp->qlock);
  if (has_jobs) {
    gt__set_error("sftp browse: requires a dedicated owner without transfer jobs");
    return GT_ERR_INVALID_ARG;
  }
  void *raw = NULL;
  int socket = -1;
  status = gt__ssh_borrow(sftp->ssh, &raw, &socket);
  if (status != GT_OK) return status;
  char *canonical = gt__alloc(resolved_cap);
  if (!canonical) return GT_ERR_INTERNAL;
  LIBSSH2_SESSION *session = raw;
  LIBSSH2_SFTP_HANDLE *directory = NULL;
  gt_sftp_entry_t *entries = NULL;
  size_t count = 0, capacity = 0;
  SFTP_LOCK(sftp);
  int blocking = libssh2_session_get_blocking(session);
  long timeout = libssh2_session_get_timeout(session);
  libssh2_session_set_blocking(session, 0);
  int result = LIBSSH2_ERROR_EAGAIN;
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    result = libssh2_sftp_realpath(sftp->sftp, path, canonical, (unsigned)resolved_cap);
    if (result != LIBSSH2_ERROR_EAGAIN) break;
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  if (result <= 0 || (size_t)result >= resolved_cap || canonical[0] != '/' ||
      memchr(canonical, '\0', (size_t)result)) {
    status = GT_ERR_IO; goto finished;
  }
  canonical[result] = '\0';
  size_t path_length = (size_t)result;
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    directory = libssh2_sftp_opendir(sftp->sftp, canonical);
    if (directory) break;
    if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
      status = GT_ERR_IO; goto finished;
    }
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    char name[4096];
    LIBSSH2_SFTP_ATTRIBUTES attributes = {0};
    result = libssh2_sftp_readdir_ex(directory, name, sizeof name, NULL, 0, &attributes);
    if (result == LIBSSH2_ERROR_EAGAIN) {
      status = browse_wait(session, socket, deadline, cancelled, context);
      if (status != GT_OK) goto finished;
      continue;
    }
    if (result == 0) break;
    if (result < 0 || (size_t)result >= sizeof name ||
        memchr(name, '\0', (size_t)result) || memchr(name, '/', (size_t)result)) {
      status = GT_ERR_IO; goto finished;
    }
    name[result] = '\0';
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    if (count == max_entries || path_length + (size_t)result + 2 > 65536) {
      status = GT_ERR_IO; goto finished;
    }
    if (count == capacity) {
      size_t next = capacity ? capacity * 2 : 16;
      if (next > max_entries) next = max_entries;
      void *grown = gt__realloc(entries, next * sizeof *entries);
      if (!grown) { status = GT_ERR_INTERNAL; goto finished; }
      entries = grown; capacity = next;
    }
    gt_sftp_entry_t *entry = &entries[count];
    memset(entry, 0, sizeof *entry);
    entry->full_path = gt__alloc(path_length + (size_t)result + 2);
    if (!entry->full_path) { status = GT_ERR_INTERNAL; goto finished; }
    size_t offset = path_length;
    memcpy(entry->full_path, canonical, path_length);
    if (entry->full_path[offset - 1] != '/') entry->full_path[offset++] = '/';
    memcpy(entry->full_path + offset, name, (size_t)result + 1);
    attrs_to_stat(&attributes, &entry->stat);
    count++;
  }
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    result = libssh2_sftp_closedir(directory);
    if (result != LIBSSH2_ERROR_EAGAIN) {
      directory = NULL; /* libssh2 frees handles on every non-EAGAIN result. */
      if (result != 0) status = GT_ERR_IO;
      break;
    }
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  if (status != GT_OK) goto finished;
  if (count > 1) {
    qsort(entries, count, sizeof *entries, browse_entry_compare);
    for (size_t i = 1; i < count; i++) {
      if (!strcmp(entries[i-1].full_path, entries[i].full_path)) {
        status = GT_ERR_IO; goto finished;
      }
    }
  }
  status = open_status(deadline, cancelled, context);
finished:
  if (status != GT_OK) {
    gt__ssh_abort(sftp->ssh);
    /* Disconnect before cleanup; no waiting on a remote CLOSE acknowledgement. */
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, 50);
    if (directory) libssh2_sftp_closedir(directory);
    gt__set_error("sftp browse: cancelled, timed out, invalid directory or read/close failure");
  } else {
    libssh2_session_set_blocking(session, blocking);
    libssh2_session_set_timeout(session, timeout);
    memcpy(resolved_path, canonical, strlen(canonical) + 1);
    *out = entries; *out_count = count;
    entries = NULL;
  }
  SFTP_UNLOCK(sftp);
  if (entries) gt_sftp_list_free(entries, count);
  free(canonical);
  return status;
}

static int stat_common(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out,
                       bool follow) {
  if (!sftp || !path || !*path || !out) {
    gt__set_error("sftp_stat: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  LIBSSH2_SFTP_ATTRIBUTES a;
  memset(&a, 0, sizeof a);
  SFTP_LOCK(sftp);
  int rc = follow ? libssh2_sftp_stat(sftp->sftp, path, &a)
                  : libssh2_sftp_lstat(sftp->sftp, path, &a);
  SFTP_UNLOCK(sftp);
  if (rc != 0) {
    gt__set_error("sftp_stat: %s 不存在或不可访问", path);
    return GT_ERR_IO;
  }
  attrs_to_stat(&a, out);
  return GT_OK;
}

int gt_sftp_preview_controlled(gt_sftp_t *sftp, const char *path,
    uint8_t *buffer, size_t capacity, size_t *out_count, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context) {
  if (out_count) *out_count = 0;
  if (buffer && capacity <= 1048576) memset(buffer, 0, capacity);
  if (!sftp || !path || !*path || strlen(path) >= 65536 || !buffer ||
      !capacity || capacity > 1048576 || !out_count || !timeout_ms) {
    gt__set_error("sftp preview: invalid arguments or limits");
    return GT_ERR_INVALID_ARG;
  }
  const int64_t deadline = mono_ms() + timeout_ms;
  int status = open_status(deadline, cancelled, context);
  if (status != GT_OK) return status;
  pthread_mutex_lock(&sftp->qlock);
  bool has_jobs = sftp->all_head != NULL;
  pthread_mutex_unlock(&sftp->qlock);
  if (has_jobs) {
    gt__set_error("sftp preview: requires a dedicated owner without transfer jobs");
    return GT_ERR_INVALID_ARG;
  }
  void *raw = NULL;
  int socket = -1;
  status = gt__ssh_borrow(sftp->ssh, &raw, &socket);
  if (status != GT_OK) return status;
  uint8_t *bytes = gt__alloc(capacity + 1);
  if (!bytes) return GT_ERR_INTERNAL;
  LIBSSH2_SESSION *session = raw;
  LIBSSH2_SFTP_HANDLE *file = NULL;
  LIBSSH2_SFTP_ATTRIBUTES attributes = {0};
  size_t count = 0;
  SFTP_LOCK(sftp);
  int blocking = libssh2_session_get_blocking(session);
  long timeout = libssh2_session_get_timeout(session);
  libssh2_session_set_blocking(session, 0);
  int result;
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    result = libssh2_sftp_lstat(sftp->sftp, path, &attributes);
    if (result != LIBSSH2_ERROR_EAGAIN) break;
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  if (result != 0) { status = GT_ERR_IO; goto finished; }
  if (!(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ||
      (attributes.permissions & LIBSSH2_SFTP_S_IFMT) != LIBSSH2_SFTP_S_IFREG) {
    status = GT_ERR_UNSUPPORTED; goto finished;
  }
  if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) && attributes.filesize > capacity) {
    status = GT_ERR_IO; goto finished;
  }
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    file = libssh2_sftp_open(sftp->sftp, path, LIBSSH2_FXF_READ, 0);
    if (file) break;
    if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
      status = GT_ERR_IO; goto finished;
    }
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  memset(&attributes, 0, sizeof attributes);
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    result = libssh2_sftp_fstat(file, &attributes);
    if (result != LIBSSH2_ERROR_EAGAIN) break;
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  if (result != 0) { status = GT_ERR_IO; goto finished; }
  if (!(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ||
      (attributes.permissions & LIBSSH2_SFTP_S_IFMT) != LIBSSH2_SFTP_S_IFREG) {
    status = GT_ERR_UNSUPPORTED; goto finished;
  }
  if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) && attributes.filesize > capacity) {
    status = GT_ERR_IO; goto finished;
  }
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    ssize_t length = libssh2_sftp_read(file, (char *)bytes + count, capacity + 1 - count);
    if (length == LIBSSH2_ERROR_EAGAIN) {
      status = browse_wait(session, socket, deadline, cancelled, context);
      if (status != GT_OK) goto finished;
      continue;
    }
    if (length < 0 || (size_t)length > capacity - count) {
      status = GT_ERR_IO; goto finished;
    }
    if (!length) break;
    count += (size_t)length;
  }
  if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) && attributes.filesize != count) {
    status = GT_ERR_IO; goto finished;
  }
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    result = libssh2_sftp_close(file);
    if (result != LIBSSH2_ERROR_EAGAIN) {
      file = NULL; /* Every non-EAGAIN close result consumes the handle. */
      if (result != 0) status = GT_ERR_IO;
      break;
    }
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  if (status == GT_OK) status = open_status(deadline, cancelled, context);
finished:
  if (status != GT_OK) {
    gt__ssh_abort(sftp->ssh);
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, 50);
    if (file) libssh2_sftp_close(file);
    gt__set_error("sftp preview: cancelled, timed out, unsupported type, size limit or read/close failure");
  } else {
    libssh2_session_set_blocking(session, blocking);
    libssh2_session_set_timeout(session, timeout);
    memcpy(buffer, bytes, count);
    *out_count = count;
  }
  SFTP_UNLOCK(sftp);
  free(bytes);
  return status;
}

int gt_sftp_create_controlled(gt_sftp_t *sftp, const char *path, int source_fd,
    uint64_t expected_size, gt_sftp_create_result_t *out, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context) {
  if (out) *out = (gt_sftp_create_result_t){GT_SFTP_CREATE_NOT_ATTEMPTED, 0};
  if (!sftp || !path || !*path || strlen(path) >= 65536 || source_fd < 0 ||
      !out || !timeout_ms || expected_size > INT64_MAX) {
    gt__set_error("sftp create: invalid source, path or budget");
    return GT_ERR_INVALID_ARG;
  }
  int64_t deadline = mono_ms() + timeout_ms;
  int status = open_status(deadline, cancelled, context);
  if (status != GT_OK) return status;
  struct stat before, after;
  int flags = fcntl(source_fd, F_GETFL);
  if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY ||
      fstat(source_fd, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0 || (uint64_t)before.st_size != expected_size) {
    gt__set_error("sftp create: source must be a stable regular file of expected size");
    return GT_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&sftp->qlock);
  bool has_jobs = sftp->all_head != NULL;
  pthread_mutex_unlock(&sftp->qlock);
  if (has_jobs) return GT_ERR_INVALID_ARG;
  void *raw = NULL; int socket = -1;
  status = gt__ssh_borrow(sftp->ssh, &raw, &socket);
  if (status != GT_OK) return status;
  LIBSSH2_SESSION *session = raw;
  LIBSSH2_SFTP_HANDLE *file = NULL;
  SFTP_LOCK(sftp);
  int blocking = libssh2_session_get_blocking(session);
  long timeout = libssh2_session_get_timeout(session);
  libssh2_session_set_blocking(session, 0);
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    out->state = GT_SFTP_CREATE_UNKNOWN;
    file = libssh2_sftp_open(sftp->sftp, path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0600);
    if (file) { out->state = GT_SFTP_CREATE_CREATED; break; }
    int error = libssh2_session_last_errno(session);
    if (error != LIBSSH2_ERROR_EAGAIN) {
      /* The library also uses SFTP_PROTOCOL for malformed HANDLE/STATUS.
       * Only a decoded nonzero server status proves explicit rejection. */
      if (error == LIBSSH2_ERROR_SFTP_PROTOCOL &&
          libssh2_sftp_last_error(sftp->sftp) != LIBSSH2_FX_OK)
        out->state = GT_SFTP_CREATE_REJECTED;
      status = GT_ERR_IO; goto finished;
    }
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
  uint8_t buffer[GT_SFTP_CHUNK];
  while (out->bytes_acked < expected_size) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    size_t wanted = expected_size - out->bytes_acked > sizeof buffer
        ? sizeof buffer : (size_t)(expected_size - out->bytes_acked);
    ssize_t count = pread(source_fd, buffer, wanted, (off_t)out->bytes_acked);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { status = GT_ERR_IO; goto finished; }
    size_t offset = 0;
    while (offset < (size_t)count) {
      status = open_status(deadline, cancelled, context);
      if (status != GT_OK) goto finished;
      ssize_t written = libssh2_sftp_write(file, (char *)buffer + offset, (size_t)count - offset);
      if (written == 0 || written == LIBSSH2_ERROR_EAGAIN) {
        status = browse_wait(session, socket, deadline, cancelled, context);
        if (status != GT_OK) goto finished;
        continue;
      }
      if (written < 0 || (size_t)written > (size_t)count - offset) { status = GT_ERR_IO; goto finished; }
      offset += (size_t)written;
      out->bytes_acked += (uint64_t)written;
    }
  }
  if (fstat(source_fd, &after) != 0 || after.st_size != before.st_size ||
      after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
      after.st_mtime != before.st_mtime || after.st_ctime != before.st_ctime
#ifdef __APPLE__
      || after.st_mtimespec.tv_nsec != before.st_mtimespec.tv_nsec
      || after.st_ctimespec.tv_nsec != before.st_ctimespec.tv_nsec
#else
      || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec
      || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec
#endif
      ) {
    status = GT_ERR_IO; goto finished;
  }
  for (;;) {
    status = open_status(deadline, cancelled, context);
    if (status != GT_OK) goto finished;
    int result = libssh2_sftp_close(file);
    if (result != LIBSSH2_ERROR_EAGAIN) {
      file = NULL;
      status = result == 0 ? GT_OK : GT_ERR_IO;
      if (status == GT_OK) out->state = GT_SFTP_CREATE_COMPLETE;
      break;
    }
    status = browse_wait(session, socket, deadline, cancelled, context);
    if (status != GT_OK) goto finished;
  }
finished:
  if (status != GT_OK && out->state != GT_SFTP_CREATE_NOT_ATTEMPTED) {
    gt__ssh_abort(sftp->ssh);
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, 50);
    if (file) libssh2_sftp_close(file);
    gt__set_error("sftp create: failed; inspect creation state, retain partial/unknown path");
  } else {
    libssh2_session_set_blocking(session, blocking);
    libssh2_session_set_timeout(session, timeout);
  }
  SFTP_UNLOCK(sftp);
  return status;
}

int gt_sftp_stat(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out) {
  return stat_common(sftp, path, out, true);
}

int gt_sftp_lstat(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out) {
  return stat_common(sftp, path, out, false);
}

int gt_sftp_readlink(gt_sftp_t *sftp, const char *path, char *buf, size_t cap) {
  if (!sftp || !path || !*path || !buf || cap == 0) {
    gt__set_error("sftp_readlink: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  SFTP_LOCK(sftp);
  int n = libssh2_sftp_symlink_ex(sftp->sftp, path, (unsigned)strlen(path),
                                  buf, (int)(cap - 1), LIBSSH2_SFTP_READLINK);
  SFTP_UNLOCK(sftp);
  if (n < 0) {
    gt__set_error("sftp_readlink: 读链接失败（%s）", path);
    return GT_ERR_IO;
  }
  buf[n] = '\0';
  return GT_OK;
}

int gt_sftp_realpath(gt_sftp_t *sftp, const char *path, char *buf, size_t cap) {
  if (!sftp || !path || !*path || !buf || cap == 0) {
    gt__set_error("sftp_realpath: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  SFTP_LOCK(sftp);
  int n = libssh2_sftp_realpath(sftp->sftp, path, buf, (int)cap);
  SFTP_UNLOCK(sftp);
  if (n < 0) {
    gt__set_error("sftp_realpath: %s 解析失败", path);
    return GT_ERR_IO;
  }
  buf[cap - 1] = '\0';
  return GT_OK;
}

int gt_sftp_list(gt_sftp_t *sftp, const char *path,
                 gt_sftp_entry_t **out, size_t *out_count) {
  if (!sftp || !path || !*path || !out || !out_count) {
    gt__set_error("sftp_list: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  *out = NULL;
  *out_count = 0;

  SFTP_LOCK(sftp);
  LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(sftp->sftp, path);
  SFTP_UNLOCK(sftp);
  if (!h) {
    gt__set_error("sftp_list: 打不开目录 %s", path);
    return GT_ERR_IO;
  }

  gt_sftp_entry_t *entries = NULL;
  size_t n = 0, cap = 0;
  char name[512];
  char longentry[1024];
  LIBSSH2_SFTP_ATTRIBUTES a;
  size_t plen = strlen(path);

  int rc = GT_OK;
  for (;;) {
    memset(name, 0, sizeof name);
    memset(&a, 0, sizeof a);
    SFTP_LOCK(sftp);
    ssize_t got = libssh2_sftp_readdir_ex(h, name, sizeof name - 1,
                                          longentry, sizeof longentry, &a);
    SFTP_UNLOCK(sftp);
    if (got == 0) break;      /* 目录读完 */
    if (got < 0) {
      gt__set_error("sftp_list: readdir 失败（%s）", path);
      rc = GT_ERR_IO;
      break;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      gt_sftp_entry_t *q = gt__realloc(entries, nc * sizeof *q);
      if (!q) { rc = GT_ERR_INTERNAL; break; }
      entries = q;
      cap = nc;
    }
    gt_sftp_entry_t *e = &entries[n];
    memset(e, 0, sizeof *e);
    e->name = NULL; /* 名字由 full_path 持有（见 gt_sftp_list_free 约定） */
    e->full_path = gt__alloc(plen + 1 + strlen(name) + 1);
    if (!e->full_path) { rc = GT_ERR_INTERNAL; break; }
    memcpy(e->full_path, path, plen);
    size_t off = plen;
    if (off == 0 || e->full_path[off - 1] != '/') e->full_path[off++] = '/';
    strcpy(e->full_path + off, name);
    attrs_to_stat(&a, &e->stat);
    n++;
    if (n >= GT_SFTP_MAX_ENTRIES) {
      gt__set_error("sftp_list: 条目数超上限 %d", GT_SFTP_MAX_ENTRIES);
      rc = GT_ERR_INTERNAL;
      break;
    }
  }
  SFTP_LOCK(sftp);
  libssh2_sftp_closedir(h);
  SFTP_UNLOCK(sftp);

  if (rc != GT_OK) {
    gt_sftp_list_free(entries, n);
    return rc;
  }
  *out = entries;
  *out_count = n;
  return GT_OK;
}

void gt_sftp_list_free(gt_sftp_entry_t *entries, size_t count) {
  if (!entries) return;
  for (size_t i = 0; i < count; i++) free(entries[i].full_path);
  free(entries);
}

/* ------------------------------------------------------------------ */
/* 写侧原语                                                            */
/* ------------------------------------------------------------------ */

int gt_sftp_mkdir(gt_sftp_t *sftp, const char *path, uint32_t mode) {
  if (!sftp || !path || !*path) {
    gt__set_error("sftp_mkdir: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  SFTP_LOCK(sftp);
  int rc = libssh2_sftp_mkdir_ex(sftp->sftp, path, (unsigned)strlen(path),
                                 mode ? (long)mode : 0755);
  SFTP_UNLOCK(sftp);
  if (rc != 0) {
    gt__set_error("sftp_mkdir: 创建目录失败（%s）", path);
    return GT_ERR_IO;
  }
  return GT_OK;
}

int gt_sftp_rmdir(gt_sftp_t *sftp, const char *path) {
  if (!sftp || !path || !*path) {
    gt__set_error("sftp_rmdir: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  SFTP_LOCK(sftp);
  int rc = libssh2_sftp_rmdir_ex(sftp->sftp, path, (unsigned)strlen(path));
  SFTP_UNLOCK(sftp);
  if (rc != 0) {
    gt__set_error("sftp_rmdir: 删除目录失败（仅空目录，%s）", path);
    return GT_ERR_IO;
  }
  return GT_OK;
}

int gt_sftp_remove(gt_sftp_t *sftp, const char *path) {
  if (!sftp || !path || !*path) {
    gt__set_error("sftp_remove: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  SFTP_LOCK(sftp);
  int rc = libssh2_sftp_unlink_ex(sftp->sftp, path, (unsigned)strlen(path));
  SFTP_UNLOCK(sftp);
  if (rc != 0) {
    gt__set_error("sftp_remove: 删除失败（%s）", path);
    return GT_ERR_IO;
  }
  return GT_OK;
}

/* 递归删除：先删孩子，再删目录自己。失败如实返回（不折叠成 OK）。 */
static int remove_all_rec(gt_sftp_t *sftp, const char *path, int depth) {
  if (depth > GT_SFTP_MAX_DEPTH) {
    gt__set_error("sftp_remove_all: 递归深度超上限 %d", GT_SFTP_MAX_DEPTH);
    return GT_ERR_INTERNAL;
  }
  gt_sftp_stat_t st;
  int rc = gt_sftp_lstat(sftp, path, &st);
  if (rc != GT_OK) return rc; /* 不存在 / 不可访问：如实报错 */
  if (!st.is_dir) return gt_sftp_remove(sftp, path);

  gt_sftp_entry_t *ents = NULL;
  size_t n = 0;
  rc = gt_sftp_list(sftp, path, &ents, &n);
  if (rc != GT_OK) return rc;
  for (size_t i = 0; i < n; i++) {
    int r = remove_all_rec(sftp, ents[i].full_path, depth + 1);
    if (r != GT_OK) { gt_sftp_list_free(ents, n); return r; }
  }
  gt_sftp_list_free(ents, n);
  return gt_sftp_rmdir(sftp, path);
}

int gt_sftp_remove_all(gt_sftp_t *sftp, const char *path) {
  if (!sftp || !path || !*path) {
    gt__set_error("sftp_remove_all: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  return remove_all_rec(sftp, path, 0);
}

int gt_sftp_rename(gt_sftp_t *sftp, const char *from, const char *to) {
  if (!sftp || !from || !*from || !to || !*to) {
    gt__set_error("sftp_rename: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  SFTP_LOCK(sftp);
  int rc = libssh2_sftp_rename_ex(sftp->sftp, from, (unsigned)strlen(from),
                                  to, (unsigned)strlen(to),
                                  LIBSSH2_SFTP_RENAME_OVERWRITE |
                                      LIBSSH2_SFTP_RENAME_ATOMIC);
  if (rc != 0)
    rc = libssh2_sftp_rename(sftp->sftp, from, to); /* 无 posix-rename 扩展时回退 */
  SFTP_UNLOCK(sftp);
  if (rc != 0) {
    gt__set_error("sftp_rename: 重命名/移动失败（%s → %s）", from, to);
    return GT_ERR_IO;
  }
  return GT_OK;
}

int gt_sftp_chmod(gt_sftp_t *sftp, const char *path, uint32_t mode) {
  if (!sftp || !path || !*path) {
    gt__set_error("sftp_chmod: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  LIBSSH2_SFTP_ATTRIBUTES a;
  memset(&a, 0, sizeof a);
  a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
  a.permissions = (unsigned long)(mode & 07777);
  SFTP_LOCK(sftp);
  int rc = libssh2_sftp_setstat(sftp->sftp, path, &a);
  SFTP_UNLOCK(sftp);
  if (rc != 0) {
    gt__set_error("sftp_chmod: 改权限失败（%s）", path);
    return GT_ERR_IO;
  }
  return GT_OK;
}

/* ------------------------------------------------------------------ */
/* 递归枚举（walk）                                                     */
/* ------------------------------------------------------------------ */

static int walk_rec(gt_sftp_t *sftp, const char *dir, int depth, int max_depth,
                    gt_sftp_walk_fn fn, void *ud) {
  gt_sftp_entry_t *ents = NULL;
  size_t n = 0;
  int rc = gt_sftp_list(sftp, dir, &ents, &n);
  if (rc != GT_OK) return rc;
  for (size_t i = 0; i < n; i++) {
    gt_sftp_entry_t *e = &ents[i];
    const char *base = strrchr(e->full_path, '/');
    gt_sftp_walk_ent_t we = {
        .path = e->full_path,
        .name = base ? base + 1 : e->full_path,
        .stat = &e->stat,
    };
    if (fn(ud, &we) != 0) { gt_sftp_list_free(ents, n); return GT_ERR_CANCELLED; }
    if (e->stat.is_dir && (max_depth < 0 || depth < max_depth)) {
      int r = walk_rec(sftp, e->full_path, depth + 1, max_depth, fn, ud);
      if (r != GT_OK) { gt_sftp_list_free(ents, n); return r; }
    }
  }
  gt_sftp_list_free(ents, n);
  return GT_OK;
}

int gt_sftp_walk(gt_sftp_t *sftp, const char *root, int max_depth,
                 gt_sftp_walk_fn fn, void *ud) {
  if (!sftp || !root || !*root || !fn) {
    gt__set_error("sftp_walk: null/empty argument");
    return GT_ERR_INVALID_ARG;
  }
  if (max_depth > GT_SFTP_MAX_DEPTH) max_depth = GT_SFTP_MAX_DEPTH;
  return walk_rec(sftp, root, 1, max_depth, fn, ud);
}

/* ------------------------------------------------------------------ */
/* 传输 job（单 worker 线程 + 分块推进）                                */
/* ------------------------------------------------------------------ */

static int job_download(sftp_job_t *j) {
  int rc = GT_JOB_FAILED;
  gt_sftp_t *s = j->sftp;
  SFTP_LOCK(s);
  LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(s->sftp, j->remote,
                                             LIBSSH2_FXF_READ, 0);
  SFTP_UNLOCK(s);
  if (!h) {
    emit_job_done(j->job_id, GT_JOB_FAILED, "打开远端文件失败");
    return GT_JOB_FAILED;
  }
  /* Do not truncate a local target until the source has actually opened. This
   * legacy API still overwrites; safe copy needs an independent commit API. */
  FILE *f = fopen(j->local, "wb");
  if (!f) {
    SFTP_LOCK(s);
    libssh2_sftp_close(h);
    SFTP_UNLOCK(s);
    emit_job_done(j->job_id, GT_JOB_FAILED, "打开本地目标失败");
    return GT_JOB_FAILED;
  }
  LIBSSH2_SFTP_ATTRIBUTES a;
  memset(&a, 0, sizeof a);
  SFTP_LOCK(s);
  int frc = libssh2_sftp_fstat(h, &a);
  SFTP_UNLOCK(s);
  uint64_t total = (frc == 0 && (a.flags & LIBSSH2_SFTP_ATTR_SIZE)) ? a.filesize : 0;

  char buf[GT_SFTP_CHUNK];
  uint64_t done = 0;
  int64_t last_emit = 0;
  for (;;) {
    if (atomic_load(&j->cancelled)) { rc = GT_JOB_CANCELLED; break; }
    if (atomic_load(&j->paused)) { usleep(20 * 1000); continue; }
    SFTP_LOCK(s);
    ssize_t n = libssh2_sftp_read(h, buf, sizeof buf);
    SFTP_UNLOCK(s);
    if (n == 0) { rc = GT_JOB_OK; break; }
    if (n < 0) { rc = GT_JOB_FAILED; break; }
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { rc = GT_JOB_FAILED; break; }
    done += (uint64_t)n;
    int64_t now = mono_ms();
    if (now - last_emit >= 16) { emit_progress(j->job_id, done, total); last_emit = now; }
  }
  SFTP_LOCK(s);
  int remote_close = libssh2_sftp_close(h);
  SFTP_UNLOCK(s);
  int local_close = fclose(f);
  bool close_failed = remote_close != 0 || local_close != 0;
  if (close_failed && rc != GT_JOB_CANCELLED) rc = GT_JOB_FAILED;
  const char *detail = close_failed
      ? (rc == GT_JOB_CANCELLED ? "download cancelled; close failed（部分文件保留）"
                                : "download close failed")
      :
      rc == GT_JOB_OK ? "download done"
      : rc == GT_JOB_CANCELLED ? "download cancelled（已写部分保留）"
                               : "download failed";
  emit_job_done(j->job_id, rc, detail);
  return rc;
}

static int job_upload(sftp_job_t *j) {
  int rc = GT_JOB_FAILED;
  FILE *f = fopen(j->local, "rb");
  if (!f) {
    emit_job_done(j->job_id, GT_JOB_FAILED, strerror(errno));
    return GT_JOB_FAILED;
  }
  off_t length;
  if (fseeko(f, 0, SEEK_END) != 0 || (length = ftello(f)) < 0 ||
      fseeko(f, 0, SEEK_SET) != 0) {
    fclose(f);
    emit_job_done(j->job_id, GT_JOB_FAILED, "读取本地源尺寸或定位失败");
    return GT_JOB_FAILED;
  }
  uint64_t total = (uint64_t)length;

  gt_sftp_t *s = j->sftp;
  SFTP_LOCK(s);
  LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(s->sftp, j->remote,
                                             LIBSSH2_FXF_WRITE |
                                                 LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                             0644);
  SFTP_UNLOCK(s);
  if (!h) {
    fclose(f);
    emit_job_done(j->job_id, GT_JOB_FAILED, "打开远端文件失败");
    return GT_JOB_FAILED;
  }

  char buf[GT_SFTP_CHUNK];
  uint64_t done = 0;
  int64_t last_emit = 0;
  for (;;) {
    if (atomic_load(&j->cancelled)) { rc = GT_JOB_CANCELLED; break; }
    if (atomic_load(&j->paused)) { usleep(20 * 1000); continue; }
    size_t n = fread(buf, 1, sizeof buf, f);
    if (n == 0) { rc = ferror(f) ? GT_JOB_FAILED : GT_JOB_OK; break; }
    size_t w = 0;
    while (w < n) {
      if (atomic_load(&j->cancelled)) break;
      SFTP_LOCK(s);
      ssize_t k = libssh2_sftp_write(h, buf + w, n - w);
      SFTP_UNLOCK(s);
      if (k < 0) { rc = GT_JOB_FAILED; goto out; }
      w += (size_t)k;
    }
    if (w < n && atomic_load(&j->cancelled)) { rc = GT_JOB_CANCELLED; break; }
    done += n;
    int64_t now = mono_ms();
    if (now - last_emit >= 16) { emit_progress(j->job_id, done, total); last_emit = now; }
  }
out:
  SFTP_LOCK(s);
  int remote_close = libssh2_sftp_close(h);
  SFTP_UNLOCK(s);
  int local_close = fclose(f);
  bool close_failed = remote_close != 0 || local_close != 0;
  if (close_failed && rc != GT_JOB_CANCELLED) rc = GT_JOB_FAILED;
  const char *detail = close_failed
      ? (rc == GT_JOB_CANCELLED ? "upload cancelled; close failed（部分文件保留）"
                                : "upload close failed")
      :
      rc == GT_JOB_OK ? "upload done"
      : rc == GT_JOB_CANCELLED ? "upload cancelled（已传部分保留）"
                               : "upload failed";
  emit_job_done(j->job_id, rc, detail);
  return rc;
}

static void *worker_main(void *arg) {
  gt_sftp_t *sftp = arg;
  for (;;) {
    pthread_mutex_lock(&sftp->qlock);
    while (!sftp->pending_head && !sftp->quit)
      pthread_cond_wait(&sftp->qcond, &sftp->qlock);
    if (sftp->quit) { pthread_mutex_unlock(&sftp->qlock); break; }
    sftp_job_t *j = sftp->pending_head;
    sftp->pending_head = j->next_pending;
    if (!sftp->pending_head) sftp->pending_tail = NULL;
    pthread_mutex_unlock(&sftp->qlock);

    if (atomic_load(&j->cancelled))
      emit_job_done(j->job_id, GT_JOB_CANCELLED, "cancelled before opening files");
    else if (j->upload) job_upload(j);
    else job_download(j);

    /* 从在途表摘除并释放。 */
    pthread_mutex_lock(&sftp->qlock);
    sftp_job_t **pp = &sftp->all_head;
    while (*pp && *pp != j) pp = &(*pp)->next_all;
    if (*pp == j) *pp = j->next_all;
    pthread_mutex_unlock(&sftp->qlock);
    free(j->remote);
    free(j->local);
    free(j);
  }
  return NULL;
}

static uint64_t submit_job(gt_sftp_t *sftp, const char *a, const char *b,
                           bool upload) {
  if (!sftp || !sftp->sftp || !a || !b || !*a || !*b) {
    gt__set_error("sftp job: 参数非法或 SFTP 未就绪");
    return 0;
  }
  if (!sftp->worker_started) {
    gt__set_error("sftp job: I/O worker 未起（不可用）");
    return 0;
  }
  sftp_job_t *j = gt__alloc(sizeof *j);
  if (!j) return 0;
  j->sftp = sftp;
  j->upload = upload;
  j->remote = gt__strdup(upload ? b : a);
  j->local = gt__strdup(upload ? a : b);
  j->job_id = __sync_fetch_and_add(&g_next_job_id, 1);
  atomic_init(&j->cancelled, false);
  atomic_init(&j->paused, false);
  if (!j->remote || !j->local) {
    free(j->remote);
    free(j->local);
    free(j);
    return 0;
  }
  /* Publishing the queue transfers ownership to the worker. It may finish and
   * free j immediately after unlock, before this caller resumes. */
  const uint64_t job_id = j->job_id;
  pthread_mutex_lock(&sftp->qlock);
  j->next_pending = NULL;
  if (sftp->pending_tail) sftp->pending_tail->next_pending = j;
  else sftp->pending_head = j;
  sftp->pending_tail = j;
  j->next_all = sftp->all_head;
  sftp->all_head = j;
  pthread_cond_signal(&sftp->qcond);
  pthread_mutex_unlock(&sftp->qlock);
  return job_id;
}

uint64_t gt_sftp_download(gt_sftp_t *sftp, const char *remote, const char *local) {
  return submit_job(sftp, remote, local, false);
}

uint64_t gt_sftp_upload(gt_sftp_t *sftp, const char *local, const char *remote) {
  return submit_job(sftp, local, remote, true);
}

/* cancel / pause 都按 job_id 查在途表（调用可来自任意线程）。 */
static sftp_job_t *find_job(gt_sftp_t *sftp, uint64_t job_id) {
  for (sftp_job_t *j = sftp->all_head; j; j = j->next_all)
    if (j->job_id == job_id) return j;
  return NULL;
}

int gt_sftp_cancel(gt_sftp_t *sftp, uint64_t job_id) {
  if (!sftp || job_id == 0) return GT_ERR_INVALID_ARG;
  pthread_mutex_lock(&sftp->qlock);
  sftp_job_t *j = find_job(sftp, job_id);
  if (j) atomic_store(&j->cancelled, true);
  pthread_mutex_unlock(&sftp->qlock);
  return j ? GT_OK : GT_ERR_NOT_FOUND;
}

int gt_sftp_pause(gt_sftp_t *sftp, uint64_t job_id, bool paused) {
  if (!sftp || job_id == 0) return GT_ERR_INVALID_ARG;
  pthread_mutex_lock(&sftp->qlock);
  sftp_job_t *j = find_job(sftp, job_id);
  if (j) atomic_store(&j->paused, paused);
  pthread_mutex_unlock(&sftp->qlock);
  return j ? GT_OK : GT_ERR_NOT_FOUND;
}

#else /* !GT_HAVE_LIBSSH2 —— 显式 UNSUPPORTED */

struct gt_sftp { void *unused; };

int gt_sftp_create_controlled(gt_sftp_t *sftp, const char *path, int source_fd,
    uint64_t expected_size, gt_sftp_create_result_t *out, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context) {
  (void)cancelled; (void)context;
  if (out) *out = (gt_sftp_create_result_t){GT_SFTP_CREATE_NOT_ATTEMPTED, 0};
  if (!sftp || !path || !*path || strlen(path) >= 65536 || source_fd < 0 ||
      !out || !timeout_ms || expected_size > INT64_MAX) return GT_ERR_INVALID_ARG;
  gt__set_error("sftp create: libssh2 unavailable");
  return GT_ERR_UNSUPPORTED;
}


int gt_sftp_browse_controlled(gt_sftp_t *sftp, const char *path,
    char *resolved_path, size_t resolved_cap,
    gt_sftp_entry_t **out, size_t *out_count,
    uint32_t timeout_ms, size_t max_entries,
    bool (*cancelled)(void *), void *context) {
  (void)cancelled; (void)context;
  if (out) *out = NULL;
  if (out_count) *out_count = 0;
  if (resolved_path && resolved_cap) resolved_path[0] = '\0';
  if (!sftp || !path || !*path || !resolved_path || resolved_cap < 2 ||
      resolved_cap > 65536 || strlen(path) >= 65536 || !out || !out_count ||
      !timeout_ms || !max_entries || max_entries > 65536) return GT_ERR_INVALID_ARG;
  gt__set_error("sftp browse: libssh2 unavailable");
  return GT_ERR_UNSUPPORTED;
}


gt_sftp_t *gt_ssh_sftp_controlled(gt_ssh_t *ssh, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context, gt_status_t *status) {
  (void)cancelled; (void)context;
  if (status) *status = (!ssh || !timeout_ms) ? GT_ERR_INVALID_ARG : GT_ERR_UNSUPPORTED;
  gt__set_error("sftp open: libssh2 unavailable");
  return NULL;
}

gt_sftp_t *gt__sftp_from_session(void *session, gt_ssh_t *ssh) {
  (void)session; (void)ssh;
  gt__set_error("sftp: libssh2 未编译进核心（显式 UNSUPPORTED）");
  return NULL;
}

int gt_sftp_list(gt_sftp_t *sftp, const char *path,
                 gt_sftp_entry_t **out, size_t *out_count) {
  if (out) *out = NULL;
  if (out_count) *out_count = 0;
  if (!sftp || !path || !*path || !out || !out_count) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_list: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}

void gt_sftp_list_free(gt_sftp_entry_t *entries, size_t count) {
  (void)count;
  if (!entries) return;
  for (size_t i = 0; i < count; i++) free(entries[i].full_path);
  free(entries);
}

int gt_sftp_preview_controlled(gt_sftp_t *sftp, const char *path,
    uint8_t *buffer, size_t capacity, size_t *out_count, uint32_t timeout_ms,
    bool (*cancelled)(void *), void *context) {
  (void)cancelled; (void)context;
  if (out_count) *out_count = 0;
  if (buffer && capacity <= 1048576) memset(buffer, 0, capacity);
  if (!sftp || !path || !*path || strlen(path) >= 65536 || !buffer ||
      !capacity || capacity > 1048576 || !out_count || !timeout_ms) return GT_ERR_INVALID_ARG;
  gt__set_error("sftp preview: libssh2 unavailable");
  return GT_ERR_UNSUPPORTED;
}

int gt_sftp_stat(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out) {
  if (!sftp || !path || !*path || !out) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_stat: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_lstat(gt_sftp_t *sftp, const char *path, gt_sftp_stat_t *out) {
  if (!sftp || !path || !*path || !out) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_lstat: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_readlink(gt_sftp_t *sftp, const char *path, char *buf, size_t cap) {
  if (!sftp || !path || !*path || !buf || cap == 0) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_readlink: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_realpath(gt_sftp_t *sftp, const char *path, char *buf, size_t cap) {
  if (!sftp || !path || !*path || !buf || cap == 0) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_realpath: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_mkdir(gt_sftp_t *sftp, const char *path, uint32_t mode) {
  if (!sftp || !path || !*path) return GT_ERR_INVALID_ARG;
  (void)mode;
  gt__set_error("gt_sftp_mkdir: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_rmdir(gt_sftp_t *sftp, const char *path) {
  if (!sftp || !path || !*path) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_rmdir: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_remove(gt_sftp_t *sftp, const char *path) {
  if (!sftp || !path || !*path) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_remove: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_remove_all(gt_sftp_t *sftp, const char *path) {
  if (!sftp || !path || !*path) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_remove_all: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_rename(gt_sftp_t *sftp, const char *from, const char *to) {
  if (!sftp || !from || !*from || !to || !*to) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_rename: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_chmod(gt_sftp_t *sftp, const char *path, uint32_t mode) {
  if (!sftp || !path || !*path) return GT_ERR_INVALID_ARG;
  (void)mode;
  gt__set_error("gt_sftp_chmod: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_walk(gt_sftp_t *sftp, const char *root, int max_depth,
                 gt_sftp_walk_fn fn, void *ud) {
  (void)max_depth; (void)ud;
  if (!sftp || !root || !*root || !fn) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_walk: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
uint64_t gt_sftp_download(gt_sftp_t *sftp, const char *remote, const char *local) {
  (void)sftp; (void)remote; (void)local;
  gt__set_error("gt_sftp_download: libssh2 未编译进核心");
  return 0;
}
uint64_t gt_sftp_upload(gt_sftp_t *sftp, const char *local, const char *remote) {
  (void)sftp; (void)local; (void)remote;
  gt__set_error("gt_sftp_upload: libssh2 未编译进核心");
  return 0;
}
int gt_sftp_cancel(gt_sftp_t *sftp, uint64_t job_id) {
  (void)sftp;
  if (job_id == 0) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_cancel: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
int gt_sftp_pause(gt_sftp_t *sftp, uint64_t job_id, bool paused) {
  (void)sftp; (void)paused;
  if (job_id == 0) return GT_ERR_INVALID_ARG;
  gt__set_error("gt_sftp_pause: libssh2 未编译进核心");
  return GT_ERR_UNSUPPORTED;
}
void gt_sftp_close(gt_sftp_t *sftp) { free(sftp); }

#endif /* GT_HAVE_LIBSSH2 */
