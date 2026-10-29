#ifndef GHOSTERM_ANDROID_SURFACE_H
#define GHOSTERM_ANDROID_SURFACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Engine-only seam. All calls belong to one IO owner; no transport/UI types. */
typedef struct gat_surface gat_surface;
typedef bool (*gat_write_fn)(void *, const uint8_t *, size_t);
gat_surface *gat_surface_new(gat_write_fn write, void *context, const char **error);
void gat_surface_free(gat_surface *surface);
void gat_surface_feed(gat_surface *surface, const uint8_t *bytes, size_t length);
bool gat_surface_resize(gat_surface *surface, uint16_t cols, uint16_t rows, uint16_t cw, uint16_t ch);
bool gat_surface_text(gat_surface *surface, const uint8_t *text, size_t length);
bool gat_surface_key(gat_surface *surface, int key, int mods, const uint8_t *text, size_t length);
void gat_surface_scroll(gat_surface *surface, int delta);
/* Caller owns returned allocation. Integers describe cells, never raw PTY output. */
bool gat_surface_frame(gat_surface *surface, int32_t **values, size_t *count);
#endif
