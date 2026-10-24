#include "vt-snapshot.h"
#include <string.h>
typedef struct {
  gt_snapshot_blob_t blob;
  gt_allocator_t domain;
  size_t charge;
  uint8_t bytes[];
} vt_blob;
static void release_blob(void *context,gt_snapshot_blob_t *blob) {
  (void)context;if(!blob)return;
  vt_blob *b=(vt_blob *)blob;gt_allocator_t a=b->domain;size_t n=b->charge;
  /* Owned terminal contents must not remain in the caller's allocator cache. */
  volatile uint8_t *p=(volatile uint8_t *)b;for(size_t i=0;i<n;i++)p[i]=0;
  a.release(a.context,b,n);
}
static gt_status_t encode(void *context,const void *frozen,size_t limit,
  const gt_allocator_t *domain,gt_snapshot_blob_t **out) {
  if(out)*out=NULL;
  if(!context || !frozen || !out || !domain || !domain->allocate || !domain->release ||
    !limit || limit>GT_VT_SNAPSHOT_LIMIT)return GT_ERR_INVALID_ARG;
  if(limit<=GT_VT_SNAPSHOT_HEADER)return GT_ERR_IO;
  gt_vt_encoded_t *encoded=NULL;
  gt_status_t rc=gt__vt_checkpoint_encode(context,frozen,limit-GT_VT_SNAPSHOT_HEADER,&encoded);
  if(rc!=GT_OK)return rc;
  gt_vt_snapshot_view_t view=gt__vt_encoded_view(encoded);size_t n=0;
  rc=gt_vt_snapshot_size(&view,limit,&n);
  vt_blob *b=NULL;
  if(rc==GT_OK) {
    b=domain->allocate(domain->context,sizeof(*b)+n);
    if(!b)rc=GT_ERR_IO;
    else {
      b->domain=*domain;b->charge=sizeof(*b)+n;b->blob=(gt_snapshot_blob_t){b->bytes,n};
      size_t written=0;rc=gt_vt_snapshot_write(&view,b->bytes,n,&written);
      if(rc==GT_OK && written!=n)rc=GT_ERR_INTERNAL;
    }
  }
  gt__vt_encoded_release(encoded);
  if(rc!=GT_OK){if(b)release_blob(context,&b->blob);return rc;}
  *out=&b->blob;return GT_OK;
}
gt_snapshot_encoder_t gt__vt_snapshot_encoder(gt_vt_factory_t *f) {
  return f?(gt_snapshot_encoder_t){f,encode,release_blob}:(gt_snapshot_encoder_t){0};
}
