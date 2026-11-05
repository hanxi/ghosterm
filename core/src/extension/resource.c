#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/resource.h"
#include "base/sha256.h"
#include "base/secure-random.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  uint64_t handle,last_generation,reserved,budget;size_t resources;
  bool sealed,retiring;gt_bridge_lease_t lease;
} resource_scope_t;
typedef enum {RESOURCE_UNUSED,RESOURCE_STAGING,RESOURCE_PUBLISHED,RESOURCE_CLEANING} resource_state_t;
typedef struct {
  resource_state_t state;uint64_t owner,received,reserved;
  gt_bridge_resource_ref_t ref;gt_bridge_resource_descriptor_t descriptor;
  uint32_t references;int reader,writer;char name[40];dev_t device;ino_t inode;
  gt_sha256_t hash;
} resource_item_t;
struct gt_bridge_resource_store {
  gt_allocator_t allocator;gt_bridge_resource_config_t config;
  uint64_t reserved,next_owner,next_item;size_t resources;
  char connection[33];uint64_t epoch;
  resource_scope_t scopes[GT_BRIDGE_RESOURCE_SCOPES];
  resource_item_t items[GT_BRIDGE_RESOURCE_SLOTS];
};
static void *resource_allocate(void *context,size_t size){(void)context;return malloc(size);}
static void resource_free(void *context,void *p,size_t size){(void)context;(void)size;free(p);}
static bool hex_string(const char *p,size_t n){
  if(!p || strnlen(p,n+1)!=n)return false;
  for(size_t i=0;i<n;i++)if(!((p[i]>='0' && p[i]<='9') || (p[i]>='a' && p[i]<='f')))return false;
  return true;
}
static bool resource_lease_valid(const gt_bridge_lease_t *l){return l && l->epoch && hex_string(l->connection,32) && hex_string(l->instance,32) && hex_string(l->token,32);}
static bool resource_lease_equal(const gt_bridge_lease_t *a,const gt_bridge_lease_t *b){return a->epoch==b->epoch && !memcmp(a->connection,b->connection,33) && !memcmp(a->instance,b->instance,33) && !memcmp(a->token,b->token,33);}
static resource_scope_t *resource_scope(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *l){
  if(!s || !owner || !l)return NULL;
  for(size_t i=0;i<GT_BRIDGE_RESOURCE_SCOPES;i++)if(s->scopes[i].handle==owner && resource_lease_equal(&s->scopes[i].lease,l))return &s->scopes[i];return NULL;
}
static resource_item_t *resource_item(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_resource_ref_t *ref){
  if(!ref || !ref->handle)return NULL;
  for(size_t i=0;i<s->config.slots;i++)if(s->items[i].state!=RESOURCE_UNUSED && s->items[i].owner==owner && s->items[i].ref.handle==ref->handle && !memcmp(s->items[i].ref.token,ref->token,33))return &s->items[i];return NULL;
}
static gt_bridge_resource_status_t resource_random(char out[33]){
  uint8_t data[16];gt_status_t status=gt__secure_random(data,sizeof(data));
  if(status!=GT_OK)return status==GT_ERR_UNSUPPORTED?GT_BRIDGE_RESOURCE_UNSUPPORTED:GT_BRIDGE_RESOURCE_IO;
  static const char hex[]="0123456789abcdef";
  for(size_t i=0;i<16;i++){out[2*i]=hex[data[i]>>4];out[2*i+1]=hex[data[i]&15];}out[32]=0;gt__secure_erase(data,sizeof(data));return GT_BRIDGE_RESOURCE_OK;
}
static bool resource_display(const char *p){
  size_t n=strnlen(p,1025),at=0,count=0;if(!n || n>1024)return false;
  while(at<n){
    uint32_t cp;unsigned tails;uint32_t minimum;uint8_t b=(uint8_t)p[at++];
    if(b<0x80){cp=b;tails=0;minimum=0;}
    else if(b>=0xc2 && b<=0xdf){cp=b&31;tails=1;minimum=0x80;}
    else if(b>=0xe0 && b<=0xef){cp=b&15;tails=2;minimum=0x800;}
    else if(b>=0xf0 && b<=0xf4){cp=b&7;tails=3;minimum=0x10000;}else return false;
    if(tails>n-at)return false;
    while(tails--){b=(uint8_t)p[at++];if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
    if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || cp<32 || cp==127 || ++count>256)return false;
  }
  return true;
}
static bool resource_mime(const char *p){
  size_t n=strnlen(p,129);if(!n || n>128)return false;size_t slash=0;
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)p[i];if(c=='/'){if(!i || i==n-1 || ++slash>1)return false;continue;}
    if(!((c>='a' && c<='z') || (c>='0' && c<='9') || strchr("!#$&^_.+-",c)))return false;
  }
  return slash==1;
}
static gt_bridge_resource_status_t resource_drop(gt_bridge_resource_store_t *s,resource_scope_t *scope,resource_item_t *item){
  bool failed=false;item->state=RESOURCE_CLEANING;
  if(item->writer>=0){int fd=item->writer;item->writer=-1;if(close(fd))failed=true;}
  if(item->reader>=0){int fd=item->reader;item->reader=-1;if(close(fd))failed=true;}
  if(item->name[0]){
    struct stat st;
    if(fstatat(s->config.directory_fd,item->name,&st,AT_SYMLINK_NOFOLLOW)){
      if(errno!=ENOENT)return GT_BRIDGE_RESOURCE_IO;
    }else if(st.st_dev!=item->device || st.st_ino!=item->inode || !S_ISREG(st.st_mode) || unlinkat(s->config.directory_fd,item->name,0))return GT_BRIDGE_RESOURCE_IO;
    item->name[0]=0;
  }
  s->reserved-=item->reserved;scope->reserved-=item->reserved;s->resources--;scope->resources--;
  if(scope->retiring && !scope->resources && !failed)gt__secure_erase(scope,sizeof(*scope));
  gt__secure_erase(item,sizeof(*item));return failed?GT_BRIDGE_RESOURCE_IO:GT_BRIDGE_RESOURCE_OK;
}
static gt_bridge_resource_status_t resource_fail(gt_bridge_resource_store_t *s,resource_scope_t *scope,resource_item_t *item,gt_bridge_resource_status_t status){
  gt_bridge_resource_status_t dropped=resource_drop(s,scope,item);return dropped==GT_BRIDGE_RESOURCE_OK?status:dropped;
}
static gt_bridge_resource_status_t resource_file(gt_bridge_resource_store_t *s,resource_item_t *item){
  for(unsigned attempt=0;attempt<4;attempt++){
    char random[33];gt_bridge_resource_status_t status=resource_random(random);if(status!=GT_BRIDGE_RESOURCE_OK)return status;
    char name[40]="gtr-";memcpy(name+4,random,33);
    int fd=openat(s->config.directory_fd,name,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){if(errno==EEXIST)continue;return GT_BRIDGE_RESOURCE_IO;}
    item->writer=fd;memcpy(item->name,name,37);struct stat write_stat;
    if(fstat(fd,&write_stat))return GT_BRIDGE_RESOURCE_IO;
    item->device=write_stat.st_dev;item->inode=write_stat.st_ino;
    if(!S_ISREG(write_stat.st_mode) || write_stat.st_uid!=geteuid() || (write_stat.st_mode&07777)!=0600 || write_stat.st_nlink!=1 || write_stat.st_size)return GT_BRIDGE_RESOURCE_IO;
    item->reader=openat(s->config.directory_fd,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat read_stat;
    if(item->reader<0 || fstat(item->reader,&read_stat) || read_stat.st_dev!=write_stat.st_dev || read_stat.st_ino!=write_stat.st_ino)return GT_BRIDGE_RESOURCE_IO;
    if(unlinkat(s->config.directory_fd,name,0))return GT_BRIDGE_RESOURCE_IO;
    item->name[0]=0;return GT_BRIDGE_RESOURCE_OK;
  }
  return GT_BRIDGE_RESOURCE_IO;
}
gt_bridge_resource_status_t gt__bridge_resource_open(const gt_bridge_resource_config_t *config,const gt_allocator_t *allocator,gt_bridge_resource_store_t **out){
  if(out)*out=NULL;
  if(!out || !config || config->directory_fd<0 || !config->slots || config->slots>GT_BRIDGE_RESOURCE_SLOTS || !config->resource_bytes || config->resource_bytes>GT_BRIDGE_RESOURCE_BYTES_MAX || config->total_bytes<config->resource_bytes || config->total_bytes>GT_BRIDGE_RESOURCE_TOTAL_MAX)return GT_BRIDGE_RESOURCE_INVALID;
  struct stat st;if(fstat(config->directory_fd,&st))return GT_BRIDGE_RESOURCE_IO;
  if(!S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&07777)!=0700)return GT_BRIDGE_RESOURCE_DENIED;
  gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,resource_allocate,resource_free};if(!a.allocate || !a.release)return GT_BRIDGE_RESOURCE_INVALID;
  gt_bridge_resource_store_t *s=a.allocate(a.context,sizeof(*s));if(!s)return GT_BRIDGE_RESOURCE_IO;
  memset(s,0,sizeof(*s));s->allocator=a;s->config=*config;
  s->config.directory_fd=fcntl(config->directory_fd,F_DUPFD_CLOEXEC,0);
  if(s->config.directory_fd<0){gt__secure_erase(s,sizeof(*s));a.release(a.context,s,sizeof(*s));return GT_BRIDGE_RESOURCE_IO;}
  *out=s;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_bind(gt_bridge_resource_store_t *s,const gt_bridge_lease_t *lease,uint64_t budget,uint64_t *owner){
  if(owner)*owner=0;if(!s || !owner || !resource_lease_valid(lease) || !budget || budget>s->config.total_bytes)return GT_BRIDGE_RESOURCE_INVALID;
  if(s->connection[0] && (memcmp(s->connection,lease->connection,33) || s->epoch!=lease->epoch))return GT_BRIDGE_RESOURCE_DENIED;
  for(size_t i=0;i<GT_BRIDGE_RESOURCE_SCOPES;i++)if(s->scopes[i].handle && !memcmp(s->scopes[i].lease.instance,lease->instance,33))return GT_BRIDGE_RESOURCE_DENIED;
  if(s->next_owner==UINT64_MAX)return GT_BRIDGE_RESOURCE_UNSUPPORTED;
  for(size_t i=0;i<GT_BRIDGE_RESOURCE_SCOPES;i++)if(!s->scopes[i].handle){
    resource_scope_t *scope=&s->scopes[i];scope->handle=++s->next_owner;scope->budget=budget;scope->lease=*lease;
    memcpy(s->connection,lease->connection,33);s->epoch=lease->epoch;*owner=scope->handle;return GT_BRIDGE_RESOURCE_OK;
  }
  return GT_BRIDGE_RESOURCE_WAIT;
}
gt_bridge_resource_status_t gt__bridge_resource_begin(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_descriptor_t *d,gt_bridge_resource_ref_t *out){
  if(out)memset(out,0,sizeof(*out));if(!s || !d || !out)return GT_BRIDGE_RESOURCE_INVALID;
  resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;if(scope->sealed)return GT_BRIDGE_RESOURCE_CLOSED;
  if(!hex_string(d->resource_id,32) || !hex_string(d->sha256,64) || !d->generation || !resource_display(d->display_name) || !resource_mime(d->mime) || (!d->size_known && d->size))return GT_BRIDGE_RESOURCE_INVALID;
  if(d->generation<=scope->last_generation)return GT_BRIDGE_RESOURCE_PROTOCOL;
  for(size_t i=0;i<s->config.slots;i++)if(s->items[i].state!=RESOURCE_UNUSED && s->items[i].owner==owner && !memcmp(s->items[i].descriptor.resource_id,d->resource_id,33))return GT_BRIDGE_RESOURCE_PROTOCOL;
  uint64_t reserve=d->size_known?d->size:s->config.resource_bytes;
  if(reserve>s->config.resource_bytes || reserve>scope->budget)return GT_BRIDGE_RESOURCE_LIMIT;
  if(reserve>s->config.total_bytes-s->reserved || reserve>scope->budget-scope->reserved || s->resources==s->config.slots)return GT_BRIDGE_RESOURCE_WAIT;
  if(s->next_item==UINT64_MAX)return GT_BRIDGE_RESOURCE_UNSUPPORTED;
  resource_item_t *item=NULL;for(size_t i=0;i<s->config.slots;i++)if(s->items[i].state==RESOURCE_UNUSED){item=&s->items[i];break;}if(!item)return GT_BRIDGE_RESOURCE_WAIT;
  gt_bridge_resource_status_t status=resource_random(item->ref.token);if(status!=GT_BRIDGE_RESOURCE_OK){gt__secure_erase(item,sizeof(*item));return status;}
  item->state=RESOURCE_STAGING;item->owner=owner;item->reserved=reserve;item->references=1;item->reader=-1;item->writer=-1;item->descriptor=*d;
  item->ref.handle=++s->next_item;s->reserved+=reserve;scope->reserved+=reserve;s->resources++;scope->resources++;
  status=resource_file(s,item);if(status!=GT_BRIDGE_RESOURCE_OK)return resource_fail(s,scope,item,status);
  gt__sha256_init(&item->hash);scope->last_generation=d->generation;*out=item->ref;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_write(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_ref_t *ref,uint64_t offset,const void *data,size_t size,size_t *accepted){
  if(accepted)*accepted=0;if(!s || !accepted || !data || !size || size>GT_BRIDGE_RESOURCE_CHUNK_MAX)return GT_BRIDGE_RESOURCE_INVALID;
  resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;if(scope->sealed)return GT_BRIDGE_RESOURCE_CLOSED;
  resource_item_t *item=resource_item(s,owner,ref);if(!item)return GT_BRIDGE_RESOURCE_NOT_FOUND;if(item->state!=RESOURCE_STAGING)return GT_BRIDGE_RESOURCE_CLOSED;
  if(offset!=item->received)return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_PROTOCOL);
  if(size>item->reserved-item->received)return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_LIMIT);
  ssize_t n=write(item->writer,data,size);if(n<0 && errno==EINTR)return GT_BRIDGE_RESOURCE_WAIT;
  if(n<=0 || (size_t)n>size)return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_IO);
  if(!gt__sha256_update(&item->hash,data,(size_t)n))return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_IO);
  item->received+=(uint64_t)n;*accepted=(size_t)n;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_finish(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_ref_t *ref,bool complete,uint64_t size,const char digest[65]){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;if(scope->sealed)return GT_BRIDGE_RESOURCE_CLOSED;
  resource_item_t *item=resource_item(s,owner,ref);if(!item)return GT_BRIDGE_RESOURCE_NOT_FOUND;
  if(item->state==RESOURCE_PUBLISHED)return complete && size==item->received && hex_string(digest,64) && !memcmp(digest,item->descriptor.sha256,65)?GT_BRIDGE_RESOURCE_OK:GT_BRIDGE_RESOURCE_PROTOCOL;
  if(item->state!=RESOURCE_STAGING)return GT_BRIDGE_RESOURCE_CLOSED;
  if(!complete || size!=item->received || (item->descriptor.size_known && size!=item->descriptor.size) || !hex_string(digest,64) || memcmp(digest,item->descriptor.sha256,65))return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_PROTOCOL);
  char calculated[65];bool matched=gt__sha256_final(&item->hash,calculated) && !memcmp(calculated,digest,65);gt__secure_erase(calculated,sizeof(calculated));
  if(!matched)return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_PROTOCOL);
  struct stat st;
  if(fstat(item->reader,&st) || !S_ISREG(st.st_mode) || st.st_dev!=item->device || st.st_ino!=item->inode || st.st_nlink || st.st_size<0 || (uint64_t)st.st_size!=size || fchmod(item->writer,0400))return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_IO);
  int writer=item->writer;item->writer=-1;if(close(writer))return resource_fail(s,scope,item,GT_BRIDGE_RESOURCE_IO);
  s->reserved-=item->reserved-size;scope->reserved-=item->reserved-size;item->reserved=size;
  item->descriptor.size=size;item->descriptor.size_known=true;item->state=RESOURCE_PUBLISHED;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_info(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_ref_t *ref,gt_bridge_resource_info_t *out){
  if(out)memset(out,0,sizeof(*out));if(!s || !out)return GT_BRIDGE_RESOURCE_INVALID;if(!resource_scope(s,owner,lease))return GT_BRIDGE_RESOURCE_DENIED;
  resource_item_t *item=resource_item(s,owner,ref);if(!item || item->state==RESOURCE_CLEANING)return GT_BRIDGE_RESOURCE_NOT_FOUND;
  out->descriptor=item->descriptor;out->received=item->received;out->reserved=item->reserved;out->references=item->references;out->published=item->state==RESOURCE_PUBLISHED;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_read(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_ref_t *ref,uint64_t offset,void *data,size_t capacity,size_t *received){
  if(received)*received=0;if(!s || !received || !data || !capacity || capacity>GT_BRIDGE_RESOURCE_CHUNK_MAX)return GT_BRIDGE_RESOURCE_INVALID;
  if(!resource_scope(s,owner,lease))return GT_BRIDGE_RESOURCE_DENIED;resource_item_t *item=resource_item(s,owner,ref);if(!item)return GT_BRIDGE_RESOURCE_NOT_FOUND;
  if(item->state!=RESOURCE_PUBLISHED)return GT_BRIDGE_RESOURCE_CLOSED;if(offset>item->received)return GT_BRIDGE_RESOURCE_INVALID;
  if(capacity>item->received-offset)capacity=(size_t)(item->received-offset);if(!capacity)return GT_BRIDGE_RESOURCE_OK;
  ssize_t n=pread(item->reader,data,capacity,(off_t)offset);if(n<0 && errno==EINTR)return GT_BRIDGE_RESOURCE_WAIT;
  if(n<=0 || (size_t)n>capacity)return GT_BRIDGE_RESOURCE_IO;*received=(size_t)n;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_retain(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_ref_t *ref){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;if(!resource_scope(s,owner,lease))return GT_BRIDGE_RESOURCE_DENIED;resource_item_t *item=resource_item(s,owner,ref);if(!item)return GT_BRIDGE_RESOURCE_NOT_FOUND;
  if(item->state!=RESOURCE_PUBLISHED)return GT_BRIDGE_RESOURCE_CLOSED;if(item->references==UINT32_MAX)return GT_BRIDGE_RESOURCE_LIMIT;item->references++;return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_release(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease,const gt_bridge_resource_ref_t *ref){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;resource_item_t *item=resource_item(s,owner,ref);if(!item)return GT_BRIDGE_RESOURCE_NOT_FOUND;
  if(item->references>1){item->references--;return GT_BRIDGE_RESOURCE_OK;}return resource_drop(s,scope,item);
}
gt_bridge_resource_status_t gt__bridge_resource_seal(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;scope->sealed=true;gt_bridge_resource_status_t result=GT_BRIDGE_RESOURCE_OK;
  for(size_t i=0;i<s->config.slots;i++)if(s->items[i].state!=RESOURCE_UNUSED && s->items[i].state!=RESOURCE_PUBLISHED && s->items[i].owner==owner)if(resource_drop(s,scope,&s->items[i])!=GT_BRIDGE_RESOURCE_OK)result=GT_BRIDGE_RESOURCE_IO;return result;
}
gt_bridge_resource_status_t gt__bridge_resource_unbind(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;
  if(!scope->sealed || scope->resources)return GT_BRIDGE_RESOURCE_WAIT;gt__secure_erase(scope,sizeof(*scope));return GT_BRIDGE_RESOURCE_OK;
}
gt_bridge_resource_status_t gt__bridge_resource_retire(gt_bridge_resource_store_t *s,uint64_t owner,const gt_bridge_lease_t *lease){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;resource_scope_t *scope=resource_scope(s,owner,lease);if(!scope)return GT_BRIDGE_RESOURCE_DENIED;
  scope->retiring=true;gt_bridge_resource_status_t status=gt__bridge_resource_seal(s,owner,lease);
  if(status==GT_BRIDGE_RESOURCE_OK && !scope->resources)gt__secure_erase(scope,sizeof(*scope));return status;
}
bool gt__bridge_resource_usage(const gt_bridge_resource_store_t *s,gt_bridge_resource_usage_t *out){
  if(out)memset(out,0,sizeof(*out));if(!s || !out)return false;out->reserved_bytes=s->reserved;out->resources=s->resources;
  for(size_t i=0;i<GT_BRIDGE_RESOURCE_SCOPES;i++)if(s->scopes[i].handle)out->scopes++;return true;
}
void gt__bridge_resource_collect_retired(gt_bridge_resource_store_t *s){
  if(s)for(size_t i=0;i<GT_BRIDGE_RESOURCE_SCOPES;i++)if(s->scopes[i].retiring && !s->scopes[i].resources)gt__secure_erase(&s->scopes[i],sizeof(s->scopes[i]));
}
gt_bridge_resource_status_t gt__bridge_resource_destroy(gt_bridge_resource_store_t *s){
  if(!s)return GT_BRIDGE_RESOURCE_INVALID;if(s->resources)return GT_BRIDGE_RESOURCE_WAIT;for(size_t i=0;i<GT_BRIDGE_RESOURCE_SCOPES;i++)if(s->scopes[i].handle)return GT_BRIDGE_RESOURCE_WAIT;
  gt_allocator_t a=s->allocator;int directory=s->config.directory_fd;gt__secure_erase(s,sizeof(*s));a.release(a.context,s,sizeof(*s));return close(directory)?GT_BRIDGE_RESOURCE_IO:GT_BRIDGE_RESOURCE_OK;
}
