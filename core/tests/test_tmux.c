#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include "internal.h"
#include "tmux/command.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c, m)                                                            \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, m,            \
              gt_last_error());                                                \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static gt_tmux_session_t rows[GT_TMUX_SESSION_MAX];
static size_t count;
static gt_tmux_server_t server;
static gt_tmux_t *manager;
static const char *exe, *socket_path;
static gt_ssh_t *management_ssh, *attach_owners[16];
static size_t attach_count;
static const char *ssh_host, *ssh_user, *ssh_key, *ssh_work;
static uint16_t ssh_port;
static gt_hostkey_decision_t accept_host(void *u, const char *h, uint16_t p,
                                         const char *t, const char *f) {
  (void)u;
  (void)h;
  (void)p;
  (void)t;
  (void)f;
  return GT_HOSTKEY_ACCEPT;
}
static gt_ssh_t *connect_ssh(void) {
  FILE *f = fopen(ssh_key, "rb");
  CHECK(f, "fixture private key");
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  char *pem = calloc(1, (size_t)len + 1);
  CHECK(pem && fread(pem, 1, (size_t)len, f) == (size_t)len,
        "read fixture key");
  fclose(f);
  char kh[4096];
  snprintf(kh, sizeof kh, "%s/tmux-known-hosts", ssh_work);
  gt_ssh_opts_t opts = {.host = ssh_host,
                        .port = ssh_port,
                        .username = ssh_user,
                        .known_hosts_path = kh,
                        .connect_timeout_ms = 5000};
  gt_ssh_t *ssh = gt_ssh_new(&opts);
  CHECK(ssh, "new SSH");
  gt_ssh_set_hostkey_policy(ssh, accept_host, NULL);
  CHECK(gt_ssh_auth_private_key(ssh, pem, NULL) == GT_OK,
        "unlocked fixture key");
  memset(pem, 0, (size_t)len);
  free(pem);
  CHECK(gt_ssh_connect(ssh) == GT_OK, "authenticate real SSH");
  return ssh;
}
static gt_status_t attach_status;
static gt_transport_t *attach(const gt_tmux_session_t *row,
                              const gt_shell_opts_t *size) {
  gt_ssh_t *ssh = NULL;
  if (management_ssh) {
    CHECK(attach_count < 16, "attach owner capacity");
    ssh = connect_ssh();
    attach_owners[attach_count++] = ssh;
  }
  gt_transport_t *transport = gt_tmux_attach(manager, row, ssh, size, &attach_status);
  if (transport) {
    /* A real surface answers DA queries. This byte-only fixture lets tmux's
     * capability discovery settle while draining its initial screen. */
    int64_t until = gt__command_now() + 1200;
    uint8_t bytes[8192];
    while (gt__command_now() < until) {
      int n = gt_transport_read(transport, bytes, sizeof bytes, 20);
      if (n == 0 || n == -1)
        break;
    }
  }
  return transport;
}
static void list(void) {
  CHECK(gt_tmux_list(manager, rows, GT_TMUX_SESSION_MAX, &count, &server) ==
            GT_OK,
        "fresh list");
}
static void change(gt_tmux_operation_t op, const gt_tmux_session_t *row,
                   const char *name) {
  gt_tmux_result_t result = GT_TMUX_UNKNOWN;
  int rc = gt_tmux_change(manager, op, row, name, &result);
  CHECK(rc == GT_OK && result == GT_TMUX_CONFIRMED, "confirmed change");
}
static void cli(const char *a, const char *b, const char *c, const char *d) {
  const char *args[] = {exe, "-u", "-S", socket_path, a, b, c, d, NULL};
  gt_command_budget_t budget = {gt__command_now() + 5000, NULL, NULL};
  gt_command_result_t r = {0};
  int rc;
  if (management_ssh) {
    char *command = NULL;
    size_t n = 0;
    for (size_t i = 0; args[i]; i++) {
      char *q = gt__command_quote(args[i]);
      CHECK(q, "quote CLI");
      CHECK(gt__command_append(&command, &n, " ", 1) == 0 &&
                gt__command_append(&command, &n, q, strlen(q)) == 0,
            "CLI buffer");
      free(q);
    }
    rc = gt__command_ssh(management_ssh, command, &budget, &r);
    free(command);
  } else
    rc = gt__command_local(exe, args, &budget, &r);
  CHECK(rc == GT_OK && r.completed && !r.exit_code, "independent tmux CLI");
  gt__command_free(&r);
}
static bool read_until(gt_transport_t *t, const char *needle) {
  char output[131072] = {0};
  size_t n = 0;
  int64_t deadline = gt__command_now() + 5000;
  while (gt__command_now() < deadline && n + 1 < sizeof output) {
    int got =
        gt_transport_read(t, (uint8_t *)output + n, sizeof output - n - 1, 20);
    if (got > 0) {
      n += (size_t)got;
      output[n] = 0;
      if (strstr(output, needle))
        return true;
    } else if (got != -2)
      break;
  }
  fprintf(stderr, "OUTPUT: %.10000s\n", output);
  return false;
}
static void send(gt_transport_t *t, const char *s) {
  CHECK(gt_transport_write(t, (const uint8_t *)s, strlen(s)) == 0,
        "transport input");
}
static void command_tests(void) {
  char *quoted = gt__command_quote("a'$(touch nope);\n");
  CHECK(quoted && !strcmp(quoted, "'a'\\''$(touch nope);\n'"),
        "POSIX single quoting");
  free(quoted);
  gt_command_budget_t b = {gt__command_now() + 2000, NULL, NULL};
  gt_command_result_t r = {0};
  const char *args[] = {"/bin/sh", "-c",
                        "printf output; printf error >&2; exit 7", NULL};
  CHECK(gt__command_local(args[0], args, &b, &r) == 0 && r.completed &&
            r.exit_code == 7 && !strcmp(r.out, "output") &&
            !strcmp(r.err, "error"),
        "separate output/status");
  gt__command_free(&r);
  pid_t child = fork();
  CHECK(child >= 0, "closed-stdio fixture fork");
  if (child == 0) {
    close(0);
    close(1);
    close(2);
    gt_command_result_t captured = {0};
    gt_command_budget_t limit = {gt__command_now() + 2000, NULL, NULL};
    int status = gt__command_local(args[0], args, &limit, &captured);
    bool ok = status == GT_OK && captured.completed &&
              captured.exit_code == 7 && captured.out && captured.err &&
              !strcmp(captured.out, "output") && !strcmp(captured.err, "error");
    gt__command_free(&captured);
    _exit(ok ? 0 : 1);
  }
  int child_status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  CHECK(waited == child && WIFEXITED(child_status) &&
            WEXITSTATUS(child_status) == 0,
        "command captures output even with host stdio closed");
  child = fork();
  CHECK(child >= 0, "unknown-exit fixture fork");
  if (child == 0) {
    struct sigaction ignored = {.sa_handler = SIG_IGN,
                                .sa_flags = SA_NOCLDWAIT};
    sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGCHLD, &ignored, NULL))
      _exit(2);
    const char *arguments[] = {"-c", "exit 7", NULL};
    gt_pty_opts_t options = {.executable = "/bin/sh",
                             .argv = arguments,
                             .cmd_kind = GT_PTY_CMD_DIRECT};
    gt_transport_t *transport = gt_transport_open_pty(&options);
    if (!transport)
      _exit(2);
    uint8_t buffer[128];
    int status = -2;
    int64_t until = gt__command_now() + 5000;
    while (status != 0 && status != -1 && gt__command_now() < until)
      status = gt_transport_read(transport, buffer, sizeof buffer, 20);
    while (gt_transport_child_state(transport) != GT_TRANSPORT_CHILD_EXITED &&
           gt__command_now() < until)
      nanosleep(&(struct timespec){0, 1000000}, NULL);
    bool observed =
        gt_transport_child_state(transport) == GT_TRANSPORT_CHILD_EXITED;
    int exit_code = 123;
    bool known = gt_transport_exited(transport, &exit_code);
    gt_transport_close(transport);
    _exit(status == 0 && observed && !known ? 0 : 1);
  }
  do {
    waited = waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  CHECK(waited == child && WIFEXITED(child_status) &&
            WEXITSTATUS(child_status) == 0,
        "PTY without wait status reports unknown exit");
  const char *sleep[] = {"/bin/sh", "-c", "sleep 30", NULL};
  b.deadline = gt__command_now() + 150;
  CHECK(gt__command_local(sleep[0], sleep, &b, &r) == GT_ERR_CANCELLED &&
            !r.completed && r.started,
        "timeout unknown");
  gt__command_free(&r);
  const char *missing[] = {"/ghosterm-missing-executable", NULL};
  b.deadline = gt__command_now() + 1000;
  CHECK(gt__command_local(missing[0], missing, &b, &r) == GT_ERR_NOT_FOUND &&
            !r.started,
        "missing distinct");
  gt__command_free(&r);
  const char *overflow[] = {"/bin/sh", "-c", "yes x", NULL};
  b.deadline = gt__command_now() + 5000;
  CHECK(gt__command_local(overflow[0], overflow, &b, &r) == GT_ERR_PROTOCOL &&
            !r.completed,
        "bounded output");
  gt__command_free(&r);
  gt_tmux_opts_t bad = {.executable = "relative"};
  CHECK(!gt_tmux_open(NULL, &bad, NULL), "absolute paths required");
  gt_tmux_result_t result = GT_TMUX_CONFIRMED;
  CHECK(gt_tmux_change(NULL, GT_TMUX_TERMINATE, NULL, NULL, &result) ==
                GT_ERR_INVALID_ARG &&
            result == GT_TMUX_REJECTED,
        "invalid != success");
  puts("PASS tmux command");
}
static bool cancel_all(void *context) {
  (void)context;
  return true;
}
static void adversarial(void) {
  const char *invalid[] = {"permission", "numeric",   "duplicate",
                           "utf8",       "truncated", "nul"};
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
    setenv("GT_TMUX_FAKE_MODE", invalid[i], 1);
    count = 123;
    int rc = gt_tmux_list(manager, rows, GT_TMUX_SESSION_MAX, &count, &server);
    if (getenv("GT_TMUX_TRACE"))
      fprintf(stderr, "case %s rc=%d count=%zu error=%s\n", invalid[i], rc,
              count, gt_last_error());
    CHECK(rc == GT_ERR_PROTOCOL && count == 0,
          "bad output is a protocol error, never empty");
  }
  setenv("GT_TMUX_FAKE_MODE", "absent", 1);
  list();
  CHECK(!count && server == GT_TMUX_SERVER_ABSENT,
        "verified absent diagnostic");
  setenv("GT_TMUX_FAKE_MODE", "empty", 1);
  list();
  CHECK(!count && server == GT_TMUX_SERVER_RUNNING,
        "successful empty distinct");
  setenv("GT_TMUX_FAKE_MODE", "policy", 1);
  list();
  CHECK(count == 1 && rows[0].destroy_unattached == -1 &&
            rows[0].exit_unattached == -1,
        "unrecognized policy remains unknown");
  gt_tmux_result_t result;
  setenv("GT_TMUX_FAKE_MODE", "timeout", 1);
  CHECK(gt_tmux_change(manager, GT_TMUX_CREATE, NULL, "new", &result) !=
                GT_OK &&
            result == GT_TMUX_UNKNOWN,
        "lost mutation reply is unknown");
  CHECK(access(getenv("GT_TMUX_FAKE_MARKER"), F_OK) == 0,
        "timeout occurred after command started");
  setenv("GT_TMUX_FAKE_MODE", "dirty", 1);
  CHECK(gt_tmux_change(manager, GT_TMUX_CREATE, NULL, "new", &result) !=
                GT_OK &&
            result == GT_TMUX_UNKNOWN,
        "successful dirty reply not rejected");
  setenv("GT_TMUX_FAKE_MODE", "rejected", 1);
  CHECK(gt_tmux_change(manager, GT_TMUX_CREATE, NULL, "new", &result) !=
                GT_OK &&
            result == GT_TMUX_REJECTED,
        "confirmed failure distinct");
  setenv("GT_TMUX_FAKE_MODE", "old", 1);
  gt_tmux_opts_t opts = {.executable = exe, .socket_path = socket_path};
  gt_status_t status = GT_OK;
  CHECK(!gt_tmux_open(NULL, &opts, &status) && status == GT_ERR_UNSUPPORTED,
        "unsupported tmux version refused with typed capability error");
  setenv("GT_TMUX_FAKE_MODE", "version-dirty", 1);
  CHECK(!gt_tmux_open(NULL, &opts, &status) && status == GT_ERR_PROTOCOL,
        "dirty version output is not an unsupported version");
  gt_tmux_opts_t discovery = {.login_shell = exe};
  setenv("GT_TMUX_FAKE_MODE", "discovery-missing", 1);
  CHECK(!gt_tmux_open(NULL, &discovery, &status) && status == GT_ERR_NOT_FOUND,
        "completed missing discovery has typed not-found status");
  setenv("GT_TMUX_FAKE_MODE", "discovery-dirty", 1);
  CHECK(!gt_tmux_open(NULL, &discovery, &status) && status == GT_ERR_PROTOCOL,
        "startup pollution cannot claim tmux is missing");
  discovery.login_shell = "/ghosterm-missing-login-shell";
  CHECK(!gt_tmux_open(NULL, &discovery, &status) && status == GT_ERR_IO,
        "missing login shell cannot claim tmux is missing");
  setenv("GT_TMUX_FAKE_MODE", "good", 1);
  opts.cancelled = cancel_all;
  CHECK(!gt_tmux_open(NULL, &opts, NULL) && strstr(gt_last_error(), "cancelled"),
        "explicit cancellation stops discovery with current error");
  gt_tmux_close(manager);
  puts("PASS tmux adversarial");
}
static void proxy_fault(const char *mode) {
  char path[4096], ack[4096];
  snprintf(ack, sizeof ack, "%s/fault-done", ssh_work);
  unlink(ack);
  snprintf(path, sizeof path, "%s/fault-%s", ssh_work, mode);
  FILE *f = fopen(path, "wb");
  CHECK(f, "request owned proxy fault");
  fclose(f);
  int64_t deadline = gt__command_now() + 5000;
  while (access(ack, F_OK) != 0 && gt__command_now() < deadline)
    nanosleep(&(struct timespec){0, 10000000}, NULL);
  f = fopen(ack, "rb");
  CHECK(f, "proxy confirms actual fault");
  char applied[32] = {0};
  (void)fread(applied, 1, sizeof applied - 1, f);
  fclose(f);
  CHECK(!strcmp(applied, mode), "acknowledgement matches requested fault");
}
static atomic_bool query_cancel;
static bool query_cancelled(void *context) {
  return atomic_load((atomic_bool *)context);
}
static void *cancel_query(void *context) {
  nanosleep(&(struct timespec){0, 200000000}, NULL);
  atomic_store((atomic_bool *)context, true);
  return NULL;
}
static long live_pid(gt_transport_t *transport) {
  char output[65536] = {0};
  size_t used = 0;
  int64_t deadline = gt__command_now() + 5000;
  while (gt__command_now() < deadline) {
    if (used + 8192 >= sizeof output) {
      memmove(output, output + used - 256, 256);
      used = 256;
    }
    int n = gt_transport_read(transport, (uint8_t *)output + used,
                              sizeof output - used - 1, 20);
    if (n == 0 || n == -1)
      break;
    if (n > 0) {
      used += (size_t)n;
      output[used] = 0;
      for (char *p = output; (p = strstr(p, "LOOP_")); p++) {
        long pid = 0;
        int end = 0;
        if (sscanf(p, "LOOP_%ld_END%n", &pid, &end) == 1 && end > 0 && pid > 0)
          return pid;
      }
    }
  }
  fprintf(stderr, "OUTPUT: %.8000s\n", output);
  return 0;
}
static void reconnect_manager(const gt_tmux_opts_t *options) {
  gt_tmux_close(manager);
  gt_ssh_close(management_ssh);
  management_ssh = connect_ssh();
  manager = gt_tmux_open(management_ssh, options, NULL);
  CHECK(manager, "explicit reauthenticated management");
  list();
}
static void owner_exit(const gt_tmux_opts_t *options) {
  gt_tmux_session_t before = rows[0];
  int report[2];
  CHECK(pipe(report) == 0, "owner-report pipe");
  for (int i = 0; i < 2; i++)
    CHECK(fcntl(report[i], F_SETFD, FD_CLOEXEC) == 0, "owner-report CLOEXEC");
  pid_t child = fork();
  CHECK(child >= 0, "fork isolated client owner");
  if (child == 0) {
    close(report[0]);
    if (management_ssh) {
      /* Never use the parent's copied SSH state. Closing only this process's
       * descriptor cannot send a disconnect on the parent's connection. */
      void *session = NULL;
      int socket = -1;
      if (gt__ssh_borrow(management_ssh, &session, &socket) != GT_OK)
        _exit(2);
      close(socket);
      management_ssh = connect_ssh();
      manager = gt_tmux_open(management_ssh, options, NULL);
      if (!manager)
        _exit(2);
    }
    gt_transport_t *transport = attach(&before, NULL);
    if (!transport)
      _exit(2);
    send(transport,
         "while :; do printf 'LOOP_%s_END\\n' \"$$\"; sleep 0.1; done\r");
    int64_t pid = live_pid(transport);
    if (pid <= 0)
      _exit(2);
    if (write(report[1], &pid, sizeof pid) != (ssize_t)sizeof pid)
      _exit(2);
    close(report[1]);
    for (;;) {
      uint8_t bytes[8192];
      if (gt_transport_read(transport, bytes, sizeof bytes, 20) == -1)
        _exit(2);
    }
  }
  close(report[1]);
  struct pollfd ready = {report[0], POLLIN, 0};
  int result;
  do {
    result = poll(&ready, 1, 15000);
  } while (result < 0 && errno == EINTR);
  int64_t task = 0;
  ssize_t got = result > 0 ? read(report[0], &task, sizeof task) : -1;
  close(report[0]);
  CHECK(kill(child, SIGKILL) == 0, "kill actual client owner process");
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  CHECK(got == (ssize_t)sizeof task && task > 0,
        "client and actual inner task were running before owner death");
  CHECK(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "owner really died without cleanup/destructors");
  int64_t deadline = gt__command_now() + 5000;
  do {
    list();
    if (count == 1 && rows[0].attached == 0)
      break;
  } while (gt__command_now() < deadline);
  CHECK(count == 1 && rows[0].attached == 0 &&
            rows[0].server_pid == before.server_pid && rows[0].id == before.id,
        "owner death detaches client and leaves same session");
  gt_transport_t *transport = attach(&rows[0], NULL);
  CHECK(transport, "rejoin after owner process death");
  CHECK(live_pid(transport) == task,
        "task keeps producing output after owner process death");
  send(transport, "\003");
  gt_transport_close(transport);
}

