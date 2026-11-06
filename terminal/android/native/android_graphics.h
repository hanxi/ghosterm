#ifndef GHOSTERM_ANDROID_GRAPHICS_H
#define GHOSTERM_ANDROID_GRAPHICS_H
#include <jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <ghostty/vt.h>

typedef struct gat_graphics_cache gat_graphics_cache;
typedef struct gat_graphics_frame gat_graphics_frame;
gat_graphics_cache *gat_graphics_new(void);
void gat_graphics_free(gat_graphics_cache *cache);
/* VT owner only. changed=false returns no packet and retains the prior cache. */
bool gat_graphics_capture(gat_graphics_cache *cache,
                          GhosttyTerminal terminal,
                          uint16_t cw,
                          uint16_t ch,
                          gat_graphics_frame **frame,
                          bool *changed);
void gat_graphics_frame_retain(gat_graphics_frame *frame);
void gat_graphics_frame_free(gat_graphics_frame *frame);
/* Reads only immutable cached data; no VT call or pixel conversion on this caller. */
jobjectArray gat_graphics_frame_java(JNIEnv *env, gat_graphics_frame *frame, uint64_t revision);
void gat_graphics_metrics(int64_t *bitmaps, int64_t *frames);
#endif
