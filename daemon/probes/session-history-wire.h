/* P0 session history policy/diagnostic only, not public protocol or ABI. */
#ifndef GT_SESSION_HISTORY_WIRE_H
#define GT_SESSION_HISTORY_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum {HSW_UNKNOWN,HSW_COMPLETE,HSW_TRUNCATED};
enum {HSW_BYTES=32,HSW_EVENT_PREFIX=40};
typedef struct {uint64_t bytes,lines;unsigned state;} hsw_state;
/* Unknown has no inferred limits; known policy requires two positive bounds.
 * Fixed endian encoding, reserved bytes zero. Output is transactional. */
bool hsw_encode(hsw_state,void *,size_t);
bool hsw_decode(const void *,size_t,hsw_state *);
#endif
