/* Private resource-message owner. One ordered bulk transfer per direction and
 * instance, bounded native producer buffer, 64 snapshot slots, 8 cached receipts.
 * No source-file access/authorization, runtime, UI or public ABI. */
#ifndef GT_BRIDGE_TRANSFER_H
#define GT_BRIDGE_TRANSFER_H
/* Internal ownership check for transactional runtime adoption. */
#include "extension/resource.h"
#define GT_BRIDGE_TRANSFER_WINDOW 8192u
#define GT_BRIDGE_TRANSFER_CHUNK 768u
#define GT_BRIDGE_TRANSFER_SNAPSHOTS 64u
#define GT_BRIDGE_TRANSFER_RECEIPTS 8u
typedef struct gt_bridge_transfer gt_bridge_transfer_t;
typedef enum {GT_BRIDGE_TRANSFER_QUEUED,GT_BRIDGE_TRANSFER_SENDING,GT_BRIDGE_TRANSFER_PENDING,
 GT_BRIDGE_TRANSFER_SUCCEEDED,GT_BRIDGE_TRANSFER_FAILED,GT_BRIDGE_TRANSFER_CANCELLED,GT_BRIDGE_TRANSFER_UNKNOWN} gt_bridge_transfer_state_t;
typedef struct {
  uint64_t handle,accepted;gt_bridge_transfer_state_t state;
  bool confirmed,cancel_requested;char resource_id[33],code[33];uint64_t generation;
} gt_bridge_transfer_result_t;
typedef struct {uint64_t owner;gt_bridge_resource_ref_t ref;gt_bridge_resource_descriptor_t descriptor;} gt_bridge_snapshot_t;
gt_status_t gt__bridge_transfer_new(gt_bridge_session_t *,gt_bridge_wire_t *,gt_bridge_resource_store_t *,uint64_t,const gt_allocator_t *,gt_bridge_transfer_t **);
void gt__bridge_transfer_free(gt_bridge_transfer_t *);
void gt__bridge_transfer_end(gt_bridge_transfer_t *);
/* Cleanup unsnatched snapshots; handed-off snapshots retain Store scope after
 * Session destruction. IO cleanup failure retains this owner for a retry. */
gt_status_t gt__bridge_transfer_cleanup(gt_bridge_transfer_t *);
gt_status_t gt__bridge_transfer_receive(gt_bridge_transfer_t *,const gt_bridge_message_t *,size_t);
gt_status_t gt__bridge_transfer_prepare(gt_bridge_transfer_t *,uint64_t,char *,size_t,size_t *,bool *);
void gt__bridge_transfer_commit(gt_bridge_transfer_t *);
void gt__bridge_transfer_sent(gt_bridge_transfer_t *);
gt_bridge_resource_status_t gt__bridge_transfer_start(gt_bridge_transfer_t *,const gt_bridge_lease_t *,const gt_bridge_resource_descriptor_t *,uint64_t *);
gt_bridge_resource_status_t gt__bridge_transfer_write(gt_bridge_transfer_t *,const gt_bridge_lease_t *,uint64_t,const void *,size_t,size_t *);
gt_bridge_resource_status_t gt__bridge_transfer_finish(gt_bridge_transfer_t *,const gt_bridge_lease_t *,uint64_t,bool source_complete);
gt_bridge_resource_status_t gt__bridge_transfer_cancel(gt_bridge_transfer_t *,const gt_bridge_lease_t *,uint64_t);
bool gt__bridge_transfer_result(gt_bridge_transfer_t *,const gt_bridge_lease_t *,uint64_t,gt_bridge_transfer_result_t *);
gt_bridge_resource_status_t gt__bridge_transfer_forget(gt_bridge_transfer_t *,const gt_bridge_lease_t *,uint64_t);
gt_bridge_resource_status_t gt__bridge_transfer_take(gt_bridge_transfer_t *,const gt_bridge_lease_t *,gt_bridge_snapshot_t *);
gt_bridge_resource_status_t gt__bridge_transfer_reject(gt_bridge_transfer_t *,const gt_bridge_lease_t *);
/* Private native Session entry points; all calls use the single IO owner. */
gt_bridge_resource_status_t gt__bridge_session_send_resource(gt_bridge_session_t *,const gt_bridge_lease_t *,const gt_bridge_resource_descriptor_t *,uint64_t *);
gt_bridge_resource_status_t gt__bridge_session_write_resource(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t,const void *,size_t,size_t *);
gt_bridge_resource_status_t gt__bridge_session_finish_resource(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t,bool);
gt_bridge_resource_status_t gt__bridge_session_cancel_resource(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t);
bool gt__bridge_session_resource_result(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t,gt_bridge_transfer_result_t *);
gt_bridge_resource_status_t gt__bridge_session_forget_resource(gt_bridge_session_t *,const gt_bridge_lease_t *,uint64_t);
gt_bridge_resource_status_t gt__bridge_session_take_resource(gt_bridge_session_t *,const gt_bridge_lease_t *,gt_bridge_snapshot_t *);
gt_bridge_resource_status_t gt__bridge_session_reject_resource(gt_bridge_session_t *,const gt_bridge_lease_t *);
bool gt__bridge_transfer_has_owner(const gt_bridge_transfer_t *);

#endif
