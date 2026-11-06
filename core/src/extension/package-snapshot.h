/* Private physical package capture, before metadata/trust/authorization checks.
 * C11 + POSIX; single non-UI owner. Does not execute code, install files, follow
 * links, or claim that a captured directory is a valid/authenticated package.
 * Later validators must check NFC/Unicode case collisions, manifest semantics,
 * exact integrity coverage, signatures/source policy and host compatibility on
 * these retained bytes, then load these same bytes (never reopen the directory).
 * Files are immutable memory copies; namespace observations are not an atomic
 * filesystem snapshot. Metadata checks detect observed replacement/modification.
 * IO work is chunked, not a hard filesystem latency/deadline guarantee. */
#ifndef GT_PRIVATE_PACKAGE_SNAPSHOT_H
#define GT_PRIVATE_PACKAGE_SNAPSHOT_H
#include "base/allocator.h"
#include <stdbool.h>
#include <stdint.h>
#define GT_PACKAGE_MEMBERS_MAX 1024u
#define GT_PACKAGE_ENTRIES_MAX 4096u /* files + directories, excluding root */
#define GT_PACKAGE_MEMBER_BYTES_MAX 16777216u
#define GT_PACKAGE_TOTAL_BYTES_MAX 67108864u
#define GT_PACKAGE_PATH_BYTES_MAX 4096u
#define GT_PACKAGE_PATH_POINTS_MAX 1024u
#define GT_PACKAGE_CHUNK_MAX 16384u
typedef struct gt_package_snapshot gt_package_snapshot_t;
typedef enum {
 GT_PACKAGE_OK,GT_PACKAGE_WAIT,GT_PACKAGE_INVALID,GT_PACKAGE_LIMIT,
 GT_PACKAGE_IO,GT_PACKAGE_CHANGED,GT_PACKAGE_NOMEM,GT_PACKAGE_NOT_FOUND
} gt_package_status_t;
typedef struct {
 int directory_fd;size_t members,entries;uint64_t member_bytes,total_bytes;
} gt_package_capture_config_t;
typedef struct {const char *path;uint64_t bytes;char sha256[65];} gt_package_member_t;
/* All limits explicit and bounded by the constants above. directory_fd borrowed,
 * its directory offset is untouched; owned opens are read-only and CLOEXEC.
 * Allocator context outlives destroy. Failed open leaves *out NULL. */
gt_package_status_t gt__package_capture_open(const gt_package_capture_config_t *,const gt_allocator_t *,gt_package_snapshot_t **out);
/* WAIT means more work. OK means all bytes retained and views available. Errors
 * are sticky until destroy; no partial catalog/read is exposed. Empty physical
 * directories can be captured, but are not valid logical plugin packages. */
gt_package_status_t gt__package_capture_step(gt_package_snapshot_t *);
bool gt__package_snapshot_count(const gt_package_snapshot_t *,size_t *count,uint64_t *bytes);
bool gt__package_snapshot_member(const gt_package_snapshot_t *,size_t,gt_package_member_t *);
gt_package_status_t gt__package_snapshot_find(const gt_package_snapshot_t *,const char *,size_t *index);
gt_package_status_t gt__package_snapshot_read(const gt_package_snapshot_t *,size_t,uint64_t,void *,size_t,size_t *read_bytes);
/* Private C validator/loader view; immutable, not necessarily NUL-terminated.
 * Borrowed until destroy, never handed across a platform/plugin boundary. */
bool gt__package_snapshot_view(const gt_package_snapshot_t *,size_t,const uint8_t **,size_t *);
/* Consumes even on close error; no fd is retried. Wipes copies before release. */
gt_package_status_t gt__package_snapshot_destroy(gt_package_snapshot_t **);
#endif
