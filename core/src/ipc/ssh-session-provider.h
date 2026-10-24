/* Native credential/resolver admission -> common cancellable SSH owner. */
#ifndef GT_IPC_SSH_SESSION_PROVIDER_H
#define GT_IPC_SSH_SESSION_PROVIDER_H
#include "ssh-connector.h"
#include "session-client.h"
typedef struct gt_ssh_session_provider gt_ssh_session_provider_t;
typedef struct {
  const char *proxy,*daemon,*runtime;
  uint32_t authentication_timeout_ms;
  void *clock_context;bool (*clock)(void *,uint64_t *);
  void *context;
  /* Low frequency, nonblocking, no reentry. Native code asynchronously resolves
   * the original host and unlocks credentials, then supplies on this IO owner.
   * Request IDs never repeat within this provider; late completions are refused.
   * Deadline covers resolver, credential/hostkey UI, TCP and userauth together. */
  void (*requested)(void *,uint64_t request,uint32_t attempt);
  void (*cancelled)(void *,uint64_t request);
  void (*hostkey)(void *,uint64_t request,const gt_ssh_connector_info_t *);
} gt_ssh_session_provider_config_t;
typedef enum {GT_SSH_PROVIDER_IDLE,GT_SSH_PROVIDER_REQUESTED,GT_SSH_PROVIDER_AUTHENTICATING,
  GT_SSH_PROVIDER_ACTIVE,GT_SSH_PROVIDER_FAILED} gt_ssh_provider_phase_t;
typedef struct {gt_ssh_provider_phase_t phase;uint64_t request;gt_status_t error;bool deadline;gt_ssh_connector_info_t authentication;} gt_ssh_provider_info_t;
gt_status_t gt__ssh_session_provider_open(const gt_ssh_session_provider_config_t *,const gt_allocator_t *,gt_ssh_session_provider_t **);
/* Borrowed link; provider and callback contexts outlive coordinator close. */
bool gt__ssh_session_provider_link(gt_ssh_session_provider_t *,gt_session_link_t *);
/* All APIs share the coordinator's one IO owner, including native completions.
 * Stale/invalid/expired supply owns nothing. Resolved config's clock/deadline
 * fields are ignored: provider stamps one remaining deadline and own clock. */
gt_status_t gt__ssh_session_provider_supply(gt_ssh_session_provider_t *,uint64_t request,gt_ssh_t **,const gt_ssh_connector_config_t *resolved);
gt_status_t gt__ssh_session_provider_adopt(gt_ssh_session_provider_t *,uint64_t request,gt_ssh_t **);
gt_status_t gt__ssh_session_provider_reject(gt_ssh_session_provider_t *,uint64_t request,gt_status_t error);
gt_status_t gt__ssh_session_provider_decide(gt_ssh_session_provider_t *,uint64_t request,gt_hostkey_decision_t);
bool gt__ssh_session_provider_info(const gt_ssh_session_provider_t *,gt_ssh_provider_info_t *);
/* Coordinator/streams must be closed first. Cancels native requests, closes
 * connector/SSH socket locally, never waits for DNS/worker/peer or terminates PTY. */
void gt__ssh_session_provider_close(gt_ssh_session_provider_t *);
#endif
