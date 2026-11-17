#define _POSIX_C_SOURCE 200809L
#include "foreground.h"
#include "stdio.h"
#include "ghosterm.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

typedef struct {
    const char *runtime, *executor, *client, *command;
    const char *arguments[32], *methods[16], *host_methods[16], *capabilities[4];
    size_t argument_count, method_count, host_count, capability_count;
} declaration_t;

static bool utf8(const unsigned char *s, size_t size) {
    for (size_t i = 0; i < size;) {
        uint32_t c = s[i++], minimum;
        size_t count;
        if (c < 0x80) {
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            minimum = 0x80;
            c &= 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            minimum = 0x800;
            c &= 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            minimum = 0x10000;
            c &= 7;
        } else {
            return false;
        }
        if (count > size - i) {
            return false;
        }
        while (count--) {
            unsigned char part = s[i++];
            if ((part & 0xc0) != 0x80) {
                return false;
            }
            c = c << 6 | (part & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static bool digest(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

static bool method(const char *s) {
    size_t n = strlen(s);
    return n > 0 && n <= 128 &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == n;
}

static bool valid(const declaration_t *d) {
    if (!d->runtime ||
        (strcmp(d->runtime, "portable-js") && strcmp(d->runtime, "node") &&
         strcmp(d->runtime, "exec")) ||
        !digest(d->executor) || !digest(d->client) || !d->command || !*d->command ||
        strlen(d->command) > 519 || d->argument_count > 32 ||
        d->method_count + d->host_count > 16 || d->capability_count > 4) {
        return false;
    }
    size_t total = strlen(d->command) + 1;
    for (size_t i = 0; i < d->argument_count; i++) {
        size_t n = strlen(d->arguments[i]);
        if (n > 4096 || n + 1 > 16384 - total) {
            return false;
        }
        total += n + 1;
    }
    const char *all[16];
    size_t count = 0;
    for (size_t i = 0; i < d->method_count + d->host_count; i++) {
        const char *s = i < d->method_count ? d->methods[i] : d->host_methods[i - d->method_count];
        if (!method(s)) {
            return false;
        }
        for (size_t j = 0; j < count; j++) {
            if (!strcmp(all[j], s)) {
                return false;
            }
        }
        all[count++] = s;
    }
    for (size_t i = 0; i < d->capability_count; i++) {
        const char *s = d->capabilities[i];
        if (strcmp(s, "ui.table") && strcmp(s, "ui.diff") && strcmp(s, "ui.task") &&
            strcmp(s, "ui.views")) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(d->capabilities[j], s)) {
                return false;
            }
        }
    }
    return true;
}

typedef struct {
    unsigned char *bytes;
    size_t size, at;
    bool reading;
} buffer_t;

static bool number(buffer_t *b, size_t *value) {
    if (b->size - b->at < 4 || *value > UINT32_MAX) {
        return false;
    }
    if (b->reading) {
        *value = 0;
    }
    for (size_t i = 0; i < 4; i++) {
        if (b->reading) {
            *value = *value << 8 | b->bytes[b->at++];
        } else {
            b->bytes[b->at++] = (unsigned char)(*value >> (24 - i * 8));
        }
    }
    return true;
}

static bool string(buffer_t *b, const char **s) {
    size_t n = b->reading ? 0 : strlen(*s) + 1;
    if (!number(b, &n) || n < 1 || n > 4097 || n > b->size - b->at) {
        return false;
    }
    if (!b->reading) {
        memcpy(b->bytes + b->at, *s, n);
    }
    unsigned char *p = b->bytes + b->at;
    if (p[n - 1] || memchr(p, 0, n - 1) || !utf8(p, n - 1)) {
        return false;
    }
    *s = (const char *)p;
    b->at += n;
    return true;
}

static bool vector(buffer_t *b, const char **s, size_t *n, size_t maximum) {
    if (!number(b, n) || *n > maximum) {
        return false;
    }
    for (size_t i = 0; i < *n; i++) {
        if (!string(b, s + i)) {
            return false;
        }
    }
    return true;
}

static bool wire(buffer_t *b, declaration_t *d) {
    return string(b, &d->runtime) && string(b, &d->executor) && string(b, &d->client) &&
           string(b, &d->command) && vector(b, d->arguments, &d->argument_count, 32) &&
           vector(b, d->methods, &d->method_count, 16) &&
           vector(b, d->host_methods, &d->host_count, 16) &&
           vector(b, d->capabilities, &d->capability_count, 4) && valid(d);
}

bool gterm_foreground_encode(int argc, char **argv, unsigned char *bytes, size_t *size) {
    int boundary = gterm_stdio_invocation_index(argc, argv);
    if (boundary < 0 || gterm_stdio_validate(argc, argv)) {
        return false;
    }
    declaration_t d = {.runtime = "portable-js"};
    d.command = argv[boundary + 2];
    d.argument_count = (size_t)(argc - boundary - 3);
    if (d.argument_count > 32) {
        return false;
    }
    for (size_t i = 0; i < d.argument_count; i++) {
        d.arguments[i] = argv[boundary + 3 + i];
    }
    for (int at = 0; at < boundary;) {
        const char *option = argv[at], *value = argv[at + 1];
        if (!strcmp(option, "--runtime")) {
            d.runtime = value;
        } else if (!strcmp(option, "--trust-development-digest")) {
            d.executor = value;
        } else if (!strcmp(option, "--client-digest")) {
            d.client = value;
        } else if (!strcmp(option, "--method")) {
            if (d.method_count == 16) {
                return false;
            }
            d.methods[d.method_count++] = value;
        } else if (!strcmp(option, "--host-method")) {
            if (d.host_count == 16) {
                return false;
            }
            d.host_methods[d.host_count++] = value;
        } else if (!strcmp(option, "--client-capability")) {
            if (d.capability_count == 4) {
                return false;
            }
            d.capabilities[d.capability_count++] = value;
        }
        at += gterm_stdio_option_arity(option) + 1;
    }
    if (!valid(&d)) {
        return false;
    }
    buffer_t b = {bytes, GTERM_FOREGROUND_MAX, 0, false};
    if (!wire(&b, &d)) {
        return false;
    }
    *size = b.at;
    return true;
}

static void quoted(FILE *f, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0x80) {
            unsigned n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            c &= n == 1 ? 0x1f : n == 2 ? 0x0f : 7;
            for (unsigned i = 0; i < n; i++) {
                c = c << 6 | (*p++ & 63);
            }
        }
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c < 32 || c >= 127) {
            if (c <= 0xffff) {
                fprintf(f, "\\u%04x", (unsigned)c);
            } else {
                c -= 0x10000;
                fprintf(f,
                        "\\u%04x\\u%04x",
                        (unsigned)(0xd800 + (c >> 10)),
                        (unsigned)(0xdc00 + (c & 1023)));
            }
        } else {
            fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static void array(FILE *f, const char *const *s, size_t n) {
    fputc('[', f);
    for (size_t i = 0; i < n; i++) {
        if (i) {
            fputc(',', f);
        }
        quoted(f, s[i]);
    }
    fputc(']', f);
}

char *
gterm_foreground_json(unsigned char *bytes, size_t size, const char *instance, size_t *length) {
    if (size > GTERM_FOREGROUND_MAX || strlen(instance) != 32 ||
        strspn(instance, "0123456789abcdef") != 32) {
        return NULL;
    }
    buffer_t b = {bytes, size, 0, true};
    declaration_t d = {0};
    if (!wire(&b, &d) || b.at != size) {
        return NULL;
    }
    char *json = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&json, &n);
    if (!f) {
        return NULL;
    }
    fputs("{\"schemaVersion\":1,\"instance\":", f);
    quoted(f, instance);
    fputs(",\"runtime\":", f);
    quoted(f, d.runtime);
    fputs(",\"executorDigest\":", f);
    quoted(f, d.executor);
    fputs(",\"clientDigest\":", f);
    quoted(f, d.client);
    fputs(",\"command\":", f);
    quoted(f, d.command);
    fputs(",\"arguments\":", f);
    array(f, d.arguments, d.argument_count);
    fputs(",\"methods\":", f);
    array(f, d.methods, d.method_count);
    fputs(",\"hostMethods\":", f);
    array(f, d.host_methods, d.host_count);
    fputs(",\"clientCapabilities\":", f);
    array(f, d.capabilities, d.capability_count);
    fputs(",\"authorizationGranted\":false}\n", f);
    bool ok = !ferror(f);
    if (fclose(f)) {
        ok = false;
    }
    if (!ok || n > 153600) {
        free(json);
        return NULL;
    }
    *length = n;
    return json;
}

bool gterm_foreground_announce(const char *directory,
                               const unsigned char *instance,
                               int argc,
                               char **argv) {
    size_t n = strlen(directory);
    int boundary = gterm_stdio_invocation_index(argc, argv);
    const char *client = NULL;
    if (boundary < 0 || n > GT_EXT_DISCOVERY_DIRECTORY_MAX ||
        !utf8((const unsigned char *)directory, n)) {
        return false;
    }
    for (int at = 0; at < boundary;) {
        if (!strcmp(argv[at], "--client-digest")) {
            if (client) {
                return false;
            }
            client = argv[at + 1];
        }
        at += gterm_stdio_option_arity(argv[at]) + 1;
    }
    if (!digest(client)) {
        return false;
    }
    const char *setting = getenv("GTERM_DISCOVERY_TMUX_LAYERS"), *tmux = getenv("TMUX");
    unsigned layers = tmux && *tmux ? 1 : 0;
    if (setting) {
        if (strlen(setting) != 1 || setting[0] < '0' || setting[0] > '4') {
            fputs("gterm: discovery hint unavailable: tmux layers must be 0..4\n", stderr);
            return false;
        }
        layers = (unsigned)(setting[0] - '0');
    }
    int fd = open("/dev/tty", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    if (!isatty(fd) || tcgetpgrp(fd) != getpgrp()) {
        close(fd);
        return false;
    }
    unsigned char raw[50 + GT_EXT_DISCOVERY_DIRECTORY_MAX];
    memcpy(raw, instance, 16);
    for (size_t i = 0; i < 32; i++) {
        unsigned a =
            (unsigned)(client[i * 2] <= '9' ? client[i * 2] - '0' : client[i * 2] - 'a' + 10);
        unsigned b = (unsigned)(client[i * 2 + 1] <= '9' ? client[i * 2 + 1] - '0'
                                                         : client[i * 2 + 1] - 'a' + 10);
        raw[16 + i] = (unsigned char)(a * 16 + b);
    }
    raw[48] = (unsigned char)(n >> 8);
    raw[49] = (unsigned char)n;
    memcpy(raw + 50, directory, n);
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char frame[2048], wrapped[2048];
    static const char tag[] = "\033]777;ghosterm-discovery;1;";
    size_t used = sizeof(tag) - 1;
    memcpy(frame, tag, used);
    for (size_t at = 0; at < n + 50; at += 3) {
        size_t left = n + 50 - at;
        unsigned a = raw[at], b = left > 1 ? raw[at + 1] : 0, c = left > 2 ? raw[at + 2] : 0;
        frame[used++] = (unsigned char)alphabet[a >> 2];
        frame[used++] = (unsigned char)alphabet[((a & 3) << 4) | (b >> 4)];
        frame[used++] = left > 1 ? (unsigned char)alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        frame[used++] = left > 2 ? (unsigned char)alphabet[c & 63] : '=';
    }
    frame[used++] = 0x1b;
    frame[used++] = '\\';
    for (unsigned layer = 0; layer < layers; layer++) {
        size_t at = 7;
        memcpy(wrapped, "\033Ptmux;", at);
        for (size_t i = 0; i < used; i++) {
            if (at + 4 > sizeof(wrapped)) {
                close(fd);
                return false;
            }
            if (frame[i] == 0x1b) {
                wrapped[at++] = 0x1b;
            }
            wrapped[at++] = frame[i];
        }
        wrapped[at++] = 0x1b;
        wrapped[at++] = '\\';
        used = at;
        memcpy(frame, wrapped, used);
    }
    /* One bounded nonblocking write, without borrowing the shell's fd flags or
     * termios. CAN is best-effort repair for a partial control string. */
    ssize_t sent = write(fd, frame, used);
    bool ok = sent >= 0 && (size_t)sent == used;
    if (sent > 0 && !ok) {
        unsigned char cancel = 0x18;
        (void)write(fd, &cancel, 1);
    }
    if (close(fd)) {
        ok = false;
    }
    return ok;
}
