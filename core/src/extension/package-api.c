#define _POSIX_C_SOURCE 200809L
#include "extension/package-owner.h"
#include "extension/memory.h"
#include "base/secure-random.h"
#include <string.h>
#include <time.h>
static bool package_clock(void *context,uint64_t *out){(void)context;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static gt_ext_package_phase_t package_fail(gt_ext_package_t *p,gt_ext_package_error_t error){p->info.error=error;p->info.phase=GT_EXT_PACKAGE_FAILED;return p->info.phase;}
static gt_ext_package_error_t capture_error(gt_package_status_t s){
 switch(s){case GT_PACKAGE_INVALID:case GT_PACKAGE_NOT_FOUND:return GT_EXT_PACKAGE_INVALID;case GT_PACKAGE_LIMIT:return GT_EXT_PACKAGE_LIMIT;case GT_PACKAGE_CHANGED:return GT_EXT_PACKAGE_CHANGED;case GT_PACKAGE_NOMEM:return GT_EXT_PACKAGE_MEMORY;case GT_PACKAGE_IO:return GT_EXT_PACKAGE_IO;case GT_PACKAGE_OK:case GT_PACKAGE_WAIT:return GT_EXT_PACKAGE_NONE;}return GT_EXT_PACKAGE_INTERNAL;
}
gt_status_t gt_ext_package_open(const gt_ext_package_config_t *c,gt_ext_package_t **out){
 if(out)*out=NULL;if(!out)return GT_ERR_INVALID_ARG;
#ifndef GT_HAVE_PLUGIN_JS
 (void)c;return GT_ERR_UNSUPPORTED;
#else
 if(!c)return GT_ERR_INVALID_ARG;gt_ext_package_t *p=gt__plugin_memory_calloc(1,sizeof(*p));if(!p)return GT_ERR_IO;atomic_init(&p->stopped,false);
 gt_package_capture_config_t config={c->directory_fd,c->members,c->entries,c->member_bytes,c->total_bytes};
 gt_package_status_t status=gt__package_capture_open(&config,NULL,&p->snapshot);
 if(status!=GT_PACKAGE_OK){gt__plugin_memory_free(NULL,p,sizeof(*p));return status==GT_PACKAGE_INVALID || status==GT_PACKAGE_LIMIT?GT_ERR_INVALID_ARG:GT_ERR_IO;}
 p->info.phase=GT_EXT_PACKAGE_CAPTURING;*out=p;return GT_OK;
#endif
}
gt_ext_package_phase_t gt_ext_package_step(gt_ext_package_t *p){
 if(!p)return GT_EXT_PACKAGE_CLOSED;if(p->info.phase!=GT_EXT_PACKAGE_CAPTURING)return p->info.phase;
 if(atomic_load_explicit(&p->stopped,memory_order_relaxed))return package_fail(p,GT_EXT_PACKAGE_CANCELLED);
 gt_package_status_t status=gt__package_capture_step(p->snapshot);
 if(status==GT_PACKAGE_WAIT)return p->info.phase;if(status!=GT_PACKAGE_OK)return package_fail(p,capture_error(status));
 size_t count;uint64_t bytes;if(!gt__package_snapshot_count(p->snapshot,&count,&bytes))return package_fail(p,GT_EXT_PACKAGE_INTERNAL);
 p->info.members=count;p->info.bytes=bytes;p->info.phase=GT_EXT_PACKAGE_CAPTURED;return p->info.phase;
}
gt_ext_package_phase_t gt_ext_package_check(gt_ext_package_t *p,const gt_ext_package_check_config_t *c){
 if(!p)return GT_EXT_PACKAGE_CLOSED;if(p->info.phase!=GT_EXT_PACKAGE_CAPTURED)return p->info.phase;
 if(atomic_load_explicit(&p->stopped,memory_order_relaxed))return package_fail(p,GT_EXT_PACKAGE_CANCELLED);
 if(!c || c->memory_bytes>SIZE_MAX || c->stack_bytes>SIZE_MAX)return package_fail(p,GT_EXT_PACKAGE_INVALID);
 size_t host=c->host_json?strnlen(c->host_json,65537):0,registry=c->registry_json?strnlen(c->registry_json,1048577):0;
 if(host>65536 || registry>1048576)return package_fail(p,GT_EXT_PACKAGE_LIMIT);
 gt_package_check_config_t config={.host_json=c->host_json,.host_size=host,.registry_json=c->registry_json,.registry_size=registry,.memory_bytes=(size_t)c->memory_bytes,.stack_bytes=(size_t)c->stack_bytes,.cpu_ms=c->cpu_ms,.stop=&p->stopped,.clock=package_clock};
 gt_package_check_report_t report;gt_package_check_status_t s=gt__package_check(&p->snapshot,&config,NULL,&p->checked,&report);memcpy(p->info.code,report.code,sizeof(p->info.code));
 if(s==GT_PACKAGE_CHECK_COMPATIBLE){p->info.phase=GT_EXT_PACKAGE_CHECKED;return p->info.phase;}
 gt_ext_package_error_t error;
 switch(s){case GT_PACKAGE_CHECK_INVALID:error=GT_EXT_PACKAGE_INVALID;break;case GT_PACKAGE_CHECK_UNKNOWN:error=GT_EXT_PACKAGE_UNKNOWN;break;case GT_PACKAGE_CHECK_UNSUPPORTED:error=GT_EXT_PACKAGE_UNSUPPORTED;break;case GT_PACKAGE_CHECK_DENIED:error=GT_EXT_PACKAGE_DENIED;break;case GT_PACKAGE_CHECK_LIMIT:error=GT_EXT_PACKAGE_LIMIT;break;case GT_PACKAGE_CHECK_MEMORY:error=GT_EXT_PACKAGE_MEMORY;break;case GT_PACKAGE_CHECK_CANCELLED:error=GT_EXT_PACKAGE_CANCELLED;break;case GT_PACKAGE_CHECK_IO:error=GT_EXT_PACKAGE_IO;break;default:error=GT_EXT_PACKAGE_INTERNAL;break;}
 return package_fail(p,error);
}
bool gt_ext_package_info(const gt_ext_package_t *p,gt_ext_package_info_t *out){if(out)memset(out,0,sizeof(*out));if(!p || !out)return false;*out=p->info;return true;}
bool gt_ext_package_metadata(const gt_ext_package_t *p,gt_ext_package_metadata_t *out){
 if(out)memset(out,0,sizeof(*out));if(!p || !out || p->info.phase!=GT_EXT_PACKAGE_CHECKED)return false;gt_package_check_info_t i;if(!gt__package_checked_info(p->checked,&i))return false;
#define COPY(field) memcpy(out->field,i.field,sizeof(out->field))
 COPY(id);COPY(version);COPY(publisher);COPY(license);memcpy(out->entry,i.entry,sizeof(i.entry));COPY(runtime);COPY(variant);COPY(sdk_version);COPY(package_digest);COPY(manifest_digest);COPY(requires);COPY(optional_missing);COPY(protocols);
#undef COPY
 out->requires_count=(uint32_t)i.requires_count;out->optional_missing_count=(uint32_t)i.optional_missing_count;out->protocol_count=(uint32_t)i.protocol_count;out->executor=i.executor;out->association_required=i.association_required;out->authorization_granted=i.authorization_granted;out->publisher_authenticated=i.publisher_authenticated;return true;
}
void gt_ext_package_stop(gt_ext_package_t *p){if(p)atomic_store_explicit(&p->stopped,true,memory_order_relaxed);}
gt_status_t gt_ext_package_destroy(gt_ext_package_t **out){
 if(!out || !*out)return GT_ERR_INVALID_ARG;gt_ext_package_t *p=*out;*out=NULL;gt_status_t status=GT_OK;
 if(p->snapshot && gt__package_snapshot_destroy(&p->snapshot)!=GT_PACKAGE_OK)status=GT_ERR_IO;
 if(p->checked && gt__package_checked_destroy(&p->checked)!=GT_PACKAGE_OK)status=GT_ERR_IO;
 gt__secure_erase(p,sizeof(*p));gt__plugin_memory_free(NULL,p,sizeof(*p));return status;
}
