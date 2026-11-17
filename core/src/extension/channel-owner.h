/* Internal transactional transfer to the Session facade; no platform pointers. */
#ifndef GT_EXT_CHANNEL_OWNER_H
#define GT_EXT_CHANNEL_OWNER_H
#include "ghosterm/extension.h"
#include "extension/stream.h"

struct gt_ext_channel {
    gt_bridge_stream_t *stream;
    gt_bridge_stream_t *process_stream;
    bool ssh, closed, counted, multiplexed;
    gt_ext_channel_info_t info;
    uint64_t challenge_counter;
    uint32_t wait_hop;
    gt_ssh_auth_phase_t wait_phase;
    bool waiting;
    uint8_t write_buffer[GT_EXT_IO_MAX];
    size_t write_size, write_at;
    void *retire_context;
    void (*did_retire)(void *);
};

gt_ext_channel_t *gt__ext_channel_new_branch(bool ssh);
bool gt__ext_channel_retained_info(gt_ext_channel_t *,
                                   gt_bridge_stream_t *,
                                   gt_ext_channel_info_t *);
gt_status_t gt__ext_channel_admit(gt_ext_channel_t *, bool ssh, gt_bridge_stream_t **);
void gt__ext_channel_commit(gt_ext_channel_t *);
void gt__ext_channel_retire(gt_ext_channel_t *);
#endif
