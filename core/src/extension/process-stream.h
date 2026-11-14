/* Private native child provider for the common plugin byte stream. Not a
 * plugin process.spawn grant, shell, PTY, installer, daemon or public ABI. */
#ifndef GT_PROCESS_STREAM_H
#define GT_PROCESS_STREAM_H
#include "extension/process-owner.h"
#include "extension/stream.h"

#define GT_PROCESS_STREAM_DIAGNOSTIC_MAX 4096u

typedef struct {
    const char *executable;
    const char *const *arguments;
    size_t argument_count;
    int cwd_fd; /* borrowed during construction; caller explicitly chose it */
    const char *const *environment;
    size_t environment_count;
    bool execution_authorized; /* trusted Native approval, never plugin data */
    uint32_t lifetime_ms, stop_grace_ms;
    uint64_t stdout_limit, stderr_limit;
} gt_process_stream_config_t;

typedef enum {
    GT_PROCESS_STREAM_NONE,
    GT_PROCESS_STREAM_DEADLINE,
    GT_PROCESS_STREAM_STDOUT_LIMIT,
    GT_PROCESS_STREAM_STDERR_LIMIT
} gt_process_stream_reason_t;

typedef struct {
    gt_status_t error, cleanup_error;
    int group_signal_error; /* known leader exit can leave group cleanup unknown */
    gt_process_stream_reason_t reason;
    gt_ext_process_info_t child;
    bool reaped, group_fenced, stdout_eof, stderr_eof, stopping, diagnostic_truncated;
    uint64_t stdout_bytes, stderr_bytes;
    size_t diagnostic_size;
    uint8_t diagnostic[GT_PROCESS_STREAM_DIAGNOSTIC_MAX];
} gt_process_stream_info_t;

/* Starts exactly the explicitly approved absolute executable with raw argv,
 * selected environment and cwd. No shell, PATH search or ambient inheritance.
 * Native caller establishes executable trust; no publisher authentication or
 * OS sandbox is implied. Fork/exec has native latency; call only on an IO owner.
 * Zero limits: lifetime 60s, TERM grace 100ms, stdout 16MiB, stderr 64KiB.
 * Maxima: 1h / 5s / 64MiB / 1MiB. All time uses monotonic wall time.
 * Failure retains caller fds. Success owns a fresh process group and pipes.
 * stop only wakes; IO owner drives bounded close/reap. CLOSE_ERROR retains the
 * owner. Natural leader exit signals its group before reap to avoid PID reuse.
 * Darwin may reject an all-zombie group with EPERM: preserve group_signal_error,
 * reap the known leader and release local resources without claiming that its
 * descendants were cleaned. This observation is distinct from local release.
 * Pending buffers / diagnostics / memory remain charged until destroy. */
gt_status_t gt__bridge_process_open(const gt_process_stream_config_t *, gt_bridge_stream_t **);
bool gt__bridge_process_info(const gt_bridge_stream_t *, gt_process_stream_info_t *);
#endif
