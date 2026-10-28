/*
 * ghosterm.h — Ghosterm 共享 C 核心的稳定 ABI
 *
 * 对外只有这一层。Swift / Kotlin(JNI) / ArkTS(NAPI) / C++ 一律通过
 * include/ghosterm.h 消费；任何平台绑定都不得依赖内部结构体。
 *
 * 设计原则
 *   1. opaque handle：所有句柄都是不透明指针，ABI 稳定、实现可换。
 *   2. 单线程会话模型：一个会话一个线程；libssh2 的阻塞语义止步于 core。
 *   3. 字节流走 transport，不走事件队列、不过 UI 线程（高频过桥是大忌）。
 *   4. 事件队列只承载低频事件，由宿主 poll 后投递到 UI 线程。
 *   5. 三态语义不得折叠（未知 ≠ 拒绝）；能力缺失必须显式报错，不静默降级。
 *
 * 目录：config(ssh_config) · transport(pty/ssh) · ssh · sftp
 */
#ifndef GHOSTERM_H
#define GHOSTERM_H

#include "ghosterm/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 版本 / 错误                                                          */
/* ------------------------------------------------------------------ */

/* 最近一次错误的可读说明（线程局部；指针在下次调用前有效）。 */
const char *gt_last_error(void);

/* 运行时版本，形如 "0.1.0-dev"；调用方不得 free。 */
const char *gt_version(void);

/* ------------------------------------------------------------------ */
/* 事件队列（低频 / 批量 / 非阻塞）                                      */
/* ------------------------------------------------------------------ */

typedef enum {
  GT_EVENT_NONE = 0,
  GT_EVENT_STATUS,   /* 连接/会话状态变化 */
  GT_EVENT_PROGRESS, /* 传输进度（已按 ~16ms 合帧） */
  GT_EVENT_JOB_DONE, /* job 结束：成功 / 失败 / 取消，见 code 三态 */
  GT_EVENT_LOG,      /* 诊断日志（不是终端字节流） */
} gt_event_kind_t;

/* GT_EVENT_JOB_DONE 的 code 三态。 */
typedef enum {
  GT_JOB_OK = 0,
  GT_JOB_FAILED = 1,
  GT_JOB_CANCELLED = 2,
} gt_job_result_t;

#define GT_EVENT_MESSAGE_MAX 512

typedef struct {
  gt_event_kind_t kind;
  uint64_t session_id; /* 0 = 全局 */
  uint64_t job_id;     /* 0 = 非 job 事件 */
  int32_t code;        /* 含义随 kind；JOB_DONE 用 gt_job_result_t */
  char message[GT_EVENT_MESSAGE_MAX];
} gt_event_t;

/*
 * 取出至多 cap 个待消费事件，返回写入数量。**非阻塞**，无事件返回 0。
 * 可由任意线程调用（实现内部加锁）；返回后事件即被消费。
 */
size_t gt_core_poll_events(gt_event_t *out, size_t cap);

/* ------------------------------------------------------------------ */
/* 子系统                                                              */
/* ------------------------------------------------------------------ */

#include "ghosterm/config.h"
#include "ghosterm/transport.h"
#include "ghosterm/ssh.h"
#include "ghosterm/sftp.h"
#include "ghosterm/ssh_auth.h"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GHOSTERM_H */
