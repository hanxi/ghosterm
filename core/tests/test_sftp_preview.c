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
  bool preview;
  struct allocation *next;
} allocation;
static allocation *allocations;
static bool tracking_preview;
static size_t preview_allocations;
static void *tracked_alloc(size_t size, void **context) {
  (void)context;
  void *pointer = malloc(size);
  if (pointer) {
    allocation *entry = malloc(sizeof *entry); if (!entry) abort();
    *entry = (allocation){pointer, tracking_preview, allocations}; allocations = entry;
    if (tracking_preview) preview_allocations++;
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
  char known[4096]; snprintf(known, sizeof known, "%s/preview-known", argv[5]);
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
  if (gt_ssh_connect(ssh) != GT_OK) { puts("FAIL: preview fixture authentication"); exit(1); }
  return ssh;
}
static void check_allocations(void) {
  CHECK(preview_allocations > 0, "preview allocation probe actually installed");
  size_t leaked = 0, earlier = 0;
  while (allocations) {
    if (allocations->preview) leaked++; else earlier++;
    tracked_free(allocations->pointer, NULL);
  }
  CHECK(leaked == 0, "preview SFTP allocations all released");
  printf("PREVIEW ALLOCATIONS: observed=%zu remaining=%zu preexisting=%zu\n", preview_allocations, leaked, earlier);
}
static const uint8_t content[] = "hello\0远端预览\xff\n";
int main(int argc, char **argv) {
  if (argc != 6 && argc != 7) return 2;
  setvbuf(stdout, NULL, _IONBF, 0); signal(SIGUSR1, stop);
  const char *mode = argc == 7 ? argv[6] : "normal";
  bool normal = !strcmp(mode, "normal");
  char path[8192]; snprintf(path, sizeof path, "%s/preview-file", argv[5]);
  if (normal) {
    FILE *f = fopen(path, "wb"); if (!f) return 2;
    if (fwrite(content, 1, sizeof content-1, f) != sizeof content-1 || fclose(f)) return 2;
  }
  gt_ssh_t *ssh = connect_fixture(argv); tracking_preview = true;
  gt_status_t opened; gt_sftp_t *sftp = gt_ssh_sftp_controlled(ssh, 3000, cancelled, NULL, &opened);
  if (!sftp || opened != GT_OK) return 2;
  void *session=NULL; int socket=-1;
  if (gt__ssh_borrow(ssh,&session,&socket)!=GT_OK) return 2;
  libssh2_session_set_timeout(session,2345);
  int64_t calibration=now();
  for (int i=0;i<20;i++) { struct timespec t={.tv_nsec=10000000}; nanosleep(&t,NULL); }
  calibration=now()-calibration;
  int64_t limit=calibration*3/2+500; if (limit<3000) limit=3000;
  uint8_t buffer[64]; memset(buffer, 0x55, sizeof buffer); size_t count=99;
  int rc = gt_sftp_preview_controlled(sftp, path, buffer, sizeof buffer, &count, 0, NULL, NULL);
  CHECK(rc == GT_ERR_INVALID_ARG && !count && !buffer[0], "preview zero budget clears outputs");
  rc = gt_sftp_preview_controlled(sftp, path, buffer, 0, &count, 100, NULL, NULL);
  CHECK(rc == GT_ERR_INVALID_ARG && !count, "preview rejects zero capacity");
  rc = gt_sftp_preview_controlled(sftp, path, buffer, 1048577, &count, 100, NULL, NULL);
  CHECK(rc == GT_ERR_INVALID_ARG && !count, "preview rejects excessive capacity before touching buffer");
  stopped=1;
  rc = gt_sftp_preview_controlled(sftp, path, buffer, sizeof buffer, &count, 3000, cancelled, NULL);
  CHECK(rc == GT_ERR_CANCELLED && !count, "preview preflight cancellation"); stopped=0;
  printf("READY %ld\n", (long)getpid());
  int64_t begin=now();
  rc = gt_sftp_preview_controlled(sftp, normal ? path : "/preview", buffer, sizeof buffer, &count,
      !strcmp(mode,"timeout") ? 500 : 5000, cancelled, NULL);
  int64_t elapsed=now()-begin;
  bool success = normal || !strcmp(mode,"allow") || !strcmp(mode,"empty") || !strcmp(mode,"exact");
  if (success) {
    size_t expected = !strcmp(mode,"empty") ? 0 : !strcmp(mode,"exact") ? 64 : sizeof content-1;
    CHECK(rc == GT_OK && count == expected, "preview complete byte count");
    uint8_t exact[64]; memset(exact, 'x', sizeof exact);
    CHECK(!memcmp(buffer, !strcmp(mode,"exact") ? exact : content, expected), "preview preserves raw bytes including NUL and invalid UTF8");
    void *raw=NULL; int fd=-1;
    CHECK(gt__ssh_borrow(ssh,&raw,&fd)==GT_OK, "preview success preserves owner");
    if (raw) {
      CHECK(libssh2_session_get_blocking(raw)==1, "preview restores blocking mode");
      CHECK(libssh2_session_get_timeout(raw)==2345, "preview restores session timeout");
    }
  } else {
    int expected = !strcmp(mode,"cancel") ? GT_ERR_CANCELLED : !strcmp(mode,"unsupported") ? GT_ERR_UNSUPPORTED : GT_ERR_IO;
    CHECK(rc==expected, "preview fault status");
    uint8_t zero[64]={0};
    CHECK(!count && !memcmp(buffer,zero,sizeof buffer), "preview failure publishes no bytes");
    void *raw=NULL; int fd=-1;
    CHECK(gt__ssh_borrow(ssh,&raw,&fd)!=GT_OK, "preview failure invalidates owner");
    if (!strcmp(mode,"timeout")) CHECK(elapsed>=498, "preview total budget consumed");
  }
  CHECK(elapsed < limit, "preview bounded operation");
  if (normal) {
    CHECK(gt_ssh_abort(ssh)==GT_OK, "preview owner abort succeeds");
    void *raw=NULL; int fd=-1;
    CHECK(gt__ssh_borrow(ssh,&raw,&fd)!=GT_OK, "preview owner abort invalidates connection");
    CHECK(gt_ssh_abort(ssh)==GT_OK, "preview owner abort is idempotent");
  }
  begin=now(); gt_sftp_close(sftp); gt_ssh_close(ssh);
  CHECK(now()-begin < limit, "preview bounded owner release");
  check_allocations();
  if (!failures) puts("SFTP PREVIEW PASS");
  return failures ? 1 : 0;
}
