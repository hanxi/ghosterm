/* Shared bootstrap schemas; no service or client state machine. */
#ifndef GT_IPC_BOOTSTRAP_WIRE_H
#define GT_IPC_BOOTSTRAP_WIRE_H
#include "tokens.h"
enum {
  GT_MSG_HELLO=1,GT_MSG_HELLO_REPLY=2,GT_MSG_HELLO_ACK=3,
  GT_MSG_BIND=4,GT_MSG_BIND_REPLY=5,GT_MSG_BIND_ACK=6,
  GT_HELLO_BYTES=60,GT_HELLO_REPLY_BYTES=80,GT_BIND_BYTES=72,GT_BIND_REPLY_BYTES=36,
  GT_COMPAT_BYTES=32,GT_CHALLENGE_BYTES=16
};
typedef enum {GT_ROLE_MANAGEMENT=1,GT_ROLE_DATA=2} gt_protocol_role_t;
typedef enum {GT_HELLO_OK,GT_HELLO_INCOMPATIBLE,GT_HELLO_UNSUPPORTED,
  GT_HELLO_WRONG_INSTANCE,GT_HELLO_DENIED} gt_hello_status_t;
typedef struct {
  uint8_t instance[GT_INSTANCE_BYTES],compatibility[GT_COMPAT_BYTES];
  uint64_t features; /* explicit composition-root offer, not auto-detected */
} gt_protocol_identity_t;
typedef struct {
  gt_protocol_role_t role;uint64_t required_features;
  uint8_t compatibility[GT_COMPAT_BYTES],expected_instance[GT_INSTANCE_BYTES];
} gt_protocol_hello_t;
typedef struct {
  gt_hello_status_t status;gt_protocol_role_t role;gt_protocol_identity_t identity;
  uint8_t challenge[GT_CHALLENGE_BYTES];
} gt_protocol_hello_reply_t;
typedef struct {
  uint8_t instance[GT_INSTANCE_BYTES];uint64_t session,generation;
  gt_attachment_token_t token;
} gt_protocol_bind_t;
typedef struct {
  gt_hello_status_t status;uint64_t session,generation;
  uint8_t challenge[GT_CHALLENGE_BYTES];
} gt_protocol_bind_reply_t;
/* Fixed big-endian schemas, exact length, reserved zero. Codecs clear supplied
 * outputs on rejection. Identity compatibility is a platform-independent build/
 * snapshot contract fingerprint, NOT a platform binary hash. */
bool gt__hello_encode(const gt_protocol_hello_t *,uint8_t out[GT_HELLO_BYTES]);
bool gt__hello_decode(const void *,size_t,gt_protocol_hello_t *);
bool gt__hello_reply_decode(const void *,size_t,gt_protocol_hello_reply_t *);
bool gt__bind_encode(const gt_protocol_bind_t *,uint8_t out[GT_BIND_BYTES]);
bool gt__bind_reply_decode(const void *,size_t,gt_protocol_bind_reply_t *);
gt_status_t gt__protocol_identity_new(const uint8_t compatibility[GT_COMPAT_BYTES],uint64_t features,
  const gt_token_random_t *,gt_protocol_identity_t *);

#endif
