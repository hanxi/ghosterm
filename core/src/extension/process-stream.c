#define _POSIX_C_SOURCE 200809L
#include "extension/process-stream.h"
#include "extension/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    gt_bridge_stream_t base;
    gt_ext_process_t *child;
    gt_process_stream_info_t info;
    int wake[2];
    uint64_t began, stop_at, lifetime, grace, stdout_limit, stderr_limit;
    bool closing;
} process_stream_t;

static bool now_ms(uint64_t *out) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return false;
    }
    *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return true;
}

static gt_bridge_io_t io(gt_bridge_io_state_t state, size_t count, gt_status_t error) {
    return (gt_bridge_io_t){state, count, error};
}

static void fail(process_stream_t *s, gt_status_t error) {
    if (!s->info.error) {
        s->info.error = error;
    }
}

static bool count_stdout(process_stream_t *s, size_t count) {
    s->info.stdout_bytes += count;
    if (s->info.stdout_bytes > s->stdout_limit) {
        if (!s->info.error) {
            s->info.reason = GT_PROCESS_STREAM_STDOUT_LIMIT;
        }
        fail(s, GT_ERR_IO);
        return false;
    }
    return true;
}

/* Reserve the zombie leader's PID until group cleanup has been attempted. No
 * process-group signal is ever sent after waitpid releases that identity. */
static bool observe(process_stream_t *s) {
    if (s->info.reaped) {
        return true;
    }
    gt_ext_process_info_t child;
    if (gt__ext_process_observe_exit(s->child, &child) != GT_OK) {
        s->info.cleanup_error = GT_ERR_IO;
        return false;
    }
    s->info.child = child;
    if (!child.exited) {
        return true;
    }
    if (!s->info.group_fenced && !s->info.group_signal_error) {
        if (gt__ext_process_signal(s->child, SIGKILL) != GT_OK) {
            int error = errno ? errno : EIO;
            if (error != EPERM) {
                s->info.cleanup_error = GT_ERR_IO;
                return false;
            }
            /* The leader is already observed exited and still unreaped.
             * Do not turn Darwin's all-zombie EPERM into a false group fence.
             * Preserve the same uncertain-group observation as process runtime. */
            s->info.group_signal_error = error;
        } else {
            s->info.group_fenced = true;
        }
    }
    if (gt__ext_process_poll(s->child, &child) != GT_OK) {
        s->info.cleanup_error = GT_ERR_IO;
        return false;
    }
    if (!child.exited) {
        return true;
    }
    if (!s->info.group_signal_error) {
        s->info.group_signal_error = gt__ext_process_group_error(s->child);
    }
    s->info.child = child;
    s->info.reaped = true;
    return true;
}

static bool signal_process(process_stream_t *s, int number) {
    if (gt__ext_process_reaping(s->child)) {
        return true;
    }
    if (gt__ext_process_signal(s->child, number) == GT_OK) {
        if (number == SIGKILL) {
            s->info.group_fenced = true;
        }
        return true;
    }
    int error = errno;
    /* Exit can race a successful final protocol read and the first TERM, or
     * the grace deadline's KILL. Only an observed dead leader permits reap. */
    if (error == EPERM) {
        if (!observe(s)) {
            return false;
        }
        if (!s->info.group_signal_error) {
            s->info.group_signal_error = error;
        }
        if (s->info.reaped || number == SIGTERM) {
            /* A dying Darwin leader may not be waitable yet. Keep the actual
             * signal error, and use the existing TERM grace for observation.
             * No signal is claimed delivered and the leader remains owned. */
            return true;
        }
    }
    s->info.cleanup_error = GT_ERR_IO;
    return false;
}

static bool drain_stderr(process_stream_t *s) {
    if (s->info.stderr_eof) {
        return true;
    }
    uint8_t bytes[GT_BRIDGE_IO_QUANTUM];
    gt_ext_process_io_t r = gt__ext_process_read_stderr(s->child, bytes, sizeof(bytes));
    if (r.state == GT_EXT_PROCESS_DATA) {
        s->info.stderr_bytes += r.count;
        size_t room = sizeof(s->info.diagnostic) - s->info.diagnostic_size;
        size_t keep = r.count < room ? r.count : room;
        memcpy(s->info.diagnostic + s->info.diagnostic_size, bytes, keep);
        s->info.diagnostic_size += keep;
        s->info.diagnostic_truncated |= keep < r.count;
        if (s->info.stderr_bytes > s->stderr_limit) {
            if (!s->info.error) {
                s->info.reason = GT_PROCESS_STREAM_STDERR_LIMIT;
            }
            fail(s, GT_ERR_IO);
        }
    } else if (r.state == GT_EXT_PROCESS_EOF) {
        s->info.stderr_eof = true;
    } else if (r.state != GT_EXT_PROCESS_WAIT) {
        fail(s, GT_ERR_IO);
        return false;
    }
    return true;
}

