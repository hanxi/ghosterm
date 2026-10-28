#define _POSIX_C_SOURCE 200809L
#include "base/secure-random.h"
#include "ghosterm.h"
#include "ssh/connector.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct gt_ssh_authenticator {
  gt_ssh_connector_t *connector;
};
static bool system_clock(void *context, uint64_t *out) {
  (void)context;
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 ||
      (uint64_t)t.tv_sec > (UINT64_MAX - 999) / 1000)
    return false;
  *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
  return true;
}
gt_status_t
gt_ssh_authenticator_open(gt_ssh_t **ssh,
                              const gt_ssh_auth_options_t *options,
                              gt_ssh_authenticator_t **out) {
  if (out)
    *out = NULL;
  if (!out || !options || !ssh || !*ssh)
    return GT_ERR_INVALID_ARG;
  gt_ssh_authenticator_t *a = calloc(1, sizeof(*a));
  if (!a)
    return GT_ERR_IO;
  gt_ssh_connector_config_t c = {
      .family = options->family,
      .scope = options->scope,
      .timeout_ms = options->timeout_ms ? options->timeout_ms : 60000,
      .clock_context = options->clock_context,
      .clock = options->clock ? options->clock : system_clock,
      .defer_credentials = options->defer_credentials};
  memcpy(c.address, options->address, 16);
  gt_status_t e = gt__ssh_connector_open(ssh, &c, NULL, &a->connector);
  if (e != GT_OK) {
    free(a);
    return e;
  }
  *out = a;
  return GT_OK;
}
gt_ssh_auth_phase_t
gt_ssh_authenticator_step(gt_ssh_authenticator_t *a) {
  return a ? (gt_ssh_auth_phase_t)gt__ssh_connector_step(a->connector)
           : GT_SSH_AUTH_PHASE_FAILED;
}
bool gt_ssh_authenticator_info(const gt_ssh_authenticator_t *a,
                                   gt_ssh_auth_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!a || !out)
    return false;
  gt_ssh_connector_info_t i;
  if (!gt__ssh_connector_info(a->connector, &i))
    return false;
  *out = (gt_ssh_auth_info_t){.phase = (gt_ssh_auth_phase_t)i.phase,
                                  .error = i.error,
                                  .host = (gt_ssh_auth_host_state_t)i.host,
                                  .deadline = i.deadline,
                                  .auth_methods = i.auth_methods};
  memcpy(out->key_type, i.key_type, sizeof(out->key_type));
  memcpy(out->fingerprint, i.fingerprint, sizeof(out->fingerprint));
  return true;
}
gt_status_t gt_ssh_authenticator_decide(gt_ssh_authenticator_t *a,
                                            gt_hostkey_decision_t d) {
  return a ? gt__ssh_connector_decide(a->connector, d) : GT_ERR_INVALID_ARG;
}
bool gt_ssh_authenticator_take(gt_ssh_authenticator_t *a,
                                   gt_ssh_t **out) {
  if (out)
    *out = NULL;
  return a && out && gt__ssh_connector_take(a->connector, out);
}
void gt_ssh_authenticator_close(gt_ssh_authenticator_t *a) {
  if (!a)
    return;
  gt__ssh_connector_close(a->connector);
  gt__secure_erase(a, sizeof(*a));
  free(a);
}
gt_status_t gt_ssh_authenticator_supply(gt_ssh_authenticator_t *a,gt_ssh_t **material) {
  return a?gt__ssh_connector_supply(a->connector,material):GT_ERR_INVALID_ARG;
}
