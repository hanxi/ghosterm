#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/runtime-resource.h"
#include "base/secure-random.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {char id[65];int fd;uint64_t limit;} runtime_root_t;
typedef struct {uint64_t id,deadline;gt_ext_source_t *source;bool expired,clock_error;} runtime_source_t;
typedef struct {uint64_t id,read_at,deadline;gt_ext_resource_snapshot_t snapshot;gt_runtime_resource_status_t read_error;bool reading,complete;} runtime_pin_t;
struct gt_runtime_resources {
 gt_ext_session_t *session;gt_ext_resource_store_t *store;gt_ext_lease_t lease;
 runtime_root_t roots[GT_RUNTIME_ROOTS_MAX];size_t root_count,cursor,read_budget;
 runtime_source_t sources[GT_EXT_SOURCE_LIMIT];runtime_pin_t pins[GT_RUNTIME_PINS_MAX];
 uint64_t next;bool stopped;void *clock_context;bool (*clock)(void *,uint64_t *);
};
static gt_runtime_resource_status_t resource_status(gt_ext_resource_status_t s){
 switch(s){case GT_EXT_RESOURCE_OK:return GT_RUNTIME_RESOURCE_OK;case GT_EXT_RESOURCE_WAIT:return GT_RUNTIME_RESOURCE_WAIT;case GT_EXT_RESOURCE_INVALID:return GT_RUNTIME_RESOURCE_INVALID;case GT_EXT_RESOURCE_DENIED:return GT_RUNTIME_RESOURCE_DENIED;case GT_EXT_RESOURCE_CLOSED:return GT_RUNTIME_RESOURCE_CLOSED;case GT_EXT_RESOURCE_PROTOCOL:return GT_RUNTIME_RESOURCE_PROTOCOL;case GT_EXT_RESOURCE_LIMIT:return GT_RUNTIME_RESOURCE_LIMIT;case GT_EXT_RESOURCE_IO:return GT_RUNTIME_RESOURCE_IO;case GT_EXT_RESOURCE_UNSUPPORTED:return GT_RUNTIME_RESOURCE_UNSUPPORTED;case GT_EXT_RESOURCE_NOT_FOUND:return GT_RUNTIME_RESOURCE_NOT_FOUND;}return GT_RUNTIME_RESOURCE_IO;
}
static gt_runtime_resource_status_t source_status(gt_ext_source_status_t s){
 switch(s){case GT_EXT_SOURCE_OK:return GT_RUNTIME_RESOURCE_OK;case GT_EXT_SOURCE_WAIT:return GT_RUNTIME_RESOURCE_WAIT;case GT_EXT_SOURCE_INVALID:return GT_RUNTIME_RESOURCE_INVALID;case GT_EXT_SOURCE_DENIED:return GT_RUNTIME_RESOURCE_DENIED;case GT_EXT_SOURCE_UNSUPPORTED:return GT_RUNTIME_RESOURCE_UNSUPPORTED;case GT_EXT_SOURCE_IO:return GT_RUNTIME_RESOURCE_IO;case GT_EXT_SOURCE_LIMIT_REACHED:return GT_RUNTIME_RESOURCE_LIMIT;case GT_EXT_SOURCE_CHANGED:return GT_RUNTIME_RESOURCE_CHANGED;case GT_EXT_SOURCE_CANCELLED:return GT_RUNTIME_RESOURCE_CANCELLED;case GT_EXT_SOURCE_BUSY:return GT_RUNTIME_RESOURCE_BUSY;case GT_EXT_SOURCE_UNKNOWN:return GT_RUNTIME_RESOURCE_UNKNOWN;case GT_EXT_SOURCE_CLOSED:return GT_RUNTIME_RESOURCE_CLOSED;case GT_EXT_SOURCE_NOT_FOUND:return GT_RUNTIME_RESOURCE_NOT_FOUND;}return GT_RUNTIME_RESOURCE_IO;
}
static bool root_id(const char *s){
 if(!s || s[0]<'a' || s[0]>'z' || strnlen(s,65)>64)return false;
 for(const char *p=s;*p;p++)if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='_' || *p=='-' || *p=='.'))return false;return true;
}
static runtime_source_t *source_owner(gt_runtime_resources_t *r,uint64_t id){if(r && id)for(size_t i=0;i<GT_EXT_SOURCE_LIMIT;i++)if(r->sources[i].id==id)return &r->sources[i];return NULL;}
static runtime_pin_t *pin_owner(gt_runtime_resources_t *r,uint64_t id){if(r && id)for(size_t i=0;i<GT_RUNTIME_PINS_MAX;i++)if(r->pins[i].id==id)return &r->pins[i];return NULL;}
static bool source_terminal(gt_ext_source_phase_t p){return p==GT_EXT_SOURCE_SUCCEEDED || p==GT_EXT_SOURCE_FAILED || p==GT_EXT_SOURCE_STOPPED || p==GT_EXT_SOURCE_UNCERTAIN;}
static gt_runtime_resource_status_t resource_access(gt_runtime_resources_t *r){
 if(r->stopped)return GT_RUNTIME_RESOURCE_CLOSED;
 switch(gt_ext_session_access(r->session,&r->lease,"resource.receive")){case GT_EXT_ACCESS_ALLOWED:return GT_RUNTIME_RESOURCE_OK;case GT_EXT_ACCESS_UNKNOWN:return GT_RUNTIME_RESOURCE_WAIT;case GT_EXT_ACCESS_DENIED:return GT_RUNTIME_RESOURCE_DENIED;case GT_EXT_ACCESS_REVOKED:return GT_RUNTIME_RESOURCE_CLOSED;case GT_EXT_ACCESS_UNSUPPORTED:return GT_RUNTIME_RESOURCE_UNSUPPORTED;}return GT_RUNTIME_RESOURCE_DENIED;
}
gt_status_t gt__runtime_resources_open(gt_ext_session_t *session,gt_ext_resource_store_t *store,const gt_ext_lease_t *lease,const gt_runtime_root_t *roots,size_t count,void *clock_context,bool (*clock)(void *,uint64_t *),gt_runtime_resources_t **out){
 if(out)*out=NULL;if(!out || !session || !lease || !clock || count>GT_RUNTIME_ROOTS_MAX || (count && !roots))return GT_ERR_INVALID_ARG;if(!store)return GT_ERR_UNSUPPORTED;
 /* Preflight all PODs before duplicating anything. Native caller owns policy. */
 for(size_t i=0;i<count;i++){struct stat st;if(!root_id(roots[i].id) || !roots[i].byte_limit || roots[i].byte_limit>16777216 || roots[i].directory_fd<0 || fstat(roots[i].directory_fd,&st) || !S_ISDIR(st.st_mode))return GT_ERR_INVALID_ARG;for(size_t j=0;j<i;j++)if(!strcmp(roots[i].id,roots[j].id))return GT_ERR_INVALID_ARG;}
 gt_runtime_resources_t *r=gt__plugin_memory_calloc(1,sizeof(*r));if(!r)return GT_ERR_IO;r->session=session;r->store=store;r->lease=*lease;r->clock_context=clock_context;r->clock=clock;r->next=1;
 for(size_t i=0;i<GT_RUNTIME_ROOTS_MAX;i++)r->roots[i].fd=-1;
 for(size_t i=0;i<count;i++){int fd=fcntl(roots[i].directory_fd,F_DUPFD_CLOEXEC,3);if(fd<0){for(size_t j=0;j<r->root_count;j++)close(r->roots[j].fd);gt__plugin_memory_free(NULL,r,sizeof(*r));return GT_ERR_IO;}r->roots[i].fd=fd;r->roots[i].limit=roots[i].byte_limit;strcpy(r->roots[i].id,roots[i].id);r->root_count++;}
 *out=r;return GT_OK;
}
size_t gt__runtime_resources_roots(const gt_runtime_resources_t *r){return r?r->root_count:0;}
const char *gt__runtime_resources_root(const gt_runtime_resources_t *r,size_t i){return r && i<r->root_count?r->roots[i].id:NULL;}
gt_runtime_resource_status_t gt__runtime_resources_publish(gt_runtime_resources_t *r,const char *root,const char *path,const char *mime,uint64_t limit,uint32_t timeout,uint64_t *out){
 if(out)*out=0;if(!r || !out || !root || !path || !mime || !timeout || timeout>3600000)return GT_RUNTIME_RESOURCE_INVALID;if(r->stopped)return GT_RUNTIME_RESOURCE_CLOSED;
 runtime_root_t *scope=NULL;for(size_t i=0;i<r->root_count;i++)if(!strcmp(root,r->roots[i].id)){scope=&r->roots[i];break;}if(!scope)return GT_RUNTIME_RESOURCE_DENIED;if(!limit || limit>scope->limit)return GT_RUNTIME_RESOURCE_LIMIT;
 runtime_source_t *slot=NULL;if(r->next)for(size_t i=0;i<GT_EXT_SOURCE_LIMIT;i++)if(!r->sources[i].id){slot=&r->sources[i];break;}if(!slot)return GT_RUNTIME_RESOURCE_LIMIT;
 uint64_t now;if(!r->clock(r->clock_context,&now) || now>UINT64_MAX-timeout)return GT_RUNTIME_RESOURCE_IO;
 gt_ext_source_config_t c={scope->fd,path,mime,limit};gt_runtime_resource_status_t status=source_status(gt_ext_source_open(r->session,&r->lease,&c,&slot->source));if(status!=GT_RUNTIME_RESOURCE_OK)return status;
 slot->id=r->next++;slot->deadline=now+timeout;*out=slot->id;return GT_RUNTIME_RESOURCE_OK;
}
void gt__runtime_resources_step(gt_runtime_resources_t *r){
 if(!r || r->stopped)return;r->read_budget=GT_EXT_RESOURCE_READ_MAX;uint64_t now;bool valid=r->clock(r->clock_context,&now);
 for(size_t i=0;i<GT_EXT_SOURCE_LIMIT;i++)if(r->sources[i].source){gt_ext_source_info_t v;if(gt_ext_source_info(r->sources[i].source,&v) && !source_terminal(v.phase) && !r->sources[i].expired && !r->sources[i].clock_error && (!valid || now>=r->sources[i].deadline)){r->sources[i].expired=valid;r->sources[i].clock_error=!valid;gt_ext_source_cancel(r->sources[i].source);}}
 for(size_t n=0;n<GT_EXT_SOURCE_LIMIT;n++){size_t i=r->cursor;r->cursor=(r->cursor+1)%GT_EXT_SOURCE_LIMIT;if(!r->sources[i].source)continue;gt_ext_source_info_t v;if(!gt_ext_source_info(r->sources[i].source,&v) || source_terminal(v.phase))continue;gt_ext_source_phase_t phase=gt_ext_source_step(r->sources[i].source);if(phase==GT_EXT_SOURCE_READY && !r->sources[i].expired)(void)gt_ext_source_send(r->sources[i].source);break;}
}
gt_runtime_resource_status_t gt__runtime_resources_publication(gt_runtime_resources_t *r,uint64_t id,gt_runtime_publication_t *out){
 if(out)memset(out,0,sizeof(*out));runtime_source_t *s=source_owner(r,id);if(!s || !out)return GT_RUNTIME_RESOURCE_INVALID;if(!gt_ext_source_info(s->source,&out->source))return GT_RUNTIME_RESOURCE_IO;out->expired=s->expired;out->clock_error=s->clock_error;return GT_RUNTIME_RESOURCE_OK;
}
gt_runtime_resource_status_t gt__runtime_resources_cancel(gt_runtime_resources_t *r,uint64_t id){runtime_source_t *s=source_owner(r,id);if(!s)return GT_RUNTIME_RESOURCE_INVALID;gt_ext_source_cancel(s->source);return GT_RUNTIME_RESOURCE_OK;}
gt_runtime_resource_status_t gt__runtime_resources_forget(gt_runtime_resources_t *r,uint64_t id){
 runtime_source_t *s=source_owner(r,id);if(!s)return GT_RUNTIME_RESOURCE_INVALID;gt_ext_source_info_t v;if(!gt_ext_source_info(s->source,&v))return GT_RUNTIME_RESOURCE_IO;if(!source_terminal(v.phase))return GT_RUNTIME_RESOURCE_WAIT;
 gt_runtime_resource_status_t status=source_status(gt_ext_source_destroy(&s->source));if(status==GT_RUNTIME_RESOURCE_OK)memset(s,0,sizeof(*s));return status;
}
gt_runtime_resource_status_t gt__runtime_resources_take(gt_runtime_resources_t *r,uint64_t *id,gt_ext_resource_descriptor_t *out){
 if(id)*id=0;if(out)memset(out,0,sizeof(*out));if(!r || !id || !out)return GT_RUNTIME_RESOURCE_INVALID;gt_runtime_resource_status_t access=resource_access(r);if(access!=GT_RUNTIME_RESOURCE_OK)return access;
 runtime_pin_t *slot=NULL;if(r->next)for(size_t i=0;i<GT_RUNTIME_PINS_MAX;i++)if(!r->pins[i].id){slot=&r->pins[i];break;}if(!slot)return GT_RUNTIME_RESOURCE_LIMIT;
 gt_runtime_resource_status_t status=resource_status(gt_ext_session_take_resource(r->session,&r->lease,&slot->snapshot));if(status!=GT_RUNTIME_RESOURCE_OK)return status;slot->id=r->next++;*id=slot->id;*out=slot->snapshot.descriptor;return GT_RUNTIME_RESOURCE_OK;
}
gt_runtime_resource_status_t gt__runtime_resources_read(gt_runtime_resources_t *r,uint64_t id,uint64_t at,void *bytes,size_t cap,size_t *read){
 if(read)*read=0;runtime_pin_t *slot=pin_owner(r,id);if(!slot || !read || !bytes || !cap || cap>GT_EXT_RESOURCE_READ_MAX)return GT_RUNTIME_RESOURCE_INVALID;
 /* Published snapshots may survive native revoke, but runtime stop forbids new
  * plugin reads. Cleanup release remains available after stop. */
 gt_runtime_resource_status_t access=resource_access(r);if(access!=GT_RUNTIME_RESOURCE_OK)return access;
 if(!slot->reading || at!=slot->read_at)return GT_RUNTIME_RESOURCE_INVALID;if(slot->read_error!=GT_RUNTIME_RESOURCE_OK)return slot->read_error;if(slot->complete)return GT_RUNTIME_RESOURCE_CLOSED;
 uint64_t now;if(!r->clock(r->clock_context,&now))return slot->read_error=GT_RUNTIME_RESOURCE_IO;if(now>=slot->deadline)return slot->read_error=GT_RUNTIME_RESOURCE_EXPIRED;
 if(!r->read_budget)return GT_RUNTIME_RESOURCE_WAIT;if(cap>r->read_budget)cap=r->read_budget;
 gt_runtime_resource_status_t status=resource_status(gt_ext_resource_store_read(r->store,&r->lease,&slot->snapshot,at,bytes,cap,read));
 if(status==GT_RUNTIME_RESOURCE_OK){r->read_budget-=*read;slot->read_at+=*read;if(!*read)slot->complete=true;}else if(status!=GT_RUNTIME_RESOURCE_WAIT)slot->read_error=status;return status;
}
gt_runtime_resource_status_t gt__runtime_resources_begin_read(gt_runtime_resources_t *r,uint64_t id,uint64_t limit,uint32_t timeout){
 runtime_pin_t *slot=pin_owner(r,id);if(!slot || !timeout || timeout>3600000)return GT_RUNTIME_RESOURCE_INVALID;gt_runtime_resource_status_t access=resource_access(r);if(access!=GT_RUNTIME_RESOURCE_OK)return access;if(slot->reading)return GT_RUNTIME_RESOURCE_BUSY;
 if(limit>16777216 || limit<slot->snapshot.descriptor.size)return GT_RUNTIME_RESOURCE_LIMIT;uint64_t now;if(!r->clock(r->clock_context,&now) || now>UINT64_MAX-timeout)return GT_RUNTIME_RESOURCE_IO;
 slot->reading=true;slot->deadline=now+timeout;return GT_RUNTIME_RESOURCE_OK;
}
gt_runtime_resource_status_t gt__runtime_resources_release(gt_runtime_resources_t *r,uint64_t id){runtime_pin_t *slot=pin_owner(r,id);if(!slot)return GT_RUNTIME_RESOURCE_INVALID;gt_runtime_resource_status_t status=resource_status(gt_ext_resource_store_release(r->store,&r->lease,&slot->snapshot));if(status==GT_RUNTIME_RESOURCE_NOT_FOUND)status=GT_RUNTIME_RESOURCE_OK;if(status==GT_RUNTIME_RESOURCE_OK)memset(slot,0,sizeof(*slot));return status;}
void gt__runtime_resources_stop(gt_runtime_resources_t *r){if(r){r->stopped=true;for(size_t i=0;i<GT_EXT_SOURCE_LIMIT;i++)if(r->sources[i].source)gt_ext_source_cancel(r->sources[i].source);}}
gt_runtime_resource_status_t gt__runtime_resources_destroy(gt_runtime_resources_t **out){
 if(!out || !*out)return GT_RUNTIME_RESOURCE_INVALID;gt_runtime_resources_t *r=*out;gt__runtime_resources_stop(r);gt_runtime_resource_status_t result=GT_RUNTIME_RESOURCE_OK;
 for(size_t i=0;i<GT_EXT_SOURCE_LIMIT;i++)if(r->sources[i].source){gt_runtime_resource_status_t s=source_status(gt_ext_source_destroy(&r->sources[i].source));if(s!=GT_RUNTIME_RESOURCE_OK){if(result==GT_RUNTIME_RESOURCE_OK || result==GT_RUNTIME_RESOURCE_WAIT)result=s;}else memset(&r->sources[i],0,sizeof(r->sources[i]));}
 for(size_t i=0;i<GT_RUNTIME_PINS_MAX;i++)if(r->pins[i].id){gt_runtime_resource_status_t s=gt__runtime_resources_release(r,r->pins[i].id);if(s!=GT_RUNTIME_RESOURCE_OK && (result==GT_RUNTIME_RESOURCE_OK || result==GT_RUNTIME_RESOURCE_WAIT))result=s;}
 if(result!=GT_RUNTIME_RESOURCE_OK)return result;
 bool error=false;for(size_t i=0;i<r->root_count;i++)if(r->roots[i].fd>=0){int fd=r->roots[i].fd;r->roots[i].fd=-1;if(close(fd))error=true;}if(error)return GT_RUNTIME_RESOURCE_IO;
 gt__secure_erase(r,sizeof(*r));gt__plugin_memory_free(NULL,r,sizeof(*r));*out=NULL;return GT_RUNTIME_RESOURCE_OK;
}
