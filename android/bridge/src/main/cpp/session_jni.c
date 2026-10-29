#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "ghosterm.h"
#include "surface.h"

/* Each transport AND VT has one native owner. JNI never calls transport/VT.
 * The registry owns one reference; detached worker and JNI borrows own others.
 * Removing a generation from the registry closes access before asynchronous teardown. */
#define SLOTS 64
#define INPUT_LIMIT (1024 * 1024)
#define COMMAND_LIMIT 1024

typedef struct command {
    struct command *next;
    int kind, key, mods;
    uint64_t action_id;
    size_t length;
    uint8_t bytes[];
} command;
typedef struct session {
    size_t refs;
    atomic_bool stop, frame_requested;
    pthread_mutex_t mutex;
    char *home;
    command *first, *last;
    size_t queued_bytes, queued_commands;
    bool size_pending;
    uint16_t cols, rows, cw, ch;
    int state, exit_code, child;
    bool exit_known;
    char error[256];
    uint64_t action_sequence, action_id;
    int action_status; /* 0 pending, 1 success, 2 confirmation, 3 error */
    uint8_t *action_result;
    size_t action_length;
    int32_t *frame;
    size_t frame_length;
    uint64_t revision, frame_revision;
    /* Below: exclusively accessed by owner. */
    gt_transport_t *transport;
    gat_surface *surface;
} session;
static struct { session *value; uint64_t generation; } registry[SLOTS];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int owners;

