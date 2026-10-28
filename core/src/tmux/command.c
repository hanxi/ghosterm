#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "tmux/command.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
int64_t gt__command_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
bool gt__command_expired(const gt_command_budget_t *b) {
  return gt__command_now() >= b->deadline ||
         (b->cancelled && b->cancelled(b->context));
}
int gt__command_append(char **dst, size_t *len, const char *src, size_t n) {
  if (n > GT_COMMAND_LIMIT - *len || memchr(src, 0, n)) {
    gt__set_error("tmux: command output invalid or exceeds 1 MiB");
    return GT_ERR_PROTOCOL;
  }
  char *p = realloc(*dst, *len + n + 1);
  if (!p)
    return GT_ERR_INTERNAL;
  memcpy(p + *len, src, n);
  *len += n;
  p[*len] = 0;
  *dst = p;
  return GT_OK;
}
void gt__command_free(gt_command_result_t *r) {
  free(r->out);
  free(r->err);
  memset(r, 0, sizeof *r);
}
char *gt__command_quote(const char *s) {
  size_t n = 2;
  for (const char *p = s; *p; p++)
    n += *p == '\'' ? 4 : 1;
  char *r = malloc(n + 1);
  if (!r)
    return NULL;
  char *p = r;
  *p++ = '\'';
  for (; *s; s++) {
    if (*s == '\'') {
      memcpy(p, "'\\''", 4);
      p += 4;
    } else
      *p++ = *s;
  }
  *p++ = '\'';
  *p = 0;
  return r;
}
static int make_pipe(int p[2]) {
  if (pipe(p))
    return -1;
  for (int i = 0; i < 2; i++)
    if (fcntl(p[i], F_SETFD, FD_CLOEXEC) < 0) {
      close(p[0]);
      close(p[1]);
      return -1;
    }
  return 0;
}
int gt__command_local(const char *exe, const char *const *argv,
                      const gt_command_budget_t *b, gt_command_result_t *r) {
  memset(r, 0, sizeof *r);
  int pipes[3][2];
  size_t made = 0;
  int rc = GT_ERR_IO;
  if (gt__command_expired(b))
    return GT_ERR_CANCELLED;
  for (; made < 3; made++)
    if (make_pipe(pipes[made]))
      goto pipe_fail;
  /* Build the environment before fork; no allocator/locale calls in child. */
  size_t count = 0;
  while (environ[count])
    count++;
  char **env = calloc(count + 2, sizeof *env);
  if (!env) {
    rc = GT_ERR_INTERNAL;
    goto pipe_fail;
  }
  size_t used = 0;
  for (size_t i = 0; i < count; i++)
    if (strncmp(environ[i], "TMUX=", 5) && strncmp(environ[i], "LC_ALL=", 7) &&
        strncmp(environ[i], "LC_MESSAGES=", 12))
      env[used++] = environ[i];
  env[used++] = "LC_MESSAGES=C";
  long maxfd = sysconf(_SC_OPEN_MAX);
  if (maxfd < 0)
    maxfd = 65536;
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd < 0 || dup2(nullfd, 0) < 0 || dup2(pipes[0][1], 1) < 0 ||
        dup2(pipes[1][1], 2) < 0)
      _exit(126);
    for (int fd = 3; fd < maxfd; fd++)
      if (fd != pipes[2][1])
        close(fd);
    execve(exe, (char *const *)argv, env);
    int error = errno;
    (void)write(pipes[2][1], &error, sizeof error);
    _exit(127);
  }
  free(env);
  if (pid < 0)
    goto pipe_fail;
  (void)setpgid(pid, pid);
  for (int i = 0; i < 3; i++) {
    close(pipes[i][1]);
    fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
  }
  r->started = true;
  bool openfd[3] = {true, true, true}, done = false;
  int status = 0;
  rc = GT_OK;
  while (!done || openfd[0] || openfd[1] || openfd[2]) {
    if (gt__command_expired(b)) {
      gt__set_error("tmux: command timed out or cancelled");
      rc = GT_ERR_CANCELLED;
      break;
    }
    struct pollfd fds[3];
    for (int i = 0; i < 3; i++)
      fds[i] = (struct pollfd){openfd[i] ? pipes[i][0] : -1, POLLIN, 0};
    int p = poll(fds, 3, 10);
    if (p < 0 && errno != EINTR) {
      rc = GT_ERR_IO;
      break;
    }
    for (int i = 0; i < 3 && rc == GT_OK; i++)
      if (openfd[i] && fds[i].revents) {
        char buf[8192];
        ssize_t n = read(pipes[i][0], buf, sizeof buf);
        if (n > 0) {
          if (i == 2) {
            int e = EIO;
            if (n == (ssize_t)sizeof e)
              memcpy(&e, buf, sizeof e);
            r->started = false;
            gt__set_error("tmux: exec %s: %s", exe, strerror(e));
            rc = e == ENOENT ? GT_ERR_NOT_FOUND : GT_ERR_IO;
          } else
            rc = gt__command_append(i == 0 ? &r->out : &r->err,
                                    i == 0 ? &r->out_len : &r->err_len, buf,
                                    (size_t)n);
        } else if (n == 0) {
          openfd[i] = false;
          close(pipes[i][0]);
        } else if (errno != EINTR && errno != EAGAIN) {
          rc = GT_ERR_IO;
        }
      }
    if (rc != GT_OK)
      break;
    if (!done) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid)
        done = true;
      else if (w < 0 && errno != EINTR) {
        rc = GT_ERR_IO;
        break;
      }
    }
  }
  for (int i = 0; i < 3; i++)
    if (openfd[i])
      close(pipes[i][0]);
  if (rc != GT_OK) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
  }
  if (!done)
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  if (rc == GT_OK && WIFEXITED(status)) {
    r->completed = true;
    r->exit_code = WEXITSTATUS(status);
  } else if (rc == GT_OK) {
    gt__set_error("tmux: command killed by signal");
    rc = GT_ERR_IO;
  }
  return rc;
pipe_fail:
  for (size_t i = 0; i < made; i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  gt__set_error("tmux: cannot launch command");
  return rc;
}
