/* Private package/module origin boundary. Only fixed checked bytes and the
 * host SDK capsule; no filesystem reopens, Node, plugin-provided SDK or grant.
 * These helpers borrow an existing checked reference on the caller's worker. */
#ifndef GT_PRIVATE_PACKAGE_RUNTIME_H
#define GT_PRIVATE_PACKAGE_RUNTIME_H
#include "extension/package-check.h"
#include "extension/session.h"
#define GT_RUNTIME_MODULE_NAME_MAX (GT_PACKAGE_PATH_BYTES_MAX+8u)
typedef struct {
 const uint8_t *bytes;size_t size;char sha256[65];
 const char *const *chunks;size_t chunk_count;
} gt_runtime_source_t;
bool gt__runtime_module_name(const char *);
bool gt__runtime_source_utf8(const void *,size_t);
/* No code evaluation. Native association selection is separate from consent:
 * a conflicting command cannot use the default unassociated open path. Required
 * services are restricted to the currently implemented RPC/resource adapters. */
gt_status_t gt__runtime_package_entry(gt_package_checked_t *,gt_bridge_session_t *,
 gt_bridge_role_t,const gt_bridge_lease_t *,bool associated,char *entry,size_t);
gt_status_t gt__runtime_package_source(const gt_package_checked_t *,const char *,gt_runtime_source_t *);
#endif