static void fail(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env, type, message);
}
static void release(session *s) {
    pthread_mutex_lock(&registry_mutex);
    bool destroy = --s->refs == 0;
    pthread_mutex_unlock(&registry_mutex);
    if (!destroy) return;
    command *cmd = s->first;
    while (cmd) { command *next = cmd->next; free(cmd); cmd = next; }
    free(s->action_result); free(s->frame); free(s->home); pthread_mutex_destroy(&s->mutex); free(s);
}
static session *borrow(JNIEnv *env, jlong id) {
    if (id <= 0) { fail(env, "终端会话已关闭"); return NULL; }
    size_t slot = (uint64_t)id % SLOTS; uint64_t generation = (uint64_t)id / SLOTS;
    pthread_mutex_lock(&registry_mutex);
    session *s = registry[slot].generation == generation ? registry[slot].value : NULL;
    if (s) s->refs++;
    pthread_mutex_unlock(&registry_mutex);
    if (!s) fail(env, "终端会话已关闭");
    return s;
}
static void failed(session *s, const char *reason) {
    pthread_mutex_lock(&s->mutex);
    s->state = 3; snprintf(s->error, sizeof(s->error), "%s", reason);
    pthread_mutex_unlock(&s->mutex);
    atomic_store(&s->stop, true);
}
static bool write_input(void *context, const uint8_t *bytes, size_t length) {
    session *s = context;
    if (atomic_load(&s->stop)) return false;
    if (!length) return true;
    if (gt_transport_write(s->transport, bytes, length) == GT_OK) return true;
    failed(s, "终端输入失败，连接已停止"); return false;
}
static bool publish(session *s) {
    int32_t *frame = NULL; size_t length = 0;
    if (!gat_surface_frame(s->surface, &frame, &length)) { failed(s, "无法生成终端画面"); return false; }
    pthread_mutex_lock(&s->mutex);
    int32_t *previous = s->frame;
    s->frame = frame; s->frame_length = length; s->frame_revision = s->revision;
    pthread_mutex_unlock(&s->mutex);
    free(previous); return true;
}
static void *owner(void *context) {
    session *s = context;
    struct stat home_stat;
    if (stat(s->home, &home_stat) != 0 || !S_ISDIR(home_stat.st_mode) || access(s->home, X_OK) != 0) {
        failed(s, "本地终端目录不存在或不可访问"); goto finished;
    }
    char *home_env = malloc(strlen(s->home) + 6);
    if (!home_env) { failed(s, "无法准备本地终端环境"); goto finished; }
    sprintf(home_env, "HOME=%s", s->home);
    const char *env[] = {home_env, "SHELL=/system/bin/sh", NULL};
    gt_pty_opts_t opts = {.executable = "/system/bin/sh", .env = env, .cwd = s->home,
        .term = "xterm-256color", .cmd_kind = GT_PTY_CMD_SHELL, .cols = 80, .rows = 24};
    if (access(opts.executable, X_OK) != 0) { free(home_env); failed(s, "此设备不支持系统 shell /system/bin/sh"); goto finished; }
    if (!atomic_load(&s->stop)) s->transport = gt_transport_open_pty(&opts);
    free(home_env);
    if (!s->transport) { if (!atomic_load(&s->stop)) failed(s, gt_last_error()); goto finished; }
    const char *engine_error = NULL;
    s->surface = gat_surface_new(write_input, s, &engine_error);
    if (!s->surface) { failed(s, engine_error); goto finished; }
    pthread_mutex_lock(&s->mutex); s->state = 1; pthread_mutex_unlock(&s->mutex);
    s->revision = 1;
    uint8_t output[4096];
    while (!atomic_load(&s->stop)) {
        pthread_mutex_lock(&s->mutex);
        bool resize = s->size_pending; s->size_pending = false;
        uint16_t cols = s->cols, rows = s->rows, cw = s->cw, ch = s->ch;
        command *cmd = s->first;
        if (cmd) {
            s->first = cmd->next; if (!s->first) s->last = NULL;
            s->queued_bytes -= cmd->length; s->queued_commands--;
        }
        pthread_mutex_unlock(&s->mutex);
        if (resize) {
            if ((s->transport && gt_transport_resize(s->transport, cols, rows, (uint32_t)cols * cw, (uint32_t)rows * ch) != GT_OK) ||
                !gat_surface_resize(s->surface, cols, rows, cw, ch)) {
                free(cmd); failed(s, "终端尺寸更新失败"); break;
            }
            s->revision++;
        }
        bool had_input = cmd != NULL;
        if (cmd) {
            bool ok = true;
            if (cmd->action_id) {
                pthread_mutex_lock(&s->mutex);
                bool current = s->action_id == cmd->action_id;
                pthread_mutex_unlock(&s->mutex);
                uint8_t *result = NULL; size_t result_length = 0;
                int status = 2;
                if (current && (cmd->kind != 1 || s->transport))
                    status = gat_surface_action(s->surface, cmd->kind, cmd->key, cmd->mods, cmd->bytes, cmd->length, &result, &result_length);
                pthread_mutex_lock(&s->mutex);
                if (s->action_id == cmd->action_id) {
                    s->action_status = status + 1; s->action_result = result; s->action_length = result_length; result = NULL;
                }
                pthread_mutex_unlock(&s->mutex);
                free(result);
            } else if (cmd->kind == 1) ok = gat_surface_text(s->surface, cmd->bytes, cmd->length);
            else if (cmd->kind == 2) ok = gat_surface_key(s->surface, cmd->key, cmd->mods, cmd->bytes, cmd->length);
            else if (cmd->kind == 3) gat_surface_scroll(s->surface, cmd->key);
            else ok = false;
            free(cmd);
            if (!ok) { failed(s, "终端输入或按键编码失败"); break; }
            s->revision++;
        }
        int length = GT_TRANSPORT_READ_TIMEOUT;
        if (s->transport) length = gt_transport_read(s->transport, output, sizeof(output), had_input ? 0 : 20);
        else { struct timespec delay = {.tv_nsec = 20 * 1000 * 1000}; nanosleep(&delay, NULL); }
        if (length > 0) { gat_surface_feed(s->surface, output, (size_t)length); s->revision++; }
        else if (length == GT_TRANSPORT_READ_ERROR) { failed(s, "终端读取失败"); break; }
        else if (length == GT_TRANSPORT_READ_EOF) {
            int code = 0; bool known = gt_transport_exited(s->transport, &code);
            /* Slave EOF can precede the waitpid thread. Allow bounded reaping;
             * no known result by the deadline remains UNKNOWN, never zero. */
            for (int attempt = 0; !known && attempt < 20 && !atomic_load(&s->stop); attempt++) {
                if (gt_transport_child_state(s->transport) == GT_TRANSPORT_CHILD_EXITED) break;
                struct timespec delay = {.tv_nsec = 10 * 1000 * 1000}; nanosleep(&delay, NULL);
                known = gt_transport_exited(s->transport, &code);
            }
            pthread_mutex_lock(&s->mutex);
            s->state = 2; s->exit_known = known; s->exit_code = code; s->child = GT_TRANSPORT_CHILD_EXITED;
            pthread_mutex_unlock(&s->mutex);
            gt_transport_close(s->transport); s->transport = NULL;
            s->revision++; (void)publish(s);
        }
        int child = s->transport ? gt_transport_child_state(s->transport) : GT_TRANSPORT_CHILD_EXITED;
        pthread_mutex_lock(&s->mutex); s->child = child; pthread_mutex_unlock(&s->mutex);
        if (atomic_exchange(&s->frame_requested, false) && s->revision != s->frame_revision && !publish(s)) break;
    }
finished:
    /* Never join or free on the UI thread. Registry close only cancels. */
    gt_transport_close(s->transport); s->transport = NULL;
    gat_surface_free(s->surface); s->surface = NULL;
    atomic_fetch_sub(&owners, 1); release(s); return NULL;
}

JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_create(JNIEnv *env, jobject self, jbyteArray home) {
    (void)self;
    if (!home) { fail(env, "终端目录缺失"); return 0; }
    jsize n = (*env)->GetArrayLength(env, home);
    if (n <= 0 || n > 4096) { fail(env, "终端目录无效"); return 0; }
    session *s = calloc(1, sizeof(*s));
    if (!s) { fail(env, "无法分配终端会话"); return 0; }
    s->home = malloc((size_t)n + 1);
    if (!s->home) { free(s); fail(env, "无法分配终端目录"); return 0; }
    (*env)->GetByteArrayRegion(env, home, 0, n, (jbyte *)s->home); s->home[n] = 0;
    if ((*env)->ExceptionCheck(env) || s->home[0] != '/' || memchr(s->home, 0, (size_t)n)) {
        free(s->home); free(s); if (!(*env)->ExceptionCheck(env)) fail(env, "终端目录无效"); return 0;
    }
    if (pthread_mutex_init(&s->mutex, NULL)) { free(s->home); free(s); fail(env, "无法初始化终端锁"); return 0; }
    atomic_init(&s->stop, false); atomic_init(&s->frame_requested, true);
    s->refs = 2;
    pthread_mutex_lock(&registry_mutex);
    size_t slot = 0; while (slot < SLOTS && registry[slot].value) slot++;
    if (slot == SLOTS) { pthread_mutex_unlock(&registry_mutex); s->refs = 1; release(s); fail(env, "终端会话数量已达上限"); return 0; }
    uint64_t generation = ++registry[slot].generation;
    registry[slot].value = s;
    jlong id = (jlong)(generation * SLOTS + slot);
    pthread_mutex_unlock(&registry_mutex);
    pthread_t thread; pthread_attr_t attr;
    pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&owners, 1);
    int rc = pthread_create(&thread, &attr, owner, s); pthread_attr_destroy(&attr);
    if (rc) {
        atomic_fetch_sub(&owners, 1);
        pthread_mutex_lock(&registry_mutex); registry[slot].value = NULL; pthread_mutex_unlock(&registry_mutex);
        release(s); release(s); fail(env, "无法启动终端线程"); return 0;
    }
    return id;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_dispose(JNIEnv *env, jobject self, jlong id) {
    (void)env; (void)self;
    if (id <= 0) return;
    size_t slot = (uint64_t)id % SLOTS; uint64_t generation = (uint64_t)id / SLOTS;
    pthread_mutex_lock(&registry_mutex);
    session *s = registry[slot].generation == generation ? registry[slot].value : NULL;
    if (s) { registry[slot].value = NULL; atomic_store(&s->stop, true); }
    pthread_mutex_unlock(&registry_mutex);
    if (s) release(s);
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_enqueue(JNIEnv *env, jobject self, jlong id,
        jint kind, jint key, jint mods, jbyteArray bytes) {
    (void)self;
    if (!bytes || kind < 1 || kind > 3 || mods < 0 || mods > 15) { fail(env, "终端输入无效"); return; }
    jsize length = (*env)->GetArrayLength(env, bytes);
    if (length > INPUT_LIMIT) { fail(env, "输入内容过长，请分段输入"); return; }
    session *s = borrow(env, id); if (!s) return;
    command *cmd = malloc(sizeof(*cmd) + (size_t)length);
    if (!cmd) { release(s); fail(env, "无法分配终端输入"); return; }
    *cmd = (command){.kind = kind, .key = key, .mods = mods, .length = (size_t)length};
    (*env)->GetByteArrayRegion(env, bytes, 0, length, (jbyte *)cmd->bytes);
    if ((*env)->ExceptionCheck(env)) { free(cmd); release(s); return; }
    pthread_mutex_lock(&s->mutex);
    bool connected = (s->state <= 1 || (kind == 3 && s->state == 2)) && !atomic_load(&s->stop);
    bool room = s->queued_bytes + (size_t)length <= INPUT_LIMIT && s->queued_commands < COMMAND_LIMIT;
    if (connected && room) {
        if (s->last) s->last->next = cmd; else s->first = cmd;
        s->last = cmd; s->queued_bytes += (size_t)length; s->queued_commands++;
    }
    pthread_mutex_unlock(&s->mutex); release(s);
    if (!connected || !room) { free(cmd); fail(env, connected ? "终端输入队列已满，请稍后重试" : "终端已断开"); }
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_setSize(JNIEnv *env, jobject self, jlong id,
        jint cols, jint rows, jint cw, jint ch) {
    (void)self;
    if (cols < 1 || rows < 1 || cols > 256 || rows > 256 || cw < 1 || ch < 1 || cw > 512 || ch > 512 ||
        cols * cw > 65535 || rows * ch > 65535) { fail(env, "终端尺寸超出支持范围"); return; }
    session *s = borrow(env, id); if (!s) return;
    pthread_mutex_lock(&s->mutex);
    s->cols = (uint16_t)cols; s->rows = (uint16_t)rows; s->cw = (uint16_t)cw; s->ch = (uint16_t)ch; s->size_pending = true;
    pthread_mutex_unlock(&s->mutex); release(s);
}
JNIEXPORT jintArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_readFrame(JNIEnv *env, jobject self, jlong id, jlong last) {
    (void)self;
    session *s = borrow(env, id); if (!s) return NULL;
    atomic_store(&s->frame_requested, true);
    jintArray result = NULL;
    pthread_mutex_lock(&s->mutex);
    if (s->frame && s->frame_revision != (uint64_t)last) {
        result = (*env)->NewIntArray(env, (jsize)s->frame_length + 2);
        if (result) {
            jint revision[2] = {(jint)s->frame_revision, (jint)(s->frame_revision >> 32)};
            (*env)->SetIntArrayRegion(env, result, 0, 2, revision);
            (*env)->SetIntArrayRegion(env, result, 2, (jsize)s->frame_length, s->frame);
        }
    }
    pthread_mutex_unlock(&s->mutex); release(s); return result;
}
JNIEXPORT jintArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_readState(JNIEnv *env, jobject self, jlong id) {
    (void)self;
    session *s = borrow(env, id); if (!s) return NULL;
    pthread_mutex_lock(&s->mutex);
    jint values[] = {s->state, s->exit_known, s->exit_code, s->child};
    pthread_mutex_unlock(&s->mutex); release(s);
    jintArray result = (*env)->NewIntArray(env, 4);
    if (result) (*env)->SetIntArrayRegion(env, result, 0, 4, values);
    return result;
}
JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_readError(JNIEnv *env, jobject self, jlong id) {
    (void)self;
    session *s = borrow(env, id); if (!s) return NULL;
    pthread_mutex_lock(&s->mutex);
    jsize length = (jsize)strlen(s->error);
    jbyteArray result = (*env)->NewByteArray(env, length);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, length, (const jbyte *)s->error);
    pthread_mutex_unlock(&s->mutex); release(s); return result;
}
JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_ownerCount(JNIEnv *env, jobject self) {
    (void)env; (void)self; return atomic_load(&owners);
}

/* One acknowledged control request per session. A cancelled await removes the
 * result slot; the owner discards stale completions without touching a newer one. */
JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_beginAction(JNIEnv *env, jobject self, jlong id,
        jint kind, jint x, jint y, jbyteArray bytes) {
    (void)self;
    if (!bytes || kind < 1 || kind > 11) { fail(env, "文本操作无效"); return 0; }
    jsize length = (*env)->GetArrayLength(env, bytes);
    if (length > INPUT_LIMIT) { fail(env, "文本超过 1 MiB 限制"); return 0; }
    session *s = borrow(env, id); if (!s) return 0;
    command *cmd = malloc(sizeof(*cmd) + (size_t)length);
    if (!cmd) { release(s); fail(env, "无法准备文本操作"); return 0; }
    *cmd = (command){.kind = kind, .key = x, .mods = y, .length = (size_t)length};
    (*env)->GetByteArrayRegion(env, bytes, 0, length, (jbyte *)cmd->bytes);
    if ((*env)->ExceptionCheck(env)) { free(cmd); release(s); return 0; }
    pthread_mutex_lock(&s->mutex);
    bool ready = !s->action_id && !atomic_load(&s->stop) && s->state != 3 && (kind != 1 || s->state <= 1);
    bool room = s->queued_bytes + (size_t)length <= INPUT_LIMIT && s->queued_commands < COMMAND_LIMIT;
    uint64_t request = 0;
    if (ready && room) {
        request = ++s->action_sequence; s->action_id = request; s->action_status = 0; cmd->action_id = request;
        if (s->last) s->last->next = cmd; else s->first = cmd;
        s->last = cmd; s->queued_bytes += (size_t)length; s->queued_commands++;
    }
    pthread_mutex_unlock(&s->mutex); release(s);
    if (!request) { free(cmd); fail(env, "终端文本操作不可用或忙碌"); }
    return (jlong)request;
}
JNIEXPORT jbyteArray JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_readAction(JNIEnv *env, jobject self, jlong id, jlong request) {
    (void)self;
    session *s = borrow(env, id); if (!s) return NULL;
    jbyteArray result = NULL;
    pthread_mutex_lock(&s->mutex);
    if (s->action_id != (uint64_t)request) fail(env, "文本操作已失效");
    else if (s->action_status || atomic_load(&s->stop)) {
        jbyte status = s->action_status ? s->action_status : 3;
        result = (*env)->NewByteArray(env, (jsize)s->action_length + 1);
        if (result) {
            (*env)->SetByteArrayRegion(env, result, 0, 1, &status);
            if (s->action_length) (*env)->SetByteArrayRegion(env, result, 1, (jsize)s->action_length, (const jbyte *)s->action_result);
        }
    }
    pthread_mutex_unlock(&s->mutex); release(s); return result;
}
JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeSession_endAction(JNIEnv *env, jobject self, jlong id, jlong request) {
    (void)self;
    session *s = borrow(env, id); if (!s) return;
    pthread_mutex_lock(&s->mutex);
    if (s->action_id == (uint64_t)request) {
        s->action_id = 0; s->action_status = 0;
        free(s->action_result); s->action_result = NULL; s->action_length = 0;
    }
    pthread_mutex_unlock(&s->mutex); release(s);
}
