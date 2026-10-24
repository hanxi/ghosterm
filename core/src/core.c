#include "ghosterm.h"
#include "internal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GT_VERSION_STRING "0.1.0-dev"

const char *gt_version(void) { return GT_VERSION_STRING; }

/* ------------------------------------------------------------------ */
/* 线程局部错误                                                        */
/* ------------------------------------------------------------------ */

static _Thread_local char g_error[512];

const char *gt_last_error(void) { return g_error[0] ? g_error : ""; }

void gt__set_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_error, sizeof g_error, fmt, ap);
  va_end(ap);
}

void *gt__alloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) gt__set_error("out of memory (%zu bytes)", n);
  return p;
}

void *gt__realloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) gt__set_error("out of memory (%zu bytes)", n);
  return q;
}

char *gt__strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *p = gt__alloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

char *gt__strndup(const char *s, size_t n) {
  if (!s) return NULL;
  char *p = gt__alloc(n + 1);
  if (p) { memcpy(p, s, n); p[n] = '\0'; }
  return p;
}

/* ------------------------------------------------------------------ */
/* 事件队列（环形缓冲，加锁；只承载低频事件）                            */
/* ------------------------------------------------------------------ */

#define GT_EVENT_QUEUE_CAP 256

static gt_event_t g_events[GT_EVENT_QUEUE_CAP];
static size_t g_ev_head, g_ev_count;
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;

static void event_push(const gt_event_t *ev) {
  pthread_mutex_lock(&g_ev_lock);
  if (g_ev_count == GT_EVENT_QUEUE_CAP) {
    /* 覆盖最旧的一条：低频事件丢最旧优于阻塞会话线程。 */
    g_ev_head = (g_ev_head + 1) % GT_EVENT_QUEUE_CAP;
    g_ev_count--;
  }
  g_events[(g_ev_head + g_ev_count) % GT_EVENT_QUEUE_CAP] = *ev;
  g_ev_count++;
  pthread_mutex_unlock(&g_ev_lock);
}

size_t gt_core_poll_events(gt_event_t *out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  pthread_mutex_lock(&g_ev_lock);
  while (n < cap && g_ev_count) {
    out[n++] = g_events[g_ev_head];
    g_ev_head = (g_ev_head + 1) % GT_EVENT_QUEUE_CAP;
    g_ev_count--;
  }
  pthread_mutex_unlock(&g_ev_lock);
  return n;
}

void gt__emit_status(int code, const char *message) {
  gt__emit_event(GT_EVENT_STATUS, 0, code, message);
}

void gt__emit_event(gt_event_kind_t kind, uint64_t job_id, int code,
                    const char *message) {
  gt_event_t ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = kind;
  ev.job_id = job_id;
  ev.code = code;
  if (message) {
    strncpy(ev.message, message, GT_EVENT_MESSAGE_MAX - 1);
  }
  event_push(&ev);
}
