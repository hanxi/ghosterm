#include "data-wire.h"
#include "session/process-meta.h"
#include <string.h>
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v=(v<<8)|p[i];return v;}
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=n;i;i--){p[i-1]=(uint8_t)v;v>>=8;}}
static size_t length(const gt_data_message_t *m) {
  if(!m || !m->generation)return 0;
  switch(m->type) {
  case GT_MSG_PROCESS_META:return gt__process_valid(&m->process)?20+strlen(m->process.cwd):0;
  case GT_MSG_SNAPSHOT_BEGIN:return m->total && m->total<=GT_DATA_MAX_SNAPSHOT && m->format && !(m->flags&~GT_DATA_READONLY)?40:0;
  case GT_MSG_SNAPSHOT_CHUNK:return m->bytes && m->length && m->length<=GT_DATA_MAX_CHUNK && m->offset<=GT_DATA_MAX_SNAPSHOT && m->length<=GT_DATA_MAX_SNAPSHOT-m->offset?16+m->length:0;
  case GT_MSG_SNAPSHOT_END:return m->total && m->total<=GT_DATA_MAX_SNAPSHOT?24:0;
  case GT_MSG_READY_S:case GT_MSG_LIVE_BEGIN:case GT_MSG_READY_L:return 16;
  case GT_MSG_DATA_PING:case GT_MSG_DATA_PONG:return m->nonce?16:0;
  case GT_MSG_STATE_EVENT:
    if(!m->sequence)return 0;
    if(m->kind==GT_STATE_OUTPUT)return m->bytes && m->length && m->length<=GT_DATA_MAX_OUTPUT?24+m->length:0;
    if(m->kind==GT_STATE_RESIZE)return m->size.cols && m->size.rows?36:0;
    if(m->kind==GT_STATE_STYLE)return m->style.dark<=1?805:0;
    if(m->kind==GT_STATE_COLOR_SCHEME)return m->color.dark<=1?28:0;
    return 0;
  case GT_MSG_SESSION_END:return !(m->flags&~GT_DATA_NO_SNAPSHOT) && (unsigned)m->error<=GT_ERR_INTERNAL &&
    (((m->flags&GT_DATA_NO_SNAPSHOT)!=0)==(m->error!=GT_OK)) && m->terminal.has_result &&
    ((m->flags&GT_DATA_NO_SNAPSHOT) || m->terminal.result.has_sequence)?80:0;
  case GT_MSG_RESTORE_ERROR:case GT_MSG_RESTORE_ERROR_ACK:return m->error>GT_OK && m->error<=GT_ERR_INTERNAL &&
    m->stage>=GT_DATA_ERROR_CAPTURE && m->stage<=GT_DATA_ERROR_PROTOCOL?16:0;
  default:return 0;
  }
}
bool gt__data_encode(const gt_data_message_t *m,void *bytes,size_t cap,gt_wire_header_t *h) {
  if(h)memset(h,0,sizeof(*h));
  size_t n=length(m);if(!h || !bytes || !n || n>cap)return false;
  uint8_t *p=bytes;memset(p,0,n);put(p,m->generation,8);
  switch(m->type) {
  case GT_MSG_PROCESS_META:put(p+8,m->process.has_cwd?1:0,4);put(p+12,m->process.child,4);put(p+16,n-20,4);memcpy(p+20,m->process.cwd,n-20);break;
  case GT_MSG_SNAPSHOT_BEGIN:put(p+8,m->sequence,8);put(p+16,m->total,8);put(p+24,m->format,4);put(p+28,m->flags,4);break;
  case GT_MSG_SNAPSHOT_CHUNK:put(p+8,m->offset,8);memcpy(p+16,m->bytes,m->length);break;
  case GT_MSG_SNAPSHOT_END:put(p+8,m->sequence,8);put(p+16,m->total,8);break;
  case GT_MSG_READY_S:case GT_MSG_LIVE_BEGIN:case GT_MSG_READY_L:put(p+8,m->sequence,8);break;
  case GT_MSG_DATA_PING:case GT_MSG_DATA_PONG:put(p+8,m->nonce,8);break;
  case GT_MSG_STATE_EVENT:
    put(p+8,m->sequence,8);put(p+16,m->kind,4);put(p+20,n-24,4);
    if(m->kind==GT_STATE_OUTPUT)memcpy(p+24,m->bytes,m->length);
    else if(m->kind==GT_STATE_RESIZE){put(p+24,m->size.cols,2);put(p+26,m->size.rows,2);put(p+28,m->size.px_width,4);put(p+32,m->size.px_height,4);}
    else if(m->kind==GT_STATE_STYLE){put(p+24,m->style.dark,4);memcpy(p+28,m->style.background,3);memcpy(p+31,m->style.foreground,3);memcpy(p+34,m->style.cursor,3);memcpy(p+37,m->style.palette,768);}
    else put(p+24,m->color.dark,4);
    break;
  case GT_MSG_SESSION_END:put(p+8,m->flags,4);put(p+12,m->error,4);if(!gt__control_info_encode(&m->terminal,p+16))return false;break;
  default:put(p+8,m->error,4);put(p+12,m->stage,4);break;
  }
  *h=(gt_wire_header_t){.type=m->type,.payload_length=(uint32_t)n};return true;
}
bool gt__data_decode(const gt_wire_frame_t *f,gt_data_message_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!out || !f || !f->payload || f->header.flags || f->header.request_id || f->header.payload_length<8)return false;
  const uint8_t *p=f->payload;size_t n=f->header.payload_length;gt_data_message_t m={.type=f->header.type,.generation=get(p,8)};
  switch(m.type) {
  case GT_MSG_PROCESS_META:
    if(n<20 || n>1043 || get(p+8,4)>1 || get(p+16,4)!=n-20 || ((get(p+8,4)!=0)!=(n>20)))return false;
    m.process.has_cwd=get(p+8,4)!=0;m.process.child=(gt_transport_child_t)get(p+12,4);
    if(memchr(p+20,0,n-20))return false;
    memcpy(m.process.cwd,p+20,n-20);break;
  case GT_MSG_SNAPSHOT_BEGIN:if(n!=40 || get(p+32,8))return false;m.sequence=get(p+8,8);m.total=get(p+16,8);m.format=(uint32_t)get(p+24,4);m.flags=(uint32_t)get(p+28,4);break;
  case GT_MSG_SNAPSHOT_CHUNK:if(n<=16 || n>16+GT_DATA_MAX_CHUNK)return false;m.offset=get(p+8,8);m.bytes=p+16;m.length=n-16;break;
  case GT_MSG_SNAPSHOT_END:if(n!=24)return false;m.sequence=get(p+8,8);m.total=get(p+16,8);break;
  case GT_MSG_READY_S:case GT_MSG_LIVE_BEGIN:case GT_MSG_READY_L:if(n!=16)return false;m.sequence=get(p+8,8);break;
  case GT_MSG_DATA_PING:case GT_MSG_DATA_PONG:if(n!=16)return false;m.nonce=get(p+8,8);break;
  case GT_MSG_STATE_EVENT:
    if(n<24 || get(p+20,4)!=n-24)return false;
    m.sequence=get(p+8,8);m.kind=(uint32_t)get(p+16,4);
    if(m.kind==GT_STATE_OUTPUT){m.bytes=p+24;m.length=n-24;}
    else if(m.kind==GT_STATE_RESIZE){if(n!=36)return false;m.size=(gt_state_size_t){(uint16_t)get(p+24,2),(uint16_t)get(p+26,2),(uint32_t)get(p+28,4),(uint32_t)get(p+32,4)};}
    else if(m.kind==GT_STATE_STYLE){if(n!=805)return false;m.style.dark=(uint32_t)get(p+24,4);memcpy(m.style.background,p+28,3);memcpy(m.style.foreground,p+31,3);memcpy(m.style.cursor,p+34,3);memcpy(m.style.palette,p+37,768);}
    else if(m.kind==GT_STATE_COLOR_SCHEME){if(n!=28)return false;m.color.dark=(uint32_t)get(p+24,4);}
    break;
  case GT_MSG_SESSION_END:if(n!=80 || !gt__control_info_decode(p+16,64,&m.terminal))return false;m.flags=(uint32_t)get(p+8,4);m.error=(gt_status_t)get(p+12,4);break;
  case GT_MSG_RESTORE_ERROR:case GT_MSG_RESTORE_ERROR_ACK:if(n!=16)return false;m.error=(gt_status_t)get(p+8,4);m.stage=(gt_data_error_stage_t)get(p+12,4);break;
  default:return false;
  }
  if(length(&m)!=n)return false;
  *out=m;return true;
}
