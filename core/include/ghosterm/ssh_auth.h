/* Nonblocking SSH authentication. No session daemon or UI dependency. */
#ifndef GHOSTERM_SSH_AUTH_H
#define GHOSTERM_SSH_AUTH_H
#include "ghosterm/ssh.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gt_ssh_authenticator gt_ssh_authenticator_t;

typedef enum {
    GT_SSH_AUTH_PHASE_TCP,
    GT_SSH_AUTH_PHASE_NEGOTIATING,
    GT_SSH_AUTH_PHASE_HOSTKEY,
    GT_SSH_AUTH_PHASE_AUTHENTICATING,
    GT_SSH_AUTH_PHASE_READY,
    GT_SSH_AUTH_PHASE_TAKEN,
    GT_SSH_AUTH_PHASE_FAILED,
    GT_SSH_AUTH_PHASE_CANCELLED,
    GT_SSH_AUTH_PHASE_CREDENTIALS
} gt_ssh_auth_phase_t;

typedef enum {
    GT_SSH_AUTH_HOST_UNCHECKED,
    GT_SSH_AUTH_HOST_KNOWN,
    GT_SSH_AUTH_HOST_UNKNOWN,
    GT_SSH_AUTH_HOST_MISMATCH,
    GT_SSH_AUTH_HOST_REVOKED /* terminal rejection, never an unknown-host prompt */
} gt_ssh_auth_host_state_t;

typedef struct {
    uint8_t family, address[16];
    uint32_t scope, timeout_ms;
    bool defer_credentials; /* discover methods before requesting unlocked material */
    void *clock_context;
    bool (*clock)(void *, uint64_t *monotonic_ms);
} gt_ssh_auth_options_t;

typedef struct {
    gt_ssh_auth_phase_t phase;
    gt_status_t error;
    gt_ssh_auth_host_state_t host;
    bool deadline;
    int32_t auth_methods;
    char key_type[32], fingerprint[80];
} gt_ssh_auth_info_t;

/* One non-UI serial owner. Success takes/clears fresh unlocked SSH only.
 * Resolved binary address is separate from original SSH hostkey identity.
 * Nonblocking step, one total deadline through hostkey UI (default 60000ms).
 * Unknown host waits for explicit decide; mismatch/revocation is terminal.
 * Unsupported known_hosts rules fail explicitly without a trust prompt. */
gt_status_t
gt_ssh_authenticator_open(gt_ssh_t **, const gt_ssh_auth_options_t *, gt_ssh_authenticator_t **);
gt_ssh_auth_phase_t gt_ssh_authenticator_step(gt_ssh_authenticator_t *);
bool gt_ssh_authenticator_info(const gt_ssh_authenticator_t *, gt_ssh_auth_info_t *);
gt_status_t gt_ssh_authenticator_decide(gt_ssh_authenticator_t *, gt_hostkey_decision_t);
/* CREDENTIALS only. Success takes/clears fresh unlocked material with the same
 * host/user/port/known_hosts identity; no extra TCP connection or handshake.
 * Method mismatch is UNSUPPORTED and retains caller ownership. Bad credentials
 * return to CREDENTIALS with error=AUTH, without resetting the total deadline. */
gt_status_t gt_ssh_authenticator_supply(gt_ssh_authenticator_t *, gt_ssh_t **);
/* READY once. Caller owns dedicated authenticated SSH; no SFTP/terminal
 * aliases. */
bool gt_ssh_authenticator_take(gt_ssh_authenticator_t *, gt_ssh_t **);
void gt_ssh_authenticator_close(gt_ssh_authenticator_t *);
/* Managed authentication chain, maximum eight independently verified hops.
 * One non-UI serial owner for every operation, including disposal. Only hop 0
 * has a resolved family/address (4 or 6); later hops use family 0 and via_host
 * as the preceding server's direct-tcpip destination. No local inner DNS/dial.
 * All clocks/contexts must match; NULL clock selects CLOCK_MONOTONIC.
 * Options/strings are copied. Clock context must outlive disposal. */
#define GT_SSH_ROUTE_MAX_HOPS 8u
typedef struct gt_ssh_route gt_ssh_route_t;

typedef struct {
    gt_ssh_auth_options_t authentication;
    const char *via_host; /* NULL for first hop, nonempty thereafter */
} gt_ssh_route_hop_t;

typedef enum {
    GT_SSH_ROUTE_AUTHENTICATING,
    GT_SSH_ROUTE_FORWARDING,
    GT_SSH_ROUTE_READY,
    GT_SSH_ROUTE_FAILED,
    GT_SSH_ROUTE_CANCELLED
} gt_ssh_route_phase_t;

typedef struct {
    gt_ssh_route_phase_t phase;
    gt_status_t error;
    uint32_t hop, count; /* zero-based current hop */
    bool deadline;
    gt_ssh_auth_info_t authentication; /* current hop only */
} gt_ssh_route_info_t;

/* Success consumes/clears every fresh SSH handle, failure retains all.
 * total_ms=0 selects 60000; valid explicit budgets are 1..60000. The total
 * deadline includes user decisions and credential retries, never resets at
 * a hop boundary. Deferred credentials follow the single-hop contract above. */
gt_status_t gt_ssh_route_open(gt_ssh_t **owned,
                              const gt_ssh_route_hop_t *,
                              uint32_t count,
                              uint32_t total_ms,
                              gt_ssh_route_t **);
gt_ssh_route_phase_t gt_ssh_route_step(gt_ssh_route_t *);
bool gt_ssh_route_info(const gt_ssh_route_t *, gt_ssh_route_info_t *);
gt_status_t gt_ssh_route_decide(gt_ssh_route_t *, gt_hostkey_decision_t);
gt_status_t gt_ssh_route_supply(gt_ssh_route_t *, gt_ssh_t **);
/* READY once: borrowed handle, valid only until disposal starts. Close every
 * PTY/SFTP child BEFORE disposing the route. Never call gt_ssh_close on this
 * handle or transfer it to another owning API. Route retains the entire chain.
 * Only controlled PTY/SFTP operations are currently supported on inner hops. */
gt_status_t gt_ssh_route_borrow(gt_ssh_route_t *, gt_ssh_t **);
/* Cancel/close one nonblocking step. Retain and retry while released=false,
 * including on error; never drop the owner. On release frees and clears *route.
 * NULL *route is already released. No protocol replay or background worker. */
gt_status_t gt_ssh_route_dispose(gt_ssh_route_t **, bool *released);
#ifdef __cplusplus
}
#endif
#endif