static gt_bridge_io_t
transfer(process_stream_t *s, void *bytes, size_t size, int ms, bool writing) {
    uint64_t now;
    if (!now_ms(&now)) {
        fail(s, GT_ERR_IO);
        return io(GT_BRIDGE_ERROR, 0, s->info.error);
    }
    uint64_t end = now + (unsigned)ms;
    for (;;) {
        if (atomic_load_explicit(&s->base.stopping, memory_order_acquire)) {
            return io(GT_BRIDGE_CLOSED, 0, GT_ERR_CANCELLED);
        }
        if (!now_ms(&now)) {
            fail(s, GT_ERR_IO);
        } else if (now - s->began >= s->lifetime) {
            if (!s->info.error) {
                s->info.reason = GT_PROCESS_STREAM_DEADLINE;
            }
            fail(s, GT_ERR_IO);
        }
        if (!s->info.error && (!drain_stderr(s) || !observe(s))) {
            fail(s, GT_ERR_IO);
        }
        if (s->info.error) {
            return io(GT_BRIDGE_ERROR, 0, s->info.error);
        }
        if (!writing && s->info.stdout_eof) {
            return io(GT_BRIDGE_EOF, 0, GT_OK);
        }
        gt_ext_process_io_t r = writing ? gt__ext_process_write(s->child, bytes, size)
                                        : gt__ext_process_read_stdout(s->child, bytes, size);
        if (r.state == GT_EXT_PROCESS_DATA) {
            if (!writing && !count_stdout(s, r.count)) {
                return io(GT_BRIDGE_ERROR, 0, s->info.error);
            }
            return io(GT_BRIDGE_DATA, r.count, GT_OK);
        }
        if (!writing && r.state == GT_EXT_PROCESS_EOF) {
            s->info.stdout_eof = true;
            return io(GT_BRIDGE_EOF, 0, GT_OK);
        }
        if (r.state != GT_EXT_PROCESS_WAIT) {
            fail(s, r.error ? r.error : GT_ERR_IO);
            return io(GT_BRIDGE_ERROR, 0, s->info.error);
        }
        if (!now_ms(&now)) {
            fail(s, GT_ERR_IO);
            return io(GT_BRIDGE_ERROR, 0, s->info.error);
        }
        if (now >= end) {
            return io(GT_BRIDGE_WAIT, 0, GT_OK);
        }
        uint64_t remaining = end - now;
        uint64_t life = now - s->began < s->lifetime ? s->lifetime - (now - s->began) : 0;
        if (remaining > life) {
            remaining = life;
        }
        unsigned interests = (writing ? GT_EXT_PROCESS_WAIT_INPUT : GT_EXT_PROCESS_WAIT_OUTPUT) |
                             GT_EXT_PROCESS_WAIT_ERROR;
        if (gt__ext_process_wait(s->child, interests, s->wake[0], (int)remaining) != GT_OK) {
            fail(s, GT_ERR_IO);
        }
    }
}

static gt_bridge_io_t process_read(gt_bridge_stream_t *base, void *bytes, size_t size, int ms) {
    return transfer((process_stream_t *)base, bytes, size, ms, false);
}

static gt_bridge_io_t
process_write(gt_bridge_stream_t *base, const void *bytes, size_t size, int ms) {
    return transfer((process_stream_t *)base, (void *)bytes, size, ms, true);
}

static void wake(gt_bridge_stream_t *base) {
    process_stream_t *s = (process_stream_t *)base;
    uint8_t byte = 1;
    while (write(s->wake[1], &byte, 1) < 0 && errno == EINTR) {
    }
}

