/* P0 only: same fixtures on the real Android libghostty-vt archive. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghostty/vt.h>

struct replies { unsigned char bytes[128]; size_t count; };

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "ASSERT: %s\n", message); exit(1); }
}

static void reply(GhosttyTerminal terminal, void *context, const uint8_t *bytes, size_t count) {
    (void)terminal;
    struct replies *out = context;
    check(count <= sizeof(out->bytes) - out->count, "terminal reply budget exceeded");
    memcpy(out->bytes + out->count, bytes, count);
    out->count += count;
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    char *end;
    unsigned long chunk = strtoul(argv[3], &end, 10);
    check(!*end && chunk && chunk <= 8192, "invalid chunk size");
    unsigned char input[16384];
    FILE *file = fopen(argv[1], "rb");
    check(file != NULL, "fixture file missing");
    size_t count = fread(input, 1, sizeof(input), file);
    check(!ferror(file) && fgetc(file) == EOF, "fixture exceeds budget");
    fclose(file);
    GhosttyTerminal terminal = NULL;
    struct replies replies = {0};
    check(ghostty_terminal_new(NULL, &terminal, 80, 24) == GHOSTTY_SUCCESS, "VT allocation failed");
    check(ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_USERDATA, &replies) == GHOSTTY_SUCCESS, "VT userdata failed");
    check(ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)reply) == GHOSTTY_SUCCESS, "VT reply hook failed");
    for (size_t start = 0; start < count; start += chunk)
        ghostty_terminal_vt_write(terminal, input + start, count - start < chunk ? count - start : chunk);
    ghostty_terminal_vt_write(terminal, (const uint8_t *)"\033[6n", 4);
    char expected_reply[128];
    int reply_count = snprintf(expected_reply, sizeof(expected_reply), "%s\033[1;%zuR", argv[5], strlen(argv[2]) + 1);
    check(reply_count > 0 && replies.count == (size_t)reply_count &&
          memcmp(replies.bytes, expected_reply, replies.count) == 0, "VT unexpected bridge reply or cursor position");
    GhosttyFormatter formatter = NULL;
    GhosttyFormatterTerminalOptions options = {
        .size = sizeof(options), .emit = GHOSTTY_FORMATTER_FORMAT_PLAIN, .trim = true
    };
    check(ghostty_formatter_terminal_new(NULL, &formatter, terminal, options) == GHOSTTY_SUCCESS, "VT formatter unavailable");
    unsigned char text[8192];
    size_t written = 0;
    check(ghostty_formatter_format_buf(formatter, text, sizeof(text), &written) == GHOSTTY_SUCCESS, "VT text read failed");
    while (written && (text[written-1] == '\n' || text[written-1] == '\r' || text[written-1] == ' ')) --written;
    check(written == strlen(argv[2]) && memcmp(text, argv[2], written) == 0, "VT control payload appeared as text");
    ghostty_formatter_free(formatter);
    ghostty_terminal_free(terminal);
    printf("PASS %s\n", argv[4]);
    return 0;
}
