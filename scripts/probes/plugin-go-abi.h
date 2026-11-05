/* Private P0 research ABI. Not ghosterm.h, not a production contract. */
#ifndef GHOSTERM_PROBE_GO_ABI_H
#define GHOSTERM_PROBE_GO_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PGABI_OK = 0, PGABI_INVALID = -1, PGABI_CANCELLED = -2,
       PGABI_BUSY = -3, PGABI_LIMIT = -4, PGABI_BUFFER = -5,
       PGABI_NETWORK = -6, PGABI_TIMEOUT = -7 };
typedef struct pgabi_stats {
    uint64_t handles, active, goroutines, heap_alloc, heap_sys, cgo_calls;
} pgabi_stats;
uint64_t pgabi_new(void);
int pgabi_copy(uint64_t id, void *input, uint32_t length, void *output,
               uint32_t capacity, uint32_t *written);
/* mode=0: real local TailPocket/DERP round trip. mode=1: cancellable wait,
 * used ONLY to test calls from C threads and concurrent cancel/close. */
int pgabi_run(uint64_t id, int mode, void *input, uint32_t length,
              void *output, uint32_t capacity, uint32_t *written);
int pgabi_cancel(uint64_t id);
int pgabi_close(uint64_t id);
int pgabi_snapshot(pgabi_stats *out, int collect);
#ifdef __cplusplus
}
#endif
#endif
