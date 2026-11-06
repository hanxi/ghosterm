/* Private metadata-only check on fixed package bytes. Not installation, source
 * authentication, command association, permission approval or plugin execution.
 * Single non-UI owner; only the borrowed atomic stop flag may change concurrently.
 * Native caller supplies host/registry observations, never remote self-claims.
 * Optional pinned JS engine evaluates trusted validator code only, no plugin JS.
 * No Node, OS service, filesystem module loader, dynamic code fetch or gateway. */
#ifndef GT_PRIVATE_PACKAGE_CHECK_H
#define GT_PRIVATE_PACKAGE_CHECK_H
#include "extension/package-snapshot.h"
#include <stdatomic.h>
typedef struct gt_package_checked gt_package_checked_t;
typedef enum {
 GT_PACKAGE_CHECK_COMPATIBLE,GT_PACKAGE_CHECK_INVALID,GT_PACKAGE_CHECK_UNKNOWN,
 GT_PACKAGE_CHECK_UNSUPPORTED,GT_PACKAGE_CHECK_DENIED,GT_PACKAGE_CHECK_LIMIT,
 GT_PACKAGE_CHECK_MEMORY,GT_PACKAGE_CHECK_CANCELLED,GT_PACKAGE_CHECK_IO,
 GT_PACKAGE_CHECK_INTERNAL
} gt_package_check_status_t;
typedef struct {gt_package_check_status_t status;char code[65];} gt_package_check_report_t;
typedef struct {
 const void *host_json;size_t host_size; /* <=64 KiB; absent means UNKNOWN */
 const void *registry_json;size_t registry_size; /* <=1 MiB; absent means [] */
 size_t memory_bytes,stack_bytes;uint32_t cpu_ms;
 const atomic_bool *stop;void *clock_context;bool (*clock)(void *,uint64_t *);
} gt_package_check_config_t;
typedef struct {
 char id[129],version[65],publisher[513],license[513],entry[4097],runtime[16],variant[129],sdk_version[65];
 char package_digest[65],manifest_digest[65]; /* raw integrity.json / plugin.json SHA256 */
 char requires[16][129],optional_missing[16][129];size_t requires_count,optional_missing_count;
 uint16_t protocols[8];size_t protocol_count;bool executor,association_required;
 bool authorization_granted,publisher_authenticated; /* always false here */
} gt_package_check_info_t;
/* 0 limits select 64 MiB heap / 512 KiB stack / 1s elapsed CPU budget; maxima
 * 128 MiB / 1 MiB / 5s, minima 4 MiB / 64 KiB / 1ms. Heap includes allocator
 * headers/realloc peaks, excludes bounded native JSON workspace/input copies and
 * the retained snapshot. Cancellation/deadline checked around bounded native
 * scans and in JS interrupts; no preemptive C/filesystem latency guarantee.
 * COMPATIBLE alone consumes *snapshot and sets *out. Every failure leaves the
 * original snapshot with the caller and never returns a partial checked object.
 * OFF returns explicit UNSUPPORTED. Registry conflicts require native association
 * before activation; this check never grants execution or publisher identity. */
gt_package_check_status_t gt__package_check(gt_package_snapshot_t **snapshot,const gt_package_check_config_t *,const gt_allocator_t *,gt_package_checked_t **out,gt_package_check_report_t *);
bool gt__package_checked_info(const gt_package_checked_t *,gt_package_check_info_t *);
bool gt__package_checked_member(const gt_package_checked_t *,const char *,gt_package_member_t *,const uint8_t **,size_t *);
/* Retain while holding an existing reference. Immutable reads may then run on
 * independent workers. Custom allocator context must outlive all references
 * and support whichever worker performs the final release. Not a global cache.
 * Destroy releases only this reference, consumes even on final close error. */
bool gt__package_checked_retain(gt_package_checked_t *,gt_package_checked_t **);
gt_package_status_t gt__package_checked_destroy(gt_package_checked_t **);
#endif
