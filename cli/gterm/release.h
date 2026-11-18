#ifndef GTERM_RELEASE_H
#define GTERM_RELEASE_H
#include "ghosterm.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char id[129], version[513], publisher[513], digest[65];
    char descriptor_digest[65], policy_digest[65], artifact_digest[65];
    uint64_t artifact_bytes;
    unsigned char *descriptor, *signature;
    size_t descriptor_size;
} gterm_release_t;

/* Private native CLI owners. No plugin code, installation hooks or grants. */
bool gterm_release_supported(void);
unsigned char *gterm_release_file(const char *, size_t, size_t *);
bool gterm_release_hash(const void *, size_t, char[65]);
int gterm_release_validate_policy(const char *, char[65], volatile sig_atomic_t *);
int gterm_release_authenticate(const char *,
                               const char *,
                               const char *,
                               const char *,
                               gterm_release_t *,
                               volatile sig_atomic_t *);
/* Review binds the exact material actually parsed, including denied evidence. */
int gterm_release_authenticate_snapshot(const char *,
                                        const char *,
                                        const char *,
                                        const char *,
                                        const char *,
                                        const char *,
                                        gterm_release_t *,
                                        volatile sig_atomic_t *);
int gterm_release_installed(const char *,
                            const char *,
                            const char *,
                            const char *,
                            const gt_ext_package_metadata_t *,
                            volatile sig_atomic_t *);
int gterm_release_profile(
    int, char **, const char *, const gt_ext_package_metadata_t *, volatile sig_atomic_t *);
/* Directory references are cooperative and held through pending IPC / Runtime. */
int gterm_release_pin_profile(int, char **, int *);
int gterm_release_pending(int, const char *);
int gterm_release_manage(const char *, int, char **, volatile sig_atomic_t *);
void gterm_release_clear(gterm_release_t *);
/* Owns a new fixed version. Published versions survive catalog-save failures.
 * On success output is an allocated canonical payload path. */
int gterm_release_install(const char *,
                          const char *,
                          const char *,
                          const char *,
                          const char *,
                          const char *,
                          char **,
                          char[65],
                          bool *,
                          volatile sig_atomic_t *);
#endif
