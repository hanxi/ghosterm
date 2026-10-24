/* Setup for the same local/SSH session Transport. No UI or engine dependency.
 */
#ifndef GHOSTERM_SESSION_SETUP_H
#define GHOSTERM_SESSION_SETUP_H
#include "ghosterm/ssh.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gt_session_authenticator gt_session_authenticator_t;
typedef enum {
  GT_SESSION_AUTH_TCP,
  GT_SESSION_AUTH_NEGOTIATING,
  GT_SESSION_AUTH_HOSTKEY,
  GT_SESSION_AUTH_AUTHENTICATING,
  GT_SESSION_AUTH_READY,
  GT_SESSION_AUTH_TAKEN,
  GT_SESSION_AUTH_FAILED,
  GT_SESSION_AUTH_CANCELLED,
  GT_SESSION_AUTH_CREDENTIALS
} gt_session_auth_phase_t;
typedef enum {
  GT_SESSION_HOST_UNCHECKED,
  GT_SESSION_HOST_KNOWN,
  GT_SESSION_HOST_UNKNOWN,
  GT_SESSION_HOST_MISMATCH
} gt_session_host_state_t;
typedef struct {
  uint8_t family, address[16];
  uint32_t scope, timeout_ms;
  bool defer_credentials; /* discover methods before requesting unlocked material */
  void *clock_context;
  bool (*clock)(void *, uint64_t *monotonic_ms);
} gt_session_auth_options_t;
typedef struct {
  gt_session_auth_phase_t phase;
  gt_status_t error;
  gt_session_host_state_t host;
  bool deadline;
  int32_t auth_methods;
  char key_type[32], fingerprint[80];
} gt_session_auth_info_t;
/* One non-UI serial owner. Success takes/clears fresh unlocked SSH only.
 * Resolved binary address is separate from original SSH hostkey identity.
 * Nonblocking step, one total deadline through hostkey UI (default 60000ms).
 * Unknown host waits for explicit decide; mismatch is terminal. */
gt_status_t gt_session_authenticator_open(gt_ssh_t **,
                                          const gt_session_auth_options_t *,
                                          gt_session_authenticator_t **);
gt_session_auth_phase_t
gt_session_authenticator_step(gt_session_authenticator_t *);
bool gt_session_authenticator_info(const gt_session_authenticator_t *,
                                   gt_session_auth_info_t *);
gt_status_t gt_session_authenticator_decide(gt_session_authenticator_t *,
                                            gt_hostkey_decision_t);
/* CREDENTIALS only. Success takes/clears fresh unlocked material with the same
 * host/user/port/known_hosts identity; no extra TCP connection or handshake.
 * Method mismatch is UNSUPPORTED and retains caller ownership. Bad credentials
 * return to CREDENTIALS with error=AUTH, without resetting the total deadline. */
gt_status_t gt_session_authenticator_supply(gt_session_authenticator_t *,gt_ssh_t **);
/* READY once. Caller owns dedicated authenticated SSH; no SFTP/terminal
 * aliases. */
bool gt_session_authenticator_take(gt_session_authenticator_t *, gt_ssh_t **);
void gt_session_authenticator_close(gt_session_authenticator_t *);
typedef enum {
  GT_SESSION_INSTALL_PROBE,
  GT_SESSION_INSTALL_UPLOAD,
  GT_SESSION_INSTALL_VERIFY,
  GT_SESSION_INSTALL_ACTIVATE,
  GT_SESSION_INSTALL_DONE
} gt_session_install_phase_t;
typedef struct {
  uint32_t timeout_ms; /* 0 -> 120000, max 120000, remote IO total deadline */
  void *context;
  bool (*cancelled)(void *);
  void (*progress)(void *, gt_session_install_phase_t, unsigned file,
                   uint64_t bytes, uint64_t total);
} gt_session_install_options_t;
typedef struct {
  char arch[24], glibc[24], home[512];
  uint32_t uid;
} gt_session_linux_info_t;
typedef struct {
  bool reused, published, publication_unknown;
  char daemon[1024], proxy[1024];
} gt_session_install_result_t;
/* Bounded blocking setup operations: dedicated non-UI worker, exclusive SSH.
 * Cancellation may invalidate SSH. Caller retains/closes SSH after return.
 * No remote compiler/package manager/sudo; userspace prebuilt fixed inventory.
 * Catalog is caller-trusted, not a signature verifier. Remote SHA256 verified
 * before executing payload. Existing releases/daemon/session never stopped.
 * On ambiguous publication inspect publication_unknown, do not assume rollback.
 */
gt_status_t gt_session_linux_probe(gt_ssh_t *,
                                   const gt_session_install_options_t *,
                                   gt_session_linux_info_t *);
gt_status_t gt_session_linux_install(gt_ssh_t *, const char *bundle_directory,
                                     const char *absolute_install_root,
                                     const gt_session_install_options_t *,
                                     gt_session_install_result_t *);
#ifdef __cplusplus
}
#endif
#endif
