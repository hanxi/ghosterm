/* Experimental low-frequency terminal result, not production IPC. */
#ifndef GT_SESSION_RESULT_WIRE_H
#define GT_SESSION_RESULT_WIRE_H
#include "session/worker.h"
enum {RSW_BYTES=56};
/* Fixed GWR1 big-endian POD. No terminal text, pointers or native padding.
 * Failed encode writes length=0; failed decode zeroes both outputs. */
bool rsw_encode(uint64_t,const gt_worker_result_t *,void *,size_t,size_t *);
bool rsw_decode(const void *,size_t,uint64_t *,gt_worker_result_t *);
#endif
