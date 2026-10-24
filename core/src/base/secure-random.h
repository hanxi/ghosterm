#ifndef GT_SECURE_RANDOM_H
#define GT_SECURE_RANDOM_H
#include <stddef.h>
#include <stdint.h>
#include "ghosterm/types.h"
/* One OS call, 1..256 bytes. Linux uses GRND_NONBLOCK, no weak/disk fallback.
 * Failure wipes valid output; unsupported entropy is never success. */
gt_status_t gt__secure_random(uint8_t *,size_t);
void gt__secure_erase(void *,size_t);
#endif
