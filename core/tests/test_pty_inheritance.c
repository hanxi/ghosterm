/* Regression: opening another PTY must not leak masters/wakeup pipes across
 * exec. A leaked master prevents hangup and can wedge close's waiter join. */
#include "ghosterm.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; \
} } while (0)

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int child_check(int argc, char **argv) {
  for (int i = 2; i < argc; i++) {
    char *end;
    long fd = strtol(argv[i], &end, 10);
    if (*end || fd < 3 || fd > INT_MAX) return 2;
    errno = 0;
    if (fcntl((int)fd, F_GETFD) != -1 || errno != EBADF) {
      puts("INHERITANCE_FAIL");
      return 1;
    }
  }
  puts("INHERITANCE_OK");
  fflush(stdout);
  /* Stay alive while the parent closes the first, unrelated PTY. */
  char byte;
  while (read(STDIN_FILENO, &byte, 1) < 0 && errno == EINTR) {}
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--child-check") == 0)
    return child_check(argc, argv);

  enum { MAX_FDS = 4096, MAX_OWNED = 32 };
  unsigned char before[MAX_FDS];
  /* Also test caller-owned inheritable descriptors: the fork/exec boundary
   * must expose only stdin/stdout/stderr, even if another thread opens fds. */
  int sentinel[2];
  if (pipe(sentinel) != 0) return 1;
  for (int fd = 3; fd < MAX_FDS; fd++) before[fd] = fcntl(fd, F_GETFD) >= 0;
  gt_pty_opts_t options = {0};
  options.cmd_kind = GT_PTY_CMD_DIRECT;
  options.executable = "/bin/cat";
  gt_transport_t *first = gt_transport_open_pty(&options);
  CHECK(first != NULL);
  if (!first) { close(sentinel[0]); close(sentinel[1]); return 1; }

  char numbers[MAX_OWNED + 2][32];
  const char *arguments[MAX_OWNED + 4] = {"--child-check"};
  size_t count = 0;
  for (int fd = 3; fd < MAX_FDS; fd++) {
    int flags = fcntl(fd, F_GETFD);
    if (before[fd] || flags < 0) continue;
    CHECK((flags & FD_CLOEXEC) != 0);
    CHECK(count < MAX_OWNED);
    if (count >= MAX_OWNED) continue;
    snprintf(numbers[count], sizeof numbers[count], "%d", fd);
    arguments[count + 1] = numbers[count];
    count++;
  }
  CHECK(count >= 3); /* PTY master plus both wakeup descriptors. */
  for (int i = 0; i < 2; i++) {
    snprintf(numbers[count], sizeof numbers[count], "%d", sentinel[i]);
    arguments[count + 1] = numbers[count];
    count++;
  }
  arguments[count + 1] = NULL;
  char executable[PATH_MAX];
  CHECK(realpath(argv[0], executable) != NULL);
  options.executable = executable;
  options.argv = arguments;
  gt_transport_t *second = gt_transport_open_pty(&options);
  CHECK(second != NULL);
  if (second) {
    char output[256] = {0};
    size_t used = 0;
    double deadline = now() + 3;
    while (used + 1 < sizeof output && now() < deadline) {
      int n = gt_transport_read(second, (uint8_t *)output + used,
                                sizeof output - used - 1, 100);
      if (n > 0) {
        used += (size_t)n;
        output[used] = 0;
        if (strstr(output, "INHERITANCE_OK") || strstr(output, "INHERITANCE_FAIL")) break;
      } else if (n == GT_TRANSPORT_READ_EOF || n == GT_TRANSPORT_READ_ERROR) break;
    }
    CHECK(strstr(output, "INHERITANCE_OK") != NULL);
    /* Only close the first while second lives after isolation is proven.
     * On a broken implementation, clean up second first to keep this test
     * diagnostic instead of hanging forever in the known defect. */
    if (failures == 0) {
      double start = now();
      gt_transport_close(first);
      first = NULL;
      CHECK(now() - start < 2);
    }
    gt_transport_close(second);
  }
  gt_transport_close(first);
  close(sentinel[0]);
  close(sentinel[1]);
  printf("test_pty_inheritance: %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
