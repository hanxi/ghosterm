#ifndef GT_RUNTIME_VIEWS_H
#define GT_RUNTIME_VIEWS_H
#include <stdint.h>

typedef struct {
    char handle[65], id[65], kind[8], describe[129], read[129], cancel[129];
} runtime_view_t;

typedef struct {
    uint32_t kinds;
    uint64_t next_id, revision;
    char nonce[33], json[8193];
    runtime_view_t entries[8];
} runtime_views_t;
#endif
