#include "connection.h"
#include "writer.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
struct gt_connection {
  gt_allocator_t allocator;
  gt_stream_ops_t ops;
  gt_connection_limits_t limits;
  gt_connection_state_t state;
  int system_error;
  uint64_t now,receive_started,send_started;
  bool receiving;
  gt_wire_decoder_t decoder;
  gt_wire_writer_t writer;
  uint8_t scratch[4096];
  uint8_t storage[];
};
static void *allocate(void *context,size_t n){(void)context;return malloc(n);}
bool gt__connection_limits(const gt_connection_t *c,gt_connection_limits_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!c || !out)return false;
  *out=c->limits;return true;
}
static void release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
static void finish(gt_connection_t *c,gt_connection_state_t state,int error) {
  if(c->state!=GT_CONNECTION_OPEN)return;
  c->state=state;c->system_error=error;
  gt__wire_writer_cancel(&c->writer);c->decoder.failed=true;c->receiving=false;
  gt__secure_erase(c->storage,(size_t)c->limits.receive_limit+c->limits.send_limit);
  gt__secure_erase(c->scratch,sizeof(c->scratch));
  c->ops.close(c->ops.context);
}
static bool check_time(gt_connection_t *c) {
  if(c->state!=GT_CONNECTION_OPEN)return false;
  uint64_t now;
  if(!c->ops.clock(c->ops.context,&now) || now<c->now) {
    finish(c,GT_CONNECTION_CLOCK_ERROR,0);return false;
  }
  c->now=now;
  if((c->receiving && now-c->receive_started>=c->limits.frame_timeout_ms) ||
     (c->writer.state==GT_WRITER_PENDING && now-c->send_started>=c->limits.frame_timeout_ms)) {
    finish(c,GT_CONNECTION_TIMEOUT,0);return false;
  }
  return true;
}
gt_connection_open_t gt__connection_open(const gt_stream_ops_t *ops,gt_connection_limits_t limits,
  const gt_allocator_t *allocator,gt_connection_t **out) {
  if(out)*out=NULL;
  if(!out || !ops || !ops->read || !ops->write || !ops->clock || !ops->close ||
     limits.receive_limit>GT_WIRE_MAX_PAYLOAD || limits.send_limit>GT_WIRE_MAX_PAYLOAD ||
     !limits.frame_timeout_ms || limits.frame_timeout_ms>60000)return GT_CONNECTION_INVALID;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};
  if(!a.allocate || !a.release)return GT_CONNECTION_INVALID;
  size_t bytes=sizeof(gt_connection_t)+(size_t)limits.receive_limit+limits.send_limit;
  gt_connection_t *c=a.allocate(a.context,bytes);if(!c)return GT_CONNECTION_NO_MEMORY;
  memset(c,0,sizeof(*c));c->allocator=a;c->ops=*ops;c->limits=limits;
  if(!ops->clock(ops->context,&c->now)){a.release(a.context,c,bytes);return GT_CONNECTION_NO_CLOCK;}
  if(!gt__wire_decoder_init(&c->decoder,c->storage,limits.receive_limit,limits.receive_limit)) {
    a.release(a.context,c,bytes);return GT_CONNECTION_INVALID;
  }
  gt__wire_writer_init(&c->writer);*out=c;return GT_CONNECTION_CREATED;
}
static bool valid_io(gt_connection_t *c,gt_stream_result_t r,size_t requested) {
  if((r.kind==GT_STREAM_DATA && r.length && r.length<=requested) ||
     ((r.kind==GT_STREAM_WAIT || r.kind==GT_STREAM_END || r.kind==GT_STREAM_ERROR) && !r.length))return true;
  finish(c,GT_CONNECTION_IO_ERROR,r.system_error);return false;
}
gt_connection_state_t gt__connection_step(gt_connection_t *c) {
  if(!c)return GT_CONNECTION_IO_ERROR;
  if(!check_time(c))return c->state;
  if(!c->decoder.ready) {
    size_t wanted=c->decoder.envelope_used<GT_WIRE_HEADER_BYTES?
      GT_WIRE_HEADER_BYTES-c->decoder.envelope_used:c->decoder.header.payload_length-c->decoder.payload_used;
    if(wanted>sizeof(c->scratch))wanted=sizeof(c->scratch);
    uint64_t started=c->now;gt_stream_result_t r=c->ops.read(c->ops.context,c->scratch,wanted);
    if(!valid_io(c,r,wanted))return c->state;
    if(r.kind==GT_STREAM_DATA && !c->receiving){c->receiving=true;c->receive_started=started;}
    if(!check_time(c))return c->state;
    if(r.kind==GT_STREAM_ERROR){finish(c,GT_CONNECTION_IO_ERROR,r.system_error);return c->state;}
    if(r.kind==GT_STREAM_END) {
      gt_wire_result_t end=gt__wire_decoder_finish(&c->decoder);
      finish(c,end==GT_WIRE_END?GT_CONNECTION_END:GT_CONNECTION_PROTOCOL_ERROR,0);return c->state;
    }
    if(r.kind==GT_STREAM_DATA) {
      size_t consumed=0;gt_wire_result_t parsed=gt__wire_decoder_feed(&c->decoder,c->scratch,r.length,&consumed);
      gt__secure_erase(c->scratch,r.length);
      if(parsed==GT_WIRE_ERROR || consumed!=r.length){finish(c,GT_CONNECTION_PROTOCOL_ERROR,0);return c->state;}
      if(parsed==GT_WIRE_FRAME)c->receiving=false;
    }
  }
  if(c->writer.state==GT_WRITER_PENDING) {
    const uint8_t *bytes;size_t length;
    if(!gt__wire_writer_view(&c->writer,&bytes,&length)){finish(c,GT_CONNECTION_IO_ERROR,0);return c->state;}
    if(length>sizeof(c->scratch))length=sizeof(c->scratch);
    gt_stream_result_t r=c->ops.write(c->ops.context,bytes,length);
    if(!valid_io(c,r,length) || !check_time(c))return c->state;
    if(r.kind==GT_STREAM_ERROR || r.kind==GT_STREAM_END){finish(c,GT_CONNECTION_IO_ERROR,r.system_error);return c->state;}
    if(r.kind==GT_STREAM_DATA && !gt__wire_writer_advance(&c->writer,r.length))finish(c,GT_CONNECTION_IO_ERROR,0);
    if(c->writer.state==GT_WRITER_DONE)gt__secure_erase(c->storage+c->limits.receive_limit,c->writer.payload_length);
  }
  return c->state;
}
gt_connection_state_t gt__connection_state(const gt_connection_t *c){return c?c->state:GT_CONNECTION_IO_ERROR;}
int gt__connection_system_error(const gt_connection_t *c){return c?c->system_error:0;}
gt_connection_send_t gt__connection_send(gt_connection_t *c,const gt_wire_header_t *h,const void *payload) {
  if(!c)return GT_SEND_REJECTED;
  if(!check_time(c))return GT_SEND_CLOSED;
  if(c->writer.state==GT_WRITER_PENDING)return GT_SEND_BUSY;
  uint8_t header[GT_WIRE_HEADER_BYTES];
  if(!h || h->payload_length>c->limits.send_limit || (h->payload_length && !payload) ||
     !gt__wire_header_encode(h,header))return GT_SEND_REJECTED;
  uint8_t *copy=c->storage+c->limits.receive_limit;
  if(h->payload_length)memcpy(copy,payload,h->payload_length);
  if(!gt__wire_writer_begin(&c->writer,h,copy))return GT_SEND_REJECTED;
  c->send_started=c->now;return GT_SEND_ACCEPTED;
}
bool gt__connection_wants_write(const gt_connection_t *c){return c && c->state==GT_CONNECTION_OPEN && c->writer.state==GT_WRITER_PENDING;}
bool gt__connection_frame(const gt_connection_t *c,gt_wire_frame_t *out) {
  if(out)memset(out,0,sizeof(*out));
  return c && c->state==GT_CONNECTION_OPEN && gt__wire_decoder_frame(&c->decoder,out);
}
bool gt__connection_release_frame(gt_connection_t *c){
  if(!c || c->state!=GT_CONNECTION_OPEN || !c->decoder.ready)return false;
  gt__secure_erase(c->storage,c->decoder.payload_used);
  return gt__wire_decoder_next(&c->decoder);
}
void gt__connection_cancel(gt_connection_t *c){if(c)finish(c,GT_CONNECTION_CANCELLED,0);}
void gt__connection_destroy(gt_connection_t *c) {
  if(!c)return;
  gt__connection_cancel(c);gt_allocator_t a=c->allocator;
  size_t bytes=sizeof(*c)+(size_t)c->limits.receive_limit+c->limits.send_limit;
  gt__secure_erase(c,bytes);
  a.release(a.context,c,bytes);
}
