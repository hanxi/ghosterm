/* P0 evidence probe. This is not the production session-state adapter. */
#include <ghostty/vt.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#define REQUIRE(c) do { if (!(c)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

static const size_t continuation_limit = 1024 * 1024;
static const size_t snapshot_limit = 16 * 1024 * 1024;

typedef struct {
  uint8_t *data;
  size_t len;
} Bytes;

static GhosttyTerminal new_terminal(void) {
  GhosttyTerminal terminal = NULL;
  REQUIRE(ghostty_terminal_new(NULL, &terminal, 80, 24) == GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_set(terminal,
      GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES,
      &continuation_limit) == GHOSTTY_SUCCESS);
  return terminal;
}

static void feed(GhosttyTerminal terminal, const char *text) {
  ghostty_terminal_vt_write(terminal, (const uint8_t *)text, strlen(text));
}

static Bytes capture(GhosttyTerminal terminal) {
  Bytes bytes = {0};
  REQUIRE(ghostty_snapshot_encode_buf(terminal, NULL, 0, &bytes.len)
      == GHOSTTY_OUT_OF_SPACE);
  REQUIRE(bytes.len > 0 && bytes.len <= snapshot_limit);
  bytes.data = malloc(bytes.len);
  REQUIRE(bytes.data != NULL);
  REQUIRE(ghostty_snapshot_encode_buf(terminal, bytes.data, bytes.len, &bytes.len)
      == GHOSTTY_SUCCESS);
  return bytes;
}

static GhosttyResult restore_result(Bytes bytes, GhosttyTerminal *terminal) {
  GhosttySnapshotDecoder decoder = NULL;
  GhosttyResult result = ghostty_snapshot_decoder_new_buf(
      NULL, &decoder, bytes.data, bytes.len);
  if (result != GHOSTTY_SUCCESS) return result;
  const bool retain = true;
  REQUIRE(ghostty_snapshot_decoder_set(decoder,
      GHOSTTY_SNAPSHOT_DECODER_OPT_RETAIN_CONTINUATION, &retain)
      == GHOSTTY_SUCCESS);
  REQUIRE(ghostty_snapshot_decoder_set(decoder,
      GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES, &continuation_limit)
      == GHOSTTY_SUCCESS);
  result = ghostty_snapshot_decoder_decode(decoder, terminal);
  ghostty_snapshot_decoder_free(decoder);
  return result;
}

static GhosttyTerminal restore(Bytes bytes) {
  GhosttyTerminal terminal = NULL;
  REQUIRE(restore_result(bytes, &terminal) == GHOSTTY_SUCCESS);
  REQUIRE(terminal != NULL);
  return terminal;
}

static void require_equal_state(GhosttyTerminal a, GhosttyTerminal b) {
  Bytes left = capture(a), right = capture(b);
  REQUIRE(left.len == right.len);
  REQUIRE(memcmp(left.data, right.data, left.len) == 0);
  free(left.data);
  free(right.data);
}

static void test_continuation(void) {
  static const struct { const char *name, *prefix, *suffix; } cases[] = {
    {"CSI", "\x1b[38;2;1;2", ";3mCOLOR"},
    {"OSC", "\x1b]2;session", " title\x1b\\"},
    {"UTF-8", "\xe4\xb8", "\xad\xe6\x96\x87"},
    {"ESC", "\x1b", "[2;7Hnext"},
    {"DCS", "\x1bP+q544e", "\x1b\\after"},
    {"APC", "\x1b_Gi=7,f=32,s=1,v=1;", "/wAA/w==\x1b\\"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    GhosttyTerminal source = new_terminal();
    for (int row = 0; row < 80; row++) feed(source, "history\r\n");
    feed(source, "\x1b[?1049h\x1b[?2004h\x1b[?1006h\x1b[?1000h");
    feed(source, "wide: 中文 e\xcc\x81 \x1b]8;;https://example.invalid\x1b\\link\x1b]8;;\x1b\\");
    feed(source, cases[i].prefix);
    Bytes bytes = capture(source);
    GhosttyTerminal copy = restore(bytes);
    require_equal_state(source, copy);
    feed(source, cases[i].suffix);
    feed(copy, cases[i].suffix);
    require_equal_state(source, copy);
    /* Returning from the alternate screen must restore primary history. */
    feed(source, "\x1b[?1049l");
    feed(copy, "\x1b[?1049l");
    REQUIRE(ghostty_terminal_resize(source, 51, 17, 8, 16) == GHOSTTY_SUCCESS);
    REQUIRE(ghostty_terminal_resize(copy, 51, 17, 8, 16) == GHOSTTY_SUCCESS);
    require_equal_state(source, copy);
    printf("PASS continuation/%s\n", cases[i].name);
    ghostty_terminal_free(copy);
    ghostty_terminal_free(source);
    free(bytes.data);
  }
}

typedef struct { uint8_t data[256]; size_t len; unsigned calls; } Replies;

static void on_reply(GhosttyTerminal terminal, void *userdata,
                     const uint8_t *data, size_t len) {
  (void)terminal;
  Replies *replies = userdata;
  REQUIRE(len <= sizeof replies->data - replies->len);
  memcpy(replies->data + replies->len, data, len);
  replies->len += len;
  replies->calls++;
}

static void enable_replies(GhosttyTerminal terminal, Replies *replies) {
  REQUIRE(ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_USERDATA, replies)
      == GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
      (const void *)on_reply) == GHOSTTY_SUCCESS);
}

