/* Only host:/bootstrap.js can import these native functions. Plugin-visible
 * ResourceRef objects never contain native snapshot/lease/fd tokens. */
static const char *resource_status_names[]={"ok","wait","invalid","denied","unsupported","io","limit","changed","cancelled","busy","unknown","closed","not-found","expired","protocol"};
static const char *source_phase_names[]={"hashing","ready","sending","receipt","cancelling","succeeded","failed","stopped","uncertain"};
static const char *source_error_names[]={"ok","wait","invalid","denied","unsupported","io","limit","changed","cancelled","busy","unknown","closed","not-found"};
static const char *transfer_state_names[]={"queued","sending","pending","succeeded","failed","cancelled","unknown"};
static bool resource_decimal(JSContext *c,JSValueConst v,uint64_t *out){
 if(!JS_IsString(v))return false;size_t n;const char *s=JS_ToCStringLen(c,&n,v);if(!s)return false;if(n==1 && s[0]=='0'){*out=0;JS_FreeCString(c,s);return true;}JS_FreeCString(c,s);return runtime_handle(c,v,out);
}
static bool resource_string(JSContext *c,JSValueConst v,size_t cap,const char **out){
 *out=NULL;if(!JS_IsString(v))return false;size_t n;const char *s=JS_ToCStringLen(c,&n,v);if(!s)return false;if(!n || n>cap || memchr(s,0,n)){JS_FreeCString(c,s);return false;}*out=s;return true;
}
static JSValue resource_oom(gt_bridge_runtime_t *r,JSValue value){JS_FreeValue(r->context,value);runtime_fail(r,GT_RUNTIME_MEMORY);return JS_EXCEPTION;}
static JSValue resource_descriptor(JSContext *c,const gt_ext_resource_descriptor_t *d){
 JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out))return out;
 if(runtime_set(c,out,"id",JS_NewString(c,d->resource_id))<0 || runtime_set(c,out,"generation",runtime_decimal(c,d->generation))<0 || runtime_set(c,out,"sha256",JS_NewString(c,d->sha256))<0 || runtime_set(c,out,"displayName",JS_NewString(c,d->display_name))<0 || runtime_set(c,out,"mediaType",JS_NewString(c,d->mime))<0 || runtime_set(c,out,"sizeKnown",JS_NewBool(c,d->size_known))<0 || runtime_set(c,out,"bytes",runtime_decimal(c,d->size))<0){JS_FreeValue(c,out);return JS_EXCEPTION;}return out;
}
static JSValue runtime_resource_native(JSContext *c,JSValueConst self,int argc,JSValueConst *argv,int op){
 (void)self;gt_bridge_runtime_t *r=JS_GetContextOpaque(c);if(!r || !r->session)return JS_ThrowInternalError(c,"runtime closed");
 if(op==0){
  if(argc)return JS_ThrowTypeError(c,"invalid resource operation");if(!r->resources)return JS_NULL;JSValue out=JS_NewArray(c);if(JS_IsException(out))return resource_oom(r,out);
  for(size_t i=0;i<gt__runtime_resources_roots(r->resources);i++){JSValue value=JS_NewString(c,gt__runtime_resources_root(r->resources,i));if(JS_IsException(value) || JS_SetPropertyUint32(c,out,(uint32_t)i,value)<0)return resource_oom(r,out);}return out;
 }
 if(!r->resources)return JS_NewString(c,"unsupported");
 bool stopped=atomic_load_explicit(&r->stopped,memory_order_relaxed) || r->info.phase>=GT_RUNTIME_FAILED;
 if(stopped && (op==1 || op==5 || op==6 || op==8))return JS_ThrowInternalError(c,"runtime stopped");
 uint64_t handle=0;gt_runtime_resource_status_t status=GT_RUNTIME_RESOURCE_INVALID;
 if(op==1){
  const char *root=NULL,*path=NULL,*mime=NULL;uint64_t limit;double timeout=0;
  if(argc!=5 || !resource_string(c,argv[0],64,&root) || !resource_string(c,argv[1],4096,&path) || !resource_string(c,argv[2],128,&mime) || !resource_decimal(c,argv[3],&limit) || !JS_IsNumber(argv[4]) || JS_ToFloat64(c,&timeout,argv[4]) || !(timeout>=1 && timeout<=3600000) || timeout!=(uint32_t)timeout){JS_FreeCString(c,root);JS_FreeCString(c,path);JS_FreeCString(c,mime);goto invalid;}
  JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out)){JS_FreeCString(c,root);JS_FreeCString(c,path);JS_FreeCString(c,mime);return resource_oom(r,out);}
  status=gt__runtime_resources_publish(r->resources,root,path,mime,limit,(uint32_t)timeout,&handle);JS_FreeCString(c,root);JS_FreeCString(c,path);JS_FreeCString(c,mime);
  if(runtime_set(c,out,"status",JS_NewString(c,resource_status_names[status]))<0 || (handle && runtime_set(c,out,"handle",runtime_decimal(c,handle))<0))return resource_oom(r,out);return out;
 }
 if(op==5){
  if(argc)goto invalid;JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out))return resource_oom(r,out);gt_ext_resource_descriptor_t d;
  status=gt__runtime_resources_take(r->resources,&handle,&d);
  if(runtime_set(c,out,"status",JS_NewString(c,resource_status_names[status]))<0 || (status==GT_RUNTIME_RESOURCE_OK && (runtime_set(c,out,"handle",runtime_decimal(c,handle))<0 || runtime_set(c,out,"descriptor",resource_descriptor(c,&d))<0)))return resource_oom(r,out);return out;
 }
 if(argc<1 || !runtime_handle(c,argv[0],&handle))goto invalid;
 if(op==2){
  if(argc!=1)goto invalid;gt_runtime_publication_t p;status=gt__runtime_resources_publication(r->resources,handle,&p);if(status!=GT_RUNTIME_RESOURCE_OK)return JS_ThrowTypeError(c,"publication unavailable");
  JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out))return resource_oom(r,out);gt_ext_resource_descriptor_t d=p.source.descriptor;memcpy(d.resource_id,p.source.resource_id,33);d.generation=p.source.generation;
  if(runtime_set(c,out,"phase",JS_NewString(c,source_phase_names[p.source.phase]))<0 || runtime_set(c,out,"error",JS_NewString(c,source_error_names[p.source.error]))<0 || runtime_set(c,out,"expired",JS_NewBool(c,p.expired))<0 || runtime_set(c,out,"clockError",JS_NewBool(c,p.clock_error))<0 || runtime_set(c,out,"hasTransfer",JS_NewBool(c,p.source.has_transfer))<0 || runtime_set(c,out,"confirmed",JS_NewBool(c,p.source.confirmed))<0 || runtime_set(c,out,"transferState",JS_NewString(c,transfer_state_names[p.source.transfer_state]))<0 || runtime_set(c,out,"readBytes",runtime_decimal(c,p.source.read_bytes))<0 || runtime_set(c,out,"accepted",runtime_decimal(c,p.source.accepted))<0 || runtime_set(c,out,"descriptor",resource_descriptor(c,&d))<0)return resource_oom(r,out);return out;
 }
 if(op==6){
  uint64_t offset;double cap;if(argc!=3 || !resource_decimal(c,argv[1],&offset) || !JS_IsNumber(argv[2]) || JS_ToFloat64(c,&cap,argv[2]) || !(cap>=1 && cap<=GT_EXT_RESOURCE_READ_MAX) || cap!=(size_t)cap)goto invalid;
  JSValue out=JS_NewObjectProto(c,JS_NULL);if(JS_IsException(out))return resource_oom(r,out);uint8_t bytes[GT_EXT_RESOURCE_READ_MAX];size_t read=0;status=gt__runtime_resources_read(r->resources,handle,offset,bytes,(size_t)cap,&read);
  if(runtime_set(c,out,"status",JS_NewString(c,resource_status_names[status]))<0 || runtime_set(c,out,"bytes",JS_NewUint8ArrayCopy(c,bytes,read))<0)return resource_oom(r,out);return out;
 }
 if(op==8){
  uint64_t limit;double timeout;if(argc!=3 || !resource_decimal(c,argv[1],&limit) || !JS_IsNumber(argv[2]) || JS_ToFloat64(c,&timeout,argv[2]) || !(timeout>=1 && timeout<=3600000) || timeout!=(uint32_t)timeout)goto invalid;
  status=gt__runtime_resources_begin_read(r->resources,handle,limit,(uint32_t)timeout);return JS_NewString(c,resource_status_names[status]);
 }
 if(argc!=1)goto invalid;
 if(op==3)status=gt__runtime_resources_cancel(r->resources,handle);
 else if(op==4)status=gt__runtime_resources_forget(r->resources,handle);
 else if(op==7)status=gt__runtime_resources_release(r->resources,handle);
 else goto invalid;
 return JS_NewString(c,resource_status_names[status]);
invalid:return JS_ThrowTypeError(c,"invalid resource operation");
}
static const char *runtime_resource_exports[]={"resourceWorkspaces","resourcePublish","resourcePublication","resourceCancel","resourceForget","resourceTake","resourceRead","resourceRelease","resourceOpen"};
