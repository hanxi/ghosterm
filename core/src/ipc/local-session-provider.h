/* Local stdio proxy adapter for the same session coordinator as SSH. */
#ifndef GT_IPC_LOCAL_SESSION_PROVIDER_H
#define GT_IPC_LOCAL_SESSION_PROVIDER_H
#include "session-client.h"
typedef struct gt_local_session_provider gt_local_session_provider_t;
typedef struct {
  const char *proxy, *daemon, *runtime;
  void *clock_context;
  bool (*clock)(void *, uint64_t *);
} gt_local_session_provider_config_t;
typedef struct {
  unsigned children;
  uint64_t stderr_bytes;
  gt_status_t error;
  bool active;
} gt_local_provider_info_t;
gt_status_t
gt__local_session_provider_open(const gt_local_session_provider_config_t *,
                                const gt_allocator_t *,
                                gt_local_session_provider_t **);
bool gt__local_session_provider_link(gt_local_session_provider_t *,
                                     gt_session_link_t *);
bool gt__local_session_provider_info(const gt_local_session_provider_t *,
                                     gt_local_provider_info_t *);
/* Close protocol streams/coordinator first. Reset closes only proxy stdio and
 * cancels these exact owned child PIDs; daemon/session are never terminated.
 * Nonblocking quiesce reaps children; keep polling off the UI thread until
 * true. destroy refuses to release storage while a child or borrowed stream
 * remains. */
bool gt__local_session_provider_quiesce(gt_local_session_provider_t *);
bool gt__local_session_provider_destroy(gt_local_session_provider_t *);
#endif