static void test_effects(void) {
  GhosttyTerminal authority = new_terminal();
  Replies replies = {0};
  enable_replies(authority, &replies);
  feed(authority, "\x1b[3;5H\x1b[6n");
  REQUIRE(replies.calls == 1);
  REQUIRE(replies.len == 6 && memcmp(replies.data, "\x1b[3;5R", 6) == 0);
  Bytes bytes = capture(authority);
  GhosttyTerminal replica = restore(bytes);
  /* A VT replica has no effects installed. Full surface still needs a patch. */
  REQUIRE(ghostty_terminal_set(replica, GHOSTTY_TERMINAL_OPT_USERDATA, &replies)
      == GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_set(replica, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
      NULL) == GHOSTTY_SUCCESS);
  replies = (Replies){0};
  feed(authority, "\x1b[6n");
  feed(replica, "\x1b[6n");
  REQUIRE(replies.calls == 1);
  REQUIRE(replies.len == 6 && memcmp(replies.data, "\x1b[3;5R", 6) == 0);
  require_equal_state(authority, replica);
  puts("PASS effects/VT authority emits exactly one cursor report");
  ghostty_terminal_free(replica);
  ghostty_terminal_free(authority);
  free(bytes.data);
}

static void test_corruption(void) {
  GhosttyTerminal source = new_terminal();
  feed(source, "CRC fixture");
  Bytes bytes = capture(source);
  REQUIRE(bytes.len > 32);
  bytes.data[bytes.len / 2] ^= 1;
  GhosttyTerminal copy = NULL;
  GhosttyResult result = restore_result(bytes, &copy);
  REQUIRE(result == GHOSTTY_INVALID_VALUE);
  ghostty_terminal_free(copy);
  /* A truncated stream cannot be mistaken for a complete snapshot. */
  bytes.data[bytes.len / 2] ^= 1;
  bytes.len--;
  copy = NULL;
  result = restore_result(bytes, &copy);
  REQUIRE(result == GHOSTTY_INVALID_VALUE);
  ghostty_terminal_free(copy);
  ghostty_terminal_free(source);
  free(bytes.data);
  puts("PASS corruption/CRC and truncated FINISH rejected");
}

/* A small real TUI protocol, not a mock fd: the child owns a controlling PTY,
 * enters the alternate screen, waits for CPR replies, and observes TIOCSWINSZ.
 * Explicit handshakes keep snapshot boundaries independent of read chunking. */
static const char tui_initial[] =
    "\x1b[?1049h\x1b[?2004h\x1b[?1000h\x1b[3;5H\x1b[6n\x1b[38;2;1;2";
static const char tui_delta[] = ";3m中文 e\xcc\x81\x1b[7;9H\x1b[6n";
static const char tui_final[] = "\x1b[?1049l\r\nPTY resized 51x17\r\n";
static pid_t tui_pid = -1;

static void cleanup_tui(void) {
  if (tui_pid <= 0) return;
  kill(tui_pid, SIGKILL);
  while (waitpid(tui_pid, NULL, 0) < 0 && errno == EINTR) {}
  tui_pid = -1;
}

static void fd_write_all(int fd, const uint8_t *data, size_t len) {
  while (len) {
    ssize_t n = write(fd, data, len);
    if (n < 0 && errno == EINTR) continue;
    REQUIRE(n > 0);
    data += n;
    len -= (size_t)n;
  }
}

static void child_expect(const char *expected) {
  while (*expected) {
    uint8_t byte;
    ssize_t n = read(STDIN_FILENO, &byte, 1);
    if (n < 0 && errno == EINTR) continue;
    REQUIRE(n == 1 && byte == (uint8_t)*expected++);
  }
}

static void run_tui_child(void) {
  alarm(10);
  REQUIRE(isatty(STDIN_FILENO));
  REQUIRE(tcgetsid(STDIN_FILENO) == getsid(0));
  REQUIRE(tcgetpgrp(STDIN_FILENO) == getpgrp());
  struct termios raw;
  REQUIRE(tcgetattr(STDIN_FILENO, &raw) == 0);
  cfmakeraw(&raw);
  REQUIRE(tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);
  fd_write_all(STDOUT_FILENO, (const uint8_t *)tui_initial, strlen(tui_initial));
  child_expect("\x1b[3;5Ra");
  fd_write_all(STDOUT_FILENO, (const uint8_t *)tui_delta, strlen(tui_delta));
  child_expect("\x1b[7;9Rb");
  struct winsize size;
  REQUIRE(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0);
  REQUIRE(size.ws_col == 51 && size.ws_row == 17);
  fd_write_all(STDOUT_FILENO, (const uint8_t *)tui_final, strlen(tui_final));
  _exit(0);
}

