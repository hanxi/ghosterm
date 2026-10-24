#ifndef GT_IMPORT_FIXTURE_H
#define GT_IMPORT_FIXTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
  const uint8_t *canonical, *resources;
  size_t canonical_len, resources_len;
} import_parts;
typedef struct {
  import_parts initial, final;
  const uint8_t *delta;
  size_t delta_len;
  const void *general, *pages;
  void *scope;
  bool (*healthy)(void *);
} import_fixture;
void import_fixture_open(import_fixture *, const char *mode);
void import_fixture_revoke(void);
void import_fixture_close(void);
#endif
