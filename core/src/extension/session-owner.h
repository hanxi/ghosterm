/* Private facade lifetime; platform bindings only consume ghosterm.h. */
#ifndef GT_EXT_SESSION_OWNER_H
#define GT_EXT_SESSION_OWNER_H
#include "ghosterm/extension_resource.h"
#include "extension/channel-owner.h"
#include "extension/transfer.h"

struct gt_ext_resource_store {
    gt_bridge_resource_store_t *inner;
    size_t sessions;
    uint64_t total_bytes;
};

struct gt_ext_session {
    gt_bridge_session_t *inner;
    gt_ext_channel_t *channel;
    bool released, requests;
    gt_status_t cleanup_error;
    gt_ext_rpc_method_t methods[GT_EXT_RPC_METHODS_MAX];
    gt_ext_resource_store_t *store;
    size_t sources;
};

gt_ext_rpc_status_t gt__ext_session_take_request_methods(gt_ext_session_t *,
                                                         const gt_ext_lease_t *,
                                                         const char *const *,
                                                         size_t,
                                                         gt_ext_rpc_request_t *);
#endif
