/* Dedicated read-only SFTP owner: real OpenSSH and explicit protocol barriers. */
#include "ghosterm.h"
#include "internal.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Interpose only the public init symbol in this test executable, forwarding
 * to the actual linked library with its supported allocator callbacks. This
 * observes real SFTP allocations without production hooks or a fake backend. */
typedef struct allocation {
  void *pointer;
  bool browse;
  struct allocation *next;
} allocation;
static allocation *allocations;
static bool tracking_browse;
static size_t browse_allocations;
static void *tracked_alloc(size_t size, void **context) {
  (void)context;
  void *pointer = malloc(size);
  if (pointer) {
    allocation *entry = malloc(sizeof *entry); if (!entry) abort();
    *entry = (allocation){pointer, tracking_browse, allocations}; allocations = entry;
    if (tracking_browse) browse_allocations++;
  }
  return pointer;
}
static void tracked_free(void *pointer, void **context) {
  (void)context; if (!pointer) return;
  allocation **cursor = &allocations;
  while (*cursor && (*cursor)->pointer != pointer) cursor = &(*cursor)->next;
  if (!*cursor) abort();
  allocation *entry = *cursor; *cursor = entry->next; free(entry); free(pointer);
}
static void *tracked_realloc(void *pointer, size_t size, void **context) {
  if (!pointer) return tracked_alloc(size, context);
  if (!size) { tracked_free(pointer, context); return NULL; }
  allocation *entry = allocations;
  while (entry && entry->pointer != pointer) entry = entry->next;
  if (!entry) abort();
  void *next = realloc(pointer, size); if (next) entry->pointer = next; return next;
}
LIBSSH2_SESSION *libssh2_session_init_ex(LIBSSH2_ALLOC_FUNC((*allocate)),
    LIBSSH2_FREE_FUNC((*release)), LIBSSH2_REALLOC_FUNC((*reallocate)), void *abstract) {
  (void)allocate; (void)release; (void)reallocate; (void)abstract;
  typedef LIBSSH2_SESSION *(*init_fn)(LIBSSH2_ALLOC_FUNC((*)),
      LIBSSH2_FREE_FUNC((*)), LIBSSH2_REALLOC_FUNC((*)), void *);
  init_fn original = (init_fn)dlsym(RTLD_NEXT, "libssh2_session_init_ex");
  if (!original) abort();
  return original(tracked_alloc, tracked_free, tracked_realloc, NULL);
}

