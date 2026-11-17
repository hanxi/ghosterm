/* Passive, untrusted discovery only. Never modifies or retains terminal output,
 * opens a path, authenticates a target, or authorizes a plugin. */
#ifndef GHOSTERM_EXTENSION_DISCOVERY_H
#define GHOSTERM_EXTENSION_DISCOVERY_H
#include "types.h"

#define GT_EXT_DISCOVERY_DIRECTORY_MAX 512u
#define GT_EXT_DISCOVERY_PENDING_MAX 8u
typedef struct gt_ext_discovery gt_ext_discovery_t;

typedef struct {
    bool available;
    uint32_t skipped;
    uint64_t sequence;
    char directory[GT_EXT_DISCOVERY_DIRECTORY_MAX + 1];
    char instance[33];
    char client_digest[65];
} gt_ext_discovery_snapshot_t;

/* Calls for one owner must be externally serialized. The passive observer is
 * independent of JS availability; platform policy decides whether to open it.
 * All input bytes still go directly to TerminalSurface, including unknown OSC,
 * APC, DCS and this discovery OSC. No business carrier is negotiated here. */
gt_status_t gt_ext_discovery_open(gt_ext_discovery_t **out);
/* At most 64 KiB per call. now_ms is caller-supplied nondecreasing monotonic time;
 * an empty feed may expire an incomplete recognized offer after 500 ms. */
gt_status_t gt_ext_discovery_feed(gt_ext_discovery_t *, const void *, size_t, uint64_t now_ms);
/* Consumes at most one candidate, with skipped count even when none is ready.
 * An available snapshot is an unverified hint, never evidence of a live owner. */
gt_status_t gt_ext_discovery_poll(gt_ext_discovery_t *, gt_ext_discovery_snapshot_t *);
void gt_ext_discovery_close(gt_ext_discovery_t **);
#endif
