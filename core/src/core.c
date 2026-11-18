#include "ghosterm.h"
#include "internal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GT_VERSION_STRING "0.1.0-dev"

const char *gt_version(void) {
    return GT_VERSION_STRING;
}

/* ------------------------------------------------------------------ */
/* 线程局部错误                                                        */
/* ------------------------------------------------------------------ */

static _Thread_local char g_error[512];

const char *gt_last_error(void) {
    return g_error[0] ? g_error : "";
}

void gt__set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

void *gt__alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        gt__set_error("out of memory (%zu bytes)", n);
    }
    return p;
}

void *gt__realloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        gt__set_error("out of memory (%zu bytes)", n);
    }
    return q;
}

char *gt__strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = gt__alloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

char *gt__strndup(const char *s, size_t n) {
    if (!s) {
        return NULL;
    }
    char *p = gt__alloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* 事件队列（环形缓冲，加锁；只承载低频事件）                            */
/* ------------------------------------------------------------------ */

#define GT_EVENT_QUEUE_INITIAL 256

static gt_event_t g_ev_initial[GT_EVENT_QUEUE_INITIAL];
static gt_event_t *g_events = g_ev_initial;
static size_t g_ev_head, g_ev_count;
static size_t g_ev_capacity = GT_EVENT_QUEUE_INITIAL;

typedef struct gt_terminal_event_node {
    struct gt_terminal_event_node *next;
    gt_event_t event;
} gt_terminal_event_node_t;

static gt_terminal_event_node_t *g_terminal_head, *g_terminal_tail;
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;

void *gt__event_reserve_terminal(void) {
    gt_terminal_event_node_t *node = calloc(1, sizeof *node);
    if (!node) {
        gt__set_error("out of memory reserving terminal event");
    }
    return node;
}

void gt__event_discard_reservation(void *reservation) {
    free(reservation);
}

void gt__emit_reserved_job_done(void *reservation, uint64_t job_id, int code, const char *message) {
    gt_terminal_event_node_t *node = reservation;
    if (!node) {
        return;
    }
    memset(&node->event, 0, sizeof node->event);
    node->event.kind = GT_EVENT_JOB_DONE;
    node->event.job_id = job_id;
    node->event.code = code;
    if (message) {
        strncpy(node->event.message, message, GT_EVENT_MESSAGE_MAX - 1);
    }
    pthread_mutex_lock(&g_ev_lock);
    size_t retained = 0;
    for (size_t i = 0; i < g_ev_count; i++) {
        const size_t source = (g_ev_head + i) % g_ev_capacity;
        const gt_event_t event = g_events[source];
        if (event.kind == GT_EVENT_PROGRESS && event.job_id == job_id) {
            continue;
        }
        const size_t destination = (g_ev_head + retained) % g_ev_capacity;
        g_events[destination] = event;
        retained++;
    }
    g_ev_count = retained;
    node->next = NULL;
    if (g_terminal_tail) {
        g_terminal_tail->next = node;
    } else {
        g_terminal_head = node;
    }
    g_terminal_tail = node;
    pthread_mutex_unlock(&g_ev_lock);
}

static void event_push(const gt_event_t *ev) {
    pthread_mutex_lock(&g_ev_lock);
    if (g_ev_count == g_ev_capacity) {
        if (g_ev_capacity <= SIZE_MAX / 2 / sizeof(*g_events)) {
            size_t next_capacity = g_ev_capacity * 2;
            gt_event_t *next = malloc(next_capacity * sizeof(*next));
            if (next) {
                for (size_t i = 0; i < g_ev_count; i++) {
                    next[i] = g_events[(g_ev_head + i) % g_ev_capacity];
                }
                if (g_events != g_ev_initial) {
                    free(g_events);
                }
                g_events = next;
                g_ev_capacity = next_capacity;
                g_ev_head = 0;
            }
        }
        if (g_ev_count == g_ev_capacity) {
            /* Memory pressure may drop progress, but never evict a terminal
             * event to make room for another progress snapshot. */
            if (ev->kind == GT_EVENT_PROGRESS) {
                pthread_mutex_unlock(&g_ev_lock);
                return;
            }
            size_t removable = SIZE_MAX;
            for (size_t i = 0; i < g_ev_count; i++) {
                size_t index = (g_ev_head + i) % g_ev_capacity;
                if (g_events[index].kind == GT_EVENT_PROGRESS) {
                    removable = i;
                    break;
                }
            }
            if (removable == SIZE_MAX) {
                pthread_mutex_unlock(&g_ev_lock);
                return;
            }
            for (size_t i = removable; i + 1 < g_ev_count; i++) {
                size_t dst = (g_ev_head + i) % g_ev_capacity;
                size_t src = (g_ev_head + i + 1) % g_ev_capacity;
                g_events[dst] = g_events[src];
            }
            g_ev_count--;
        }
    }
    g_events[(g_ev_head + g_ev_count) % g_ev_capacity] = *ev;
    g_ev_count++;
    pthread_mutex_unlock(&g_ev_lock);
}

size_t gt_core_poll_events(gt_event_t *out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    size_t n = 0;
    pthread_mutex_lock(&g_ev_lock);
    while (n < cap && g_terminal_head) {
        gt_terminal_event_node_t *node = g_terminal_head;
        g_terminal_head = node->next;
        if (!g_terminal_head) {
            g_terminal_tail = NULL;
        }
        out[n++] = node->event;
        free(node);
    }
    while (n < cap && g_ev_count) {
        out[n++] = g_events[g_ev_head];
        g_ev_head = (g_ev_head + 1) % g_ev_capacity;
        g_ev_count--;
    }
    pthread_mutex_unlock(&g_ev_lock);
    return n;
}

void gt__emit_status(int code, const char *message) {
    gt__emit_event(GT_EVENT_STATUS, 0, code, message);
}

void gt__emit_event(gt_event_kind_t kind, uint64_t job_id, int code, const char *message) {
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
