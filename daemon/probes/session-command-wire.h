/* Experimental command snapshot codec; never serialize the owner struct. */
#ifndef GT_SESSION_COMMAND_WIRE_H
#define GT_SESSION_COMMAND_WIRE_H
#include "session/command-status.h"
enum {CSW_HEADER=28,CSW_MAX=28+8192};
/* Big-endian fields, explicit validity flags; monotonic start time stays local.
 * Caller owns bounded buffers. Failure sets encoded length=0, or leaves decoded
 * state unchanged. No embedded NUL/trailing bytes/unknown flags/contradictions. */
bool csw_encode(const gt_command_status_t *,void *,size_t,size_t *);
bool csw_decode(const void *,size_t,gt_command_status_t *);
#endif
