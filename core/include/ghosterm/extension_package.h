/* Fixed package bytes on a native worker. No installation, publisher trust or
 * permission grant. Platform bindings enter via ghosterm.h. */
#ifndef GHOSTERM_EXTENSION_PACKAGE_H
#define GHOSTERM_EXTENSION_PACKAGE_H
#include "ghosterm/types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gt_ext_package gt_ext_package_t;

typedef enum {
    GT_EXT_PACKAGE_CAPTURING,
    GT_EXT_PACKAGE_CAPTURED,
    GT_EXT_PACKAGE_CHECKED,
    GT_EXT_PACKAGE_FAILED,
    GT_EXT_PACKAGE_CLOSED
} gt_ext_package_phase_t;

typedef enum {
    GT_EXT_PACKAGE_NONE,
    GT_EXT_PACKAGE_INVALID,
    GT_EXT_PACKAGE_UNKNOWN,
    GT_EXT_PACKAGE_UNSUPPORTED,
    GT_EXT_PACKAGE_DENIED,
    GT_EXT_PACKAGE_LIMIT,
    GT_EXT_PACKAGE_MEMORY,
    GT_EXT_PACKAGE_CANCELLED,
    GT_EXT_PACKAGE_IO,
    GT_EXT_PACKAGE_INTERNAL,
    GT_EXT_PACKAGE_CHANGED
} gt_ext_package_error_t;

typedef struct {
    int directory_fd;
    uint32_t members, entries;
    uint64_t member_bytes, total_bytes;
} gt_ext_package_config_t;

typedef struct {
    const char *host_json, *registry_json;
    uint64_t memory_bytes, stack_bytes;
    uint32_t cpu_ms;
} gt_ext_package_check_config_t;

typedef struct {
    gt_ext_package_phase_t phase;
    gt_ext_package_error_t error;
    gt_status_t cleanup_error;
    char code[65];
    uint64_t members, bytes;
} gt_ext_package_info_t;

typedef struct {
    char id[129], version[65], publisher[513], license[513], runtime[16], variant[129],
        sdk_version[65];
    /* Contiguous NUL-terminated UTF-8, 4096-byte path maximum. Chunked for Swift import. */
    char entry[17][256];
    char package_digest[65], manifest_digest[65];
    char requires[16][129], optional_missing[16][129];
    uint32_t requires_count, optional_missing_count;
    uint16_t protocols[8];
    uint32_t protocol_count;
    bool executor, association_required;
    bool authorization_granted, publisher_authenticated;
} gt_ext_package_metadata_t;

/* Explicit limits, borrowed directory fd. No plugin execution. OFF refuses. */
gt_status_t gt_ext_package_open(const gt_ext_package_config_t *, gt_ext_package_t **);
gt_ext_package_phase_t gt_ext_package_step(gt_ext_package_t *);
/* Synchronous bounded validator call on the same worker after CAPTURED.
 * JSON strings are borrowed only during this call; missing host means UNKNOWN.
 * Failure is terminal for this owner. Re-check requires a new capture. */
gt_ext_package_phase_t gt_ext_package_check(gt_ext_package_t *,
                                            const gt_ext_package_check_config_t *);
bool gt_ext_package_info(const gt_ext_package_t *, gt_ext_package_info_t *);
bool gt_ext_package_metadata(const gt_ext_package_t *, gt_ext_package_metadata_t *);
/* Only stop is cross-thread, and may not race destroy. */
void gt_ext_package_stop(gt_ext_package_t *);
/* Releases this owner's reference; running instances retain their fixed bytes.
 * Consumes even on close error. No fd retry. */
gt_status_t gt_ext_package_destroy(gt_ext_package_t **);
#ifdef __cplusplus
}
#endif
#endif
