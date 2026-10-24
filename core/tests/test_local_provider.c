#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "ipc/local-session-provider.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static gt_local_session_provider_t *provider;
static gt_session_link_t provider_link;
static uint64_t offset;
static bool bad_clock;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL local provider:%d: %s\n", __LINE__, #x);           \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static uint64_t now(void) {
  struct timespec t;
  CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static bool clock_ms(void *c, uint64_t *out) {
  (void)c;
  if (bad_clock)
    return false;
  *out = now() + offset;
  return true;
}
static void nap(void) {
  struct timespec t = {0, 1000000};
  nanosleep(&t, NULL);
}
static int helper(int argc, char **argv) {
  if (argc != 7 || strcmp(argv[1], "--runtime-dir") ||
      strcmp(argv[3], "--role") || strcmp(argv[5], "--daemon") ||
      strcmp(argv[6], "/fixture/daemon"))
    return 75;
  if (!strcmp(argv[2], "/case/status"))
    return 71;
  if (!strncmp(argv[2], "/fd/", 4)) {
    int fd = atoi(argv[2] + 4);
    if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
      return 73;
  }
  if (!strcmp(argv[2], "/case/cancel"))
    signal(SIGTERM, SIG_IGN);
  char diagnostics[4096];
  memset(diagnostics, 'E', sizeof(diagnostics));
  for (unsigned i = 0; i < 8; i++)
    if (write(2, diagnostics, sizeof(diagnostics)) !=
        (ssize_t)sizeof(diagnostics))
      return 74;
  unsigned char bytes[512];
  bool first = true;
  for (;;) {
    ssize_t n = read(0, bytes, sizeof(bytes));
    if (n <= 0) {
      if (!strcmp(argv[2], "/case/cancel"))
        for (;;)
          pause();
      return 0;
    }
    if(first){if(strcmp(argv[4],bytes[0]==77?"data":"management"))return 73;first=false;}
    ssize_t done = 0;
    while (done < n) {
      ssize_t sent = write(1, bytes + done, (size_t)(n - done));
      if (sent <= 0)
        return 74;
      done += sent;
    }
  }
}
static void open_provider(const char *exe, const char *runtime) {
  char proxy[1025], daemon[1025], directory[1025];
  CHECK(strlen(exe) < sizeof(proxy));
  strcpy(proxy, exe);
  strcpy(daemon, "/fixture/daemon");
  strcpy(directory, runtime);
  gt_local_session_provider_config_t c = {.proxy = proxy,
                                          .daemon = daemon,
                                          .runtime = directory,
                                          .clock = clock_ms};
  CHECK(gt__local_session_provider_open(&c, NULL, &provider) == GT_OK &&
        gt__local_session_provider_link(provider, &provider_link));
  memset(proxy, 'x', strlen(proxy));
  memset(daemon, 'x', strlen(daemon));
  memset(directory, 'x', strlen(directory));
}
static void start(void) {
  gt_status_t e;
  uint64_t end = now() + 3000;
  gt_session_link_state_t s =
      provider_link.step(provider_link.context, true, 1, &e);
  while (s == GT_SESSION_LINK_WAIT && now() < end) {
    nap();
    s = provider_link.step(provider_link.context, false, 1, &e);
  }
  CHECK(s == GT_SESSION_LINK_READY && e == GT_OK);
}
static void echo(gt_stream_ops_t *s, unsigned seed) {
  unsigned char input[512], output[512];
  for (unsigned i = 0; i < sizeof(input); i++)
    input[i] = (unsigned char)(i + seed);
  size_t sent = 0, received = 0;
  uint64_t end = now() + 3000;
  while (received < sizeof(output) && now() < end) {
    gt_status_t e;
    CHECK(provider_link.step(provider_link.context, false, 1, &e) ==
              GT_SESSION_LINK_READY &&
          e == GT_OK);
    if (sent < sizeof(input)) {
      gt_stream_result_t r =
          s->write(s->context, input + sent, sizeof(input) - sent);
      CHECK(r.kind == GT_STREAM_DATA || r.kind == GT_STREAM_WAIT);
      sent += r.length;
    }
    gt_stream_result_t r =
        s->read(s->context, output + received, sizeof(output) - received);
    CHECK(r.kind == GT_STREAM_DATA || r.kind == GT_STREAM_WAIT);
    received += r.length;
    nap();
  }
  CHECK(sent == sizeof(input) && received == sizeof(output) &&
        !memcmp(input, output, sizeof(input)));
}
static void close_provider(void) {
  provider_link.reset(provider_link.context);
  uint64_t end = now() + 3000;
  while (!gt__local_session_provider_quiesce(provider) && now() < end)
    nap();
  CHECK(gt__local_session_provider_destroy(provider));
  provider = NULL;
}
static void cleanup(void) {
  if (!provider)
    return;
  provider_link.reset(provider_link.context);
  uint64_t end = now() + 3000;
  while (!gt__local_session_provider_quiesce(provider) && now() < end)
    nap();
  (void)gt__local_session_provider_destroy(provider);
  provider = NULL;
}
static void *no_memory(void *c, size_t n) {
  (void)c;
  (void)n;
  return NULL;
}
static void release(void *c, void *p, size_t n) {
  (void)c;
  (void)p;
  (void)n;
}
int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "--runtime-dir"))
    return helper(argc, argv);
  CHECK(argc == 3);
  CHECK(!atexit(cleanup));
  char exe[PATH_MAX];
  CHECK(realpath(argv[0], exe));
  const char *mode = argv[1];
  if (!strcmp(mode, "validation")) {
    gt_local_session_provider_config_t c = {.proxy = exe,
                                            .daemon = "/daemon",
                                            .runtime = "relative",
                                            .clock = clock_ms};
    gt_local_session_provider_t *p = (void *)1;
    CHECK(gt__local_session_provider_open(&c, NULL, &p) == GT_ERR_INVALID_ARG &&
          !p);
    c.runtime = "/runtime";
    gt_allocator_t a = {NULL, no_memory, release};
    CHECK(gt__local_session_provider_open(&c, &a, &p) == GT_ERR_IO && !p);
    c.proxy = "/bad\npath";
    CHECK(gt__local_session_provider_open(&c, NULL, &p) == GT_ERR_INVALID_ARG &&
          !p);
    puts("PASS local validation and allocation");
    return 0;
  }
  char runtime[128];
  strcpy(runtime, "/case/basic");
  int inherited = -1;
  if (!strcmp(mode, "fd")) {
    inherited = open(argv[2], O_RDONLY);
    CHECK(inherited >= 0);
    CHECK(!fcntl(inherited, F_SETFD, 0));
    CHECK(snprintf(runtime, sizeof(runtime), "/fd/%d", inherited) > 0);
  }
  if (!strcmp(mode, "status"))
    strcpy(runtime, "/case/status");
  if (!strcmp(mode, "cancel"))
    strcpy(runtime, "/case/cancel");
  open_provider(!strcmp(mode, "missing") ? "/ghosterm-fixture-no-such-helper"
                                         : exe,
                runtime);
  start();
  CHECK(!gt__local_session_provider_destroy(provider));
  gt_stream_ops_t streams[2];
  if (!strcmp(mode, "missing")) {
    CHECK(provider_link.stream(provider_link.context, 0, &streams[0]) ==
          GT_ERR_NOT_FOUND);
    close_provider();
    puts("PASS missing local helper explicit");
    return 0;
  }
  CHECK(provider_link.stream(provider_link.context, 0, &streams[0]) == GT_OK);
  if (!strcmp(mode, "status")) {
    gt_status_t e = GT_OK;
    uint64_t end = now() + 3000;
    gt_session_link_state_t s;
    do {
      s = provider_link.step(provider_link.context, false, 1, &e);
      nap();
    } while (s == GT_SESSION_LINK_READY && now() < end);
    CHECK(s == GT_SESSION_LINK_FATAL && e == GT_ERR_UNSUPPORTED);
    streams[0].close(streams[0].context);
    close_provider();
    puts("PASS local typed proxy rejection");
    return 0;
  }
  echo(&streams[0], 1);
  if (!strcmp(mode, "binary") || !strcmp(mode, "restart")) {
    CHECK(provider_link.stream(provider_link.context, 1, &streams[1]) == GT_OK);
    echo(&streams[1], 77);
    gt_local_provider_info_t info;
    uint64_t end = now() + 3000;
    do {
      gt_status_t e;
      CHECK(provider_link.step(provider_link.context, false, 1, &e) ==
            GT_SESSION_LINK_READY);
      CHECK(gt__local_session_provider_info(provider, &info));
      if (info.stderr_bytes >= 65536)
        break;
      nap();
    } while (now() < end);
    CHECK(info.children == 2 && info.stderr_bytes >= 65536);
    streams[1].close(streams[1].context);
  }
  if (!strcmp(mode, "clock")) {
    bad_clock = true;
    gt_status_t e;
    CHECK(provider_link.step(provider_link.context, false, 1, &e) ==
              GT_SESSION_LINK_FATAL &&
          e == GT_ERR_INTERNAL);
  }
  streams[0].close(streams[0].context);
  if (!strcmp(mode, "restart")) {
    provider_link.reset(provider_link.context);
    start();
    CHECK(provider_link.stream(provider_link.context, 0, &streams[0]) == GT_OK);
    echo(&streams[0], 33);
    streams[0].close(streams[0].context);
  }
  uint64_t before = now();
  close_provider();
  CHECK(now() - before < 2000);
  if (!strcmp(mode, "cancel"))
    CHECK(now() - before >= 250);
  if (inherited >= 0)
    close(inherited);
  printf("PASS local proxy provider/%s\n", mode);
  return 0;
}
