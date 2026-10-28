/* Private nonblocking SSH authentication, independent of session backends. */
#ifndef GT_SSH_CONNECTOR_H
#define GT_SSH_CONNECTOR_H
#include "ghosterm/ssh.h"
#include "base/allocator.h"
typedef struct gt_ssh_connector gt_ssh_connector_t;
typedef struct {
  /* Resolved network address, separate from SSH host/known_hosts identity.
   * 4: first four address bytes; 6: all sixteen plus scope. No DNS calls here.
   * Native resolver must be asynchronous/cancellable, within parent deadline. */
  uint8_t family,address[16];uint32_t scope;
  uint32_t timeout_ms;void *clock_context;bool (*clock)(void *,uint64_t *);
  bool defer_credentials;
} gt_ssh_connector_config_t;
typedef enum {GT_SSH_CONNECT_TCP,GT_SSH_CONNECT_NEGOTIATING,GT_SSH_CONNECT_HOSTKEY,
  GT_SSH_CONNECT_AUTHENTICATING,GT_SSH_CONNECT_READY,GT_SSH_CONNECT_TAKEN,
  GT_SSH_CONNECT_FAILED,GT_SSH_CONNECT_CANCELLED,GT_SSH_CONNECT_CREDENTIALS} gt_ssh_connector_phase_t;
typedef enum {GT_SSH_HOST_UNCHECKED,GT_SSH_HOST_KNOWN,GT_SSH_HOST_UNKNOWN,GT_SSH_HOST_MISMATCH} gt_ssh_host_state_t;
typedef struct {
  gt_ssh_connector_phase_t phase;gt_status_t error;gt_ssh_host_state_t host;
  bool deadline;int auth_methods;char key_type[32],fingerprint[80];
} gt_ssh_connector_info_t;
/* Takes fresh preconfigured SSH with unlocked credentials on success only.
 * Original host remains hostkey identity. No credential/hostkey UI callbacks;
 * UNKNOWN is an explicit wait state; MISMATCH is terminal. No shell/SFTP aliases.
 * One fixed owner allocation plus libssh2/crypto's own bounded protocol state.
 * Single non-UI serial owner; timeout 1..60000ms covers TCP through authentication. */
gt_status_t gt__ssh_connector_open(gt_ssh_t **,const gt_ssh_connector_config_t *,const gt_allocator_t *,gt_ssh_connector_t **);
gt_ssh_connector_phase_t gt__ssh_connector_step(gt_ssh_connector_t *);
bool gt__ssh_connector_info(const gt_ssh_connector_t *,gt_ssh_connector_info_t *);
/* Only UNKNOWN; UNKNOWN decision leaves it waiting. ACCEPT writes known_hosts
 * before userauth; write failure is explicit. REJECT never becomes UNKNOWN. */
gt_status_t gt__ssh_connector_decide(gt_ssh_connector_t *,gt_hostkey_decision_t);
/* Deferred credentials: only after host verification and method discovery.
 * Takes fresh same-identity material on success, keeps the authenticated socket
 * owner. Failed authentication returns to this phase under the original deadline. */
gt_status_t gt__ssh_connector_supply(gt_ssh_connector_t *,gt_ssh_t **);
/* READY once; restores libssh2 blocking mode, keeps the OS socket NONBLOCK,
 * transfers authenticated SSH.
 * Next session owner immediately adopts it into nonblocking dedicated IO. */
bool gt__ssh_connector_take(gt_ssh_connector_t *,gt_ssh_t **);
/* Immediate local cancel/close; shut down socket before libssh2 cleanup, never
 * wait for peer replies. Erase owned credential materials when SSH is dropped. */
void gt__ssh_connector_cancel(gt_ssh_connector_t *);
void gt__ssh_connector_close(gt_ssh_connector_t *);
#endif
