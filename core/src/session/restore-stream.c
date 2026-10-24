#include "restore-stream.h"
#include <stdlib.h>
#include <string.h>
static void clear(gt_restore_stream_t *s) {s->used=0;s->count=0;s->head=0;s->read_offset=0;}
static gt_restore_rc_t fail(gt_restore_stream_t *s,gt_restore_rc_t rc) {
  s->phase=GT_RESTORE_FAILED;s->failure=rc;clear(s);return rc;
}
static gt_restore_rc_t check(const gt_restore_stream_t *s,uint64_t generation) {
  if(!generation || generation!=s->generation || s->phase==GT_RESTORE_DETACHED)return GT_RESTORE_STALE;
  if(s->phase==GT_RESTORE_FAILED)return s->failure;
  return GT_RESTORE_OK;
}
static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
bool gt__restore_init(gt_restore_stream_t *s,size_t bytes,size_t events) {
  return gt__restore_init_with_allocator(s,bytes,events,NULL);
}
bool gt__restore_init_with_allocator(gt_restore_stream_t *s,size_t bytes,size_t events,const gt_allocator_t *allocator) {
  if(!s || !bytes || bytes>SIZE_MAX/2 || !events || events>SIZE_MAX/sizeof(*s->entries))return false;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,default_allocate,default_release};
  if(!a.allocate || !a.release)return false;
  memset(s,0,sizeof(*s));s->allocator=a;s->byte_limit=bytes;s->event_limit=events;
  s->bytes=a.allocate(a.context,bytes);
  if(s->bytes)s->entries=a.allocate(a.context,events*sizeof(*s->entries));
  if(!s->bytes || !s->entries) {gt__restore_destroy(s);return false;}
  memset(s->entries,0,events*sizeof(*s->entries));return true;
}
void gt__restore_destroy(gt_restore_stream_t *s) {
  if(!s)return;
  if(s->bytes)s->allocator.release(s->allocator.context,s->bytes,s->byte_limit);
  if(s->entries)s->allocator.release(s->allocator.context,s->entries,s->event_limit*sizeof(*s->entries));
  memset(s,0,sizeof(*s));
}
void gt__restore_reset(gt_restore_stream_t *s,uint64_t generation) {
  clear(s);s->generation=generation;s->snapshot=0;s->last=0;s->dequeued=0;s->cut=0;
  s->offered=false;s->failure=GT_RESTORE_OK;s->phase=GT_RESTORE_CAPTURING;
}
void gt__restore_detach(gt_restore_stream_t *s) {clear(s);s->phase=GT_RESTORE_DETACHED;}
gt_restore_rc_t gt__restore_begin(gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL;
  if(s->phase!=GT_RESTORE_CAPTURING)return fail(s,GT_RESTORE_PROTOCOL);
  s->snapshot=sequence;s->last=sequence;s->dequeued=sequence;s->phase=GT_RESTORE_SNAPSHOT;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_offer(gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL;
  if(s->phase!=GT_RESTORE_SNAPSHOT || s->offered || sequence!=s->snapshot)return fail(s,GT_RESTORE_PROTOCOL);
  s->offered=true;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_push(gt_restore_stream_t *s,uint64_t sequence,uint32_t kind,const void *bytes,size_t length) {
  if(s->phase==GT_RESTORE_DETACHED || s->phase==GT_RESTORE_CAPTURING)return GT_RESTORE_IDLE;
  if(s->phase==GT_RESTORE_FAILED)return s->failure;
  if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL; /* Preserve accepted tail. */
  if(s->last==UINT64_MAX || sequence!=s->last+1 || (length && !bytes))return fail(s,GT_RESTORE_PROTOCOL);
  if(s->count==s->event_limit || length>s->byte_limit-s->used)return fail(s,GT_RESTORE_LIMIT);
  size_t offset=(s->read_offset+s->used)%s->byte_limit;
  size_t first=length<s->byte_limit-offset ? length : s->byte_limit-offset;
  if(first)memcpy(s->bytes+offset,bytes,first);
  if(length>first)memcpy(s->bytes,(const uint8_t *)bytes+first,length-first);
  size_t tail=(s->head+s->count)%s->event_limit;
  s->entries[tail]=(gt_restore_entry_t){sequence,kind,length,offset};
  s->used+=length;s->count++;s->last=sequence;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_next(gt_restore_stream_t *s,uint64_t generation,gt_restore_event_t *event,void *buffer,size_t capacity) {
  if(event)memset(event,0,sizeof(*event));
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(!event)return fail(s,GT_RESTORE_PROTOCOL);
  if(s->phase!=GT_RESTORE_DRAINING && s->phase!=GT_RESTORE_WAIT_L && s->phase!=GT_RESTORE_LIVE && s->phase!=GT_RESTORE_ENDED)return GT_RESTORE_WAIT;
  if(s->phase==GT_RESTORE_DRAINING && s->dequeued==s->cut)return GT_RESTORE_WAIT;
  if(!s->count)return GT_RESTORE_WAIT;
  const gt_restore_entry_t *entry=&s->entries[s->head];
  *event=(gt_restore_event_t){s->generation,entry->sequence,entry->kind,entry->length};
  if(capacity<entry->length)return GT_RESTORE_SPACE;
  if(entry->length && !buffer)return fail(s,GT_RESTORE_PROTOCOL);
  size_t first=entry->length<s->byte_limit-entry->offset ? entry->length : s->byte_limit-entry->offset;
  if(first)memcpy(buffer,s->bytes+entry->offset,first);
  if(entry->length>first)memcpy((uint8_t *)buffer+first,s->bytes,entry->length-first);
  s->used-=entry->length;s->read_offset=(entry->offset+entry->length)%s->byte_limit;
  s->dequeued=entry->sequence;s->head=(s->head+1)%s->event_limit;s->count--;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_ready_snapshot(gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL;
  if(s->phase!=GT_RESTORE_SNAPSHOT || !s->offered || sequence!=s->snapshot)return fail(s,GT_RESTORE_PROTOCOL);
  s->cut=s->last;s->phase=GT_RESTORE_DRAINING;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_live_begin(gt_restore_stream_t *s,uint64_t generation,uint64_t *sequence) {
  if(sequence)*sequence=0;
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(!sequence)return fail(s,GT_RESTORE_PROTOCOL);
  if(s->phase!=GT_RESTORE_DRAINING || s->dequeued!=s->cut)return GT_RESTORE_WAIT;
  *sequence=s->cut;s->phase=GT_RESTORE_WAIT_L;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_ready_live(gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL;
  if(s->phase!=GT_RESTORE_WAIT_L || sequence!=s->cut)return fail(s,GT_RESTORE_PROTOCOL);
  s->phase=GT_RESTORE_LIVE;return GT_RESTORE_OK;
}
gt_restore_rc_t gt__restore_end(gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  gt_restore_rc_t rc=check(s,generation);if(rc!=GT_RESTORE_OK)return rc;
  if(s->phase==GT_RESTORE_ENDED)return sequence==s->last?GT_RESTORE_OK:GT_RESTORE_PROTOCOL;
  if(sequence<s->last)return GT_RESTORE_PROTOCOL;
  if(s->phase!=GT_RESTORE_LIVE || sequence>s->last)return GT_RESTORE_WAIT;
  s->phase=GT_RESTORE_ENDED;return GT_RESTORE_OK;
}
bool gt__restore_input_allowed(const gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  return generation && generation==s->generation && s->phase==GT_RESTORE_LIVE && sequence>=s->cut && sequence<=s->dequeued;
}
