#include "ghosterm/extension_discovery.h"
#include "extension/memory.h"
#include <string.h>

/* Private OSC discovery v1, independent of Session wire v1. The binary body is
 * public instance[16], claimed Client digest[32], BE directory bytes[2], UTF-8
 * absolute directory. No permit, hostname, executable, or return route. */
static const char prefix[] = "777;ghosterm-discovery;1;";
#define RAW_MAX (50u + GT_EXT_DISCOVERY_DIRECTORY_MAX)
#define BASE64_MAX (((RAW_MAX + 2u) / 3u) * 4u)

enum state {
    GROUND,
    ESCAPE,
    INTERMEDIATE,
    CSI,
    OSC_PREFIX,
    PAYLOAD,
    STRING
};

struct gt_ext_discovery {
    enum state state;
    bool string_osc, string_escape, recognized, clock_set;
    unsigned utf8_left;
    size_t prefix_at, used;
    uint64_t clock, started, sequence;
    unsigned char bytes[BASE64_MAX];
    uint32_t head, count, skipped;
    gt_ext_discovery_snapshot_t pending[GT_EXT_DISCOVERY_PENDING_MAX];
};

static void skip(gt_ext_discovery_t *d) {
    if (d->skipped != UINT32_MAX) {
        d->skipped++;
    }
}

static void ground(gt_ext_discovery_t *d) {
    d->state = GROUND;
    d->recognized = false;
    d->string_escape = false;
    d->used = d->prefix_at = 0;
}

static int digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static bool utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint32_t c = s[i++], minimum;
        unsigned count;
        if (c < 128) {
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            minimum = 0x80;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            minimum = 0x800;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            minimum = 0x10000;
            c &= 7;
        } else {
            return false;
        }
        if (count > n - i) {
            return false;
        }
        while (count--) {
            unsigned char part = s[i++];
            if ((part & 0xc0) != 0x80) {
                return false;
            }
            c = c << 6 | (part & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static bool directory(const unsigned char *s, size_t n) {
    if (n < 2 || n > GT_EXT_DISCOVERY_DIRECTORY_MAX || s[0] != '/' || s[n - 1] == '/' ||
        memchr(s, 0, n) || memchr(s, '\n', n) || memchr(s, '\r', n) || !utf8(s, n)) {
        return false;
    }
    for (size_t at = 1; at < n;) {
        size_t end = at;
        while (end < n && s[end] != '/') {
            end++;
        }
        size_t size = end - at;
        if (!size || (size == 1 && s[at] == '.') ||
            (size == 2 && s[at] == '.' && s[at + 1] == '.')) {
            return false;
        }
        at = end + 1;
    }
    return true;
}

static void hexadecimal(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 15];
    }
    out[n * 2] = 0;
}

static bool offer(gt_ext_discovery_t *d) {
    if (!d->used || d->used % 4) {
        return false;
    }
    unsigned char raw[RAW_MAX];
    size_t size = 0;
    for (size_t at = 0; at < d->used; at += 4) {
        const unsigned char *p = d->bytes + at;
        int a = digit(p[0]), b = digit(p[1]), c = digit(p[2]), e = digit(p[3]);
        bool last = at + 4 == d->used;
        size_t count = p[2] == '=' ? 1 : p[3] == '=' ? 2 : 3;
        if (a < 0 || b < 0 || (c < 0 && p[2] != '=') || (e < 0 && p[3] != '=') ||
            (count < 3 && !last) || (count == 1 && (p[3] != '=' || (b & 15))) ||
            (count == 2 && (c < 0 || (c & 3))) || count > sizeof(raw) - size) {
            return false;
        }
        raw[size++] = (unsigned char)((a << 2) | (b >> 4));
        if (count > 1) {
            raw[size++] = (unsigned char)((b << 4) | (c >> 2));
        }
        if (count > 2) {
            raw[size++] = (unsigned char)((c << 6) | e);
        }
    }
    if (size < 52) {
        return false;
    }
    size_t n = (size_t)raw[48] << 8 | raw[49];
    if (n != size - 50 || !directory(raw + 50, n)) {
        return false;
    }
    gt_ext_discovery_snapshot_t candidate = {0};
    memcpy(candidate.directory, raw + 50, n);
    hexadecimal(raw, 16, candidate.instance);
    hexadecimal(raw + 16, 32, candidate.client_digest);
    for (uint32_t i = 0; i < d->count; i++) {
        const gt_ext_discovery_snapshot_t *p =
            &d->pending[(d->head + i) % GT_EXT_DISCOVERY_PENDING_MAX];
        if (!strcmp(p->instance, candidate.instance) &&
            !strcmp(p->client_digest, candidate.client_digest) &&
            !strcmp(p->directory, candidate.directory)) {
            return true;
        }
    }
    if (d->count == GT_EXT_DISCOVERY_PENDING_MAX || d->sequence == UINT64_MAX) {
        return false;
    }
    candidate.available = true;
    candidate.sequence = ++d->sequence;
    d->pending[(d->head + d->count++) % GT_EXT_DISCOVERY_PENDING_MAX] = candidate;
    return true;
}

