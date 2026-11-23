#ifndef GTERM_CATALOG_H
#define GTERM_CATALOG_H
#include "ghosterm.h"
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <sys/stat.h>
#define GTERM_CATALOG_ENTRIES 32u
#define GTERM_CATALOG_ARGUMENTS 256u

typedef struct {
    char *id, *version, *digest, *directory, *host;
    char **profile;
    uint32_t profile_count;
} gterm_catalog_entry_t;

typedef struct {
    int directory, lock;
    const char *path;
    struct stat root, original;
    bool present, committed, installed;
    uint64_t revision;
    uint32_t count;
    gterm_catalog_entry_t entries[GTERM_CATALOG_ENTRIES];
} gterm_catalog_t;

bool gterm_catalog_path(const char *);
bool gterm_catalog_digest(const char *);
bool gterm_catalog_utf8(const unsigned char *, size_t);
void gterm_catalog_entry_clear(gterm_catalog_entry_t *);
/* Private native CLI format; no Core ABI, discovery or permission grant. */
const char *gterm_catalog_default(bool create);
const char *gterm_store_default(bool create);
int gterm_catalog_open(gterm_catalog_t *, const char *, bool);
int gterm_catalog_save(gterm_catalog_t *, uint64_t, volatile sig_atomic_t *);
bool gterm_catalog_release(gterm_catalog_t *);
bool gterm_catalog_close(gterm_catalog_t *);
int gterm_catalog_manage(const char *, int, char **, volatile sig_atomic_t *);
int gterm_catalog_dispatch(int, char **, volatile sig_atomic_t *);
int gterm_catalog_review_policy(const char *, int, char **, volatile sig_atomic_t *);
int gterm_catalog_report(int, const char *, const gterm_catalog_t *);
bool gterm_catalog_revision(const char *, uint64_t *);
/* false is metadata-only review; every activation path passes true. */
int gterm_catalog_capture(const gterm_catalog_entry_t *,
                          gt_ext_package_t **,
                          gt_ext_package_metadata_t *,
                          bool,
                          volatile sig_atomic_t *);
void gterm_json_string(const char *);
#endif
