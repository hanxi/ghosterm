#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/memory.h"
#include "extension/package-snapshot.h"
#include "base/sha256.h"
#include "base/secure-random.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
 char *path;size_t path_bytes;uint16_t parent;struct stat observed;
 uint8_t *data;char sha256[65];bool directory;
} package_entry_t;
struct gt_package_snapshot {
 gt_allocator_t allocator;gt_package_capture_config_t config;
 package_entry_t *entries;package_entry_t *members[GT_PACKAGE_MEMBERS_MAX];
 size_t used,count,directory_cursor,active_directory,active_member,verify_cursor;
 uint64_t total,read_at;int root,file;DIR *directory;
 gt_sha256_t hash;gt_package_status_t status;bool verifying,ready;
};
static void *package_alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void package_free(void *ctx,void *p,size_t n){(void)ctx;(void)n;free(p);}
static void package_release(gt_package_snapshot_t *s,void *p,size_t n){if(p){gt__secure_erase(p,n);gt__plugin_memory_free(&s->allocator,p,n);}}
static gt_package_status_t package_fail(gt_package_snapshot_t *s,gt_package_status_t error){s->status=error;return error;}
static bool package_same(const struct stat *a,const struct stat *b){
#if defined(__APPLE__)
 const struct timespec am=a->st_mtimespec,bm=b->st_mtimespec,ac=a->st_ctimespec,bc=b->st_ctimespec;
#else
 const struct timespec am=a->st_mtim,bm=b->st_mtim,ac=a->st_ctim,bc=b->st_ctim;
#endif
 return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_mode==b->st_mode &&
  a->st_uid==b->st_uid && a->st_gid==b->st_gid && a->st_nlink==b->st_nlink &&
  a->st_size==b->st_size && am.tv_sec==bm.tv_sec && am.tv_nsec==bm.tv_nsec &&
  ac.tv_sec==bc.tv_sec && ac.tv_nsec==bc.tv_nsec;
}
/* Syntax/UTF-8 checks only. NFC and full Unicode case policy remain a required
 * metadata-validator step; rejecting invalid UTF-8 does not prove normalization. */
static bool package_path(const char *p,size_t n){
 if(!n || n>GT_PACKAGE_PATH_BYTES_MAX || p[0]=='/' || p[n-1]=='/')return false;
 size_t at=0,points=0,start=0;
 while(at<n){
  uint8_t b=(uint8_t)p[at++];uint32_t cp,minimum;unsigned tails;
  if(b<0x80){cp=b;minimum=0;tails=0;}
  else if(b>=0xc2 && b<=0xdf){cp=b&31;minimum=0x80;tails=1;}
  else if(b>=0xe0 && b<=0xef){cp=b&15;minimum=0x800;tails=2;}
  else if(b>=0xf0 && b<=0xf4){cp=b&7;minimum=0x10000;tails=3;}else return false;
  if(tails>n-at)return false;
  while(tails--){b=(uint8_t)p[at++];if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
  if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || cp<32 || cp==127 || cp=='\\' || cp==':' || ++points>GT_PACKAGE_PATH_POINTS_MAX)return false;
  if(cp=='/' || at==n){
   size_t end=cp=='/'?at-1:at,len=end-start;
   if(!len || p[end-1]=='.' || p[end-1]==' ' || (len==1 && p[start]=='.') || (len==2 && p[start]=='.' && p[start+1]=='.'))return false;
   start=at;
  }
 }
 return true;
}
static const char *package_leaf(const package_entry_t *e){const char *slash=strrchr(e->path,'/');return slash?slash+1:e->path;}
/* Reopen directory ancestry from the pinned root, one component at a time.
 * No path resolution through an unverified intermediate directory or symlink;
 * at most two transient fds, including arbitrarily deep allowed relative paths. */
