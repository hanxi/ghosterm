/* Engine-independent private checkpoint container, shared by daemon and surfaces.
 * Only public Core POD types; no daemon, private Core, OS or UI dependencies. */
#ifndef GT_VT_CHECKPOINT_WIRE_H
#define GT_VT_CHECKPOINT_WIRE_H
#include "ghosterm/types.h"
enum {GT_VT_SNAPSHOT_HEADER=128,GT_VT_SNAPSHOT_LIMIT=64*1024*1024,
  GT_VT_SNAPSHOT_FORMAT=0x47545643,GT_VT_PROFILE_INLINE=1};
typedef struct {uint8_t background[3],foreground[3];} gt_vt_colors_t;
typedef struct {
  const uint8_t *canonical,*resources;
  size_t canonical_bytes,resource_bytes;
  uint32_t profile,cell_width,cell_height;
  uint64_t history_bytes,history_lines,continuation_bytes,image_bytes;
  gt_vt_colors_t light,dark;
  bool is_dark,history_pruned;
} gt_vt_snapshot_view_t;
/* No allocation. All sizes include the fixed header. Failure clears outputs.
 * Limit 1..64MiB; LIMIT/insufficient destination is IO, malformed local input
 * INVALID_ARG, unknown profile/version UNSUPPORTED, malformed wire PROTOCOL.
 * write requires disjoint destination and source segments. parse borrows input
 * until caller frees/reuses it. CRC is corruption detection, not authentication.
 * Parse validates complete extents, reserved bytes, policies and CRC before
 * publishing any pointers. Engine admission/format validation follows parsing. */
gt_status_t gt_vt_snapshot_size(const gt_vt_snapshot_view_t *,size_t limit,size_t *);
gt_status_t gt_vt_snapshot_write(const gt_vt_snapshot_view_t *,void *,size_t,size_t *);
gt_status_t gt_vt_snapshot_parse(const void *,size_t,size_t limit,gt_vt_snapshot_view_t *);
#endif
