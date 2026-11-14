#define _POSIX_C_SOURCE 200809L
#include "extension/process-jobs.h"
#include "extension/memory.h"
#include "extension/session-owner.h"
#include "extension/session.h"
#include "base/secure-random.h"
#include <string.h>
#include <errno.h>
#include <time.h>

typedef struct {
    gt_ext_process_t *child;
    uint64_t id, deadline, stop_at;
    gt_ext_process_info_t info;
    unsigned reason;
    bool stopping, killed;
} process_job_t;

struct gt_process_jobs {
    gt_ext_session_t *session;
    gt_ext_lease_t lease;
    gt_runtime_resources_t *roots;
    gt_ext_process_service_t *service;
    process_job_t jobs[GT_PROCESS_JOBS_MAX];
    char environment_bytes[GT_EXT_PROCESS_ENVIRONMENT_BYTES_MAX];
    const char *environment[GT_EXT_PROCESS_ENVIRONMENT_MAX];
    size_t environment_count;
    uint64_t next, output_limit, output_bytes;
    uint32_t lifetime_ms, grace_ms;
    bool stopped;
    int group_error;
};

static bool jobs_now(uint64_t *out) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return false;
    }
    *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    return true;
}

static bool jobs_live(const gt_process_jobs_t *j) {
    gt_ext_lease_t live;
    return j && !j->stopped && gt_ext_session_lease(j->session, &live) &&
           live.epoch == j->lease.epoch && !strcmp(live.instance, j->lease.instance) &&
           !strcmp(live.connection, j->lease.connection) && !strcmp(live.token, j->lease.token) &&
           gt_ext_session_access(j->session, &j->lease, "process.spawn") == GT_EXT_ACCESS_ALLOWED &&
           gt_ext_session_access(j->session, &j->lease, "process.pipes") == GT_EXT_ACCESS_ALLOWED;
}

static process_job_t *jobs_find(gt_process_jobs_t *j, uint64_t id) {
    if (j && id) {
        for (size_t i = 0; i < GT_PROCESS_JOBS_MAX; i++) {
            if (j->jobs[i].id == id) {
                return &j->jobs[i];
            }
        }
    }
    return NULL;
}

static gt_status_t jobs_stop_one(gt_process_jobs_t *j, process_job_t *s, uint64_t now) {
    if (s->info.exited || s->stopping) {
        return GT_OK;
    }
    if (gt__ext_process_signal(s->child, SIGTERM) != GT_OK) {
        return GT_ERR_IO;
    }
    s->stopping = true;
    s->stop_at = now;
    (void)j;
    return GT_OK;
}

gt_status_t gt__process_jobs_open(gt_ext_session_t *session,
                                  gt_runtime_resources_t *roots,
                                  const gt_package_checked_t *package,
                                  const gt_process_jobs_config_t *c,
                                  gt_process_jobs_t **out) {
    if (out) {
        *out = NULL;
    }
    gt_ext_lease_t lease;
    gt_package_check_info_t info;
    if (!out || !c || !gt__runtime_resources_matches(roots, session) ||
        !gt_ext_session_lease(session, &lease) || !gt__runtime_resources_roots(roots) ||
        !gt__package_checked_info(package, &info) ||
        !gt__bridge_session_package_matches(session->inner,
                                            GT_BRIDGE_EXECUTOR,
                                            info.id,
                                            info.version,
                                            info.package_digest,
                                            info.sdk_version) ||
        !c->program_count || !c->programs || c->program_count > GT_EXT_PROCESS_PROGRAMS_MAX ||
        c->lifetime_ms > 3600000 || c->grace_ms > 5000 || c->output_bytes > 67108864 ||
        !gt__ext_process_environment_valid(c->environment, c->environment_count)) {
        return GT_ERR_INVALID_ARG;
    }
    gt_process_jobs_t *j = gt__plugin_memory_calloc(1, sizeof(*j));
    if (!j) {
        return GT_ERR_IO;
    }
    j->session = session;
    j->lease = lease;
    j->roots = roots;
    j->next = 1;
    j->lifetime_ms = c->lifetime_ms ? c->lifetime_ms : 60000;
    j->grace_ms = c->grace_ms ? c->grace_ms : 100;
    j->output_limit = c->output_bytes ? c->output_bytes : 1048576;
    size_t used = 0;
    for (size_t i = 0; i < c->environment_count; i++) {
        size_t n = strlen(c->environment[i]) + 1;
        if (n > sizeof(j->environment_bytes) - used || !strncmp(c->environment[i], "LD_", 3) ||
            !strncmp(c->environment[i], "DYLD_", 5) ||
            !strncmp(c->environment[i], "NODE_OPTIONS=", 13) ||
            !strncmp(c->environment[i], "NODE_PATH=", 10)) {
            gt__plugin_memory_free(NULL, j, sizeof(*j));
            return GT_ERR_INVALID_ARG;
        }
        memcpy(j->environment_bytes + used, c->environment[i], n);
        j->environment[i] = j->environment_bytes + used;
        used += n;
    }
    j->environment_count = c->environment_count;
    gt_status_t status = gt__ext_process_service_open(
        session, &lease, package, c->programs, c->program_count, &j->service);
    if (status != GT_OK) {
        gt__plugin_memory_free(NULL, j, sizeof(*j));
        return status;
    }
    *out = j;
    return GT_OK;
}

