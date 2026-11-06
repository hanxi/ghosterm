/* Public native JSON request channel. The wire parser validates bounded UTF-8
 * JSON, duplicate keys, envelopes and outcomes. Root shape is a declared codec
 * constraint, NOT JSON Schema/business validation. Trusted SDK/runtime receivers
 * must validate their method schema and call check_request immediately before
 * any effect. No callback into platform/UI code; no effect is executed here. */
#ifndef GHOSTERM_EXTENSION_RPC_H
#define GHOSTERM_EXTENSION_RPC_H
#include "ghosterm/extension_session.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_FEATURE_REQUESTS 1u
#define GT_EXT_RPC_METHODS_MAX 16u
#define GT_EXT_RPC_BODY_MAX 3072u
#define GT_EXT_RPC_SLOTS 64u

typedef enum {
    GT_EXT_JSON_ANY,
    GT_EXT_JSON_OBJECT,
    GT_EXT_JSON_ARRAY,
    GT_EXT_JSON_STRING,
    GT_EXT_JSON_BOOLEAN,
    GT_EXT_JSON_NUMBER,
    GT_EXT_JSON_NULL
} gt_ext_json_shape_t;

typedef struct {
    char method[129], capability[129];
    gt_ext_role_t receiver;
    gt_ext_json_shape_t input_shape, output_shape;
} gt_ext_rpc_method_t;

typedef enum {
    GT_EXT_RPC_OK,
    GT_EXT_RPC_WAIT,
    GT_EXT_RPC_INVALID,
    GT_EXT_RPC_DENIED,
    GT_EXT_RPC_UNSUPPORTED,
    GT_EXT_RPC_CLOSED
} gt_ext_rpc_status_t;

typedef enum {
    GT_EXT_RPC_QUEUED,
    GT_EXT_RPC_PENDING,
    GT_EXT_RPC_SUCCEEDED,
    GT_EXT_RPC_FAILED,
    GT_EXT_RPC_CANCELLED,
    GT_EXT_RPC_UNKNOWN
} gt_ext_rpc_state_t;

typedef struct {
    uint64_t handle;
    char request_id[33];
    gt_ext_rpc_state_t state;
    bool cancel_requested, deadline_elapsed, confirmed;
    uint32_t outcome_size;
    char outcome[GT_EXT_RPC_BODY_MAX + 1];
} gt_ext_rpc_result_t;

typedef struct {
    uint64_t ticket;
    char request_id[33], method[129];
    uint32_t input_size;
    char input[GT_EXT_RPC_BODY_MAX + 1];
} gt_ext_rpc_request_t;

/* Explicit requests feature, with copied native method registry (0..16). Empty
 * registry still negotiates requests but rejects unknown incoming methods.
 * config.features must equal REQUESTS; other bits are UNSUPPORTED. Generic
 * session_open retains its handshake-only semantics. Failure retains channel.
 * Shape names are checked locally; full schemas remain a separate SDK gate. */
gt_status_t gt_ext_session_open_rpc(gt_ext_channel_t **,
                                    gt_ext_role_t,
                                    const gt_ext_session_config_t *,
                                    const gt_ext_rpc_method_t *,
                                    size_t,
                                    gt_ext_session_t **);
/* All operations on original non-UI owner; leases checked for every request.
 * DATA/handle acceptance means queued locally only. No automatic replay.
 * Cancel before wire ID has effect none; sent without confirmed outcome is
 * UNKNOWN after loss/timeout, never silently converted to cancelled/success. */
gt_ext_rpc_status_t gt_ext_session_request(gt_ext_session_t *,
                                           const gt_ext_lease_t *,
                                           const char *method,
                                           const void *json,
                                           size_t,
                                           uint32_t timeout_ms,
                                           uint64_t *handle);
gt_ext_rpc_status_t
gt_ext_session_cancel_request(gt_ext_session_t *, const gt_ext_lease_t *, uint64_t);
/* Both snapshots COPY all JSON/metadata into caller POD, zero on failure.
 * No pointer survives step/forget/close/destroy. Result remains queryable after
 * revoke/close with the matching original lease while its owner is retained. */
gt_ext_rpc_status_t gt_ext_session_request_result(gt_ext_session_t *,
                                                  const gt_ext_lease_t *,
                                                  uint64_t,
                                                  gt_ext_rpc_result_t *);
gt_ext_rpc_status_t
gt_ext_session_forget_request(gt_ext_session_t *, const gt_ext_lease_t *, uint64_t);
gt_ext_rpc_status_t
gt_ext_session_take_request(gt_ext_session_t *, const gt_ext_lease_t *, gt_ext_rpc_request_t *);
gt_ext_rpc_status_t
gt_ext_session_check_request(gt_ext_session_t *, const gt_ext_lease_t *, uint64_t ticket);
/* Copies full Outcome JSON on success; successful value also matches declared
 * output root shape. User cancellation/requested interruption is not a receipt. */
gt_ext_rpc_status_t gt_ext_session_respond(
    gt_ext_session_t *, const gt_ext_lease_t *, uint64_t ticket, const void *outcome, size_t);
#ifdef __cplusplus
}
#endif
#endif
