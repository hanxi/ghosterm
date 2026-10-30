#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
/* Compile the production writer with syscall observation/faults. All successful
 * operations still use real files, locks and rename; no mock in production. */
#include "ghosterm.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum role { DIRECTORY, LOCK, CURRENT, BACKUP, STAGE };
enum operation { NONE, OPEN, WRITE, SYNC, CLOSE, REPLACE };
static enum role roles[4096];
static enum operation fault;
static enum role fault_role;
static int fault_at, seen, hit;
static bool trip(enum operation operation, enum role role) {
  if (fault != operation || fault_role != role) return false;
  if (++seen != fault_at) return false;
  hit++; errno = EIO; return true;
}
static int observed_openat(int directory, const char *name, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args); }
  enum role role = strstr(name, ".bak") ? BACKUP : strstr(name, ".new") ? STAGE : strstr(name, ".lock") ? LOCK : CURRENT;
  if (trip(OPEN, role)) return -1;
  int fd = openat(directory, name, flags, mode);
  if (fd >= 0 && fd < 4096) roles[fd] = role;
  return fd;
}
static ssize_t observed_write(int fd, const void *bytes, size_t length) {
  if (trip(WRITE, roles[fd])) return -1;
  /* Force a real short write before the injected second-call failure. */
  if (fault == WRITE && fault_role == roles[fd] && fault_at == 2 && length > 1) length = 1;
  return write(fd, bytes, length);
}
static int observed_sync(int fd) { return trip(SYNC, roles[fd]) ? -1 : fsync(fd); }
static int observed_close(int fd) {
  bool fail = trip(CLOSE, roles[fd]);
  roles[fd] = DIRECTORY;
  int result = close(fd); /* Failure still closes the descriptor, never leak it. */
  if (fail) { errno = EIO; return -1; }
  return result;
}
static int observed_rename(int from, const char *source, int to, const char *target) {
  return trip(REPLACE, STAGE) ? -1 : renameat(from, source, to, target);
}
#define openat observed_openat
#define write observed_write
#define fsync observed_sync
#define close observed_close
#define renameat observed_rename
#include "../src/known_hosts_store.c"
#undef openat
#undef write
#undef fsync
#undef close
#undef renameat

