#include "extension/session-owner.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>

static gt_ext_resource_status_t resource_status(gt_bridge_resource_status_t s){
 switch(s){
 case GT_BRIDGE_RESOURCE_OK:return GT_EXT_RESOURCE_OK;case GT_BRIDGE_RESOURCE_WAIT:return GT_EXT_RESOURCE_WAIT;
 case GT_BRIDGE_RESOURCE_INVALID:return GT_EXT_RESOURCE_INVALID;case GT_BRIDGE_RESOURCE_DENIED:return GT_EXT_RESOURCE_DENIED;
 case GT_BRIDGE_RESOURCE_CLOSED:return GT_EXT_RESOURCE_CLOSED;case GT_BRIDGE_RESOURCE_PROTOCOL:return GT_EXT_RESOURCE_PROTOCOL;
 case GT_BRIDGE_RESOURCE_LIMIT:return GT_EXT_RESOURCE_LIMIT;case GT_BRIDGE_RESOURCE_IO:return GT_EXT_RESOURCE_IO;
 case GT_BRIDGE_RESOURCE_UNSUPPORTED:return GT_EXT_RESOURCE_UNSUPPORTED;case GT_BRIDGE_RESOURCE_NOT_FOUND:return GT_EXT_RESOURCE_NOT_FOUND;
 }return GT_EXT_RESOURCE_INVALID;
}
static gt_bridge_lease_t resource_lease(const gt_ext_lease_t *l){
 gt_bridge_lease_t p;memcpy(p.connection,l->connection,33);memcpy(p.instance,l->instance,33);memcpy(p.token,l->token,33);p.epoch=l->epoch;return p;
}
static gt_bridge_resource_ref_t resource_ref(const gt_ext_resource_snapshot_t *s){gt_bridge_resource_ref_t r={.handle=s->handle};memcpy(r.token,s->token,33);return r;}
static gt_bridge_resource_descriptor_t resource_descriptor(const gt_ext_resource_descriptor_t *d){
 gt_bridge_resource_descriptor_t p={.generation=d->generation,.size=d->size,.size_known=d->size_known};memcpy(p.resource_id,d->resource_id,33);memcpy(p.sha256,d->sha256,65);memcpy(p.mime,d->mime,129);memcpy(p.display_name,d->display_name,1025);return p;
}
static gt_ext_resource_descriptor_t resource_metadata(const gt_bridge_resource_descriptor_t *p){
 gt_ext_resource_descriptor_t d={.generation=p->generation,.size=p->size,.size_known=p->size_known};memcpy(d.resource_id,p->resource_id,33);memcpy(d.sha256,p->sha256,65);memcpy(d.mime,p->mime,129);memcpy(d.display_name,p->display_name,1025);return d;
}
gt_ext_resource_status_t gt_ext_resource_store_open(const gt_ext_resource_store_config_t *c,gt_ext_resource_store_t **out){
 if(out)*out=NULL;if(!c || !out)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_store_t *s=calloc(1,sizeof(*s));if(!s)return GT_EXT_RESOURCE_IO;
 gt_bridge_resource_config_t p={c->directory_fd,c->total_bytes,c->resource_bytes,c->slots};gt_ext_resource_status_t status=resource_status(gt__bridge_resource_open(&p,NULL,&s->inner));
 if(status!=GT_EXT_RESOURCE_OK){free(s);return status;}s->total_bytes=c->total_bytes;*out=s;return GT_EXT_RESOURCE_OK;
}
bool gt_ext_resource_store_usage(const gt_ext_resource_store_t *s,gt_ext_resource_usage_t *out){
 if(out)memset(out,0,sizeof(*out));if(!s || !out)return false;gt_bridge_resource_usage_t u;if(!gt__bridge_resource_usage(s->inner,&u))return false;out->reserved_bytes=u.reserved_bytes;out->resources=u.resources;out->scopes=u.scopes;out->sessions=s->sessions;return true;
}
gt_ext_resource_status_t gt_ext_resource_store_info(gt_ext_resource_store_t *s,const gt_ext_lease_t *lease,const gt_ext_resource_snapshot_t *ref,gt_ext_resource_info_t *out){
 if(out)memset(out,0,sizeof(*out));if(!s || !lease || !ref || !out)return GT_EXT_RESOURCE_INVALID;
 gt_bridge_lease_t l=resource_lease(lease);gt_bridge_resource_ref_t r=resource_ref(ref);gt_bridge_resource_info_t i;
 gt_ext_resource_status_t status=resource_status(gt__bridge_resource_info(s->inner,ref->owner,&l,&r,&i));if(status!=GT_EXT_RESOURCE_OK)return status;
 out->descriptor=resource_metadata(&i.descriptor);out->received=i.received;out->reserved=i.reserved;out->references=i.references;out->published=i.published;return status;
}
gt_ext_resource_status_t gt_ext_resource_store_read(gt_ext_resource_store_t *s,const gt_ext_lease_t *lease,const gt_ext_resource_snapshot_t *ref,uint64_t offset,void *p,size_t cap,size_t *read_bytes){
 if(read_bytes)*read_bytes=0;if(!s || !lease || !ref || !read_bytes)return GT_EXT_RESOURCE_INVALID;
 gt_bridge_lease_t l=resource_lease(lease);gt_bridge_resource_ref_t r=resource_ref(ref);return resource_status(gt__bridge_resource_read(s->inner,ref->owner,&l,&r,offset,p,cap,read_bytes));
}
gt_ext_resource_status_t gt_ext_resource_store_retain(gt_ext_resource_store_t *s,const gt_ext_lease_t *lease,const gt_ext_resource_snapshot_t *ref){
 if(!s || !lease || !ref)return GT_EXT_RESOURCE_INVALID;gt_bridge_lease_t l=resource_lease(lease);gt_bridge_resource_ref_t r=resource_ref(ref);return resource_status(gt__bridge_resource_retain(s->inner,ref->owner,&l,&r));
}
gt_ext_resource_status_t gt_ext_resource_store_release(gt_ext_resource_store_t *s,const gt_ext_lease_t *lease,gt_ext_resource_snapshot_t *ref){
 if(!s || !lease || !ref)return GT_EXT_RESOURCE_INVALID;gt_bridge_lease_t l=resource_lease(lease);gt_bridge_resource_ref_t r=resource_ref(ref);
 gt_ext_resource_status_t status=resource_status(gt__bridge_resource_release(s->inner,ref->owner,&l,&r));if(status==GT_EXT_RESOURCE_OK || status==GT_EXT_RESOURCE_NOT_FOUND)gt__secure_erase(ref,sizeof(*ref));return status;
}
gt_ext_resource_status_t gt_ext_resource_store_destroy(gt_ext_resource_store_t **owned){
 if(!owned || !*owned)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_store_t *s=*owned;if(s->sessions)return GT_EXT_RESOURCE_WAIT;
 gt__bridge_resource_collect_retired(s->inner);gt_ext_resource_status_t status=resource_status(gt__bridge_resource_destroy(s->inner));
 if(status==GT_EXT_RESOURCE_OK || status==GT_EXT_RESOURCE_IO){gt__secure_erase(s,sizeof(*s));free(s);*owned=NULL;}return status;
}
static gt_ext_resource_status_t resource_scope(gt_ext_session_t *s,const gt_ext_lease_t *lease){
 if(!s || !lease)return GT_EXT_RESOURCE_INVALID;if(!s->store)return GT_EXT_RESOURCE_UNSUPPORTED;gt_bridge_lease_t l=resource_lease(lease);return gt__bridge_session_scope(s->inner,&l)?GT_EXT_RESOURCE_OK:GT_EXT_RESOURCE_DENIED;
}
gt_ext_resource_status_t gt_ext_session_send_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease,const gt_ext_resource_descriptor_t *d,uint64_t *handle){
 if(handle)*handle=0;if(!handle || !d)return GT_EXT_RESOURCE_INVALID;
 /* Fixed POD arrays from a C caller need not contain NUL. Reject before any
  * private formatter can read beyond one field into its adjacent metadata. */
 if(!memchr(d->resource_id,0,sizeof(d->resource_id)) || !memchr(d->sha256,0,sizeof(d->sha256)) || !memchr(d->mime,0,sizeof(d->mime)) || !memchr(d->display_name,0,sizeof(d->display_name)))return GT_EXT_RESOURCE_INVALID;
 gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);gt_bridge_resource_descriptor_t p=resource_descriptor(d);return resource_status(gt__bridge_session_send_resource(s->inner,&l,&p,handle));
}
gt_ext_resource_status_t gt_ext_session_write_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle,const void *p,size_t n,size_t *accepted){
 if(accepted)*accepted=0;if(!accepted)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);return resource_status(gt__bridge_session_write_resource(s->inner,&l,handle,p,n,accepted));
}
gt_ext_resource_status_t gt_ext_session_finish_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle,bool complete){gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);return resource_status(gt__bridge_session_finish_resource(s->inner,&l,handle,complete));}
gt_ext_resource_status_t gt_ext_session_cancel_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle){gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);return resource_status(gt__bridge_session_cancel_resource(s->inner,&l,handle));}
gt_ext_resource_status_t gt_ext_session_resource_result(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle,gt_ext_transfer_result_t *out){
 if(out)memset(out,0,sizeof(*out));if(!out)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;
 gt_bridge_lease_t l=resource_lease(lease);gt_bridge_transfer_result_t r;if(!gt__bridge_session_resource_result(s->inner,&l,handle,&r))return GT_EXT_RESOURCE_INVALID;
 out->handle=r.handle;out->accepted=r.accepted;out->confirmed=r.confirmed;out->cancel_requested=r.cancel_requested;out->generation=r.generation;memcpy(out->resource_id,r.resource_id,33);memcpy(out->code,r.code,33);
 switch(r.state){case GT_BRIDGE_TRANSFER_QUEUED:out->state=GT_EXT_TRANSFER_QUEUED;break;case GT_BRIDGE_TRANSFER_SENDING:out->state=GT_EXT_TRANSFER_SENDING;break;case GT_BRIDGE_TRANSFER_PENDING:out->state=GT_EXT_TRANSFER_PENDING;break;case GT_BRIDGE_TRANSFER_SUCCEEDED:out->state=GT_EXT_TRANSFER_SUCCEEDED;break;case GT_BRIDGE_TRANSFER_FAILED:out->state=GT_EXT_TRANSFER_FAILED;break;case GT_BRIDGE_TRANSFER_CANCELLED:out->state=GT_EXT_TRANSFER_CANCELLED;break;case GT_BRIDGE_TRANSFER_UNKNOWN:out->state=GT_EXT_TRANSFER_UNKNOWN;break;}return GT_EXT_RESOURCE_OK;
}
gt_ext_resource_status_t gt_ext_session_forget_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease,uint64_t handle){gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);return resource_status(gt__bridge_session_forget_resource(s->inner,&l,handle));}
gt_ext_resource_status_t gt_ext_session_take_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease,gt_ext_resource_snapshot_t *out){
 if(out)memset(out,0,sizeof(*out));if(!out)return GT_EXT_RESOURCE_INVALID;gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);gt_bridge_snapshot_t p;
 status=resource_status(gt__bridge_session_take_resource(s->inner,&l,&p));if(status!=GT_EXT_RESOURCE_OK)return status;out->owner=p.owner;out->handle=p.ref.handle;memcpy(out->token,p.ref.token,33);out->descriptor=resource_metadata(&p.descriptor);return status;
}
gt_ext_resource_status_t gt_ext_session_reject_resource(gt_ext_session_t *s,const gt_ext_lease_t *lease){gt_ext_resource_status_t status=resource_scope(s,lease);if(status!=GT_EXT_RESOURCE_OK)return status;gt_bridge_lease_t l=resource_lease(lease);return resource_status(gt__bridge_session_reject_resource(s->inner,&l));}
