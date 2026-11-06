/* In-process, single-worker command catalog. No disk installation, signature
 * verification or execution approval. Native callers supply source decisions. */
#ifndef GHOSTERM_EXTENSION_REGISTRY_H
#define GHOSTERM_EXTENSION_REGISTRY_H
#include "ghosterm/extension_package.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_REGISTRY_PACKAGES_MAX 32u
typedef struct gt_ext_registry gt_ext_registry_t;
typedef enum {GT_EXT_REGISTRY_OK,GT_EXT_REGISTRY_INVALID,GT_EXT_REGISTRY_UNKNOWN,GT_EXT_REGISTRY_DENIED,GT_EXT_REGISTRY_LIMIT,GT_EXT_REGISTRY_MEMORY,GT_EXT_REGISTRY_NOT_FOUND,GT_EXT_REGISTRY_CONFLICT,GT_EXT_REGISTRY_STALE,GT_EXT_REGISTRY_IO,GT_EXT_REGISTRY_UNSUPPORTED,GT_EXT_REGISTRY_CANCELLED} gt_ext_registry_status_t;
typedef enum {GT_EXT_REGISTRY_SOURCE_UNKNOWN,GT_EXT_REGISTRY_SOURCE_DENIED,GT_EXT_REGISTRY_SOURCE_ALLOWED} gt_ext_registry_source_t;
typedef struct {
 char path[520],title[1025],args_schema[17][256];char views[8][16];uint32_t view_count;
} gt_ext_package_command_t;
/* Snapshot getters never execute code. Failure clears output. */
bool gt_ext_package_command_count(const gt_ext_package_t *,uint32_t *);
bool gt_ext_package_command(const gt_ext_package_t *,uint32_t,gt_ext_package_command_t *);
gt_ext_registry_status_t gt_ext_registry_open(gt_ext_registry_t **);
/* Retains fixed bytes. expected_digest NULL adds only; otherwise compare and
 * replace current id+side. IO means replacement committed but old close failed.
 * Package cancellation after registration does not unregister fixed bytes. */
gt_ext_registry_status_t gt_ext_registry_put(gt_ext_registry_t *,const gt_ext_package_t *,gt_ext_registry_source_t,const char *expected_digest);
/* Removes only the matching current digest. IO still means entry removed. */
gt_ext_registry_status_t gt_ext_registry_remove(gt_ext_registry_t *,const char *id,bool executor,const char *expected_digest);
/* Resolve an exact declared canonical command, not shell syntax/argv. Prefix
 * conflicts require selected_id+selected_digest together. Returns a new CHECKED
 * package owner with independent stop state; selected bytes survive replacement
 * and registry destruction. No implicit command association or runtime grant. */
gt_ext_registry_status_t gt_ext_registry_resolve(gt_ext_registry_t *,const char *command,bool executor,const char *selected_id,const char *selected_digest,gt_ext_package_t **out,gt_ext_package_command_t *);
/* Consumes even on final close error; never races any registry call. */
gt_ext_registry_status_t gt_ext_registry_destroy(gt_ext_registry_t **);
#ifdef __cplusplus
}
#endif
#endif
