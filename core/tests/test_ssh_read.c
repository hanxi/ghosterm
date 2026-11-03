#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
/* Compile the real SSH implementation with only libssh2's read boundary
 * substituted. The negative-read/confirmed-EOF case was observed on real
 * libssh2; no public ABI or production testing hooks are added. */
#define libssh2_channel_read_ex test_channel_read_ex
#define libssh2_channel_eof test_channel_eof
#define libssh2_session_get_timeout test_get_timeout
#define libssh2_session_set_timeout test_set_timeout
#define libssh2_session_get_blocking test_get_blocking
#define libssh2_session_set_blocking test_set_blocking
#define libssh2_session_last_error test_last_error
#include "../src/ssh.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static ssize_t result;
static int eof, eof_calls, blocking = 1;
static long timeout = 777;
ssize_t test_channel_read_ex(LIBSSH2_CHANNEL *channel, int stream, char *buffer, size_t size) {
  CHECK(channel && stream == 0 && size >= 4);
  if (result > 0) memcpy(buffer, "tail", 4);
  return result;
}
int test_channel_eof(LIBSSH2_CHANNEL *channel) { CHECK(channel); eof_calls++; return eof; }
long test_get_timeout(LIBSSH2_SESSION *session) { CHECK(session); return timeout; }
void test_set_timeout(LIBSSH2_SESSION *session, long value) { CHECK(session); timeout = value; }
int test_get_blocking(LIBSSH2_SESSION *session) { CHECK(session); return blocking; }
void test_set_blocking(LIBSSH2_SESSION *session, int value) { CHECK(session); blocking = value; }
int test_last_error(LIBSSH2_SESSION *session, char **message, int *length, int copy) {
  CHECK(session && !copy);
  static char text[] = "transport read";
  if (message) *message = text;
  if (length) *length = (int)strlen(text);
  return (int)result;
}
static void check(ssize_t read_result, int confirmed_eof, int expected, int poll) {
  ssh_shell_t shell = {0};
  shell.sess = (LIBSSH2_SESSION *)&shell;
  shell.chan = (LIBSSH2_CHANNEL *)&shell;
  unsigned char output[8] = {0};
  result = read_result; eof = confirmed_eof; eof_calls = 0;
  CHECK(gt__ssh_shell_read(&shell, output, sizeof output, poll ? 0 : 20) == expected);
  CHECK(timeout == 777 && blocking == 1);
  if (read_result > 0) { CHECK(!memcmp(output, "tail", 4)); CHECK(eof_calls == 0); }
  if (expected == GT_TRANSPORT_READ_ERROR) CHECK(strstr(gt_last_error(), "libssh2=-43"));
}
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "all";
  CHECK(!strcmp(mode,"all") || !strcmp(mode,"tail") || !strcmp(mode,"confirmed") || !strcmp(mode,"reset") || !strcmp(mode,"idle"));
  if (!strcmp(mode, "all") || !strcmp(mode, "tail")) check(4, 1, 4, 0);
  if (!strcmp(mode, "all") || !strcmp(mode, "confirmed")) {
    check(0, 1, GT_TRANSPORT_READ_EOF, 0);
    check(LIBSSH2_ERROR_SOCKET_RECV, 1, GT_TRANSPORT_READ_EOF, 0);
    check(LIBSSH2_ERROR_TIMEOUT, 1, GT_TRANSPORT_READ_EOF, 1);
  }
  if (!strcmp(mode, "all") || !strcmp(mode, "reset")) check(LIBSSH2_ERROR_SOCKET_RECV, 0, GT_TRANSPORT_READ_ERROR, 0);
  if (!strcmp(mode, "all") || !strcmp(mode, "idle")) {
    check(0, 0, GT_TRANSPORT_READ_TIMEOUT, 0);
    check(LIBSSH2_ERROR_TIMEOUT, 0, GT_TRANSPORT_READ_TIMEOUT, 0);
    check(LIBSSH2_ERROR_EAGAIN, 0, GT_TRANSPORT_READ_TIMEOUT, 1);
  }
  puts("SSH shell read boundary PASS");
  return 0;
}