#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL store: %s (%s)\n", label, gt_last_error()); exit(1); } } while (0)
static void save(const char *path, const char *bytes) {
  FILE *f = fopen(path, "wb"); CHECK(f, "fixture open");
  CHECK(fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes), "fixture write");
  CHECK(!fclose(f), "fixture close");
}
static char *load(const char *path) {
  FILE *f = fopen(path, "rb"); CHECK(f, "read fixture");
  CHECK(!fseek(f, 0, SEEK_END), "seek"); long size = ftell(f); CHECK(size >= 0, "size"); rewind(f);
  char *bytes = malloc((size_t)size + 1); CHECK(bytes, "allocate fixture");
  CHECK(fread(bytes, 1, (size_t)size, f) == (size_t)size && !fclose(f), "read all fixture");
  bytes[size] = 0; return bytes;
}
static void equals(const char *path, const char *expected, const char *label) {
  char *bytes = load(path); CHECK(!strcmp(bytes, expected), label); free(bytes);
}
static const char *original = "# 保留\r\n  a,b ssh-ed25519 AAAAAAAAAAAAAAAA comment\r\n\t\r\nc ssh-rsa BBBBBBBBBBBBBBBB";
static const char *removed = "# 保留\r\n\t\r\nc ssh-rsa BBBBBBBBBBBBBBBB";
static gt_status_t remove_first(const char *path, gt_known_hosts_update_result_t *result) {
  return gt_known_hosts_remove(path, original, strlen(original), 0, result);
}
static void clear_directory(const char *root) {
  DIR *d = opendir(root); CHECK(d, "cleanup directory"); struct dirent *entry;
  while ((entry = readdir(d))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char path[4096]; snprintf(path, sizeof path, "%s/%s", root, entry->d_name);
    CHECK(!unlink(path), "cleanup file");
  }
  closedir(d);
}
static int backups(const char *root, const char *expected) {
  int count = 0; DIR *d = opendir(root); CHECK(d, "backup listing"); struct dirent *e;
  while ((e = readdir(d))) {
    CHECK(!strstr(e->d_name, ".new"), "no staging residual");
    if (!strstr(e->d_name, ".bak")) continue;
    char path[4096]; snprintf(path, sizeof path, "%s/%s", root, e->d_name);
    equals(path, expected, "backup complete original");
    struct stat st; CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600, "backup private mode"); count++;
  }
  closedir(d); return count;
}
static gt_status_t append(const char *bytes, size_t length, char **candidate, size_t *size, void *context) {
  const char *suffix = context;
  *size = length + strlen(suffix); *candidate = malloc(*size + 1); CHECK(*candidate, "append allocation");
  memcpy(*candidate, bytes, length); memcpy(*candidate + length, suffix, strlen(suffix) + 1);
  return GT_OK;
}
static gt_status_t replace_outside(const char *bytes, size_t length, char **candidate, size_t *size, void *context) {
  save(context, "external edit\n");
  return append(bytes, length, candidate, size, "new\n");
}
typedef struct { const char *path; gt_status_t status; gt_known_hosts_update_result_t result; } worker;
static void *remove_thread(void *context) {
  worker *w = context; w->status = remove_first(w->path, &w->result); return NULL;
}
static int locked_ready, release_lock;
static gt_status_t hold_lock(const char *bytes, size_t length, char **candidate, size_t *size, void *context) {
  (void)bytes; (void)length; (void)candidate; (void)size; (void)context;
  char c = 'x'; CHECK(write(locked_ready, &c, 1) == 1, "lock barrier ready");
  CHECK(read(release_lock, &c, 1) == 1, "lock barrier release"); return GT_OK;
}
static void *hold_thread(void *context) {
  worker *w = context;
  w->status = gt__kh_transaction(w->path, NULL, 0, hold_lock, NULL, &w->result); return NULL;
}
static void contention(const char *path, bool process) {
  int ready[2], release[2]; CHECK(!pipe(ready) && !pipe(release), "contention pipes");
  locked_ready = ready[1]; release_lock = release[0];
  worker owner = {.path = path}; pthread_t thread; pid_t child = -1;
  if (process) {
    child = fork(); CHECK(child >= 0, "contention fork");
    if (!child) { close(ready[0]); close(release[1]); hold_thread(&owner); _exit(owner.status == GT_OK ? 0 : 1); }
    close(ready[1]); close(release[0]);
  }
  else CHECK(!pthread_create(&thread, NULL, hold_thread, &owner), "contention thread");
  char c; CHECK(read(ready[0], &c, 1) == 1, "owner holds lock");
  /* Calibrate the same polling load before measuring the bounded lock call. */
  uint64_t calibration = clock_ms();
  for (int i = 0; i < 50; i++) { struct timespec pause = {0, 5000000}; nanosleep(&pause, NULL); }
  calibration = clock_ms() - calibration;
  uint64_t budget = calibration * 3 / 2 + 250;
  if (budget < 2000) budget = 2000;
  gt_known_hosts_update_result_t result; uint64_t before = clock_ms();
  CHECK(remove_first(path, &result) == GT_ERR_IO && !result.committed, "lock contention stops mutation");
  CHECK(clock_ms() - before < budget, "lock wait bounded"); equals(path, original, "contended file unchanged");
  CHECK(write(release[1], "x", 1) == 1, "release owner");
  if (process) { int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "process released lock"); }
  else CHECK(!pthread_join(thread, NULL) && owner.status == GT_OK, "thread released lock");
  close(ready[0]); close(release[1]);
  if (!process) { close(ready[1]); close(release[0]); }
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && argv[1][0] == '/', "absolute repository temp root required");
  char root[4096], path[4096];
  CHECK(snprintf(root, sizeof root, "%s/kh-store-XXXXXX", argv[1]) < (int)sizeof root, "root length");
  CHECK(mkdtemp(root), "create isolated directory");
  CHECK(snprintf(path, sizeof path, "%s/known_hosts", root) < (int)sizeof path, "path length");
  gt_known_hosts_update_result_t result;
  save(path, original); CHECK(remove_first(path, &result) == GT_OK && result.committed && !result.conflict, "remove committed");
  equals(path, removed, "exact record range only"); CHECK(backups(root, original) == 1, "required backup exists");
  struct stat st; CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600, "replacement private mode");
  CHECK(remove_first(path, &result) == GT_ERR_IO && result.conflict && !result.committed, "stale revision rejected");
  equals(path, removed, "stale revision no changes");
  const char *readonly[] = {"@revoked a ssh-rsa AAAAAAAAAAAAAAAA\n", "@cert-authority a ssh-rsa AAAAAAAAAAAAAAAA\n",
      "@future a ssh-rsa AAAAAAAAAAAAAAAA\n", "|1|a|b ssh-rsa AAAAAAAAAAAAAAAA\n", "broken\n"};
  for (size_t i = 0; i < sizeof readonly / sizeof *readonly; i++) {
    save(path, readonly[i]); CHECK(gt_known_hosts_remove(path, readonly[i], strlen(readonly[i]), 0, &result) == GT_ERR_UNSUPPORTED && !result.committed, "nonordinary removal rejected");
    equals(path, readonly[i], "readonly unchanged");
  }
  save(path, original);
  CHECK(gt_known_hosts_remove(path, original, strlen(original), 99, &result) == GT_ERR_NOT_FOUND && !result.committed, "missing record rejected");
  CHECK(gt_known_hosts_remove(path, "x\0y", 3, 0, &result) == GT_ERR_INVALID_ARG, "NUL revision rejected");
  CHECK(gt_known_hosts_remove("relative", original, strlen(original), 0, &result) == GT_ERR_INVALID_ARG, "relative path rejected");
  CHECK(gt__kh_transaction(path, original, strlen(original), replace_outside, path, &result) == GT_ERR_IO && result.conflict && !result.committed, "external edit conflicts");
  equals(path, "external edit\n", "external edit retained");
  clear_directory(root);
  struct { const char *name; enum operation operation; enum role role; int ordinal; bool committed; } failures[] = {
    {"backup open",OPEN,BACKUP,1,false}, {"backup write",WRITE,BACKUP,1,false},
    {"backup short write",WRITE,BACKUP,2,false}, {"backup sync",SYNC,BACKUP,1,false}, {"backup close",CLOSE,BACKUP,1,false},
    {"backup directory sync",SYNC,DIRECTORY,1,false}, {"stage open",OPEN,STAGE,1,false},
    {"stage write",WRITE,STAGE,1,false}, {"stage short write",WRITE,STAGE,2,false},
    {"stage sync",SYNC,STAGE,1,false}, {"stage close",CLOSE,STAGE,1,false},
    {"replace",REPLACE,STAGE,1,false}, {"read",OPEN,CURRENT,1,false},
    {"read close",CLOSE,CURRENT,1,false}, {"recheck",OPEN,CURRENT,2,false},
    {"committed directory sync",SYNC,DIRECTORY,2,true}, {"committed readback",OPEN,CURRENT,3,true},
    {"committed readback close",CLOSE,CURRENT,3,true}, {"lock close",CLOSE,LOCK,1,true},
    {"directory close",CLOSE,DIRECTORY,1,true}
  };
  for (size_t i = 0; i < sizeof failures / sizeof *failures; i++) {
    save(path, original); fault = failures[i].operation; fault_role = failures[i].role;
    fault_at = failures[i].ordinal; seen = hit = 0;
    gt_status_t status = remove_first(path, &result); fault = NONE;
    CHECK(hit == 1, "fault actually injected");
    if (status != GT_ERR_IO || result.committed != failures[i].committed || result.conflict) {
      fprintf(stderr, "FAIL store: fault %s status=%d committed=%d\n", failures[i].name, status, result.committed); return 1;
    }
    equals(path, failures[i].committed ? removed : original, "failure preserves actual commit state");
    backups(root, original); clear_directory(root);
  }
  save(path, original);
  char other[4096]; CHECK(snprintf(other, sizeof other, "%s/other", root) < (int)sizeof other, "other path");
  CHECK(!symlink(path, other), "symlink fixture"); CHECK(remove_first(other, &result) == GT_ERR_IO && !result.committed, "symlink rejected"); unlink(other);
  CHECK(!link(path, other), "hardlink fixture"); CHECK(remove_first(path, &result) == GT_ERR_IO && !result.committed, "hardlink rejected"); unlink(other);
  CHECK(!mkfifo(other, 0600), "FIFO fixture"); CHECK(remove_first(other, &result) == GT_ERR_IO && !result.committed, "FIFO rejected without blocking"); unlink(other);
  contention(path, false); contention(path, true);
  worker a = {.path = path}, b = {.path = path}; pthread_t ta, tb;
  CHECK(!pthread_create(&ta, NULL, remove_thread, &a) && !pthread_create(&tb, NULL, remove_thread, &b), "two removers");
  CHECK(!pthread_join(ta, NULL) && !pthread_join(tb, NULL), "join removers");
  CHECK((a.status == GT_OK && b.status == GT_ERR_IO && b.result.conflict) ||
        (b.status == GT_OK && a.status == GT_ERR_IO && a.result.conflict), "one revision can commit only once");
  equals(path, removed, "concurrent removal exact bytes");
  CHECK(!unlink(path), "remove fixture");
  CHECK(gt__kh_transaction(path, NULL, 0, append, "first\n", &result) == GT_OK && result.committed, "new file transaction");
  equals(path, "first\n", "new file exact bytes");
  clear_directory(root); CHECK(!rmdir(root), "cleanup root");
  puts("known_hosts store PASS"); return 0;
}