bool gt__process_jobs_matches(const gt_process_jobs_t *j, gt_ext_session_t *session) {
    return j && j->session == session && j->next == 1 && jobs_live(j);
}

gt_status_t gt__process_jobs_spawn(gt_process_jobs_t *j,
                                   const char *program,
                                   const char *root,
                                   const char *const *argv,
                                   size_t argc,
                                   uint64_t *out,
                                   const char **version) {
    if (out) {
        *out = 0;
    }
    if (version) {
        *version = NULL;
    }
    if (!j || !program || !root || !out || !version) {
        return GT_ERR_INVALID_ARG;
    }
    if (!jobs_live(j)) {
        return GT_ERR_AUTH;
    }
    int fd = -1;
    for (size_t i = 0; i < gt__runtime_resources_roots(j->roots); i++) {
        if (!strcmp(root, gt__runtime_resources_root(j->roots, i))) {
            fd = gt__runtime_resources_root_fd(j->roots, i);
        }
    }
    if (fd < 0) {
        return GT_ERR_AUTH;
    }
    process_job_t *s = NULL;
    for (size_t i = 0; i < GT_PROCESS_JOBS_MAX; i++) {
        if (!j->jobs[i].id) {
            s = &j->jobs[i];
            break;
        }
    }
    if (!s || !j->next) {
        return GT_PROCESS_JOBS_WAIT;
    }
    uint64_t now;
    if (!jobs_now(&now) || now > UINT64_MAX - j->lifetime_ms) {
        return GT_ERR_IO;
    }
    gt_status_t status = gt__ext_process_service_spawn(
        j->service, program, argv, argc, fd, j->environment, j->environment_count, &s->child);
    if (status != GT_OK) {
        return status;
    }
    s->id = j->next++;
    s->deadline = now + j->lifetime_ms;
    *out = s->id;
    *version = gt__ext_process_service_version(j->service, program);
    return GT_OK;
}

gt_status_t gt__process_jobs_step(gt_process_jobs_t *j) {
    if (!j) {
        return GT_OK;
    }
    uint64_t now;
    bool clock_ok = jobs_now(&now);
    if (!clock_ok) {
        now = 0;
    }
    if (!jobs_live(j) || !clock_ok) {
        j->stopped = true;
    }
    gt_status_t result = clock_ok ? GT_OK : GT_ERR_IO;
    for (size_t i = 0; i < GT_PROCESS_JOBS_MAX; i++) {
        process_job_t *s = &j->jobs[i];
        if (!s->child) {
            continue;
        }
        if (!s->info.exited) {
            gt_ext_process_info_t info;
            if (gt__ext_process_observe_exit(s->child, &info) != GT_OK) {
                result = GT_ERR_IO;
                continue;
            }
            if (!info.exited && (j->stopped || now >= s->deadline)) {
                if (!j->stopped && !s->reason) {
                    s->reason = 1;
                }
                if (jobs_stop_one(j, s, now) != GT_OK) {
                    result = GT_ERR_IO;
                }
            }
            /* Fence the group before waitpid can release/reuse its leader PID. */
            if (!s->killed &&
                (info.exited || !clock_ok || (s->stopping && now - s->stop_at >= j->grace_ms))) {
                if (gt__ext_process_signal(s->child, SIGKILL) != GT_OK) {
                    if (!info.exited) {
                        result = GT_ERR_IO;
                        continue;
                    }
                    /* macOS may refuse signalling an already exited platform
                     * binary's group. Preserve errno, still reap the leader. */
                    if (!j->group_error) {
                        j->group_error = errno ? errno : EIO;
                    }
                }
                s->killed = true;
            }
            if (info.exited && gt__ext_process_poll(s->child, &s->info) != GT_OK) {
                result = GT_ERR_IO;
            }
        }
        if (j->stopped && s->info.exited) {
            if (gt__ext_process_destroy(&s->child) != GT_OK) {
                result = GT_ERR_IO;
            } else {
                memset(s, 0, sizeof(*s));
            }
        }
    }
    return result;
}