static gt_bridge_close_t close_step(gt_bridge_stream_t *base, int ms) {
    process_stream_t *s = (process_stream_t *)base;
    uint64_t now;
    if (!now_ms(&now)) {
        s->info.cleanup_error = GT_ERR_IO;
        return GT_BRIDGE_CLOSE_ERROR;
    }
    uint64_t end = now + (unsigned)ms;
    if (!s->closing) {
        if (!observe(s) || gt__ext_process_close_stdin(s->child) != GT_OK ||
            (!s->info.reaped && !signal_process(s, SIGTERM))) {
            s->info.cleanup_error = GT_ERR_IO;
            return GT_BRIDGE_CLOSE_ERROR;
        }
        s->closing = true;
        s->stop_at = now;
    }
    uint8_t bytes[GT_BRIDGE_IO_QUANTUM];
    while (read(s->wake[0], bytes, sizeof(bytes)) > 0) {
    }
    for (;;) {
        if (!drain_stderr(s) || !observe(s)) {
            s->info.cleanup_error = GT_ERR_IO;
            return GT_BRIDGE_CLOSE_ERROR;
        }
        if (s->info.reaped) {
            if (gt__ext_process_destroy(&s->child) != GT_OK) {
                s->info.cleanup_error = GT_ERR_IO;
                return GT_BRIDGE_CLOSE_ERROR;
            }
            s->info.cleanup_error = GT_OK;
            return GT_BRIDGE_RELEASED;
        }
        if (!s->info.stdout_eof) {
            gt_ext_process_io_t r = gt__ext_process_read_stdout(s->child, bytes, sizeof(bytes));
            if (r.state == GT_EXT_PROCESS_EOF) {
                s->info.stdout_eof = true;
            } else if (r.state == GT_EXT_PROCESS_DATA) {
                (void)count_stdout(s, r.count);
            } else if (r.state != GT_EXT_PROCESS_DATA && r.state != GT_EXT_PROCESS_WAIT) {
                s->info.cleanup_error = GT_ERR_IO;
                return GT_BRIDGE_CLOSE_ERROR;
            }
        }
        if (!now_ms(&now)) {
            s->info.cleanup_error = GT_ERR_IO;
            return GT_BRIDGE_CLOSE_ERROR;
        }
        if (!s->info.group_fenced && now - s->stop_at >= s->grace) {
            if (!signal_process(s, SIGKILL)) {
                s->info.cleanup_error = GT_ERR_IO;
                return GT_BRIDGE_CLOSE_ERROR;
            }
        }
        if (now >= end) {
            return GT_BRIDGE_DRAINING;
        }
        /* Short cooperative close ticks also observe exit after both pipes
         * close. A child or descendant cannot extend the chosen grace period. */
        if (gt__ext_process_wait(
                s->child, GT_EXT_PROCESS_WAIT_OUTPUT | GT_EXT_PROCESS_WAIT_ERROR, -1, 1) != GT_OK) {
            s->info.cleanup_error = GT_ERR_IO;
            return GT_BRIDGE_CLOSE_ERROR;
        }
    }
}

static void destroy(gt_bridge_stream_t *base) {
    process_stream_t *s = (process_stream_t *)base;
    close(s->wake[0]);
    close(s->wake[1]);
    gt__plugin_memory_free(NULL, s, sizeof(*s));
}

static const gt_bridge_stream_ops_t operations = {
    process_read, process_write, wake, close_step, destroy};

gt_status_t gt__bridge_process_open(const gt_process_stream_config_t *config,
                                    gt_bridge_stream_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!config || !out || config->lifetime_ms > 3600000 || config->stop_grace_ms > 5000 ||
        config->stdout_limit > 67108864 || config->stderr_limit > 1048576) {
        return GT_ERR_INVALID_ARG;
    }
    if (!config->execution_authorized) {
        return GT_ERR_AUTH;
    }
    process_stream_t *s = gt__plugin_memory_calloc(1, sizeof(*s));
    if (!s) {
        return GT_ERR_IO;
    }
    s->wake[0] = s->wake[1] = -1;
    gt_status_t status = GT_ERR_IO;
    if (!now_ms(&s->began) || pipe(s->wake)) {
        goto failed;
    }
    for (size_t i = 0; i < 2; i++) {
        int flags = fcntl(s->wake[i], F_GETFL), descriptors = fcntl(s->wake[i], F_GETFD);
        if (flags < 0 || descriptors < 0 || fcntl(s->wake[i], F_SETFL, flags | O_NONBLOCK) ||
            fcntl(s->wake[i], F_SETFD, descriptors | FD_CLOEXEC)) {
            goto failed;
        }
    }
    status = gt__ext_process_spawn(config->executable,
                                   config->arguments,
                                   config->argument_count,
                                   config->cwd_fd,
                                   config->environment,
                                   config->environment_count,
                                   &s->child);
    if (status != GT_OK) {
        goto failed;
    }
    s->lifetime = config->lifetime_ms ? config->lifetime_ms : 60000;
    s->grace = config->stop_grace_ms ? config->stop_grace_ms : 100;
    s->stdout_limit = config->stdout_limit ? config->stdout_limit : 16777216;
    s->stderr_limit = config->stderr_limit ? config->stderr_limit : 65536;
    (void)gt__bridge_stream_init(&s->base, &operations);
    *out = &s->base;
    return GT_OK;
failed:
    if (s->wake[0] >= 0) {
        close(s->wake[0]);
    }
    if (s->wake[1] >= 0) {
        close(s->wake[1]);
    }
    gt__plugin_memory_free(NULL, s, sizeof(*s));
    return status;
}

bool gt__bridge_process_info(const gt_bridge_stream_t *base, gt_process_stream_info_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!base || !out || base->ops != &operations) {
        return false;
    }
    const process_stream_t *s = (const process_stream_t *)base;
    *out = s->info;
    out->stopping = atomic_load_explicit(&base->stopping, memory_order_acquire);
    return true;
}