static volatile sig_atomic_t stopped;
static int failures;
#define CHECK(c, message) do { if (!(c)) { puts("FAIL: " message); failures++; } } while (0)
static void stop(int value) { (void)value; stopped = 1; }
static bool cancelled(void *unused) { (void)unused; return stopped != 0; }
static int64_t now(void) {
  struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC, &ts)) exit(2);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static gt_hostkey_decision_t trust(void *u, const char *h, uint16_t p, const char *t, const char *f) {
  (void)u; (void)h; (void)p; (void)t; (void)f; return GT_HOSTKEY_ACCEPT;
}
static gt_ssh_t *connect_fixture(char **argv) {
  char known[4096]; snprintf(known, sizeof known, "%s/browse-known", argv[5]);
  gt_ssh_opts_t options = {.host=argv[1], .port=(uint16_t)atoi(argv[2]), .username=argv[3], .known_hosts_path=known};
  gt_ssh_t *ssh = gt_ssh_new(&options);
  FILE *keyfile = fopen(argv[4], "rb"); if (!keyfile || !ssh) exit(2);
  char key[16384] = {0}; size_t length = fread(key, 1, sizeof key - 1, keyfile);
  if (ferror(keyfile) || !feof(keyfile) || !length) exit(2);
  fclose(keyfile);
  int status = gt_ssh_auth_private_key(ssh, key, NULL);
  memset(key, 0, sizeof key);
  if (status != GT_OK) exit(2);
  gt_ssh_set_hostkey_policy(ssh, trust, NULL);
  if (gt_ssh_connect(ssh) != GT_OK) { puts("FAIL: browse fixture authentication"); exit(1); }
  return ssh;
}
static void check_allocations(void) {
  CHECK(browse_allocations > 0, "browse allocation probe actually installed");
  size_t leaked = 0, earlier = 0;
  while (allocations) {
    if (allocations->browse) leaked++; else earlier++;
    tracked_free(allocations->pointer, NULL);
  }
  CHECK(leaked == 0, "browse SFTP allocations all released");
  printf("BROWSE ALLOCATIONS: observed=%zu remaining=%zu preexisting=%zu\n", browse_allocations, leaked, earlier);
}
static void make_file(const char *directory, const char *name) {
  char path[8192]; snprintf(path, sizeof path, "%s/%s", directory, name);
  FILE *file = fopen(path, "wb"); if (!file || fclose(file)) exit(2);
}
int main(int argc, char **argv) {
  if (argc != 6 && argc != 7) return 2;
  setvbuf(stdout, NULL, _IONBF, 0);
  signal(SIGUSR1, stop);
  const char *mode = argc == 7 ? argv[6] : "normal";
  bool normal = !strcmp(mode, "normal") || !strcmp(mode, "jobs");
  char path[8192]; snprintf(path, sizeof path, "%s/browse-fixture", argv[5]);
  if (normal) {
    if (mkdir(path, 0700)) return 2;
    make_file(path, "zero"); make_file(path, "数据.txt"); make_file(path, ".hidden");
    char child[16384]; snprintf(child, sizeof child, "%s/folder", path); if (mkdir(child, 0700)) return 2;
    snprintf(child, sizeof child, "%s/link", path); if (symlink("zero", child)) return 2;
    snprintf(child, sizeof child, "%s/pipe", path); if (mkfifo(child, 0600)) return 2;
  }
  gt_ssh_t *ssh = connect_fixture(argv);
  tracking_browse = true;
  if (!strncmp(mode, "raw-", 4)) {
    void *raw = NULL; int socket = -1;
    CHECK(gt__ssh_borrow(ssh, &raw, &socket) == GT_OK, "raw bounds authenticated fixture");
    LIBSSH2_SFTP *raw_sftp = libssh2_sftp_init(raw);
    CHECK(raw_sftp != NULL, "raw bounds SFTP fixture");
    if (!raw_sftp) return 1;
    if (!strcmp(mode, "raw-empty")) {
      LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(raw_sftp, "/fixture");
      CHECK(handle != NULL, "raw bounds directory fixture");
      if (handle) {
        char name[1024]; LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
        int result = libssh2_sftp_readdir_ex(handle, name, sizeof name, NULL, 0, &attrs);
        CHECK(result < 0, "dependency empty filename must not become directory EOF");
        libssh2_sftp_closedir(handle);
      }
    } else {
      char result[1024]; memset(result, 'x', sizeof result);
      int length = libssh2_sftp_realpath(raw_sftp, ".", result, sizeof result);
      if (!strcmp(mode, "raw-path")) {
        CHECK(length == LIBSSH2_ERROR_SFTP_PROTOCOL && result[0] == 'x', "dependency path length checked before copying");
      } else CHECK(length == 8 && !strcmp(result, "/fixture"), "dependency valid path control");
    }
    libssh2_sftp_shutdown(raw_sftp); gt_ssh_close(ssh); check_allocations();
    if (!failures) puts("SFTP BROWSE PASS");
    return failures ? 1 : 0;
  }
  gt_status_t open_status;
  gt_sftp_t *sftp = gt_ssh_sftp_controlled(ssh, 3000, cancelled, NULL, &open_status);
  if (!sftp || open_status != GT_OK) { puts("FAIL: browse fixture SFTP"); return 1; }
  int64_t calibration = now();
  for (int i = 0; i < 20; i++) { struct timespec t = {.tv_nsec=10000000}; nanosleep(&t, NULL); }
  calibration = now() - calibration;
  int64_t allowance = calibration * 3 / 2 + 500;
  int64_t limit = allowance > 3000 ? allowance : 3000;
  char resolved[65536] = "stale";
  gt_sftp_entry_t *entries = NULL; size_t count = 99;
  int rc = gt_sftp_browse_controlled(sftp, ".", resolved, sizeof resolved, &entries, &count, 0, 100, NULL, NULL);
  CHECK(rc == GT_ERR_INVALID_ARG && !entries && !count && !resolved[0], "browse zero budget clears outputs");
  rc = gt_sftp_browse_controlled(sftp, ".", resolved, sizeof resolved, &entries, &count, 100, 0, NULL, NULL);
  CHECK(rc == GT_ERR_INVALID_ARG, "browse zero entry limit");
  stopped = 1;
  rc = gt_sftp_browse_controlled(sftp, ".", resolved, sizeof resolved, &entries, &count, 3000, 100, cancelled, NULL);
  CHECK(rc == GT_ERR_CANCELLED && !entries && !count && !resolved[0], "browse preflight cancellation");
  stopped = 0;
  if (!strcmp(mode, "jobs")) {
    char source[16384], target[16384];
    snprintf(source, sizeof source, "%s/zero", path); snprintf(target, sizeof target, "%s/job-copy", path);
    CHECK(gt_sftp_download(sftp, source, target) != 0, "browse transfer control enqueued");
  }
  printf("READY %ld\n", (long)getpid());
  int64_t started = now();
  rc = gt_sftp_browse_controlled(sftp, normal ? path : ".", resolved,
      !strcmp(mode, "buffer") ? 2 : sizeof resolved, &entries, &count,
      !strcmp(mode, "timeout") ? 500 : 5000, !strcmp(mode, "limit") ? 2 : 100, cancelled, NULL);
  int64_t elapsed = now() - started;
  bool success = !strcmp(mode, "normal") || !strcmp(mode, "allow") || !strcmp(mode, "empty");
  if (success) {
    CHECK(rc == GT_OK && resolved[0] == '/', "browse complete success and real path");
    CHECK(count == (normal ? 6u : !strcmp(mode, "empty") ? 0u : 5u), "browse complete entry count");
    if (normal) CHECK(!strcmp(resolved, path), "browse resolves actual OpenSSH directory");
    bool zero = false, unknown = false, link = false, directory = false, special = false, utf8 = false;
    for (size_t i = 0; i < count; i++) {
      const char *name = strrchr(entries[i].full_path, '/'); name = name ? name + 1 : "";
      const gt_sftp_stat_t *st = &entries[i].stat;
      if (!strcmp(name, "zero")) zero = st->has_size && st->size == 0 && st->is_regular;
      if (!strcmp(name, "unknown")) unknown = !st->has_size && !st->is_regular && !st->is_dir && !st->is_symlink && st->mtime_unix < 0;
      if (!strcmp(name, "link")) link = st->is_symlink && !st->is_regular;
      if (!strcmp(name, "folder")) directory = st->is_dir && !st->is_regular;
      if (!strcmp(name, "pipe")) special = !st->is_dir && !st->is_symlink && !st->is_regular;
      if (!strcmp(name, "数据.txt")) utf8 = st->is_regular && st->has_size;
    }
    if (strcmp(mode, "empty")) {
      CHECK(zero && link && directory && utf8, "browse type and zero-size metadata");
      CHECK(normal ? special : unknown, "browse unknown metadata is not regular zero-byte file");
    }
    gt_sftp_list_free(entries, count); entries = NULL;
    char again[4096];
    CHECK(gt_sftp_realpath(sftp, ".", again, sizeof again) == GT_OK, "browse restores legacy blocking IO settings");
  } else if (!strcmp(mode, "jobs")) {
    CHECK(rc == GT_ERR_INVALID_ARG && !entries && !count && !resolved[0], "browse refuses transfer owner");
  } else {
    CHECK(rc == (!strcmp(mode, "cancel") ? GT_ERR_CANCELLED : GT_ERR_IO), "browse fault status");
    CHECK(!entries && !count && !resolved[0], "browse failure never publishes partial outputs");
    if (!strcmp(mode, "timeout")) CHECK(elapsed >= 498, "browse original total budget");
    void *raw = NULL; int fd = -1;
    CHECK(gt__ssh_borrow(ssh, &raw, &fd) != GT_OK, "browse IO failure invalidates owner");
  }
  CHECK(elapsed < limit, "browse bounded operation");
  int64_t closing = now();
  gt_sftp_close(sftp); gt_ssh_close(ssh);
  CHECK(now() - closing < limit, "browse bounded owner release");
  check_allocations();
  if (!failures) puts("SFTP BROWSE PASS");
  return failures ? 1 : 0;
}
