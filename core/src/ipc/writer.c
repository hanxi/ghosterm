#include "writer.h"
#include <string.h>
void gt__wire_writer_init(gt_wire_writer_t *w) {
  if(w)memset(w,0,sizeof(*w));
}
bool gt__wire_writer_begin(gt_wire_writer_t *w,const gt_wire_header_t *h,const void *payload) {
  if(!w || !h || w->state==GT_WRITER_PENDING || w->state==GT_WRITER_FAILED ||
     (h->payload_length && !payload))return false;
  uint8_t header[GT_WIRE_HEADER_BYTES];
  if(!gt__wire_header_encode(h,header))return false;
  memcpy(w->envelope,header,sizeof(header));w->payload=payload;
  w->payload_length=h->payload_length;w->offset=0;w->state=GT_WRITER_PENDING;return true;
}
bool gt__wire_writer_view(const gt_wire_writer_t *w,const uint8_t **bytes,size_t *length) {
  if(bytes)*bytes=NULL;
  if(length)*length=0;
  if(!w || !bytes || !length || w->state!=GT_WRITER_PENDING)return false;
  if(w->offset<GT_WIRE_HEADER_BYTES) {
    *bytes=w->envelope+w->offset;*length=GT_WIRE_HEADER_BYTES-w->offset;
  } else {
    size_t sent=w->offset-GT_WIRE_HEADER_BYTES;
    *bytes=w->payload+sent;*length=w->payload_length-sent;
  }
  return true;
}
void gt__wire_writer_cancel(gt_wire_writer_t *w) {
  if(w){w->state=GT_WRITER_FAILED;w->payload=NULL;w->payload_length=0;}
}
bool gt__wire_writer_advance(gt_wire_writer_t *w,size_t written) {
  const uint8_t *bytes;size_t length;
  if(!gt__wire_writer_view(w,&bytes,&length))return false;
  if(written>length){gt__wire_writer_cancel(w);return false;}
  w->offset+=written;
  if(w->offset==GT_WIRE_HEADER_BYTES+w->payload_length) {
    w->state=GT_WRITER_DONE;w->payload=NULL;
  }
  return true;
}
