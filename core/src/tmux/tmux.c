#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include "internal.h"
#include "tmux/command.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct gt_tmux {
  gt_ssh_t *ssh;
  char exe[GT_TMUX_PATH_MAX], socket[GT_TMUX_PATH_MAX];
  uint32_t timeout;
  bool (*cancelled)(void *);
  void *context;
};
static const char *format =
    "#{pid}\t#{start_time}\t#{session_id}\t#{session_created}\t#{session_"
    "windows}\t#{session_attached}\t#{session_name}";
static gt_command_budget_t budget(gt_tmux_t *t) {
  return (gt_command_budget_t){gt__command_now() + t->timeout, t->cancelled,
                               t->context};
}
static bool text(const char *s, size_t cap, bool path) {
  if (!s || !*s || strlen(s) >= cap || (path && s[0] != '/'))
    return false;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    if (*p < 32 || *p == 127)
      return false;
    if (*p < 128) {
      p++;
      continue;
    }
    unsigned n;
    uint32_t c;
    if (*p >= 0xc2 && *p <= 0xdf) {
      n = 1;
      c = *p & 31;
    } else if (*p >= 0xe0 && *p <= 0xef) {
      n = 2;
      c = *p & 15;
    } else if (*p >= 0xf0 && *p <= 0xf4) {
      n = 3;
      c = *p & 7;
    } else
      return false;
    unsigned count = n;
    p++;
    while (n--) {
      if ((*p & 0xc0) != 0x80)
        return false;
      c = (c << 6) | (*p++ & 63);
    }
    if ((count == 2 && c < 0x800) || (count == 3 && c < 0x10000) ||
        c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return false;
  }
  return true;
}
static bool input_name(const char *s) {
  if (!s || strlen(s) >= GT_TMUX_NAME_MAX / 2)
    return false;
  char copy[GT_TMUX_NAME_MAX];
  strcpy(copy, s);
  for (char *p = copy; *p; p++)
    if (*p == '\n' || *p == '\t' || *p == '\r')
      *p = ' ';
  return text(copy, sizeof copy, false);
}
static int append_word(char **s, size_t *len, const char *word) {
  char *q = gt__command_quote(word);
  if (!q)
    return GT_ERR_INTERNAL;
  int rc = gt__command_append(s, len, " ", 1);
  if (!rc)
    rc = gt__command_append(s, len, q, strlen(q));
  free(q);
  return rc;
}
static char *shell_command(const char *const *argv) {
  char *s = NULL;
  size_t n = 0;
  /* -u preserves Unicode even when the login environment uses the C locale.
   * Unset only diagnostic locale override; preserve user config and socket env.
   */
  const char *prefix = "unset TMUX LC_ALL; LC_MESSAGES=C; export LC_MESSAGES;";
  if (gt__command_append(&s, &n, prefix, strlen(prefix)))
    return NULL;
  for (size_t i = 0; argv[i]; i++)
    if (append_word(&s, &n, argv[i])) {
      free(s);
      return NULL;
    }
  return s;
}
static int run_argv(gt_tmux_t *t, const char *const *args, bool start,
                    const gt_command_budget_t *b, gt_command_result_t *r) {
  const char *argv[32];
  size_t n = 0;
  argv[n++] = t->exe;
  argv[n++] = "-u";
  if (t->socket[0]) {
    argv[n++] = "-S";
    argv[n++] = t->socket;
  }
  if (!start)
    argv[n++] = "-N";
  for (size_t i = 0; args[i]; i++) {
    if (n >= 31)
      return GT_ERR_INVALID_ARG;
    argv[n++] = args[i];
  }
  argv[n] = NULL;
  if (!t->ssh)
    return gt__command_local(t->exe, argv, b, r);
  char *command = shell_command(argv);
  if (!command)
    return GT_ERR_INTERNAL;
  int rc = gt__command_ssh(t->ssh, command, b, r);
  free(command);
  return rc;
}
static int checked(gt_tmux_t *t, const char *const *args, bool start,
                   const gt_command_budget_t *b, gt_command_result_t *r) {
  int rc = run_argv(t, args, start, b, r);
  if (rc)
    return rc;
  if (!r->completed || r->exit_code || r->err_len) {
    gt__set_error("tmux: command failed (%d): %.400s", r->exit_code,
                  r->err ? r->err : "unexpected output");
    return GT_ERR_IO;
  }
  return GT_OK;
}
static bool line(char *s, size_t *n) {
  if (!s || !*n || s[*n - 1] != '\n')
    return false;
  s[--*n] = 0;
  return !strchr(s, '\n') && !strchr(s, '\r');
}
static bool supported(const char *s) {
  if (strncmp(s, "tmux ", 5))
    return false;
  unsigned major = 0, minor = 0;
  int end = 0;
  if (sscanf(s + 5, "%u.%u%n", &major, &minor, &end) != 2)
    return false;
  const char *suffix = s + 5 + end;
  if (*suffix && !(suffix[0] >= 'a' && suffix[0] <= 'z' && suffix[1] == 0))
    return false;
  return major > 3 ||
         (major == 3 && (minor > 6 || (minor == 6 && *suffix >= 'a')));
}
gt_tmux_t *gt_tmux_open(gt_ssh_t *ssh, const gt_tmux_opts_t *o) {
  static const gt_tmux_opts_t zero;
  if (!o)
    o = &zero;
  if ((o->executable && !text(o->executable, GT_TMUX_PATH_MAX, true)) ||
      (o->socket_path && !text(o->socket_path, GT_TMUX_PATH_MAX, true)) ||
      o->timeout_ms > 60000) {
    gt__set_error("tmux: invalid options");
    return NULL;
  }
  gt_tmux_t *t = calloc(1, sizeof *t);
  if (!t)
    return NULL;
  t->ssh = ssh;
  t->timeout = o->timeout_ms ? o->timeout_ms : 5000;
  t->cancelled = o->cancelled;
  t->context = o->context;
  if (o->socket_path)
    strcpy(t->socket, o->socket_path);
  gt_command_budget_t b = budget(t);
  gt_command_result_t r = {0};
  int rc;
  if (o->executable)
    strcpy(t->exe, o->executable);
  else {
    if (ssh)
      rc = gt__command_ssh(ssh, "unset TMUX; command -v tmux", &b, &r);
    else {
      const char *shell = o->login_shell ? o->login_shell : getenv("SHELL");
      if (!shell || !*shell)
        shell = "/bin/sh";
      if (!text(shell, GT_TMUX_PATH_MAX, true)) {
        gt__set_error("tmux: invalid login shell");
        goto failed;
      }
      const char *argv[] = {shell, "-l", "-c", "command -v tmux", NULL};
      rc = gt__command_local(shell, argv, &b, &r);
    }
    if (rc || !r.completed || r.exit_code || r.err_len ||
        !line(r.out, &r.out_len) || !text(r.out, sizeof t->exe, true)) {
      gt__set_error("tmux: executable missing or login-shell discovery failed; "
                    "specify an absolute path");
      goto failed;
    }
    strcpy(t->exe, r.out);
    gt__command_free(&r);
  }
  const char *v[] = {"-V", NULL};
  rc = checked(t, v, false, &b, &r);
  if (rc)
    goto failed;
  if (!line(r.out, &r.out_len) || !supported(r.out)) {
    gt__set_error("tmux: requires tmux >=3.6a with stable list formats");
    goto failed;
  }
  gt__command_free(&r);
  return t;
failed:
  gt__command_free(&r);
  free(t);
  return NULL;
}
void gt_tmux_close(gt_tmux_t *t) { free(t); }
static bool absent(gt_tmux_t *t, const gt_command_result_t *r) {
  if (!r->completed || r->exit_code != 1 || r->out_len || !r->err)
    return false;
  const char *begin = NULL, *end = NULL;
  const char *prefix = "error connecting to ";
  if (!strncmp(r->err, prefix, strlen(prefix))) {
    begin = r->err + strlen(prefix);
    end = strstr(begin, " (No such file or directory)\n");
    if (!end)
      end = strstr(begin, " (Connection refused)\n");
    if (!end || strchr(end, '\n')[1])
      return false;
  } else if (!strncmp(r->err, "no server running on ", 21)) {
    begin = r->err + 21;
    end = strchr(begin, '\n');
    if (!end || end[1])
      return false;
  } else
    return false;
  size_t n = (size_t)(end - begin);
  if (!n || n >= sizeof t->socket)
    return false;
  char path[GT_TMUX_PATH_MAX];
  memcpy(path, begin, n);
  path[n] = 0;
  if (!text(path, sizeof path, true) ||
      (t->socket[0] && strcmp(path, t->socket)))
    return false;
  strcpy(t->socket, path);
  return true;
}
static bool number(char **cursor, uint64_t *out, bool dollar) {
  char *p = *cursor;
  if (dollar && *p++ != '$')
    return false;
  if (*p < '0' || *p > '9')
    return false;
  errno = 0;
  char *end;
  unsigned long long n = strtoull(p, &end, 10);
  if (errno || *end != '\t')
    return false;
  *out = n;
  *cursor = end + 1;
  return true;
}
static int parse(gt_tmux_t *t, char *s, gt_tmux_session_t *rows, size_t cap,
                 size_t *count) {
  *count = 0;
  if (!s || !*s)
    return GT_OK;
  while (*s) {
    if (*count == cap)
      return GT_ERR_PROTOCOL;
    char *end = strchr(s, '\n');
    if (!end)
      return GT_ERR_PROTOCOL;
    *end = 0;
    gt_tmux_session_t *r = &rows[*count];
    memset(r, 0, sizeof *r);
    uint64_t windows, attached;
    if (!number(&s, &r->server_pid, false) ||
        !number(&s, &r->server_started, false) || !number(&s, &r->id, true) ||
        !number(&s, &r->created, false) || !number(&s, &windows, false) ||
        !number(&s, &attached, false) || !text(s, sizeof r->name, false) ||
        windows > UINT32_MAX || attached > UINT32_MAX || !r->server_pid ||
        !r->server_started || !r->created)
      return GT_ERR_PROTOCOL;
    for (size_t i = 0; i < *count; i++)
      if (rows[i].id == r->id || rows[i].server_pid != r->server_pid ||
          rows[i].server_started != r->server_started)
        return GT_ERR_PROTOCOL;
    strcpy(r->name, s);
    strcpy(r->socket_path, t->socket);
    r->windows = (uint32_t)windows;
    r->attached = (uint32_t)attached;
    r->destroy_unattached = -1;
    r->exit_unattached = -1;
    (*count)++;
    s = end + 1;
  }
  return GT_OK;
}
static int policy(gt_tmux_t *t, gt_tmux_session_t *r,
                  const gt_command_budget_t *b) {
  char id[32];
  snprintf(id, sizeof id, "$%" PRIu64, r->id);
  const char *destroy[] = {"show-options",       "-A", "-v", "-t", id,
                           "destroy-unattached", NULL};
  const char *exit[] = {"show-options", "-s", "-v", "exit-unattached", NULL};
  const char *values[] = {"off", "on", "keep-last", "keep-group"};
  for (int i = 0; i < 2; i++) {
    gt_command_result_t out = {0};
    int rc = checked(t, i ? exit : destroy, false, b, &out);
    if (!rc && line(out.out, &out.out_len))
      for (int j = 0; j < (i ? 2 : 4); j++)
        if (!strcmp(out.out, values[j])) {
          if (i)
            r->exit_unattached = j;
          else
            r->destroy_unattached = j;
        }
    gt__command_free(&out);
    if (rc)
      return rc;
  }
  return GT_OK;
}
static int list(gt_tmux_t *t, gt_tmux_session_t *rows, size_t cap,
                size_t *count, gt_tmux_server_t *server,
                const gt_command_budget_t *b) {
  *count = 0;
  *server = GT_TMUX_SERVER_RUNNING;
  gt_command_result_t r = {0};
  const char *endpoint[] = {"display-message", "-p", "#{socket_path}", NULL};
  int rc = run_argv(t, endpoint, false, b, &r);
  if (rc)
    goto done;
  if (absent(t, &r)) {
    *server = GT_TMUX_SERVER_ABSENT;
    rc = GT_OK;
    goto done;
  }
  if (r.exit_code || r.err_len || !line(r.out, &r.out_len) ||
      !text(r.out, sizeof t->socket, true) ||
      (t->socket[0] && strcmp(t->socket, r.out))) {
    gt__set_error("tmux: endpoint query failed: %.400s",
                  r.err ? r.err : "invalid socket format");
    rc = GT_ERR_PROTOCOL;
    goto done;
  }
  strcpy(t->socket, r.out);
  gt__command_free(&r);
  const char *args[] = {"list-sessions", "-F", format, NULL};
  rc = checked(t, args, false, b, &r);
  if (rc)
    goto done;
  rc = parse(t, r.out, rows, cap, count);
  if (rc) {
    gt__set_error("tmux: malformed, duplicate or oversized session list");
    goto done;
  }
  for (size_t i = 0; i < *count; i++)
    if ((rc = policy(t, &rows[i], b)))
      break;
done:
  gt__command_free(&r);
  if (rc)
    *count = 0;
  return rc;
}
int gt_tmux_list(gt_tmux_t *t, gt_tmux_session_t *rows, size_t cap,
                 size_t *count, gt_tmux_server_t *server) {
  if (!t || !rows || !cap || cap > GT_TMUX_SESSION_MAX || !count || !server)
    return GT_ERR_INVALID_ARG;
  gt_command_budget_t b = budget(t);
  return list(t, rows, cap, count, server, &b);
}
static int validate(gt_tmux_t *t, const gt_tmux_session_t *target,
                    const gt_command_budget_t *b) {
  if (!target || !memchr(target->name, 0, sizeof target->name) ||
      !memchr(target->socket_path, 0, sizeof target->socket_path))
    return GT_ERR_INVALID_ARG;
  gt_tmux_session_t *rows = calloc(GT_TMUX_SESSION_MAX, sizeof *rows);
  if (!rows)
    return GT_ERR_INTERNAL;
  size_t count;
  gt_tmux_server_t server;
  int rc = list(t, rows, GT_TMUX_SESSION_MAX, &count, &server, b);
  bool found = false;
  if (!rc)
    for (size_t i = 0; i < count; i++) {
      gt_tmux_session_t *r = &rows[i];
      if (r->id == target->id && r->created == target->created &&
          r->server_pid == target->server_pid &&
          r->server_started == target->server_started &&
          !strcmp(r->socket_path, target->socket_path) &&
          !strcmp(r->name, target->name) &&
          r->destroy_unattached == target->destroy_unattached &&
          r->exit_unattached == target->exit_unattached) {
        found = true;
        break;
      }
    }
  free(rows);
  if (!rc && !found) {
    gt__set_error(
        "tmux: selection changed or disappeared; refresh and select again");
    return GT_ERR_NOT_FOUND;
  }
  return rc;
}
static char *literal(const char *name) {
  /* tmux formats (#) and the argv command separator (trailing ;) are separate
   * interpreters after POSIX shell quoting. Preserve the user's literal text.
   */
  size_t n = strlen(name);
  char *p = malloc(n * 2 + 2);
  if (!p)
    return NULL;
  char *out = p;
  for (size_t i = 0; i < n; i++) {
    if (name[i] == '#')
      *out++ = '#';
    if (i + 1 == n && name[i] == ';')
      *out++ = '\\';
    *out++ = name[i];
  }
  *out = 0;
  return p;
}
int gt_tmux_change(gt_tmux_t *t, gt_tmux_operation_t op,
                   const gt_tmux_session_t *target, const char *name,
                   gt_tmux_result_t *result) {
  if (!result)
    return GT_ERR_INVALID_ARG;
  *result = GT_TMUX_REJECTED;
  if (!t || op > GT_TMUX_TERMINATE || op < GT_TMUX_CREATE ||
      ((op == GT_TMUX_CREATE || op == GT_TMUX_RENAME) && !input_name(name)))
    return GT_ERR_INVALID_ARG;
  gt_command_budget_t b = budget(t);
  int rc;
  if (op != GT_TMUX_CREATE) {
    if ((rc = validate(t, target, &b)))
      return rc;
  } else {
    gt_tmux_session_t *rows = calloc(GT_TMUX_SESSION_MAX, sizeof *rows);
    if (!rows)
      return GT_ERR_INTERNAL;
    size_t n;
    gt_tmux_server_t server;
    rc = list(t, rows, GT_TMUX_SESSION_MAX, &n, &server, &b);
    free(rows);
    if (rc)
      return rc;
  }
  char id[32];
  snprintf(id, sizeof id, "$%" PRIu64, target ? target->id : 0);
  char *quoted = op == GT_TMUX_TERMINATE ? NULL : literal(name);
  if (op != GT_TMUX_TERMINATE && !quoted)
    return GT_ERR_INTERNAL;
  const char *create[] = {"new-session", "-d", "-s", quoted, NULL};
  const char *rename[] = {"rename-session", "-t", id, "--", quoted, NULL};
  const char *kill[] = {"kill-session", "-t", id, NULL};
  gt_command_result_t r = {0};
  rc = run_argv(t,
                op == GT_TMUX_CREATE   ? create
                : op == GT_TMUX_RENAME ? rename
                                       : kill,
                op == GT_TMUX_CREATE, &b, &r);
  free(quoted);
  if (rc || !r.completed) {
    *result = r.started ? GT_TMUX_UNKNOWN : GT_TMUX_REJECTED;
    if (!rc)
      rc = GT_ERR_IO;
  } else if (r.exit_code || r.err_len || r.out_len) {
    *result = r.exit_code ? GT_TMUX_REJECTED : GT_TMUX_UNKNOWN;
    gt__set_error("tmux: change rejected (%d): %.400s", r.exit_code,
                  r.err ? r.err : "unexpected output");
    rc = GT_ERR_IO;
  } else
    *result = GT_TMUX_CONFIRMED;
  gt__command_free(&r);
  return rc;
}
gt_transport_t *gt_tmux_attach(gt_tmux_t *t, const gt_tmux_session_t *target,
                               gt_ssh_t *ssh, const gt_shell_opts_t *size) {
  if (!t || ((t->ssh != NULL) != (ssh != NULL)) || (ssh && ssh == t->ssh)) {
    gt__set_error("tmux: attach needs its own SSH owner");
    return NULL;
  }
  gt_command_budget_t b = budget(t);
  if (validate(t, target, &b))
    return NULL;
  char id[32];
  snprintf(id, sizeof id, "$%" PRIu64, target->id);
  const char *argv[] = {t->exe,           "-u", "-S", t->socket, "-N",
                        "attach-session", "-t", id,   NULL};
  static const gt_shell_opts_t zero;
  if (!size)
    size = &zero;
  if (ssh) {
    char *command = shell_command(argv);
    if (!command)
      return NULL;
    int64_t left = b.deadline - gt__command_now();
    gt_transport_t *transport = NULL;
    if (left > 0)
      transport = gt_transport_open_ssh_command(
          ssh, command, size, (uint32_t)left, t->cancelled, t->context);
    else
      gt__set_error("tmux: attach admission timed out");
    free(command);
    return transport;
  }
  /* env supplied to PTY is an overlay; TMUX= prevents nesting without changing
   * the caller's process environment. -S fixes the actual endpoint. */
  const char *env[] = {"TMUX=", NULL};
  gt_pty_opts_t opts = {0};
  opts.executable = t->exe;
  opts.argv = argv + 1;
  opts.env = env;
  opts.cmd_kind = GT_PTY_CMD_DIRECT;
  opts.term = size->term;
  opts.cols = size->cols;
  opts.rows = size->rows;
  opts.px_width = size->px_width;
  opts.px_height = size->px_height;
  return gt_transport_open_pty(&opts);
}
