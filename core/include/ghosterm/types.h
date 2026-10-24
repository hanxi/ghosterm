/* Shared public POD types. No subsystem or aggregate-header dependencies. */
#ifndef GHOSTERM_TYPES_H
#define GHOSTERM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  GT_OK = 0,
  GT_ERR_INVALID_ARG = 1,
  GT_ERR_NOT_FOUND = 2,
  GT_ERR_IO = 3,
  GT_ERR_AUTH = 4,
  GT_ERR_HOST_KEY = 5,
  GT_ERR_PROTOCOL = 6,
  GT_ERR_UNSUPPORTED = 7,
  GT_ERR_CANCELLED = 8,
  GT_ERR_INTERNAL = 9,
} gt_status_t;

#endif /* GHOSTERM_TYPES_H */