static gt_package_status_t package_directory(gt_package_snapshot_t *s,size_t index,int *out){
 uint16_t chain[GT_PACKAGE_PATH_POINTS_MAX+1];size_t depth=0;
 for(size_t i=index;i;i=s->entries[i].parent){if(depth==GT_PACKAGE_PATH_POINTS_MAX)return GT_PACKAGE_LIMIT;chain[depth++]=(uint16_t)i;}
 int fd=openat(s->root,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 if(fd<0)return GT_PACKAGE_IO;
 struct stat st;gt_package_status_t status=GT_PACKAGE_OK;
 if(fstat(fd,&st))status=GT_PACKAGE_IO;else if(!package_same(&s->entries[0].observed,&st))status=GT_PACKAGE_CHANGED;
 while(status==GT_PACKAGE_OK && depth){
  const package_entry_t *e=&s->entries[chain[--depth]];
  int next=openat(fd,package_leaf(e),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
  if(next<0){status=errno==ENOENT || errno==ELOOP || errno==ENOTDIR?GT_PACKAGE_CHANGED:GT_PACKAGE_IO;break;}
  if(close(fd)){close(next);return GT_PACKAGE_IO;}fd=next;
  if(fstat(fd,&st))status=GT_PACKAGE_IO;else if(!S_ISDIR(st.st_mode) || !package_same(&e->observed,&st))status=GT_PACKAGE_CHANGED;
 }
 if(status!=GT_PACKAGE_OK){if(close(fd))status=GT_PACKAGE_IO;return status;}*out=fd;return GT_PACKAGE_OK;
}
static int package_compare(const void *a,const void *b){return strcmp((*(package_entry_t *const *)a)->path,(*(package_entry_t *const *)b)->path);}
static bool package_ascii_equal(const char *a,const char *b){
 while(*a && *b){unsigned char x=(unsigned char)*a++,y=(unsigned char)*b++;if(x>='A' && x<='Z')x+=32;if(y>='A' && y<='Z')y+=32;if(x!=y)return false;}return !*a && !*b;
}
gt_package_status_t gt__package_capture_open(const gt_package_capture_config_t *c,const gt_allocator_t *a,gt_package_snapshot_t **out){
 if(out)*out=NULL;
 if(!out || !c || c->directory_fd<0 || !c->members || c->members>GT_PACKAGE_MEMBERS_MAX || !c->entries || c->entries>GT_PACKAGE_ENTRIES_MAX || !c->member_bytes || c->member_bytes>GT_PACKAGE_MEMBER_BYTES_MAX || !c->total_bytes || c->total_bytes>GT_PACKAGE_TOTAL_BYTES_MAX)return GT_PACKAGE_INVALID;
 gt_allocator_t allocator=a?*a:(gt_allocator_t){NULL,package_alloc,package_free};if(!allocator.allocate || !allocator.release)return GT_PACKAGE_INVALID;
 struct stat original;if(fstat(c->directory_fd,&original))return GT_PACKAGE_IO;if(!S_ISDIR(original.st_mode))return GT_PACKAGE_INVALID;
 gt_package_snapshot_t *s=gt__plugin_memory_alloc(&allocator,sizeof(*s));if(!s)return GT_PACKAGE_NOMEM;memset(s,0,sizeof(*s));s->allocator=allocator;s->config=*c;s->root=-1;s->file=-1;s->status=GT_PACKAGE_WAIT;
 size_t allocation=(c->entries+1)*sizeof(*s->entries);s->entries=gt__plugin_memory_alloc(&allocator,allocation);if(!s->entries){gt__package_snapshot_destroy(&s);return GT_PACKAGE_NOMEM;}memset(s->entries,0,allocation);
 s->root=openat(c->directory_fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 gt_package_status_t status=GT_PACKAGE_OK;struct stat observed;
 if(s->root<0 || fstat(s->root,&observed))status=GT_PACKAGE_IO;else if(!package_same(&original,&observed))status=GT_PACKAGE_CHANGED;
 if(status!=GT_PACKAGE_OK){gt__package_snapshot_destroy(&s);return status;}s->entries[0].observed=observed;s->entries[0].directory=true;s->used=1;*out=s;return GT_PACKAGE_OK;
}
static gt_package_status_t package_read_step(gt_package_snapshot_t *s){
 package_entry_t *e=&s->entries[s->active_member];uint64_t size=(uint64_t)e->observed.st_size;
 if(s->read_at<size){
  size_t n=(size_t)(size-s->read_at);if(n>GT_PACKAGE_CHUNK_MAX)n=GT_PACKAGE_CHUNK_MAX;
  ssize_t got=read(s->file,e->data+s->read_at,n);
  if(got<0){if(errno==EINTR)return GT_PACKAGE_WAIT;return package_fail(s,GT_PACKAGE_IO);}
  if(!got)return package_fail(s,GT_PACKAGE_CHANGED);
  if(!gt__sha256_update(&s->hash,e->data+s->read_at,(size_t)got))return package_fail(s,GT_PACKAGE_IO);
  s->read_at+=(uint64_t)got;return GT_PACKAGE_WAIT;
 }
 uint8_t extra;ssize_t got=read(s->file,&extra,1);
 if(got<0){if(errno==EINTR)return GT_PACKAGE_WAIT;return package_fail(s,GT_PACKAGE_IO);}if(got)return package_fail(s,GT_PACKAGE_CHANGED);
 struct stat st,named;
 if(fstat(s->file,&st) || fstatat(dirfd(s->directory),package_leaf(e),&named,AT_SYMLINK_NOFOLLOW))return package_fail(s,GT_PACKAGE_IO);
 if(!package_same(&e->observed,&st) || !package_same(&e->observed,&named))return package_fail(s,GT_PACKAGE_CHANGED);
 if(!gt__sha256_final(&s->hash,e->sha256))return package_fail(s,GT_PACKAGE_IO);
 int fd=s->file;s->file=-1;if(close(fd))return package_fail(s,GT_PACKAGE_IO);return GT_PACKAGE_WAIT;
}
static gt_package_status_t package_verify_step(gt_package_snapshot_t *s){
 if(s->verify_cursor<s->used){
  package_entry_t *e=&s->entries[s->verify_cursor++];int fd=-1;
  gt_package_status_t status=package_directory(s,e->parent,&fd);if(status!=GT_PACKAGE_OK)return package_fail(s,status);
  struct stat st;if(fstatat(fd,package_leaf(e),&st,AT_SYMLINK_NOFOLLOW))status=GT_PACKAGE_IO;else if(!package_same(&e->observed,&st))status=GT_PACKAGE_CHANGED;
  if(close(fd))status=GT_PACKAGE_IO;if(status!=GT_PACKAGE_OK)return package_fail(s,status);return GT_PACKAGE_WAIT;
 }
 struct stat st;if(fstat(s->root,&st))return package_fail(s,GT_PACKAGE_IO);if(!package_same(&s->entries[0].observed,&st))return package_fail(s,GT_PACKAGE_CHANGED);
 qsort(s->members,s->count,sizeof(s->members[0]),package_compare);s->ready=true;s->status=GT_PACKAGE_OK;return GT_PACKAGE_OK;
}
gt_package_status_t gt__package_capture_step(gt_package_snapshot_t *s){
 if(!s)return GT_PACKAGE_INVALID;if(s->status!=GT_PACKAGE_WAIT)return s->status;
 if(s->file>=0)return package_read_step(s);if(s->verifying)return package_verify_step(s);
 if(!s->directory){
  while(s->directory_cursor<s->used && !s->entries[s->directory_cursor].directory)s->directory_cursor++;
  if(s->directory_cursor==s->used){s->verifying=true;s->verify_cursor=1;return GT_PACKAGE_WAIT;}
  int fd=-1;gt_package_status_t status=package_directory(s,s->directory_cursor,&fd);if(status!=GT_PACKAGE_OK)return package_fail(s,status);
  s->directory=fdopendir(fd);if(!s->directory){close(fd);return package_fail(s,GT_PACKAGE_IO);}s->active_directory=s->directory_cursor++;return GT_PACKAGE_WAIT;
 }
 errno=0;struct dirent *d=readdir(s->directory);
 if(!d){
  int error=errno;struct stat st;gt_package_status_t status=GT_PACKAGE_WAIT;
  if(error || fstat(dirfd(s->directory),&st))status=GT_PACKAGE_IO;else if(!package_same(&s->entries[s->active_directory].observed,&st))status=GT_PACKAGE_CHANGED;
  DIR *directory=s->directory;s->directory=NULL;if(closedir(directory))status=GT_PACKAGE_IO;
  return status==GT_PACKAGE_WAIT?status:package_fail(s,status);
 }
 if(!strcmp(d->d_name,".") || !strcmp(d->d_name,".."))return GT_PACKAGE_WAIT;
 if(s->used-1==s->config.entries)return package_fail(s,GT_PACKAGE_LIMIT);
 const package_entry_t *parent=&s->entries[s->active_directory];size_t leaf=strnlen(d->d_name,GT_PACKAGE_PATH_BYTES_MAX+1),prefix=parent->path_bytes;
 if(leaf>GT_PACKAGE_PATH_BYTES_MAX || prefix+(prefix?1:0)+leaf>GT_PACKAGE_PATH_BYTES_MAX)return package_fail(s,GT_PACKAGE_LIMIT);
 char path[GT_PACKAGE_PATH_BYTES_MAX+1];size_t n=0;if(prefix){memcpy(path,parent->path,prefix);n=prefix;path[n++]='/';}memcpy(path+n,d->d_name,leaf);n+=leaf;path[n]=0;
 if(!package_path(path,n))return package_fail(s,GT_PACKAGE_INVALID);
 for(size_t i=1;i<s->used;i++)if(package_ascii_equal(path,s->entries[i].path))return package_fail(s,GT_PACKAGE_INVALID);
 struct stat st;if(fstatat(dirfd(s->directory),d->d_name,&st,AT_SYMLINK_NOFOLLOW))return package_fail(s,GT_PACKAGE_IO);
 if((!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) || (S_ISREG(st.st_mode) && st.st_nlink!=1))return package_fail(s,GT_PACKAGE_INVALID);
 if(S_ISREG(st.st_mode) && (st.st_size<0 || (uint64_t)st.st_size>s->config.member_bytes || (uint64_t)st.st_size>s->config.total_bytes-s->total || s->count==s->config.members))return package_fail(s,GT_PACKAGE_LIMIT);
 package_entry_t *e=&s->entries[s->used++];e->path_bytes=n;e->parent=(uint16_t)s->active_directory;e->observed=st;e->directory=S_ISDIR(st.st_mode);
 e->path=gt__plugin_memory_alloc(&s->allocator,n+1);if(!e->path)return package_fail(s,GT_PACKAGE_NOMEM);memcpy(e->path,path,n+1);
 if(e->directory)return GT_PACKAGE_WAIT;
 if(st.st_size){e->data=gt__plugin_memory_alloc(&s->allocator,(size_t)st.st_size);if(!e->data)return package_fail(s,GT_PACKAGE_NOMEM);}
 s->members[s->count++]=e;s->total+=(uint64_t)st.st_size;s->active_member=s->used-1;s->read_at=0;gt__sha256_init(&s->hash);
 /* NONBLOCK prevents a raced replacement FIFO from blocking before fstat. */
 s->file=openat(dirfd(s->directory),d->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
 if(s->file<0)return package_fail(s,errno==ELOOP || errno==ENOENT?GT_PACKAGE_CHANGED:GT_PACKAGE_IO);
 struct stat opened;if(fstat(s->file,&opened))return package_fail(s,GT_PACKAGE_IO);
 if(!S_ISREG(opened.st_mode) || !package_same(&st,&opened))return package_fail(s,GT_PACKAGE_CHANGED);return GT_PACKAGE_WAIT;
}
bool gt__package_snapshot_count(const gt_package_snapshot_t *s,size_t *count,uint64_t *bytes){
 if(count)*count=0;if(bytes)*bytes=0;if(!s || !s->ready || !count || !bytes)return false;*count=s->count;*bytes=s->total;return true;
}
bool gt__package_snapshot_member(const gt_package_snapshot_t *s,size_t index,gt_package_member_t *out){
 if(out)memset(out,0,sizeof(*out));if(!s || !s->ready || !out || index>=s->count)return false;
 const package_entry_t *e=s->members[index];out->path=e->path;out->bytes=(uint64_t)e->observed.st_size;memcpy(out->sha256,e->sha256,65);return true;
}
gt_package_status_t gt__package_snapshot_find(const gt_package_snapshot_t *s,const char *path,size_t *index){
 if(index)*index=0;if(!s || !s->ready || !path || !index || !package_path(path,strnlen(path,GT_PACKAGE_PATH_BYTES_MAX+1)))return GT_PACKAGE_INVALID;
 size_t low=0,high=s->count;while(low<high){size_t middle=low+(high-low)/2;int order=strcmp(path,s->members[middle]->path);if(!order){*index=middle;return GT_PACKAGE_OK;}if(order<0)high=middle;else low=middle+1;}return GT_PACKAGE_NOT_FOUND;
}
gt_package_status_t gt__package_snapshot_read(const gt_package_snapshot_t *s,size_t index,uint64_t offset,void *bytes,size_t cap,size_t *read_bytes){
 if(read_bytes)*read_bytes=0;if(!s || !s->ready || index>=s->count || !read_bytes || (!bytes && cap) || cap>GT_PACKAGE_CHUNK_MAX)return GT_PACKAGE_INVALID;
 const package_entry_t *e=s->members[index];uint64_t size=(uint64_t)e->observed.st_size;if(offset>size)return GT_PACKAGE_INVALID;size_t n=(size_t)(size-offset);if(n>cap)n=cap;
 if(n)memcpy(bytes,e->data+offset,n);*read_bytes=n;return GT_PACKAGE_OK;
}
bool gt__package_snapshot_view(const gt_package_snapshot_t *s,size_t index,const uint8_t **bytes,size_t *size){
 if(bytes)*bytes=NULL;if(size)*size=0;if(!s || !s->ready || index>=s->count || !bytes || !size)return false;*bytes=s->members[index]->data;*size=(size_t)s->members[index]->observed.st_size;return true;
}
gt_package_status_t gt__package_snapshot_destroy(gt_package_snapshot_t **out){
 if(!out || !*out)return GT_PACKAGE_INVALID;gt_package_snapshot_t *s=*out;*out=NULL;bool failed=false;
 if(s->file>=0 && close(s->file))failed=true;if(s->directory && closedir(s->directory))failed=true;if(s->root>=0 && close(s->root))failed=true;
 for(size_t i=1;i<s->used;i++){package_entry_t *e=&s->entries[i];package_release(s,e->path,e->path_bytes+1);package_release(s,e->data,(size_t)e->observed.st_size);}
 package_release(s,s->entries,(s->config.entries+1)*sizeof(*s->entries));gt_allocator_t a=s->allocator;gt__secure_erase(s,sizeof(*s));gt__plugin_memory_free(&a,s,sizeof(*s));return failed?GT_PACKAGE_IO:GT_PACKAGE_OK;
}
