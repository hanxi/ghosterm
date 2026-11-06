/* P0 experiment only. Not a public ABI, protocol allocation or production mux.
 * Input: bytes on stdin. Output: terminal bytes on stdout; decoded probe frames
 * in the named file (two-byte big-endian length, then payload). No business IO.
 * Fixed storage; owner drives expiry explicitly rather than sleeping in feed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char prefix[] = "\033P+ghosterm-probe;";

enum {
    MAX_PAYLOAD = 768
};

enum state {
    NORMAL,
    ESCAPE,
    PREFIX,
    OPAQUE,
    FRAME,
    DISCARD
};

struct mux {
    enum state state;
    size_t matched, digits;
    unsigned char payload[MAX_PAYLOAD];
    int string_escape, osc, failed, disabled;
    FILE *frames;
};

static void terminal(const unsigned char *bytes, size_t count) {
    if (fwrite(bytes, 1, count, stdout) != count) {
        exit(3);
    }
}

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void opaque(struct mux *m, unsigned char c) {
    terminal(&c, 1);
    if ((m->string_escape && c == '\\') || (m->osc && c == 7) || c == 0x18 || c == 0x1a) {
        m->state = NORMAL;
    }
    m->string_escape = c == 0x1b;
}

static void reject(struct mux *m) {
    m->failed = 1;
    m->disabled = 1;
    m->state = DISCARD;
}

static void feed(struct mux *m, unsigned char c) {
    switch (m->state) {
        case NORMAL:
            if (c == 0x1b) {
                m->state = ESCAPE;
            } else {
                terminal(&c, 1);
            }
            break;
        case ESCAPE:
            if (c == 'P') {
                m->matched = 2;
                m->state = PREFIX;
            } else {
                terminal(prefix, 1);
                if (c == 0x1b) {
                    break;
                }
                terminal(&c, 1);
                m->osc = c == ']';
                m->string_escape = 0;
                m->state = (c == ']' || c == '_' || c == '^' || c == 'X') ? OPAQUE : NORMAL;
            }
            break;
        case PREFIX:
            if (c == prefix[m->matched]) {
                if (++m->matched == sizeof(prefix) - 1) {
                    m->digits = 0;
                    m->string_escape = 0;
                    m->state = m->disabled ? DISCARD : FRAME;
                }
            } else {
                terminal(prefix, m->matched);
                m->osc = m->string_escape = 0;
                m->state = OPAQUE;
                opaque(m, c);
            }
            break;
        case OPAQUE:
            opaque(m, c);
            break;
        case FRAME:
            if (m->string_escape) {
                if (c == '\\') {
                    if ((m->digits & 1) || m->digits == 0) {
                        reject(m);
                    } else {
                        size_t count = m->digits / 2;
                        unsigned char length[2] = {(unsigned char)(count >> 8),
                                                   (unsigned char)count};
                        if (fwrite(length, 1, 2, m->frames) != 2 ||
                            fwrite(m->payload, 1, count, m->frames) != count) {
                            exit(3);
                        }
                    }
                    m->state = NORMAL;
                    m->string_escape = 0;
                } else {
                    reject(m);
                    m->string_escape = c == 0x1b;
                }
            } else if (c == 0x1b) {
                m->string_escape = 1;
            } else {
                int digit = hex(c);
                if (digit < 0 || m->digits >= MAX_PAYLOAD * 2) {
                    reject(m);
                } else {
                    size_t i = m->digits / 2;
                    if (m->digits & 1) {
                        m->payload[i] |= (unsigned char)digit;
                    } else {
                        m->payload[i] = (unsigned char)(digit << 4);
                    }
                    ++m->digits;
                }
            }
            break;
        case DISCARD:
            if (m->string_escape && c == '\\') {
                m->state = NORMAL;
            }
            m->string_escape = c == 0x1b;
            break;
    }
}

/* Pending prefix expiry restores bytes. Accepted frame expiry quarantines the
 * remainder through ST; it must never become terminal text or a new frame. */
static void expire(struct mux *m) {
    if (m->state == ESCAPE) {
        terminal(prefix, 1);
        m->state = NORMAL;
    } else if (m->state == PREFIX) {
        terminal(prefix, m->matched);
        m->state = OPAQUE;
        m->osc = m->string_escape = 0;
    } else if (m->state == FRAME) {
        reject(m);
    }
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        return 3;
    }
    char *end;
    unsigned long chunk = strtoul(argv[2], &end, 10);
    if (*end || !chunk || chunk > 8192) {
        return 3;
    }
    unsigned long expire_at = 0;
    if (argc == 4) {
        expire_at = strtoul(argv[3], &end, 10);
        if (*end || !expire_at) {
            return 3;
        }
    }
    struct mux m = {0};
    m.frames = fopen(argv[1], "wb");
    if (!m.frames) {
        return 3;
    }
    unsigned char buffer[8192];
    size_t count, total = 0;
    while ((count = fread(buffer, 1, chunk, stdin)) > 0) {
        for (size_t i = 0; i < count; ++i) {
            feed(&m, buffer[i]);
            if (++total == expire_at) {
                expire(&m);
            }
        }
    }
    expire(&m);
    if (ferror(stdin) || fclose(m.frames) || fflush(stdout)) {
        return 3;
    }
    return m.failed ? 2 : 0;
}
