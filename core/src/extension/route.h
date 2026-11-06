/* Private, explicit managed SSH route. No shell nesting discovery or fallback. */
#ifndef GT_BRIDGE_ROUTE_H
#define GT_BRIDGE_ROUTE_H
#include "extension/backends/ssh_exec.h"
#include "ssh/connector.h"
#define GT_BRIDGE_ROUTE_MAX_HOPS 8u
#define GT_BRIDGE_ROUTE_HOST_MAX 255u

typedef struct {
    gt_ssh_connector_config_t connector;
    /* NULL at hop 0; explicit destination opened from the preceding SSH host.
   * Inner connector.family must be 0: it never dials a native TCP socket.
   * The SSH handle's host remains its independently verified identity; port
   * comes from that handle. No DNS or credentials are inferred from an offer. */
    const char *via_host;
} gt_bridge_route_hop_t;

typedef struct {
    uint32_t total_ms;
    gt_bridge_ssh_options_t stream;
} gt_bridge_route_options_t;

typedef enum {
    GT_BRIDGE_ROUTE_AUTH,
    GT_BRIDGE_ROUTE_FORWARD,
    GT_BRIDGE_ROUTE_EXEC,
    GT_BRIDGE_ROUTE_READY,
    GT_BRIDGE_ROUTE_FAILED,
    GT_BRIDGE_ROUTE_STOPPED,
    GT_BRIDGE_ROUTE_CLOSED
} gt_bridge_route_phase_t;

typedef struct {
    gt_bridge_route_phase_t phase, failure_phase;
    gt_status_t error;
    size_t hop, count;
    bool deadline;
    gt_ssh_connector_info_t authentication[GT_BRIDGE_ROUTE_MAX_HOPS];
    gt_hostkey_decision_t decisions[GT_BRIDGE_ROUTE_MAX_HOPS];
    gt_bridge_ssh_info_t channel;
} gt_bridge_route_info_t;

/* Takes every fresh SSH handle on success only; all array entries become NULL.
 * Each config and via_host/command is copied, allocator and shared monotonic
 * clock context must outlive destroy. All hops must use the same clock/context.
 * Default total setup 30s; no total timeout on the subsequent live byte stream.
 * One non-UI IO owner drives all layers. No extra workers, sessions shared with
 * terminal/SFTP, user config writes, remote shell relay or automatic replay. */
gt_status_t gt__bridge_route_open(gt_ssh_t **owned,
                                  const gt_bridge_route_hop_t *,
                                  size_t count,
                                  const char *command,
                                  const gt_bridge_route_options_t *,
                                  const gt_allocator_t *,
                                  gt_bridge_stream_t **);
gt_bridge_route_phase_t gt__bridge_route_step(gt_bridge_stream_t *, int ms);
bool gt__bridge_route_info(const gt_bridge_stream_t *, gt_bridge_route_info_t *);
gt_status_t gt__bridge_route_decide(gt_bridge_stream_t *, gt_hostkey_decision_t);
gt_status_t gt__bridge_route_supply(gt_bridge_stream_t *, gt_ssh_t **);
#endif
