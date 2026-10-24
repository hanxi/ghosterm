#define main vt_baseline_main
#include "probe.c"
#undef main
#include <sys/stat.h>
static void save_bytes(const char *dir, const char *name, const uint8_t *data, size_t len) {
  char path[4096];
  REQUIRE(snprintf(path, sizeof path, "%s/%s", dir, name) < (int)sizeof path);
  FILE *f = fopen(path, "wb"); REQUIRE(f != NULL);
  REQUIRE(fwrite(data, 1, len, f) == len); REQUIRE(fclose(f) == 0);
}

static void save_pty_fixture(const char *dir) {
  int fd;
  struct winsize size = { .ws_col = 80, .ws_row = 24 };
  pid_t pid = forkpty(&fd, NULL, NULL, &size);
  REQUIRE(pid >= 0);
  if (pid == 0) run_tui_child();
  tui_pid = pid;
  REQUIRE(atexit(cleanup_tui) == 0);
  GhosttyTerminal authority = new_terminal();
  Replies replies = {0};
  enable_replies(authority, &replies);
  consume_tui_output(fd, authority, NULL, tui_initial, NULL);
  flush_tui_reply(fd, &replies, "\x1b[3;5R");
  Bytes initial = capture(authority);
  save_bytes(dir, "pty.initial", initial.data, initial.len);

  /* The child advances while no GUI exists. Persist exactly the bytes read
   * from this PTY, then compare the later full surface with the authority. */
  uint8_t delta[sizeof tui_delta - 1];
  fd_write_all(fd, (const uint8_t *)"a", 1);
  consume_tui_output(fd, authority, NULL, tui_delta, delta);
  flush_tui_reply(fd, &replies, "\x1b[7;9R");
  save_bytes(dir, "pty.delta", delta, sizeof delta);
  Bytes final = capture(authority);
  save_bytes(dir, "pty.final", final.data, final.len);

  size.ws_col = 51;
  size.ws_row = 17;
  REQUIRE(ioctl(fd, TIOCSWINSZ, &size) == 0);
  REQUIRE(ghostty_terminal_resize(authority, 51, 17, 8, 16) == GHOSTTY_SUCCESS);
  fd_write_all(fd, (const uint8_t *)"b", 1);
  consume_tui_output(fd, authority, NULL, tui_final, NULL);
  REQUIRE(replies.calls == 0);
  int status;
  pid_t waited;
  do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
  REQUIRE(waited == pid);
  tui_pid = -1;
  REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(fd);
  free(initial.data);
  free(final.data);
  ghostty_terminal_free(authority);
  puts("PASS full-surface/real PTY fixture captured and child exited cleanly");
}

#include "vim-fixture.h"
#include "tmux-fixture.h"

int main(int argc, char **argv) {
  REQUIRE(argc == 2);
  static const struct { const char *name, *prefix, *suffix; } cuts[] = {
    {"ground", "", "PLAIN"},
    {"csi", "\x1b[38;2;1;2", ";3mCOLOR"},
    {"utf8", "\xe4\xb8", "\xad\xe6\x96\x87"},
    {"osc", "\x1b]2;session", " title\x1b\\"},
    {"esc", "\x1b", "[2;7Hnext"},
    {"dcs", "\x1bP+q544e", "\x1b\\after"},
    {"apc", "\x1b_Ga=q,i=9,f=32,s=1,v=1;", "/wAA/w==\x1b\\"},
  };
  for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
    GhosttyTerminal source = new_terminal();
    for (int row = 0; row < 80; row++) feed(source, "history\r\n");
    feed(source, "\x1b[?1049h\x1b[?2004h\x1b[?1006h\x1b[?1000h");
    feed(source, "SOURCE 中文 e\xcc\x81 \x1b]8;;https://example.invalid\x1b\\link\x1b]8;;\x1b\\");
    feed(source, cuts[i].prefix);
    Bytes initial = capture(source);
    char name[80];
    snprintf(name, sizeof name, "%s.initial", cuts[i].name);
    save_bytes(argv[1], name, initial.data, initial.len);
    const char *effects = "\x1b[6n\x1b[5n\x1b[?996n\x1b]52;c;aGk=\x07\x1b]777;notify;title;body\x07";
    char delta[512];
    REQUIRE(snprintf(delta, sizeof delta, "%s%s", cuts[i].suffix, effects) < (int)sizeof delta);
    snprintf(name, sizeof name, "%s.delta", cuts[i].name);
    save_bytes(argv[1], name, (const uint8_t *)delta, strlen(delta));
    feed(source, delta);
    Bytes final = capture(source);
    snprintf(name, sizeof name, "%s.final", cuts[i].name);
    save_bytes(argv[1], name, final.data, final.len);
    free(initial.data); free(final.data); ghostty_terminal_free(source);
  }
  save_pty_fixture(argv[1]);
  save_vim_fixture(argv[1]);
  save_tmux_fixture(argv[1]);
  puts("PASS full-surface/source fixtures generated with pinned VT API");
}
