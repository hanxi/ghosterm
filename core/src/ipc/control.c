#include "control.h"
#include "user-wire.h"
#include "data-wire.h"
#include "base/secure-random.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static gt_control_status_t status(gt_manager_rc_t rc) {
  switch(rc) {
    case GT_MANAGER_OK:return GT_CONTROL_OK;
    case GT_MANAGER_NOT_FOUND:return GT_CONTROL_NOT_FOUND;
    case GT_MANAGER_BUSY:case GT_MANAGER_FULL:return GT_CONTROL_BUSY;
    default:return GT_CONTROL_INTERNAL;
  }
}
static bool nonzero(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
gt_control_rc_t gt__control_process(const gt_control_context_t *context,const gt_wire_frame_t *request,
  uint8_t *bytes,size_t capacity,gt_wire_header_t *reply) {
  if(reply)memset(reply,0,sizeof(*reply));
  if(!context || !context->manager || !context->principal || !context->issuer || !request || !reply || !request->header.request_id || request->header.flags ||
     (request->header.payload_length && !request->payload))return GT_CONTROL_MALFORMED;
  gt_session_manager_t *manager=context->manager;gt_creation_t *creation=context->creation;
  uint64_t principal=context->principal,now=context->now_ms;
  if(!bytes || capacity<GT_CONTROL_REPLY_BYTES)return GT_CONTROL_NO_SPACE;
  uint16_t type=request->header.type;uint32_t n=request->header.payload_length,length=0;
  uint64_t id=0;
  if(type==GT_MSG_INFO || type==GT_MSG_TERMINATE || type==GT_MSG_ATTACH) {
    if(n!=8 || !(id=get64(request->payload)))return GT_CONTROL_MALFORMED;
  } else if(type==GT_MSG_LIST || type==GT_MSG_PING) {
    if(n)return GT_CONTROL_MALFORMED;
  } else if(type==GT_MSG_CREATE || type==GT_MSG_QUERY_CREATE) {
    if((type==GT_MSG_CREATE?(n<GT_CREATE_PREFIX_BYTES || n>GT_CREATE_MAX_BYTES):n!=16) || !nonzero(request->payload,16))return GT_CONTROL_MALFORMED;
  } else if((type>=GT_MSG_SNAPSHOT_BEGIN && type<=GT_MSG_RESTORE_ERROR_ACK) || (type>=GT_MSG_USER_INPUT && type<=GT_MSG_USER_STYLE) || type<=6 || type==GT_MSG_LIST_REPLY || type==GT_MSG_INFO_REPLY ||
            type==GT_MSG_TERMINATE_REPLY || type==GT_MSG_PING_REPLY || type==GT_MSG_CONTROL_ERROR ||
            type==GT_MSG_CREATE_REPLY || type==GT_MSG_QUERY_CREATE_REPLY || type==GT_MSG_ATTACH_REPLY || type==GT_MSG_DETACH)
    return GT_CONTROL_MALFORMED;
  memset(bytes,0,GT_CONTROL_REPLY_BYTES);
  if(type==GT_MSG_ATTACH) {
    gt_attach_result_t r={.status=GT_ATTACH_UNSUPPORTED,.detail=GT_ERR_UNSUPPORTED,.session=id};
    if(context->attachments)(void)gt__attachment_owner_issue(context->attachments,principal,context->issuer,id,now,&r);
    if(!gt__control_attach_encode(&r,bytes)){gt__secure_erase(&r,sizeof(r));return GT_CONTROL_MALFORMED;}
    gt__secure_erase(&r,sizeof(r));length=GT_ATTACH_REPLY_BYTES;type=GT_MSG_ATTACH_REPLY;
  } else if(type==GT_MSG_CREATE || type==GT_MSG_QUERY_CREATE) {
    gt_creation_result_t r=creation?(type==GT_MSG_CREATE?
      gt__creation_submit(creation,principal,request->payload,n,now):gt__creation_query(creation,principal,request->payload,now)):
      (gt_creation_result_t){GT_CREATION_UNSUPPORTED,GT_ERR_UNSUPPORTED,0};
    if(r.status==GT_CREATION_INVALID && r.detail==GT_ERR_PROTOCOL)return GT_CONTROL_MALFORMED;
    memcpy(bytes,request->payload,16);put32(bytes+16,(uint32_t)r.status);put32(bytes+20,(uint32_t)r.detail);put64(bytes+24,r.session);
    length=GT_CREATE_REPLY_BYTES;type=type==GT_MSG_CREATE?GT_MSG_CREATE_REPLY:GT_MSG_QUERY_CREATE_REPLY;
  } else if(type==GT_MSG_LIST) {
    gt_managed_info_t rows[GT_CONTROL_MAX_ROWS];size_t count=0;
    gt_manager_rc_t rc=gt__manager_list(manager,rows,GT_CONTROL_MAX_ROWS,&count);
    put32(bytes,status(rc));length=8;type=GT_MSG_LIST_REPLY;
    if(rc==GT_MANAGER_OK) {
      for(size_t i=0;i<count;i++)if(!gt__control_info_encode(&rows[i],bytes+8+i*GT_CONTROL_INFO_BYTES)) {
        memset(bytes,0,GT_CONTROL_REPLY_BYTES);put32(bytes,GT_CONTROL_INTERNAL);count=0;break;
      }
      put32(bytes+4,(uint32_t)count);length+=(uint32_t)count*GT_CONTROL_INFO_BYTES;
    }
  } else if(type==GT_MSG_INFO) {
    gt_managed_info_t row;gt_manager_rc_t rc=gt__manager_info(manager,id,&row);
    put32(bytes,status(rc));length=4+GT_CONTROL_INFO_BYTES;type=GT_MSG_INFO_REPLY;
    if(rc==GT_MANAGER_OK && !gt__control_info_encode(&row,bytes+4))put32(bytes,GT_CONTROL_INTERNAL);
  } else if(type==GT_MSG_TERMINATE) {
    put32(bytes,status(gt__manager_terminate(manager,id)));put64(bytes+4,id);length=12;type=GT_MSG_TERMINATE_REPLY;
  } else if(type==GT_MSG_PING)type=GT_MSG_PING_REPLY;
  else{put32(bytes,GT_CONTROL_UNSUPPORTED);length=4;type=GT_MSG_CONTROL_ERROR;}
  *reply=(gt_wire_header_t){.type=type,.payload_length=length,.request_id=request->header.request_id};
  return GT_CONTROL_REPLY;
}
