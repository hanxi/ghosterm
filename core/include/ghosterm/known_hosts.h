/* Read-only known_hosts snapshots. Platform storage owns locking/backups/commit. */
#ifndef GHOSTERM_KNOWN_HOSTS_H
#define GHOSTERM_KNOWN_HOSTS_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct gt_known_hosts gt_known_hosts_t;
typedef struct {
  size_t source_line; /* 1-based physical line. */
  size_t byte_start, byte_end; /* Half-open original byte range, INCLUDING newline. */
  const char *marker, *hosts, *key_type, *key_base64; /* Borrowed until snapshot free. */
  /* The current authentication reader recognizes this field shape. This is NOT
   * key validation, host matching, or a trust decision. Invalid rows remain visible. */
  bool recognized;
  bool hashed; /* Host field starts with '|'; matching is currently unsupported. */
} gt_known_host_entry_t;

/* Copies length bytes. Rejects NUL, >1 MiB, >4096 non-comment/non-empty rows.
 * Does not read files, resolve hosts, decode keys, or change authentication policy.
 * NULL signals invalid input/allocation/limit failure; gt_last_error explains it. */
gt_known_hosts_t *gt_known_hosts_parse(const char *text, size_t length);
size_t gt_known_hosts_count(const gt_known_hosts_t *snapshot);
/* NOT_FOUND at end, INVALID_ARG on NULL. Always clears a supplied output on error. */
gt_status_t gt_known_hosts_entry_at(const gt_known_hosts_t *snapshot, size_t index,
                                   gt_known_host_entry_t *out);
void gt_known_hosts_free(gt_known_hosts_t *snapshot);
#ifdef __cplusplus
}
#endif
#endif
