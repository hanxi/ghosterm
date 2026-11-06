#include "android_graphics.h"

/* These tests exercise VT input/history only; no Android bitmap or JVM is involved. */
gat_graphics_cache *gat_graphics_new(void) {
    return (gat_graphics_cache *)1;
}

void gat_graphics_free(gat_graphics_cache *cache) {
    (void)cache;
}

bool gat_graphics_capture(gat_graphics_cache *cache,
                          GhosttyTerminal terminal,
                          uint16_t cw,
                          uint16_t ch,
                          gat_graphics_frame **frame,
                          bool *changed) {
    (void)cache;
    (void)terminal;
    (void)cw;
    (void)ch;
    *frame = NULL;
    *changed = false;
    return true;
}