static void feed(gt_ext_discovery_t *d, unsigned char c) {
    if (d->state == STRING || d->state == OSC_PREFIX || d->state == PAYLOAD) {
        if (d->utf8_left) {
            if ((c & 0xc0) == 0x80) {
                d->utf8_left--;
                return;
            }
            d->utf8_left = 0;
        }
        if (c >= 0xc2 && c <= 0xf4) {
            if (d->recognized) {
                skip(d);
                d->recognized = false;
            }
            d->state = STRING;
            d->string_escape = false;
            d->utf8_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            return;
        }
    }
    if (d->state == GROUND) {
        if (d->utf8_left) {
            if ((c & 0xc0) == 0x80) {
                d->utf8_left--;
                return;
            }
            d->utf8_left = 0;
        }
        if (c >= 0xc2 && c <= 0xf4) {
            d->utf8_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            return;
        }
        if (c == 0x1b) {
            d->state = ESCAPE;
        } else if (c == 0x9b) {
            d->state = CSI;
        } else if (c == 0x9d || c == 0x90 || c == 0x98 || c == 0x9e || c == 0x9f) {
            d->state = STRING;
            d->string_osc = c == 0x9d;
        }
        return;
    }
    if (c == 0x18 || c == 0x1a) {
        if (d->recognized) {
            skip(d);
        }
        ground(d);
        return;
    }
    if (d->state == ESCAPE || d->state == INTERMEDIATE || d->state == CSI) {
        if (c == 0x1b) {
            d->state = ESCAPE;
            return;
        }
        if (d->state == ESCAPE && c == ']') {
            d->state = OSC_PREFIX;
            d->string_osc = true;
            d->started = d->clock;
            return;
        }
        if (d->state == ESCAPE && (c == 'P' || c == 'X' || c == '^' || c == '_')) {
            d->state = STRING;
            d->string_osc = false;
            return;
        }
        if (d->state == ESCAPE && c == '[') {
            d->state = CSI;
            return;
        }
        if (d->state == CSI) {
            if (c >= 0x40 && c <= 0x7e) {
                ground(d);
            }
        } else if (c >= 0x20 && c <= 0x2f) {
            d->state = INTERMEDIATE;
        } else if (c >= 0x30 && c <= 0x7e) {
            ground(d);
        }
        return;
    }
    if ((d->string_osc && c == 7) || (d->string_escape && c == '\\') || c == 0x9c) {
        if (d->state == PAYLOAD && !offer(d)) {
            skip(d);
        }
        ground(d);
        return;
    }
    if (d->string_escape) {
        if (d->recognized) {
            skip(d);
            d->recognized = false;
        }
        d->state = STRING;
    }
    d->string_escape = c == 0x1b;
    if (d->string_escape || d->state == STRING) {
        return;
    }
    if (d->state == OSC_PREFIX) {
        if (c != (unsigned char)prefix[d->prefix_at]) {
            d->state = STRING;
            return;
        }
        if (++d->prefix_at == sizeof(prefix) - 1) {
            d->state = PAYLOAD;
            d->recognized = true;
        }
    } else if (d->state == PAYLOAD) {
        if ((digit(c) < 0 && c != '=') || d->used == sizeof(d->bytes)) {
            skip(d);
            d->recognized = false;
            d->state = STRING;
        } else {
            d->bytes[d->used++] = c;
        }
    }
}

gt_status_t gt_ext_discovery_open(gt_ext_discovery_t **out) {
    if (!out) {
        return GT_ERR_INVALID_ARG;
    }
    *out = gt__plugin_memory_calloc(1, sizeof(**out));
    return *out ? GT_OK : GT_ERR_INTERNAL;
}

gt_status_t
gt_ext_discovery_feed(gt_ext_discovery_t *d, const void *bytes, size_t n, uint64_t now) {
    if (!d || (n && !bytes) || n > 65536 || (d->clock_set && now < d->clock)) {
        return GT_ERR_INVALID_ARG;
    }
    d->clock = now;
    d->clock_set = true;
    if ((d->state == OSC_PREFIX || d->state == PAYLOAD) && now - d->started >= 500) {
        if (d->recognized) {
            skip(d);
        }
        d->recognized = false;
        d->used = d->prefix_at = 0;
        d->state = STRING;
    }
    for (size_t i = 0; i < n; i++) {
        feed(d, ((const unsigned char *)bytes)[i]);
    }
    return GT_OK;
}

gt_status_t gt_ext_discovery_poll(gt_ext_discovery_t *d, gt_ext_discovery_snapshot_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!d || !out) {
        return GT_ERR_INVALID_ARG;
    }
    if (d->count) {
        *out = d->pending[d->head];
        memset(&d->pending[d->head], 0, sizeof(d->pending[d->head]));
        d->head = (d->head + 1) % GT_EXT_DISCOVERY_PENDING_MAX;
        d->count--;
    }
    out->skipped = d->skipped;
    return GT_OK;
}

void gt_ext_discovery_close(gt_ext_discovery_t **d) {
    if (d && *d) {
        gt__plugin_memory_free(NULL, *d, sizeof(**d));
        *d = NULL;
    }
}
