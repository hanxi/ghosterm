#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include "internal.h"
#include "base/secure-random.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KH_MAX_BYTES (1024 * 1024)
/* fcntl locks are process-owned: this mutex also serializes native threads. */
static pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static int wait_turn(uint64_t deadline) {
  if (clock_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
  struct timespec pause = {0, 5000000};
  nanosleep(&pause, NULL);
  return 0;
}
static bool same_file(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int read_file(int directory, const char *name, char **out, size_t *length,
                     bool *exists, struct stat *identity) {
  *out = NULL; *length = 0; *exists = false; memset(identity, 0, sizeof *identity);
  int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno != ENOENT) return -1;
    *out = gt__strdup(""); return *out ? 0 : -1;
  }
  int failure = 0;
  if (fstat(fd, identity) || !S_ISREG(identity->st_mode) || identity->st_nlink != 1 ||
      identity->st_uid != geteuid() || identity->st_size < 0 || identity->st_size > KH_MAX_BYTES) failure = EINVAL;
  char *bytes = failure ? NULL : gt__alloc(KH_MAX_BYTES + 1);
  if (!failure && !bytes) failure = ENOMEM;
  size_t count = 0;
  while (!failure) {
    ssize_t got = read(fd, bytes + count, KH_MAX_BYTES + 1 - count);
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) { failure = errno; break; }
    if (!got) break;
    count += (size_t)got;
    if (count > KH_MAX_BYTES) failure = EFBIG;
  }
  struct stat after;
  if (!failure && (fstat(fd, &after) || after.st_size != (off_t)count ||
      identity->st_size != after.st_size || identity->st_mtime != after.st_mtime)) failure = EAGAIN;
  if (close(fd) && !failure) failure = errno;
  if (failure) { free(bytes); errno = failure; return -1; }
  bytes[count] = 0; *out = bytes; *length = count; *exists = true;
  return 0;
}
static int unique_blob(int directory, const char *suffix, const char *bytes, size_t length, char name[96]) {
  uint8_t random[16];
  if (gt__secure_random(random, sizeof random) != GT_OK) { errno = EIO; return -1; }
  char token[33];
  for (size_t i = 0; i < sizeof random; i++) snprintf(token + i * 2, 3, "%02x", random[i]);
  snprintf(name, 96, ".ghosterm-known-hosts-%s.%s", token, suffix);
  int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { name[0] = 0; return -1; }
  int failure = 0;
  for (size_t offset = 0; offset < length;) {
    ssize_t written = write(fd, bytes + offset, length - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { failure = written < 0 ? errno : EIO; break; }
    offset += (size_t)written;
  }
  if (!failure && fsync(fd)) failure = errno;
  if (close(fd) && !failure) failure = errno;
  if (failure) { unlinkat(directory, name, 0); name[0] = 0; errno = failure; return -1; }
  return 0;
}

gt_status_t gt__kh_transaction(const char *path, const char *expected, size_t expected_length,
    gt__kh_prepare_fn prepare, void *context, gt_known_hosts_update_result_t *result) {
  if (result) memset(result, 0, sizeof *result);
  if (!path || path[0] != '/' || !prepare || !result || (expected && expected_length > KH_MAX_BYTES)) {
    gt__set_error("known_hosts transaction requires an absolute path and valid arguments");
    return GT_ERR_INVALID_ARG;
  }
  char *parent = gt__strdup(path), *current = NULL, *candidate = NULL, *latest = NULL;
  if (!parent) return GT_ERR_INTERNAL;
  char *slash = strrchr(parent, '/');
  char *name = gt__strdup(slash + 1);
  if (!name) { free(parent); return GT_ERR_INTERNAL; }
  if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
    gt__set_error("known_hosts requires a file name");
    free(parent); free(name); return GT_ERR_INVALID_ARG;
  }
  if (slash == parent) slash[1] = 0; else *slash = 0;
  int directory = -1, lock = -1; bool held = false, exists = false, latest_exists = false;
  char backup[96] = "", staged[96] = "";
  struct stat directory_identity, initial, updated, lock_identity, lock_path_identity;
  size_t length = 0, candidate_length = 0, latest_length = 0;
  gt_status_t status = GT_ERR_IO;
  const char *operation = "lock";
  uint64_t started = clock_ms();
  if (started == UINT64_MAX) goto done;
  uint64_t deadline = started + 250;
  for (;;) {
    int rc = pthread_mutex_trylock(&writer_mutex);
    if (!rc) { held = true; break; }
    if (rc != EBUSY) { errno = rc; goto done; }
    if (wait_turn(deadline)) goto done;
  }
  directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0 || fstat(directory, &directory_identity)) goto done;
  /* Hash only the basename to bound the lock filename. A hash collision merely
   * serializes unrelated files; it never selects the target to modify. */
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) hash = (hash ^ *p) * UINT64_C(1099511628211);
  char lock_name[64]; snprintf(lock_name, sizeof lock_name, ".ghosterm-kh-%016llx.lock", (unsigned long long)hash);
  lock = openat(directory, lock_name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (lock < 0 || fstat(lock, &lock_identity)) goto done;
  if (!S_ISREG(lock_identity.st_mode) || lock_identity.st_nlink != 1 || lock_identity.st_uid != geteuid()) { errno = EINVAL; goto done; }
  struct flock request = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  while (fcntl(lock, F_SETLK, &request) < 0) {
    if (errno != EACCES && errno != EAGAIN && errno != EINTR) goto done;
    if (wait_turn(deadline)) goto done;
  }
  if (fstatat(directory, lock_name, &lock_path_identity, AT_SYMLINK_NOFOLLOW) ||
      !same_file(&lock_identity, &lock_path_identity)) { errno = EAGAIN; goto done; }
  operation = "read";
  if (read_file(directory, name, &current, &length, &exists, &initial)) goto done;
  if (expected && (length != expected_length || memcmp(current, expected, length))) {
    result->conflict = true; errno = EAGAIN; goto done;
  }
  status = prepare(current, length, &candidate, &candidate_length, context);
  if (status != GT_OK || !candidate) goto finish;
  status = GT_ERR_IO;
  if (candidate_length > KH_MAX_BYTES || memchr(candidate, 0, candidate_length)) { errno = EINVAL; goto done; }
  operation = "backup";
  if (exists && (unique_blob(directory, "bak", current, length, backup) || fsync(directory))) goto done;
  operation = "stage";
  if (unique_blob(directory, "new", candidate, candidate_length, staged)) goto done;
  operation = "recheck";
  if (read_file(directory, name, &latest, &latest_length, &latest_exists, &updated)) goto done;
  struct stat parent_now;
  if (lstat(parent, &parent_now) || !same_file(&directory_identity, &parent_now) ||
      exists != latest_exists || (exists && !same_file(&initial, &updated)) ||
      length != latest_length || memcmp(current, latest, length)) {
    result->conflict = true; errno = EAGAIN; goto done;
  }
  operation = "replace";
  if (renameat(directory, staged, directory, name)) goto done;
  staged[0] = 0; result->committed = true;
  operation = "verify committed file";
  if (fsync(directory)) goto done;
  free(latest); latest = NULL;
  if (read_file(directory, name, &latest, &latest_length, &latest_exists, &updated) ||
      !latest_exists || latest_length != candidate_length || memcmp(latest, candidate, candidate_length)) { errno = EIO; goto done; }
  status = GT_OK;
  goto finish;
done:
  gt__set_error("known_hosts %s failed%s: %s", operation, result->committed ? " after commit" : " (not committed)", strerror(errno));
finish:
  if (*staged && directory >= 0) unlinkat(directory, staged, 0);
  if (lock >= 0 && close(lock) && status == GT_OK) { status = GT_ERR_IO; gt__set_error("known_hosts lock close failed"); }
  if (directory >= 0 && close(directory) && status == GT_OK) { status = GT_ERR_IO; gt__set_error("known_hosts directory close failed"); }
  if (held) pthread_mutex_unlock(&writer_mutex);
  free(parent); free(name); free(current); free(candidate); free(latest);
  return status;
}

