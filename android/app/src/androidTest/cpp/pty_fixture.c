#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* Instrumentation-only child program. Never included in the application APK.
 * Android 10 SELinux denies TCSETSF used by its system stty. A normal terminal
 * application can use TCSANOW; do that explicitly, without changing policy or
 * intercepting any application/terminal function. */
static int failed(const char *operation) {
    fprintf(stderr, "PTY fixture %s: %s\n", operation, strerror(errno));
    return 1;
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int main(int argc, char **argv) {
    struct termios state = {0};
    if (argc < 2) return 64;
    if (tcgetattr(STDIN_FILENO, &state) != 0) return failed("tcgetattr");
    if (argc == 2 && !strcmp(argv[1], "save")) {
        const unsigned char *bytes = (const unsigned char *)&state;
        for (size_t i = 0; i < sizeof(state); i++) printf("%02x", bytes[i]);
        return putchar('\n') == EOF || fflush(stdout) != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "size")) {
        struct winsize size = {0};
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0) return failed("size");
        printf("%u %u\n", size.ws_row, size.ws_col);
        return fflush(stdout) != 0;
    }
    if (!strcmp(argv[1], "restore")) {
        if (argc != 3 || strlen(argv[2]) != sizeof(state) * 2) return 64;
        unsigned char *bytes = (unsigned char *)&state;
        for (size_t i = 0; i < sizeof(state); i++) {
            int high = nibble(argv[2][i * 2]), low = nibble(argv[2][i * 2 + 1]);
            if (high < 0 || low < 0) return 64;
            bytes[i] = (unsigned char)((high << 4) | low);
        }
    } else {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "raw")) cfmakeraw(&state);
            else if (!strcmp(argv[i], "-echo")) state.c_lflag &= (tcflag_t)~ECHO;
            else if (!strcmp(argv[i], "echo")) state.c_lflag |= ECHO;
            else return 64;
        }
    }
    if (tcsetattr(STDIN_FILENO, TCSANOW, &state) != 0) return failed("tcsetattr");
    struct termios observed = {0};
    if (tcgetattr(STDIN_FILENO, &observed) != 0) return failed("verify");
    if (state.c_iflag != observed.c_iflag || state.c_oflag != observed.c_oflag ||
        state.c_cflag != observed.c_cflag || state.c_lflag != observed.c_lflag ||
        memcmp(state.c_cc, observed.c_cc, sizeof(state.c_cc))) {
        fprintf(stderr, "PTY fixture: requested terminal mode was not applied\n");
        return 1;
    }
    return 0;
}