static void consume_tui_output(int fd, GhosttyTerminal authority,
                               GhosttyTerminal replica, const char *expected,
                               uint8_t *recording) {
  size_t left = strlen(expected);
  while (left) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 2000);
    if (ready < 0 && errno == EINTR) continue;
    REQUIRE(ready > 0);
    uint8_t buf[11]; /* Force multiple reads even when PTY coalesces writes. */
    ssize_t n = read(fd, buf, left < sizeof buf ? left : sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    REQUIRE(n > 0);
    REQUIRE(memcmp(buf, expected, (size_t)n) == 0);
    if (recording) {
      memcpy(recording, buf, (size_t)n);
      recording += n;
    }
    ghostty_terminal_vt_write(authority, buf, (size_t)n);
    if (replica) ghostty_terminal_vt_write(replica, buf, (size_t)n);
    expected += n;
    left -= (size_t)n;
  }
}

static void flush_tui_reply(int fd, Replies *replies, const char *expected) {
  REQUIRE(replies->calls == 1);
  REQUIRE(replies->len == strlen(expected));
  REQUIRE(memcmp(replies->data, expected, replies->len) == 0);
  fd_write_all(fd, replies->data, replies->len);
  *replies = (Replies){0};
}

static void test_pty(void) {
  int fd;
  struct winsize size = { .ws_row = 24, .ws_col = 80 };
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
  /* Child now waits for 'a', with a CSI pending in the authority parser. */
  Bytes bytes = capture(authority);
  GhosttyTerminal replica = restore(bytes);
  require_equal_state(authority, replica);
  /* Keep userdata observable, but leave the replica's reply callback absent. */
  REQUIRE(ghostty_terminal_set(replica, GHOSTTY_TERMINAL_OPT_USERDATA, &replies)
      == GHOSTTY_SUCCESS);
  fd_write_all(fd, (const uint8_t *)"a", 1);
  /* Authority advances while the GUI copy consumes nothing. Reattach replays
   * the bounded delta actually read from the PTY, after its query was answered. */
  uint8_t delta[sizeof tui_delta - 1];
  consume_tui_output(fd, authority, NULL, tui_delta, delta);
  flush_tui_reply(fd, &replies, "\x1b[7;9R");
  ghostty_terminal_vt_write(replica, delta, sizeof delta);
  REQUIRE(replies.calls == 0);
  require_equal_state(authority, replica);
  size.ws_col = 51;
  size.ws_row = 17;
  REQUIRE(ioctl(fd, TIOCSWINSZ, &size) == 0);
  REQUIRE(ghostty_terminal_resize(authority, 51, 17, 8, 16) == GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_resize(replica, 51, 17, 8, 16) == GHOSTTY_SUCCESS);
  fd_write_all(fd, (const uint8_t *)"b", 1);
  consume_tui_output(fd, authority, replica, tui_final, NULL);
  REQUIRE(replies.calls == 0);
  require_equal_state(authority, replica);
  int status;
  pid_t waited;
  do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
  REQUIRE(waited == pid);
  tui_pid = -1;
  REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(fd);
  ghostty_terminal_free(replica);
  ghostty_terminal_free(authority);
  free(bytes.data);
  puts("PASS pty/real TUI, mid-CSI restore, single replies, resize, alternate screen exit");
}

static int test_resources(void) {
  GhosttyTerminal source = new_terminal();
  const uint64_t storage_limit = 1024 * 1024;
  REQUIRE(ghostty_terminal_set(source,
      GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &storage_limit)
      == GHOSTTY_SUCCESS);
  /* A real inline RGBA red pixel; no placeholder PNG decoder. */
  feed(source, "\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  GhosttyKittyGraphics graphics = NULL;
  REQUIRE(ghostty_terminal_get(source, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
      &graphics) == GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_image(graphics, 7) != NULL);
  Bytes bytes = capture(source);
  GhosttyTerminal copy = restore(bytes);
  REQUIRE(ghostty_terminal_get(copy, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
      &graphics) == GHOSTTY_SUCCESS);
  const bool restored = ghostty_kitty_graphics_image(graphics, 7) != NULL;
  ghostty_terminal_free(copy);
  ghostty_terminal_free(source);
  free(bytes.data);
  if (!restored) {
    fputs("LIMITATION resources/image 7 omitted by upstream snapshot; P0 incomplete\n",
          stderr);
    return 3;
  }
  puts("PASS resources/image survived snapshot (placement not yet verified)");
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fputs("usage: probe continuation|effects|corruption|resources|pty\n", stderr);
    return 2;
  }
  if (strcmp(argv[1], "continuation") == 0) test_continuation();
  else if (strcmp(argv[1], "effects") == 0) test_effects();
  else if (strcmp(argv[1], "corruption") == 0) test_corruption();
  else if (strcmp(argv[1], "pty") == 0) test_pty();
  else if (strcmp(argv[1], "resources") == 0) return test_resources();
  else return 2;
  return 0;
}