typedef struct { size_t index; } remove_context;
static gt_status_t remove_record(const char *bytes, size_t length, char **candidate, size_t *size, void *context) {
  gt_known_hosts_t *snapshot = gt_known_hosts_parse(bytes, length);
  if (!snapshot) return GT_ERR_INVALID_ARG;
  gt_known_host_entry_t entry;
  gt_status_t status = gt_known_hosts_entry_at(snapshot, ((remove_context *)context)->index, &entry);
  if (status == GT_OK && (!entry.recognized || *entry.marker || entry.hashed)) status = GT_ERR_UNSUPPORTED;
  if (status == GT_OK) {
    *size = length - (entry.byte_end - entry.byte_start);
    *candidate = gt__alloc(*size + 1);
    if (!*candidate) status = GT_ERR_INTERNAL;
    else {
      memcpy(*candidate, bytes, entry.byte_start);
      memcpy(*candidate + entry.byte_start, bytes + entry.byte_end, length - entry.byte_end);
      (*candidate)[*size] = 0;
    }
  }
  gt_known_hosts_free(snapshot);
  if (status != GT_OK) gt__set_error("known_hosts removal requires a current ordinary record");
  return status;
}
gt_status_t gt_known_hosts_remove(const char *path, const char *expected, size_t length,
    size_t index, gt_known_hosts_update_result_t *result) {
  if (result) memset(result, 0, sizeof *result);
  if (!expected || length > KH_MAX_BYTES || memchr(expected, 0, length)) {
    gt__set_error("known_hosts removal requires a bounded NUL-free revision");
    return GT_ERR_INVALID_ARG;
  }
  remove_context context = {index};
  return gt__kh_transaction(path, expected, length, remove_record, &context, result);
}
