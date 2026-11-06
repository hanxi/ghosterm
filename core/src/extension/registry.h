/* Private single-worker registry. No runtime, disk installation or automatic
 * source authentication. Caller supplies a native source-policy decision.
 * Selection is explicit id+digest, never install-order or a persistent grant. */
#ifndef GT_PRIVATE_PLUGIN_REGISTRY_H
#define GT_PRIVATE_PLUGIN_REGISTRY_H
#include "extension/package-check.h"
#define GT_PLUGIN_REGISTRY_MAX 32u
typedef struct gt_plugin_registry gt_plugin_registry_t;

typedef enum {
    GT_REGISTRY_OK,
    GT_REGISTRY_INVALID,
    GT_REGISTRY_UNKNOWN,
    GT_REGISTRY_DENIED,
    GT_REGISTRY_LIMIT,
    GT_REGISTRY_MEMORY,
    GT_REGISTRY_NOT_FOUND,
    GT_REGISTRY_CONFLICT,
    GT_REGISTRY_STALE,
    GT_REGISTRY_IO
} gt_registry_status_t;

typedef enum {
    GT_REGISTRY_SOURCE_UNKNOWN,
    GT_REGISTRY_SOURCE_DENIED,
    GT_REGISTRY_SOURCE_ALLOWED
} gt_registry_source_t;

gt_registry_status_t gt__registry_open(gt_plugin_registry_t **);
/* Same id+side replaces only with the expected current digest. NULL means add
 * only. Validated replacement retains first; a close error is reported after
 * the replacement commits (never retry old fd). Running owners retain old bytes. */
gt_registry_status_t gt__registry_put(gt_plugin_registry_t *,
                                      gt_package_checked_t *,
                                      gt_registry_source_t,
                                      const char *expected_digest);
gt_registry_status_t gt__registry_remove(gt_plugin_registry_t *,
                                         const char *id,
                                         bool executor,
                                         const char *expected_digest);
/* Canonical declared command, not shell syntax or argv parsing. Missing exact
 * declaration never falls back to an ancestor. Prefix conflicts require explicit
 * selection. Optional id/digest must be supplied together and match currently
 * registered bytes. Output owns a reference; this is not execution consent. */
gt_registry_status_t gt__registry_resolve(gt_plugin_registry_t *,
                                          const char *command,
                                          bool executor,
                                          const char *selected_id,
                                          const char *selected_digest,
                                          gt_package_checked_t **out,
                                          gt_package_command_t *contribution);
gt_registry_status_t gt__registry_destroy(gt_plugin_registry_t **);
#endif
