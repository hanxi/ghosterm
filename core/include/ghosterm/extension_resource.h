/* Native resource transport and retained local snapshots. One non-UI owner;
 * never expose lease/ref tokens to plugin code. Metadata is display data, not a
 * path. This API does not grant source-file access or execute filesystem jobs. */
#ifndef GHOSTERM_EXTENSION_RESOURCE_H
#define GHOSTERM_EXTENSION_RESOURCE_H
#include "ghosterm/extension_rpc.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_FEATURE_RESOURCES 2u
#define GT_EXT_RESOURCE_READ_MAX 16384u
typedef struct gt_ext_resource_store gt_ext_resource_store_t;
typedef enum {GT_EXT_RESOURCE_OK,GT_EXT_RESOURCE_WAIT,GT_EXT_RESOURCE_INVALID,
 GT_EXT_RESOURCE_DENIED,GT_EXT_RESOURCE_CLOSED,GT_EXT_RESOURCE_PROTOCOL,
 GT_EXT_RESOURCE_LIMIT,GT_EXT_RESOURCE_IO,GT_EXT_RESOURCE_UNSUPPORTED,
 GT_EXT_RESOURCE_NOT_FOUND} gt_ext_resource_status_t;
typedef enum {GT_EXT_TRANSFER_QUEUED,GT_EXT_TRANSFER_SENDING,GT_EXT_TRANSFER_PENDING,
 GT_EXT_TRANSFER_SUCCEEDED,GT_EXT_TRANSFER_FAILED,GT_EXT_TRANSFER_CANCELLED,
 GT_EXT_TRANSFER_UNKNOWN} gt_ext_transfer_state_t;
typedef struct {int directory_fd;uint64_t total_bytes,resource_bytes;size_t slots;} gt_ext_resource_store_config_t;
typedef struct {uint64_t reserved_bytes;size_t resources,scopes,sessions;} gt_ext_resource_usage_t;
typedef struct {
 char resource_id[33],sha256[65],mime[129],display_name[1025];
 uint64_t generation,size;bool size_known;
} gt_ext_resource_descriptor_t;
/* Native ref is an owning pin. Plain struct copies do NOT retain; each retain
 * needs one release. Store/lease must match, including after Session destroy. */
typedef struct {uint64_t owner,handle;char token[33];gt_ext_resource_descriptor_t descriptor;} gt_ext_resource_snapshot_t;
typedef struct {gt_ext_resource_descriptor_t descriptor;uint64_t received,reserved;uint32_t references;bool published;} gt_ext_resource_info_t;
typedef struct {
 uint64_t handle,accepted;gt_ext_transfer_state_t state;bool confirmed,cancel_requested;
 char resource_id[33],code[33];uint64_t generation;
} gt_ext_transfer_result_t;
/* Borrows fd, owns CLOEXEC duplicate. Native caller authorizes private directory
 * and its ACL/mount policy; Core checks POSIX mode/uid. Fixed limits: <=64 slots,
 * <=16 MiB/resource, <=128 MiB/store. IO has bounded size, no hard disk deadline.
 * One shared Store accounts all associated instances and retained snapshots. */
gt_ext_resource_status_t gt_ext_resource_store_open(const gt_ext_resource_store_config_t *,gt_ext_resource_store_t **);
bool gt_ext_resource_store_usage(const gt_ext_resource_store_t *,gt_ext_resource_usage_t *);
gt_ext_resource_status_t gt_ext_resource_store_info(gt_ext_resource_store_t *,const gt_ext_lease_t *,const gt_ext_resource_snapshot_t *,gt_ext_resource_info_t *);
gt_ext_resource_status_t gt_ext_resource_store_read(gt_ext_resource_store_t *,const gt_ext_lease_t *,const gt_ext_resource_snapshot_t *,uint64_t,void *,size_t,size_t *);
gt_ext_resource_status_t gt_ext_resource_store_retain(gt_ext_resource_store_t *,const gt_ext_lease_t *,const gt_ext_resource_snapshot_t *);
/* Success or already missing clears ref. IO retains ref for explicit retry;
 * never retries a consumed numeric fd. Native metadata/byte counts zero on
 * failed queries. Existing published copies are readable after revoke/close. */
gt_ext_resource_status_t gt_ext_resource_store_release(gt_ext_resource_store_t *,const gt_ext_lease_t *,gt_ext_resource_snapshot_t *);
/* WAIT retains owner while ANY Session (even before READY) or pin remains.
 * Once destruction begins, success OR directory-close IO consumes *owner. */
gt_ext_resource_status_t gt_ext_resource_store_destroy(gt_ext_resource_store_t **);
/* Exact RESOURCES, optionally REQUESTS; methods only with REQUESTS. Store stays
 * owned by caller and must outlive Session. Constructor failure retains channel
 * and does not charge a Session. No implicit capability/consent/feature grant. */
gt_status_t gt_ext_session_open_resources(gt_ext_channel_t **,gt_ext_role_t,const gt_ext_session_config_t *,const gt_ext_rpc_method_t *,size_t,gt_ext_resource_store_t *,uint64_t scope_bytes,gt_ext_session_t **);
gt_ext_resource_status_t gt_ext_session_send_resource(gt_ext_session_t *,const gt_ext_lease_t *,const gt_ext_resource_descriptor_t *,uint64_t *);
gt_ext_resource_status_t gt_ext_session_write_resource(gt_ext_session_t *,const gt_ext_lease_t *,uint64_t,const void *,size_t,size_t *);
gt_ext_resource_status_t gt_ext_session_finish_resource(gt_ext_session_t *,const gt_ext_lease_t *,uint64_t,bool source_complete);
gt_ext_resource_status_t gt_ext_session_cancel_resource(gt_ext_session_t *,const gt_ext_lease_t *,uint64_t);
gt_ext_resource_status_t gt_ext_session_resource_result(gt_ext_session_t *,const gt_ext_lease_t *,uint64_t,gt_ext_transfer_result_t *);
gt_ext_resource_status_t gt_ext_session_forget_resource(gt_ext_session_t *,const gt_ext_lease_t *,uint64_t);
/* Handoff only after digest/length/EOF validation and immutable publication.
 * Success transfers one native pin to caller; failure clears output. Sender
 * confirmation needs resource.result, not local accepted bytes or process EOF. */
gt_ext_resource_status_t gt_ext_session_take_resource(gt_ext_session_t *,const gt_ext_lease_t *,gt_ext_resource_snapshot_t *);
gt_ext_resource_status_t gt_ext_session_reject_resource(gt_ext_session_t *,const gt_ext_lease_t *);
#ifdef __cplusplus
}
#endif
#endif
