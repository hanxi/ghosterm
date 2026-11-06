#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
/* Compile the real helper with only library IO/status boundaries substituted.
 * These are boundary contracts, not evidence for real SSH delivery. */
#define libssh2_channel_read_ex fixture_read
#define libssh2_channel_eof fixture_channel_eof
#define libssh2_session_block_directions fixture_directions
#define libssh2_session_get_timeout fixture_get_timeout
#define libssh2_session_set_timeout fixture_set_timeout
#define libssh2_session_get_blocking fixture_get_blocking
#define libssh2_session_set_blocking fixture_set_blocking
#define libssh2_session_last_error fixture_error
#define libssh2_ghosterm_channel_exit_status fixture_status
#define libssh2_channel_close fixture_close
#define libssh2_channel_send_eof fixture_send_eof
#include "../src/ssh.c"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static ssize_t results[2];
static int eof, reads, eof_calls, blocking = 1, closes, sends, status_known, status_value;
static long timeout = 777;

ssize_t fixture_read(LIBSSH2_CHANNEL *channel, int stream, char *buffer, size_t n) {
    CHECK(channel && (stream == 0 || stream == 1) && n >= 4 && blocking == 0 && timeout > 0);
    reads++;
    if (results[stream] > 0) {
        memcpy(buffer, "tail", 4);
    }
    return results[stream];
}

int fixture_channel_eof(LIBSSH2_CHANNEL *channel) {
    CHECK(channel);
    eof_calls++;
    return eof;
}

int fixture_directions(LIBSSH2_SESSION *session) {
    CHECK(session);
    return LIBSSH2_SESSION_BLOCK_INBOUND;
}

long fixture_get_timeout(LIBSSH2_SESSION *session) {
    CHECK(session);
    return timeout;
}

void fixture_set_timeout(LIBSSH2_SESSION *session, long value) {
    CHECK(session);
    timeout = value;
}

int fixture_get_blocking(LIBSSH2_SESSION *session) {
    CHECK(session);
    return blocking;
}

void fixture_set_blocking(LIBSSH2_SESSION *session, int value) {
    CHECK(session);
    blocking = value;
}

int fixture_error(LIBSSH2_SESSION *session, char **message, int *length, int copy) {
    CHECK(session && !copy);
    static char text[] = "fixture transport failure";
    if (message) {
        *message = text;
    }
    if (length) {
        *length = (int)strlen(text);
    }
    return LIBSSH2_ERROR_SOCKET_RECV;
}

int fixture_status(LIBSSH2_CHANNEL *channel, int *value) {
    CHECK(channel);
    if (status_known == 1 && value) {
        *value = status_value;
    }
    return status_known;
}

int fixture_close(LIBSSH2_CHANNEL *channel) {
    CHECK(channel);
    closes++;
    return 0;
}

int fixture_send_eof(LIBSSH2_CHANNEL *channel) {
    CHECK(channel);
    sends++;
    return 0;
}

static void read_case(ssize_t a, ssize_t b, int ended, int expected, bool expected_err) {
    gt_ssh_t ssh = {.sock = -1};
    gt_ssh_exec_t exec = {
        .ssh = &ssh, .sess = (LIBSSH2_SESSION *)&ssh, .chan = (LIBSSH2_CHANNEL *)&ssh};
    uint8_t bytes[8] = {0};
    bool fromerr = true;
    results[0] = a;
    results[1] = b;
    eof = ended;
    reads = eof_calls = 0;
    CHECK(gt_ssh_exec_read(&exec, bytes, sizeof(bytes), 0, &fromerr) == expected);
    CHECK(timeout == 777 && blocking == 1 && fromerr == expected_err);
    if (expected > 0) {
        CHECK(!memcmp(bytes, "tail", 4) && eof_calls == 0);
    }
    CHECK(!exec.closed && !closes && !sends);
}

static void status_case(int present, int value) {
    gt_ssh_t ssh = {0};
    gt_ssh_exec_t exec = {
        .ssh = &ssh, .sess = (LIBSSH2_SESSION *)&ssh, .chan = (LIBSSH2_CHANNEL *)&ssh};
    status_known = present;
    status_value = value;
    reads = 0;
    int output = 987654;
    bool known = gt_ssh_exec_exit_status(&exec, &output);
    CHECK(known == (present == 1) && output == (known ? value : 987654));
    CHECK(!reads && !closes && !sends && !exec.closed && timeout == 777 && blocking == 1);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all";
    CHECK(!strcmp(mode, "all") || !strcmp(mode, "idle") || !strcmp(mode, "eof") ||
          !strcmp(mode, "tail") || !strcmp(mode, "reset") || !strcmp(mode, "status"));
    if (!strcmp(mode, "all") || !strcmp(mode, "idle")) {
        read_case(0, 0, 0, GT_EXEC_READ_TIMEOUT, false);
        read_case(LIBSSH2_ERROR_EAGAIN, LIBSSH2_ERROR_EAGAIN, 0, GT_EXEC_READ_TIMEOUT, false);
        read_case(LIBSSH2_ERROR_TIMEOUT, LIBSSH2_ERROR_TIMEOUT, 0, GT_EXEC_READ_TIMEOUT, false);
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "eof")) {
        read_case(0, 0, 1, GT_EXEC_READ_EOF, false);
        read_case(LIBSSH2_ERROR_SOCKET_RECV, 0, 1, GT_EXEC_READ_EOF, false);
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "tail")) {
        read_case(4, 0, 1, 4, false);
        read_case(0, 4, 1, 4, true);
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "reset")) {
        read_case(LIBSSH2_ERROR_SOCKET_RECV, 0, 0, GT_EXEC_READ_ERROR, false);
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "status")) {
        status_case(0, 0);
        status_case(-1, 0);
        status_case(1, 0);
        status_case(1, 23);
    }
    puts("PASS exec helper boundary");
    return 0;
}
