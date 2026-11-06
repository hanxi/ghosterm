/* Private runtime resource owner. Native caller authorizes root fds and Store;
 * no JS/native lease, fd, snapshot token or filesystem path is exported.
 * One original serial non-UI owner, bounded disk work, no extra threads. */
#ifndef GT_RUNTIME_RESOURCE_H
#define GT_RUNTIME_RESOURCE_H
#include "ghosterm/extension_source.h"
#define GT_RUNTIME_ROOTS_MAX 8u
#define GT_RUNTIME_PINS_MAX 64u
typedef struct gt_runtime_resources gt_runtime_resources_t;
typedef struct {const char *id;int directory_fd;uint64_t byte_limit;} gt_runtime_root_t;
typedef enum {
 GT_RUNTIME_RESOURCE_OK,GT_RUNTIME_RESOURCE_WAIT,GT_RUNTIME_RESOURCE_INVALID,
 GT_RUNTIME_RESOURCE_DENIED,GT_RUNTIME_RESOURCE_UNSUPPORTED,GT_RUNTIME_RESOURCE_IO,
 GT_RUNTIME_RESOURCE_LIMIT,GT_RUNTIME_RESOURCE_CHANGED,GT_RUNTIME_RESOURCE_CANCELLED,
 GT_RUNTIME_RESOURCE_BUSY,GT_RUNTIME_RESOURCE_UNKNOWN,GT_RUNTIME_RESOURCE_CLOSED,
 GT_RUNTIME_RESOURCE_NOT_FOUND,GT_RUNTIME_RESOURCE_EXPIRED,GT_RUNTIME_RESOURCE_PROTOCOL
} gt_runtime_resource_status_t;
typedef struct {
 gt_ext_source_info_t source;bool expired,clock_error;
} gt_runtime_publication_t;
/* Copies root IDs and CLOEXEC duplicates, caller retains input fds. Store and
 * Session must outlive this owner. No source file opened/read during open.
 * Root-less resources can receive snapshots; no Store means UNSUPPORTED. */
gt_status_t gt__runtime_resources_open(gt_ext_session_t *,gt_ext_resource_store_t *,const gt_ext_lease_t *,const gt_runtime_root_t *,size_t,void *,bool (*clock)(void *,uint64_t *),gt_runtime_resources_t **);
size_t gt__runtime_resources_roots(const gt_runtime_resources_t *);
const char *gt__runtime_resources_root(const gt_runtime_resources_t *,size_t);
gt_runtime_resource_status_t gt__runtime_resources_publish(gt_runtime_resources_t *,const char *,const char *,const char *,uint64_t,uint32_t,uint64_t *);
/* One source <=16 KiB operation per step, round-robin. Session step is separate.
 * Each publication deadline includes hashing, sending and receipt waiting.
 * Cancel never invents absence of side effects. Confirmation remains Core's. */
void gt__runtime_resources_step(gt_runtime_resources_t *);
gt_runtime_resource_status_t gt__runtime_resources_publication(gt_runtime_resources_t *,uint64_t,gt_runtime_publication_t *);
gt_runtime_resource_status_t gt__runtime_resources_cancel(gt_runtime_resources_t *,uint64_t);
gt_runtime_resource_status_t gt__runtime_resources_forget(gt_runtime_resources_t *,uint64_t);
gt_runtime_resource_status_t gt__runtime_resources_take(gt_runtime_resources_t *,uint64_t *,gt_ext_resource_descriptor_t *);
gt_runtime_resource_status_t gt__runtime_resources_begin_read(gt_runtime_resources_t *,uint64_t,uint64_t,uint32_t);
gt_runtime_resource_status_t gt__runtime_resources_read(gt_runtime_resources_t *,uint64_t,uint64_t,void *,size_t,size_t *);
gt_runtime_resource_status_t gt__runtime_resources_release(gt_runtime_resources_t *,uint64_t);
/* Stop before Session close to prevent new effects. Once Session is stopped or
 * closed, cleanup destroys every Source and releases every snapshot pin before
 * Session destroy. WAIT/IO keep owner; consumed fd numbers never retried. */
void gt__runtime_resources_stop(gt_runtime_resources_t *);
gt_runtime_resource_status_t gt__runtime_resources_destroy(gt_runtime_resources_t **);
#endif
