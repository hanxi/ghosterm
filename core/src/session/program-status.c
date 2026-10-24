#include "program-status.h"
#include <stdlib.h>
#include <string.h>
typedef struct {gt_program_status_record_t record;char *strings;size_t bytes;} entry_t;
struct gt_program_status_store {entry_t *entries;size_t count,used,capacity,limit;gt_status_allocator_t allocator;};
static bool length(const char *s,size_t *out) {
  if(!s)return false;
  for(size_t i=0;i<=4096;i++)if(!s[i]) {*out=i+1;return true;}
  return false;
}
static void *default_allocate(void *context,size_t n){(void)context;return malloc(n);}
static void default_release(void *context,void *p,size_t n){(void)context;(void)n;free(p);}
gt_program_status_store_t *gt__program_status_open(size_t records,size_t bytes) {
  return gt__program_status_open_with_allocator(records,bytes,NULL);
}
gt_program_status_store_t *gt__program_status_open_with_allocator(size_t records,size_t bytes,const gt_status_allocator_t *allocator) {
  if(!records || records>1024 || !bytes || bytes>1024*1024)return NULL;
  gt_status_allocator_t a=allocator?*allocator:(gt_status_allocator_t){NULL,default_allocate,default_release};
  if(!a.allocate || !a.release)return NULL;
  gt_program_status_store_t *s=a.allocate(a.context,sizeof(*s));if(!s)return NULL;
  memset(s,0,sizeof(*s));s->allocator=a;
  s->entries=a.allocate(a.context,records*sizeof(*s->entries));
  if(!s->entries){a.release(a.context,s,sizeof(*s));return NULL;}
  memset(s->entries,0,records*sizeof(*s->entries));
  s->capacity=records;s->limit=bytes;return s;
}
static void release_strings(gt_program_status_store_t *s,entry_t *e) {
  s->allocator.release(s->allocator.context,e->strings,e->bytes);
}
static bool descendant(const char *id,const char *parent,size_t n) {
  return !n || !strcmp(id,parent) || (!strncmp(id,parent,n) && id[n]=='/');
}
gt_status_store_result_t gt__program_status_apply(gt_program_status_store_t *s,const gt_program_status_record_t *r) {
  if(!s || !r || r->state>5)return GT_STATUS_INVALID;
  size_t lengths[4];const char *strings[]={r->id,r->app,r->title,r->message};
  if(!length(r->id,&lengths[0]))return GT_STATUS_INVALID;
  if(r->state==5) {
    char parent[4097];memcpy(parent,r->id,lengths[0]);
    size_t write=0;
    for(size_t i=0;i<s->count;i++) {
      entry_t *e=&s->entries[i];
      if(descendant(e->record.id,parent,lengths[0]-1)) {s->used-=e->bytes;release_strings(s,e);}
      else s->entries[write++]=*e;
    }
    s->count=write;return GT_STATUS_OK;
  }
  if(r->kind>3 || (r->state!=3 && r->kind) || r->progress < -1 || r->progress>100 ||
     ((r->state!=1 && r->state!=3) && r->progress!=-1))return GT_STATUS_INVALID;
  size_t total=lengths[0];
  for(unsigned i=1;i<4;i++) {if(!length(strings[i],&lengths[i]))return GT_STATUS_INVALID;total+=lengths[i];}
  size_t index=0;while(index<s->count && strcmp(s->entries[index].record.id,r->id))index++;
  size_t old=index<s->count ? s->entries[index].bytes : 0;
  if((index==s->count && s->count==s->capacity) || total>s->limit-(s->used-old))return GT_STATUS_LIMIT;
  char *copy=s->allocator.allocate(s->allocator.context,total);if(!copy)return GT_STATUS_NO_MEMORY;
  gt_program_status_record_t stored=*r;
  const char **fields[]={&stored.id,&stored.app,&stored.title,&stored.message};size_t offset=0;
  for(unsigned i=0;i<4;i++){memcpy(copy+offset,strings[i],lengths[i]);*fields[i]=copy+offset;offset+=lengths[i];}
  /* Caller may update using this store's borrowed view: copy before free. */
  if(index<s->count)release_strings(s,&s->entries[index]);else s->count++;
  s->entries[index]=(entry_t){stored,copy,total};s->used=s->used-old+total;return GT_STATUS_OK;
}
size_t gt__program_status_count(const gt_program_status_store_t *s){return s?s->count:0;}
size_t gt__program_status_bytes(const gt_program_status_store_t *s){return s?s->used:0;}
bool gt__program_status_at(const gt_program_status_store_t *s,size_t i,gt_program_status_record_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!s || !out || i>=s->count)return false;
  *out=s->entries[i].record;return true;
}
size_t gt__program_status_semantic(gt_program_status_store_t *s,unsigned kind) {
  if(!s || kind!=1)return 0;
  size_t write=0,removed=0;
  for(size_t i=0;i<s->count;i++) {
    entry_t *e=&s->entries[i];
    if(e->record.state==1 || e->record.state==3) {s->used-=e->bytes;release_strings(s,e);removed++;}
    else s->entries[write++]=*e;
  }
  s->count=write;return removed;
}
gt_status_store_result_t gt__program_status_copy(const gt_program_status_store_t *s,gt_program_status_store_t **out) {
  return gt__program_status_copy_with_allocator(s,s?&s->allocator:NULL,out);
}
gt_status_store_result_t gt__program_status_copy_with_allocator(const gt_program_status_store_t *s,const gt_status_allocator_t *allocator,gt_program_status_store_t **out) {
  if(out)*out=NULL;
  if(!s || !out)return GT_STATUS_INVALID;
  if(allocator && (!allocator->allocate || !allocator->release))return GT_STATUS_INVALID;
  gt_program_status_store_t *copy=gt__program_status_open_with_allocator(s->capacity,s->limit,allocator);if(!copy)return GT_STATUS_NO_MEMORY;
  for(size_t i=0;i<s->count;i++) {
    gt_status_store_result_t rc=gt__program_status_apply(copy,&s->entries[i].record);
    if(rc!=GT_STATUS_OK){gt__program_status_close(copy);return rc;}
  }
  *out=copy;return GT_STATUS_OK;
}
void gt__program_status_close(gt_program_status_store_t *s) {
  if(!s)return;
  for(size_t i=0;i<s->count;i++)release_strings(s,&s->entries[i]);
  gt_status_allocator_t a=s->allocator;
  a.release(a.context,s->entries,s->capacity*sizeof(*s->entries));
  a.release(a.context,s,sizeof(*s));
}
