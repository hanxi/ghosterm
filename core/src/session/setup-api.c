#define _POSIX_C_SOURCE 200809L
#include "base/secure-random.h"
#include "ghosterm.h"
#include "ipc/ssh-connector.h"
#include "session/deploy.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct gt_session_authenticator {
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
gt_session_authenticator_open(gt_ssh_t **ssh,
                              const gt_session_auth_options_t *options,
                              gt_session_authenticator_t **out) {
  if (out)
    *out = NULL;
  if (!out || !options || !ssh || !*ssh)
    return GT_ERR_INVALID_ARG;
  gt_session_authenticator_t *a = calloc(1, sizeof(*a));
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
gt_session_auth_phase_t
gt_session_authenticator_step(gt_session_authenticator_t *a) {
  return a ? (gt_session_auth_phase_t)gt__ssh_connector_step(a->connector)
           : GT_SESSION_AUTH_FAILED;
}
bool gt_session_authenticator_info(const gt_session_authenticator_t *a,
                                   gt_session_auth_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!a || !out)
    return false;
  gt_ssh_connector_info_t i;
  if (!gt__ssh_connector_info(a->connector, &i))
    return false;
  *out = (gt_session_auth_info_t){.phase = (gt_session_auth_phase_t)i.phase,
                                  .error = i.error,
                                  .host = (gt_session_host_state_t)i.host,
                                  .deadline = i.deadline,
                                  .auth_methods = i.auth_methods};
  memcpy(out->key_type, i.key_type, sizeof(out->key_type));
  memcpy(out->fingerprint, i.fingerprint, sizeof(out->fingerprint));
  return true;
}
gt_status_t gt_session_authenticator_decide(gt_session_authenticator_t *a,
                                            gt_hostkey_decision_t d) {
  return a ? gt__ssh_connector_decide(a->connector, d) : GT_ERR_INVALID_ARG;
}
bool gt_session_authenticator_take(gt_session_authenticator_t *a,
                                   gt_ssh_t **out) {
  if (out)
    *out = NULL;
  return a && out && gt__ssh_connector_take(a->connector, out);
}
void gt_session_authenticator_close(gt_session_authenticator_t *a) {
  if (!a)
    return;
  gt__ssh_connector_close(a->connector);
  gt__secure_erase(a, sizeof(*a));
  free(a);
}
typedef struct {
  const gt_session_install_options_t *options;
} install_context;
static bool cancelled(void *context) {
  install_context *c = context;
  return c->options && c->options->cancelled &&
         c->options->cancelled(c->options->context);
}
static void progress(void *context, gt_deploy_phase_t phase, unsigned file,
                     uint64_t bytes, uint64_t total) {
  install_context *c = context;
  if (c->options && c->options->progress)
    c->options->progress(c->options->context, (gt_session_install_phase_t)phase,
                         file, bytes, total);
}
static gt_deploy_options_t
install_options(const gt_session_install_options_t *in,
                install_context *context) {
  *context = (install_context){in};
  return (gt_deploy_options_t){
      .timeout_ms = in && in->timeout_ms ? in->timeout_ms : 120000,
      .context = context,
      .cancelled = cancelled,
      .progress = progress};
}
gt_status_t gt_session_linux_probe(gt_ssh_t *ssh,
                                   const gt_session_install_options_t *options,
                                   gt_session_linux_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!out)
    return GT_ERR_INVALID_ARG;
  install_context c;
  gt_deploy_options_t config = install_options(options, &c);
  gt_linux_probe_t p;
  gt_status_t e = gt__ssh_probe_linux(ssh, &config, &p);
  if (e == GT_OK) {
    memcpy(out->arch, p.arch, sizeof(out->arch));
    memcpy(out->glibc, p.glibc, sizeof(out->glibc));
    memcpy(out->home, p.home, sizeof(out->home));
    out->uid = p.uid;
  }
  return e;
}
gt_status_t
gt_session_linux_install(gt_ssh_t *ssh, const char *directory, const char *root,
                         const gt_session_install_options_t *options,
                         gt_session_install_result_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!out || !directory || !root)
    return GT_ERR_INVALID_ARG;
  gt_session_bundle_t bundle;
  gt_status_t e = gt__session_bundle_read(directory, &bundle);
  if (e != GT_OK)
    return e;
  install_context c;
  gt_deploy_options_t config = install_options(options, &c);
  gt_deploy_result_t r;
  memset(&r, 0, sizeof(r));
  e = gt__ssh_deploy_linux(ssh, &bundle, root, &config, &r);
  out->reused = r.reused;
  out->published = r.published;
  out->publication_unknown = r.publication_unknown;
  memcpy(out->daemon, r.daemon, sizeof(out->daemon));
  memcpy(out->proxy, r.proxy, sizeof(out->proxy));
  return e;
}

gt_status_t gt_session_authenticator_supply(gt_session_authenticator_t *a,gt_ssh_t **material) {
  return a?gt__ssh_connector_supply(a->connector,material):GT_ERR_INVALID_ARG;
}
