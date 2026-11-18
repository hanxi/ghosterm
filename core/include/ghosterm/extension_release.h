/* Raw release signature authentication. No JSON trust policy, installation or
 * runtime authorization. Platform bindings enter via ghosterm.h. */
#ifndef GHOSTERM_EXTENSION_RELEASE_H
#define GHOSTERM_EXTENSION_RELEASE_H
#include "ghosterm/types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GT_EXT_RELEASE_DESCRIPTOR_MAX 65536
/* Ed25519 over ASCII GHOSTERM-PLUGIN-RELEASE-V1, one NUL, then exactly the
 * descriptor bytes. Borrowed immutable inputs, no private material or I/O.
 * Key/signature lengths must be 32/64; descriptor length must be 1..65536.
 * GT_OK authenticates bytes only. Callers must independently validate JSON,
 * channel/key policy/revocation and bind claims to a checked package.
 * AUTH = signature mismatch; INTERNAL = verification could not complete;
 * an explicitly disabled crypto build returns UNSUPPORTED for valid inputs. */
gt_status_t gt_ext_release_verify(const uint8_t *public_key,
                                  size_t public_key_size,
                                  const uint8_t *signature,
                                  size_t signature_size,
                                  const uint8_t *descriptor,
                                  size_t descriptor_size);
/* Same bounds and statuses, using ASCII ghosterm.channel-policy.v1 plus NUL
 * and the exact statement bytes. The channel trust root is supplied by the host. */
gt_status_t gt_ext_channel_policy_verify(const uint8_t *public_key,
                                         size_t public_key_size,
                                         const uint8_t *signature,
                                         size_t signature_size,
                                         const uint8_t *statement,
                                         size_t statement_size);
#ifdef __cplusplus
}
#endif
#endif