static void network(const char *mode, const gt_tmux_opts_t *options) {
  CHECK(management_ssh, "network fixture requires SSH");
  gt_tmux_session_t before = rows[0];
  if (!strcmp(mode, "query-timeout") || !strcmp(mode, "query-cancel")) {
    int64_t calibration = gt__command_now();
    for (int i = 0; i < 20; i++)
      nanosleep(&(struct timespec){0, 10000000}, NULL);
    calibration = gt__command_now() - calibration;
    CHECK(calibration < 2500, "query cancellation fixture scheduling budget");
    proxy_fault("blackhole");
    bool cancel = !strcmp(mode, "query-cancel");
    pthread_t worker;
    int64_t started = gt__command_now();
    if (cancel)
      CHECK(pthread_create(&worker, NULL, cancel_query, &query_cancel) == 0,
            "cancellation worker");
    printf("BEGIN tmux %s\n", mode);
    fflush(stdout);
    size_t n = 123;
    gt_tmux_server_t state;
    int rc = gt_tmux_list(manager, rows, GT_TMUX_SESSION_MAX, &n, &state);
    int64_t elapsed = gt__command_now() - started;
    if (cancel)
      pthread_join(worker, NULL);
    CHECK(rc == GT_ERR_CANCELLED && n == 0,
          "blackholed management query has bounded deadline/cancellation");
    if (cancel) {
      int64_t bound = calibration * 3 / 2 + 250;
      if (bound < 1000)
        bound = 1000;
      CHECK(elapsed < bound,
            "cancel does not wait for the five-second command deadline");
    }
    atomic_store(&query_cancel, false);
    reconnect_manager(options);
    CHECK(count == 1 && rows[0].id == before.id &&
              rows[0].server_pid == before.server_pid,
          "query cancellation does not end session");
    return;
  }

  if (!strcmp(mode, "backpressure")) {
    gt_transport_t *transport = attach(&before, NULL);
    CHECK(transport, "attach for backpressure drain");
    send(transport, "stty -echo -icanon; python3 -c 'import sys,hashlib; "
                    "print(\"drain-\"+\"ready\",flush=True); "
                    "data=sys.stdin.buffer.read(3145728); "
                    "print(\"DRAIN_\"+hashlib.sha256(data).hexdigest(),flush="
                    "True)'; stty sane\r");
    CHECK(read_until(transport, "drain-ready"), "receiver ready before fault");
    proxy_fault("blackhole");
    uint8_t *payload = malloc(3 * 1024 * 1024);
    CHECK(payload, "drain payload");
    memset(payload, 'x', 3 * 1024 * 1024);
    CHECK(gt_transport_write(transport, payload, 3 * 1024 * 1024) == GT_OK,
          "backpressure accepts input");
    free(payload);
    proxy_fault("resume");
    CHECK(
        read_until(
            transport,
            "DRAIN_"
            "3bea8a9a07c1e8dcaa4c1b816815c35a29b4fb585ba6ecc70ea44840a794cfb3"),
        "queued SSH tail drains without losing or reordering bytes");
    gt_transport_close(transport);
    return;
  }

  if (!strcmp(mode, "lost-create") || !strcmp(mode, "lost-terminate")) {
    if (!strcmp(mode, "lost-terminate")) {
      change(GT_TMUX_CREATE, NULL, "survivor");
      list();
    }
    gt_tmux_result_t result = GT_TMUX_CONFIRMED;
    int rc = gt_tmux_change(
        manager,
        !strcmp(mode, "lost-create") ? GT_TMUX_CREATE : GT_TMUX_TERMINATE,
        !strcmp(mode, "lost-create") ? NULL : &before, "lost-reply", &result);
    CHECK(rc != GT_OK && result == GT_TMUX_UNKNOWN,
          "lost remote mutation reply stays unknown");
    reconnect_manager(options);
    CHECK(count == (!strcmp(mode, "lost-create") ? 2u : 1u),
          "fresh list observes exactly one mutation, no replay");
    CHECK(!strcmp(mode, "lost-create") || !strcmp(rows[0].name, "survivor"),
          "other session survives lost terminate reply");
    return;
  }
  gt_transport_t *transport = attach(&before, NULL);
  CHECK(transport, "attach before network fault");
  send(transport,
       "while :; do printf 'LOOP_%s_END\\n' \"$$\"; sleep 0.1; done\r");
  long pid = live_pid(transport);
  CHECK(pid > 0, "actual running task before fault");
  proxy_fault(!strcmp(mode, "reset") ? "reset" : "blackhole");
  uint8_t bytes[8192];
  int status = -2;
  if (!strcmp(mode, "reset")) {
    int64_t deadline = gt__command_now() + 5000;
    while (status != 0 && status != -1 && gt__command_now() < deadline)
      status = gt_transport_read(transport, bytes, sizeof bytes, 20);
    CHECK(status == 0 || status == -1, "reset detected as disconnected");
  } else {
    /* Drain any bytes already accepted before the blackhole was acknowledged.
     */
    do {
      status = gt_transport_read(transport, bytes, sizeof bytes, 20);
    } while (status > 0);
    CHECK(status == GT_TRANSPORT_READ_TIMEOUT,
          "silent loss remains unknown, not EOF");
    uint8_t *payload = malloc(3 * 1024 * 1024);
    CHECK(payload, "backpressure input");
    memset(payload, 'x', 3 * 1024 * 1024);
    CHECK(gt_transport_write(transport, payload, 3 * 1024 * 1024) == GT_OK,
          "blackhole write bounded and accepted into tail");
    free(payload);
  }
  int code = 123;
  CHECK(!gt_transport_exited(transport, &code),
        "lost SSH exit status never invented as zero");
  gt_transport_close(transport);
  reconnect_manager(options);
  CHECK(count == 1 && rows[0].server_pid == before.server_pid &&
            rows[0].server_started == before.server_started &&
            rows[0].id == before.id && rows[0].created == before.created,
        "same live session after network loss");
  transport = attach(&rows[0], NULL);
  CHECK(transport, "explicit attach after fresh authentication and list");
  CHECK(live_pid(transport) == pid,
        "same task continues producing output after reconnect");
  send(transport, "\003");
  gt_transport_close(transport);
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "command")) {
    command_tests();
    return 0;
  }
  CHECK(argc == 4 || argc == 9,
        "usage test_tmux MODE EXE SOCKET [HOST PORT USER KEY WORK]");
  if (argc == 9) {
    ssh_host = argv[4];
    ssh_port = (uint16_t)atoi(argv[5]);
    ssh_user = argv[6];
    ssh_key = argv[7];
    ssh_work = argv[8];
    management_ssh = connect_ssh();
  }
  exe = argv[2];
  socket_path = argv[3];
  gt_tmux_opts_t opts = {.executable = exe, .socket_path = socket_path};
  opts.timeout_ms = 5000;
  atomic_init(&query_cancel, false);
  opts.cancelled = query_cancelled;
  opts.context = &query_cancel;
  manager = gt_tmux_open(management_ssh, &opts, NULL);
  if (!strcmp(argv[1], "exec-denied") || !strcmp(argv[1], "sftp-only")) {
    CHECK(!manager, "restricted account cannot pretend tmux is supported");
    if (!strcmp(argv[1], "sftp-only")) {
      gt_ssh_close(management_ssh);
      management_ssh = connect_ssh();
      gt_sftp_t *sftp = gt_ssh_sftp(management_ssh);
      CHECK(sftp, "SFTP-only account really supports SFTP");
      gt_sftp_close(sftp);
    }
    gt_ssh_close(management_ssh);
    printf("PASS tmux %s\n", argv[1]);
    return 0;
  }
  CHECK(manager, "probe supported tmux");
  if (!strcmp(argv[1], "adversarial")) {
    adversarial();
    return 0;
  }
  list();
  CHECK(server == GT_TMUX_SERVER_ABSENT && count == 0,
        "no server does not start");
  change(GT_TMUX_CREATE, NULL, "first");
  list();
  CHECK(count == 1 && rows[0].windows == 1 && rows[0].attached == 0,
        "create/list real fields");
  if (!strcmp(argv[1], "owner-kill")) {
    owner_exit(&opts);
  } else if (!strcmp(argv[1], "empty")) {
    cli("set-option", "-s", "exit-empty", "off");
    change(GT_TMUX_TERMINATE, &rows[0], NULL);
    list();
    CHECK(count == 0 && server == GT_TMUX_SERVER_RUNNING,
          "real empty server is distinct from absent server");
    change(GT_TMUX_CREATE, NULL, "after-empty");
    list();
    CHECK(count == 1, "explicit creation on an existing empty server");
  } else if (!strcmp(argv[1], "reset") || !strcmp(argv[1], "blackhole") ||
             !strcmp(argv[1], "backpressure") ||
             !strncmp(argv[1], "query-", 6) || !strncmp(argv[1], "lost-", 5)) {
    network(argv[1], &opts);
  } else if (!strcmp(argv[1], "pty-denied")) {
    CHECK(management_ssh && !attach(&rows[0], NULL),
          "PTY denial is explicit attach failure");
    CHECK(attach_status == GT_ERR_UNSUPPORTED && strstr(gt_last_error(), "refused PTY"),
          "PTY failure reports missing capability");
    list();
    CHECK(count == 1, "management owner unaffected by attach PTY denial");
  } else if (!strcmp(argv[1], "race")) {
    gt_transport_t *t = attach(&rows[0], NULL);
    CHECK(t, "attach client launch");
    int64_t deadline = gt__command_now() + 3000;
    int status = -2;
    uint8_t bytes[8192];
    while (gt__command_now() < deadline && status != 0 && status != -1)
      status = gt_transport_read(t, bytes, sizeof bytes, 20);
    CHECK(status == 0, "missing selected server exits client");
    gt_transport_close(t);
    list();
    CHECK(server == GT_TMUX_SERVER_ABSENT && count == 0,
          "attach -N never starts replacement server");
  } else if (!strcmp(argv[1], "names")) {
    const char *names[] = {"中文 space", "#{pid}",     ";",
                           "tail;",      "-option",    "quote'\\$(touch NEVER)",
                           "tab\tname",  "line\nname", "a.b:c"};
    const char *expected[] = {
        "中文 space", "#{pid}",      ";",
        "tail;",      "-option",     "quote'\\\\$(touch NEVER)",
        "tab\\tname", "line\\nname", "a_b_c"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
      change(GT_TMUX_RENAME, &rows[0], names[i]);
      list();
      CHECK(count == 1 && !strcmp(rows[0].name, expected[i]),
            "literal/normalized name");
    }
  } else if (!strcmp(argv[1], "identity")) {
    gt_tmux_session_t stale = rows[0];
    change(GT_TMUX_TERMINATE, &rows[0], NULL);
    change(GT_TMUX_CREATE, NULL, "first");
    list();
    gt_tmux_result_t result;
    CHECK(gt_tmux_change(manager, GT_TMUX_TERMINATE, &stale, NULL, &result) ==
                  GT_ERR_NOT_FOUND &&
              result == GT_TMUX_REJECTED,
          "stale identity refused");
    CHECK(!attach(&stale, NULL), "stale attach never creates");
    list();
    CHECK(count == 1, "replacement remains");
    stale = rows[0];
    cli("rename-session", "-t", rows[0].name, "externally-renamed");
    CHECK(gt_tmux_change(manager, GT_TMUX_RENAME, &stale, "wrong", &result) ==
              GT_ERR_NOT_FOUND,
          "external rename invalidates selection");
    list();
    stale = rows[0];
    stale.server_started++;
    CHECK(!attach(&stale, NULL), "server epoch revalidated");
    stale = rows[0];
    stale.socket_path[1] = '!';
    CHECK(!attach(&stale, NULL), "endpoint revalidated");
  } else if (!strcmp(argv[1], "policy")) {
    CHECK(rows[0].destroy_unattached == 0 && rows[0].exit_unattached == 0,
          "inherited retention policy");
    gt_tmux_session_t stale = rows[0];
    gt_transport_t *t = attach(&rows[0], NULL);
    CHECK(t, "attach before policy change");
    send(t, "printf 'client-%s\\n' ready\r");
    CHECK(read_until(t, "client-ready"), "client ready");
    cli("set-option", "-g", "destroy-unattached", "on");
    list();
    CHECK(count == 1 && rows[0].destroy_unattached == 1,
          "dangerous policy visible");
    CHECK(!attach(&stale, NULL), "policy changed invalidates selection");
    gt_transport_close(t);
    nanosleep(&(struct timespec){0, 200000000}, NULL);
    list();
    CHECK(server == GT_TMUX_SERVER_ABSENT || count == 0,
          "destroy-unattached really ends task");
  } else if (!strcmp(argv[1], "attach")) {
    gt_tmux_session_t first = rows[0];
    gt_shell_opts_t size = {.cols = 90, .rows = 30};
    gt_transport_t *t = attach(&first, &size);
    CHECK(t, "real PTY attach");
    send(t, "printf 'hello-%s\\n' tmux\r");
    CHECK(read_until(t, "hello-tmux"), "real input and output");
    CHECK(gt_transport_resize(t, 101, 37, 0, 0) == 0, "resize accepted");
    send(t,
         "for i in 1 2 3 4 5 6 7 8; do if [ \"$(stty size)\" = \"37 101\" ]; "
         "then printf 'resize-%s\\n' reached; break; fi; sleep 0.3; done\r");
    CHECK(read_until(t, "resize-reached"), "resize reaches inner PTY");
    /* Ordinary prefix creates an inner window; there is still one transport. */
    send(t, "\002c");
    send(t, "printf 'window-%s\\n' two\r");
    CHECK(read_until(t, "window-two"), "prefix key/new window");
    list();
    CHECK(rows[0].windows == 2, "tmux inner windows remain one session");
    gt_transport_t *external = attach(&rows[0], &size);
    CHECK(external, "second client coexists");
    list();
    CHECK(rows[0].attached == 2, "no -d client eviction");
    gt_transport_close(t);
    list();
    CHECK(count == 1 && rows[0].attached == 1,
          "close only client; external survives");
    send(external, "printf 'external-%s\\n' alive\r");
    CHECK(read_until(external, "external-alive"), "external client unaffected");
    send(external, "\002d");
    CHECK(read_until(external, "detached"), "prefix detach");
    gt_transport_close(external);
    list();
    CHECK(count == 1 && rows[0].attached == 0, "detached session remains");
    t = attach(&rows[0], &size);
    CHECK(t, "fresh explicit reconnect");
    send(t, "printf 'rejoin-%s\\n' ok\r");
    CHECK(read_until(t, "rejoin-ok"), "rejoin output");
    gt_transport_close(t);
  } else
    CHECK(!strcmp(argv[1], "lifecycle"), "known mode");
  list();
  while (count) {
    change(GT_TMUX_TERMINATE, &rows[0], NULL);
    list();
  }
  gt_tmux_close(manager);
  for (size_t i = 0; i < attach_count; i++)
    gt_ssh_close(attach_owners[i]);
  gt_ssh_close(management_ssh);
  printf("PASS tmux %s\n", argv[1]);
  return 0;
}
