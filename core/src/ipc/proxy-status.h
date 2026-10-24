/* Shared private stdio-proxy exit contract. No stdout diagnostic frames. */
#ifndef GT_IPC_PROXY_STATUS_H
#define GT_IPC_PROXY_STATUS_H
#include "ghosterm/types.h"
typedef enum {
  GT_ACTIVATION_OK,GT_ACTIVATION_INVALID,GT_ACTIVATION_NOT_FOUND,GT_ACTIVATION_AUTH,
  GT_ACTIVATION_UNSUPPORTED,GT_ACTIVATION_TIMEOUT,GT_ACTIVATION_CANCELLED,
  GT_ACTIVATION_INCOMPATIBLE,GT_ACTIVATION_FEATURES,GT_ACTIVATION_INSTANCE,GT_ACTIVATION_IO
} gt_activation_rc_t;
/* Proxy already emits 64+activation result. A real nonzero exit-status is
 * present; absent/zero SSH status must remain unknown, never success evidence. */
static inline bool gt__proxy_exit_status(int code,gt_status_t *error) {
  if(error)*error=GT_OK;
  if(!error)return false;
  switch(code) {
    case 65:*error=GT_ERR_INVALID_ARG;return true;
    case 66:*error=GT_ERR_NOT_FOUND;return true;
    case 67:*error=GT_ERR_AUTH;return true;
    case 68:case 71:case 72:*error=GT_ERR_UNSUPPORTED;return true;
    case 69:case 74:*error=GT_ERR_IO;return true;
    case 70:*error=GT_ERR_CANCELLED;return true;
    case 73:*error=GT_ERR_PROTOCOL;return true;
    default:return false;
  }
}
#endif
