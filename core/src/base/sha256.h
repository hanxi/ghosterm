/* Private, byte-oriented SHA-256. No allocation or optional SSH dependency.
 * Digest consistency only: callers still authenticate/authorize the source. */
#ifndef GT_PRIVATE_SHA256_H
#define GT_PRIVATE_SHA256_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    size_t used;
    uint8_t block[64];
    bool ended;
} gt_sha256_t;

void gt__sha256_init(gt_sha256_t *);
bool gt__sha256_update(gt_sha256_t *, const void *, size_t);
bool gt__sha256_final(gt_sha256_t *, char hex[65]);
#endif
