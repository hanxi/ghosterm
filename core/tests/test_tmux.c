#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include "tmux/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static gt_transport_t *attach(const gt_tmux_session_t *row,
                              const gt_shell_opts_t *size) {
  gt_ssh_t *ssh = NULL;
  if (management_ssh) {
    CHECK(attach_count < 16, "attach owner capacity");
    ssh = connect_ssh();
    attach_owners[attach_count++] = ssh;
  }
  gt_transport_t *transport = gt_tmux_attach(manager, row, ssh, size);
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
  gt_command_budget_t b = {gt__command_now() + 150, NULL, NULL};
  gt_command_result_t r = {0};
  const char *args[] = {"/bin/sh", "-c",
                        "printf output; printf error >&2; exit 7", NULL};
  CHECK(gt__command_local(args[0], args, &b, &r) == 0 && r.completed &&
            r.exit_code == 7 && !strcmp(r.out, "output") &&
            !strcmp(r.err, "error"),
        "separate output/status");
  gt__command_free(&r);
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
  CHECK(!gt_tmux_open(NULL, &bad), "absolute paths required");
  gt_tmux_result_t result = GT_TMUX_CONFIRMED;
  CHECK(gt_tmux_change(NULL, GT_TMUX_TERMINATE, NULL, NULL, &result) ==
                GT_ERR_INVALID_ARG &&
            result == GT_TMUX_REJECTED,
        "invalid != success");
  puts("PASS tmux command");
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
  CHECK(!gt_tmux_open(NULL, &opts), "unsupported tmux version refused");
  gt_tmux_close(manager);
  puts("PASS tmux adversarial");
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
  manager = gt_tmux_open(management_ssh, &opts);
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
  if (!strcmp(argv[1], "race")) {
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
