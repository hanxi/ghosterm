/* Internal transactional transfer to the Session facade; no platform pointers. */
#ifndef GT_EXT_CHANNEL_OWNER_H
#define GT_EXT_CHANNEL_OWNER_H
#include "ghosterm/extension.h"
#include "extension/stream.h"
gt_status_t gt__ext_channel_admit(gt_ext_channel_t *,bool ssh,gt_bridge_stream_t **);
void gt__ext_channel_commit(gt_ext_channel_t *);
void gt__ext_channel_retire(gt_ext_channel_t *);
#endif