gt_ext_process_io_t
gt__process_jobs_read(gt_process_jobs_t *j, uint64_t id, bool errors, void *bytes, size_t cap) {
    process_job_t *s = jobs_find(j, id);
    if (!s || !bytes || !cap || cap > GT_PROCESS_JOBS_IO_MAX) {
        return (gt_ext_process_io_t){GT_EXT_PROCESS_ERROR, 0, GT_ERR_INVALID_ARG};
    }
    if (!jobs_live(j)) {
        return (gt_ext_process_io_t){GT_EXT_PROCESS_CLOSED, 0, GT_ERR_AUTH};
    }
    if (s->reason == 2) {
        return (gt_ext_process_io_t){GT_EXT_PROCESS_ERROR, 0, GT_ERR_IO};
    }
    gt_ext_process_io_t io = errors ? gt__ext_process_read_stderr(s->child, bytes, cap)
                                    : gt__ext_process_read_stdout(s->child, bytes, cap);
    if (io.count > j->output_limit - j->output_bytes) {
        uint64_t now;
        if (!s->reason) {
            s->reason = 2;
        }
        if (!jobs_now(&now) || jobs_stop_one(j, s, now) != GT_OK) {
            j->stopped = true;
        }
        memset(bytes, 0, io.count);
        return (gt_ext_process_io_t){GT_EXT_PROCESS_ERROR, 0, GT_ERR_IO};
    }
    j->output_bytes += io.count;
    return io;
}

gt_ext_process_io_t
gt__process_jobs_write(gt_process_jobs_t *j, uint64_t id, const void *bytes, size_t size) {
    process_job_t *s = jobs_find(j, id);
    if (!s || !bytes || !size || size > GT_PROCESS_JOBS_IO_MAX) {
        return (gt_ext_process_io_t){GT_EXT_PROCESS_ERROR, 0, GT_ERR_INVALID_ARG};
    }
    if (!jobs_live(j) || s->stopping) {
        return (gt_ext_process_io_t){GT_EXT_PROCESS_CLOSED, 0, GT_ERR_AUTH};
    }
    return gt__ext_process_write(s->child, bytes, size);
}

gt_status_t gt__process_jobs_input_close(gt_process_jobs_t *j, uint64_t id) {
    process_job_t *s = jobs_find(j, id);
    return s ? gt__ext_process_close_stdin(s->child) : GT_ERR_INVALID_ARG;
}

gt_status_t gt__process_jobs_signal(gt_process_jobs_t *j, uint64_t id, bool kill) {
    process_job_t *s = jobs_find(j, id);
    if (!s) {
        return GT_ERR_INVALID_ARG;
    }
    if (s->info.exited) {
        return GT_OK;
    }
    if (kill) {
        gt_status_t status = gt__ext_process_signal(s->child, SIGKILL);
        if (status == GT_OK) {
            s->killed = s->stopping = true;
        }
        return status;
    }
    uint64_t now;
    return jobs_now(&now) ? jobs_stop_one(j, s, now) : GT_ERR_IO;
}

gt_status_t gt__process_jobs_poll(gt_process_jobs_t *j, uint64_t id, gt_process_job_info_t *out) {
    process_job_t *s = jobs_find(j, id);
    if (!s || !out) {
        return GT_ERR_INVALID_ARG;
    }
    *out = (gt_process_job_info_t){s->info,
                                   s->reason == 1   ? "deadline"
                                   : s->reason == 2 ? "output-limit"
                                                    : "none"};
    return GT_OK;
}

gt_status_t gt__process_jobs_release(gt_process_jobs_t *j, uint64_t id) {
    process_job_t *s = jobs_find(j, id);
    if (!s) {
        return GT_ERR_INVALID_ARG;
    }
    if (!s->info.exited) {
        return GT_PROCESS_JOBS_WAIT;
    }
    gt_status_t status = gt__ext_process_destroy(&s->child);
    if (status == GT_OK) {
        memset(s, 0, sizeof(*s));
    }
    return status;
}

void gt__process_jobs_stop(gt_process_jobs_t *j) {
    if (j) {
        j->stopped = true;
    }
}

gt_status_t gt__process_jobs_destroy(gt_process_jobs_t **out) {
    if (!out || !*out) {
        return GT_ERR_INVALID_ARG;
    }
    gt_process_jobs_t *j = *out;
    for (size_t i = 0; i < GT_PROCESS_JOBS_MAX; i++) {
        if (j->jobs[i].child) {
            return GT_PROCESS_JOBS_WAIT;
        }
    }
    gt_status_t status = gt__ext_process_service_destroy(&j->service);
    gt__secure_erase(j->environment_bytes, sizeof(j->environment_bytes));
    gt__plugin_memory_free(NULL, j, sizeof(*j));
    *out = NULL;
    return status;
}

int gt__process_jobs_group_error(const gt_process_jobs_t *j) {
    return j ? j->group_error : 0;
}
