#include "surface.h"
#include "android_graphics.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char output[4096];
static size_t used;
static bool reject;
static bool write_bytes(void *context, const uint8_t *bytes, size_t length) {
    (void)context;
    if (reject) return false;
    assert(used + length <= sizeof(output));
    memcpy(output + used, bytes, length); used += length; return true;
}
static void feed(gat_surface *surface, const char *text) { gat_surface_feed(surface, (const uint8_t *)text, strlen(text)); }
static void expect(const char *text) {
    if (used != strlen(text) || memcmp(output, text, used)) {
        fprintf(stderr, "expected %zu bytes, got %zu:", strlen(text), used);
        for (size_t i = 0; i < used; i++) fprintf(stderr, " %02x", output[i]);
        fputc('\n', stderr); abort();
    }
    used = 0;
}
static int32_t *frame(gat_surface *surface, size_t *count) {
    int32_t *result = NULL; assert(gat_surface_frame(surface, &result, count)); return result;
}
int main(int argc, char **argv) {
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--history")));
    const char *error;
    gat_surface *surface = gat_surface_new(write_bytes, NULL, &error); assert(surface);
    assert(gat_surface_resize(surface, 20, 5, 8, 16));
    // Ordinary shell: wheel moves history and sends no terminal input.
    for (int i = 0; i < 30; i++) { char line[32]; snprintf(line, sizeof(line), "line-%02d\r\n", i); feed(surface, line); }
    size_t count1, count2;
    int32_t *before = frame(surface, &count1);
    assert(gat_surface_wheel(surface, -5, 16, 48, 0));
    int32_t *after = frame(surface, &count2);
    assert(count1 != count2 || memcmp(before, after, count1 * sizeof(*before)));
    expect(""); free(before); free(after);
    if (argc == 2) { gat_surface_free(surface); puts("HISTORY CONTROL PASS"); return 0; }
    // SGR wheel: correct button, modifiers and pointer cell; never a release packet.
    feed(surface, "\033[?1000h\033[?1006h");
    assert(gat_surface_wheel(surface, -2, 16, 48, 0)); expect("\033[<64;3;4M\033[<64;3;4M");
    assert(gat_surface_wheel(surface, 1, 16, 48, 1)); expect("\033[<69;3;4M");
    assert(gat_surface_wheel(surface, -1, 65535, 65535, 0)); expect("\033[<64;20;5M");
    // Legacy/X10 encodings are selected by VT state, not hard-coded SGR bytes.
    feed(surface, "\033[?1006l");
    assert(gat_surface_wheel(surface, 1, 16, 48, 0)); expect("\033[Ma#$");
    feed(surface, "\033[?1000l\033[?9h");
    assert(gat_surface_wheel(surface, -1, 16, 48, 0)); expect(""); // X10 explicitly excludes wheel buttons.
    // Alternate screen uses the key encoder, including application cursor mode.
    feed(surface, "\033[?9l\033[?1049h\033[?1007h");
    assert(gat_surface_wheel(surface, -2, 0, 0, 0)); expect("\033[A\033[A");
    feed(surface, "\033[?1h");
    assert(gat_surface_wheel(surface, 1, 0, 0, 0)); expect("\033OB");
    feed(surface, "\033[?1007l");
    assert(gat_surface_wheel(surface, -1, 0, 0, 0)); expect("");
    // Mouse reporting takes priority over alternate-scroll arrows.
    feed(surface, "\033[?1000h\033[?1006h\033[?1007h");
    assert(gat_surface_wheel(surface, -1, 0, 0, 0)); expect("\033[<64;1;1M");
    assert(!gat_surface_wheel(surface, 65, 0, 0, 0));
    assert(!gat_surface_wheel(surface, -65, 0, 0, 0));
    assert(!gat_surface_wheel(surface, 1, -1, 0, 0));
    assert(gat_surface_wheel(surface, 0, 0, 0, 0)); expect("");
    reject = true; assert(!gat_surface_wheel(surface, 1, 0, 0, 0)); reject = false;
    gat_surface_free(surface);
    puts("ANDROID SCROLL VT PASS: history / SGR / legacy / X10 / alternate / cursor / limits / write failure");
}
