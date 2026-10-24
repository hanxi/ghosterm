#include "checkpoint-wire.h"
#include <string.h>

static uint64_t get(const uint8_t *p,size_t n) {
  uint64_t v=0;for(size_t i=0;i<n;i++)v=(v<<8)|p[i];return v;
}
static void put(uint8_t *p,uint64_t v,size_t n) {
  for(size_t i=0;i<n;i++)p[n-1-i]=(uint8_t)(v>>(8*i));
}
static uint32_t crc(const uint8_t *p,size_t n) {
  static const uint32_t table[]={0,0x1db71064,0x3b6e20c8,0x26d930ac,
    0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,0xedb88320,0xf00f9344,
    0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
  uint32_t v=UINT32_MAX;
  for(size_t i=0;i<n;i++) {
    /* The checksum field is zero for both writing and verification. */
    v^=(i>=124 && i<128)?0:p[i];
    v=(v>>4)^table[v&15];v=(v>>4)^table[v&15];
  }
  return ~v;
}
static bool metadata(const gt_vt_snapshot_view_t *v) {
  return v->cell_width && v->cell_height && v->cell_width<=65535 && v->cell_height<=65535 &&
    v->history_bytes && v->history_lines && v->continuation_bytes;
}
gt_status_t gt_vt_snapshot_size(const gt_vt_snapshot_view_t *v,size_t limit,size_t *out) {
  if(out)*out=0;
  if(!v || !out || !limit || limit>GT_VT_SNAPSHOT_LIMIT || !v->canonical || !v->resources ||
    !v->canonical_bytes || !v->resource_bytes || !metadata(v))return GT_ERR_INVALID_ARG;
  if(v->profile!=GT_VT_PROFILE_INLINE)return GT_ERR_UNSUPPORTED;
  if(v->canonical_bytes>GT_VT_SNAPSHOT_LIMIT-GT_VT_SNAPSHOT_HEADER ||
    v->resource_bytes>GT_VT_SNAPSHOT_LIMIT-GT_VT_SNAPSHOT_HEADER-v->canonical_bytes)return GT_ERR_IO;
  size_t n=GT_VT_SNAPSHOT_HEADER+v->canonical_bytes+v->resource_bytes;
  if(n>limit)return GT_ERR_IO;
  *out=n;return GT_OK;
}
gt_status_t gt_vt_snapshot_write(const gt_vt_snapshot_view_t *v,void *buffer,size_t capacity,size_t *out) {
  if(out)*out=0;
  if(!buffer || !out)return GT_ERR_INVALID_ARG;
  size_t n=0;gt_status_t rc=gt_vt_snapshot_size(v,GT_VT_SNAPSHOT_LIMIT,&n);
  if(rc!=GT_OK)return rc;
  if(n>capacity)return GT_ERR_IO;
  uint8_t *p=buffer;memset(p,0,GT_VT_SNAPSHOT_HEADER);
  memcpy(p,"GTVC",4);put(p+4,1,2);put(p+6,GT_VT_SNAPSHOT_HEADER,2);
  put(p+8,(v->is_dark?1u:0u)|(v->history_pruned?2u:0u),4);put(p+12,v->profile,4);
  put(p+16,v->canonical_bytes,8);put(p+24,v->resource_bytes,8);
  put(p+32,v->cell_width,4);put(p+36,v->cell_height,4);
  put(p+40,v->history_bytes,8);put(p+48,v->history_lines,8);
  put(p+56,v->continuation_bytes,8);put(p+64,v->image_bytes,8);
  memcpy(p+72,v->light.background,3);memcpy(p+75,v->light.foreground,3);
  memcpy(p+78,v->dark.background,3);memcpy(p+81,v->dark.foreground,3);
  memcpy(p+GT_VT_SNAPSHOT_HEADER,v->canonical,v->canonical_bytes);
  memcpy(p+GT_VT_SNAPSHOT_HEADER+v->canonical_bytes,v->resources,v->resource_bytes);
  put(p+124,crc(p,n),4);*out=n;return GT_OK;
}
gt_status_t gt_vt_snapshot_parse(const void *buffer,size_t n,size_t limit,gt_vt_snapshot_view_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!buffer || !out || !limit || limit>GT_VT_SNAPSHOT_LIMIT)return GT_ERR_INVALID_ARG;
  if(n<GT_VT_SNAPSHOT_HEADER || n>limit || n>GT_VT_SNAPSHOT_LIMIT)return GT_ERR_PROTOCOL;
  const uint8_t *p=buffer;
  if(memcmp(p,"GTVC",4))return GT_ERR_PROTOCOL;
  if(get(p+4,2)!=1)return GT_ERR_UNSUPPORTED;
  if(get(p+6,2)!=GT_VT_SNAPSHOT_HEADER || (get(p+8,4)&~UINT64_C(3)))return GT_ERR_PROTOCOL;
  for(size_t i=84;i<124;i++)if(p[i])return GT_ERR_PROTOCOL;
  if(get(p+12,4)!=GT_VT_PROFILE_INLINE)return GT_ERR_UNSUPPORTED;
  uint64_t canonical=get(p+16,8),resources=get(p+24,8);
  if(!canonical || !resources || canonical>n-GT_VT_SNAPSHOT_HEADER ||
    resources!=n-GT_VT_SNAPSHOT_HEADER-canonical)return GT_ERR_PROTOCOL;
  gt_vt_snapshot_view_t v={.profile=GT_VT_PROFILE_INLINE,.canonical_bytes=(size_t)canonical,
    .resource_bytes=(size_t)resources,.cell_width=(uint32_t)get(p+32,4),.cell_height=(uint32_t)get(p+36,4),
    .history_bytes=get(p+40,8),.history_lines=get(p+48,8),.continuation_bytes=get(p+56,8),.image_bytes=get(p+64,8),
    .is_dark=(p[11]&1)!=0,.history_pruned=(p[11]&2)!=0};
  if(!metadata(&v) || get(p+124,4)!=crc(p,n))return GT_ERR_PROTOCOL;
  memcpy(v.light.background,p+72,3);memcpy(v.light.foreground,p+75,3);
  memcpy(v.dark.background,p+78,3);memcpy(v.dark.foreground,p+81,3);
  v.canonical=p+GT_VT_SNAPSHOT_HEADER;v.resources=v.canonical+v.canonical_bytes;
  *out=v;return GT_OK;
}
