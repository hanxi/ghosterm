#include "extension/memory.h"
#include "ghosterm/extension_runtime.h"
#include "extension/runtime.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
struct gt_ext_runtime {gt_bridge_runtime_t *inner;};
static gt_ext_runtime_phase_t ext_runtime_phase(gt_runtime_phase_t phase){
 switch(phase){case GT_RUNTIME_CREATED:return GT_EXT_RUNTIME_CREATED;case GT_RUNTIME_STARTING:return GT_EXT_RUNTIME_STARTING;case GT_RUNTIME_ACTIVE:return GT_EXT_RUNTIME_ACTIVE;case GT_RUNTIME_FAILED:return GT_EXT_RUNTIME_FAILED;case GT_RUNTIME_STOPPED:return GT_EXT_RUNTIME_STOPPED;case GT_RUNTIME_CLOSED:return GT_EXT_RUNTIME_CLOSED;}return GT_EXT_RUNTIME_FAILED;
}
static gt_ext_runtime_error_t ext_runtime_error(gt_runtime_error_t error){
 switch(error){case GT_RUNTIME_NONE:return GT_EXT_RUNTIME_NONE;case GT_RUNTIME_SCRIPT:return GT_EXT_RUNTIME_SCRIPT;case GT_RUNTIME_MEMORY:return GT_EXT_RUNTIME_MEMORY;case GT_RUNTIME_CPU:return GT_EXT_RUNTIME_CPU;case GT_RUNTIME_CANCELLED:return GT_EXT_RUNTIME_CANCELLED;case GT_RUNTIME_JOBS:return GT_EXT_RUNTIME_JOBS;case GT_RUNTIME_PROTOCOL:return GT_EXT_RUNTIME_PROTOCOL;case GT_RUNTIME_IO:return GT_EXT_RUNTIME_IO;case GT_RUNTIME_EXPIRED:return GT_EXT_RUNTIME_EXPIRED;}return GT_EXT_RUNTIME_IO;
}
gt_status_t gt_ext_runtime_support(void){
#ifdef GT_HAVE_PLUGIN_JS
 return GT_OK;
#else
 return GT_ERR_UNSUPPORTED;
#endif
}
gt_status_t gt_ext_runtime_open(gt_ext_session_t **session,const gt_ext_runtime_config_t *config,gt_ext_runtime_t **out){
 if(out)*out=NULL;if(!out)return GT_ERR_INVALID_ARG;if(gt_ext_runtime_support()!=GT_OK)return GT_ERR_UNSUPPORTED;
 if(!session || !*session || !config || !config->modules || !config->module_count || config->module_count>GT_EXT_RUNTIME_MODULES_MAX || config->memory_bytes>SIZE_MAX || config->stack_bytes>SIZE_MAX || config->root_count>GT_EXT_RUNTIME_ROOTS_MAX || (config->root_count && !config->roots))return GT_ERR_INVALID_ARG;
 gt_runtime_root_t roots[GT_EXT_RUNTIME_ROOTS_MAX];for(size_t i=0;i<config->root_count;i++)roots[i]=(gt_runtime_root_t){config->roots[i].id,config->roots[i].directory_fd,config->roots[i].byte_limit};
 gt_runtime_module_t modules[GT_EXT_RUNTIME_MODULES_MAX];
 for(size_t i=0;i<config->module_count;i++){const gt_ext_runtime_module_t *m=&config->modules[i];modules[i]=(gt_runtime_module_t){.name=m->name,.source=m->source,.size=m->size};memcpy(modules[i].sha256,m->sha256,65);}
 gt_runtime_config_t c={.modules=modules,.module_count=config->module_count,.entry=config->entry,.role=config->role,.memory_bytes=(size_t)config->memory_bytes,.stack_bytes=(size_t)config->stack_bytes,.cpu_ms=config->cpu_ms,.jobs_per_step=config->jobs_per_step,.activation_ms=config->activation_ms,.roots=roots,.root_count=config->root_count};
 gt_ext_runtime_t *r=gt__plugin_memory_calloc(1,sizeof(*r));if(!r)return GT_ERR_IO;gt_status_t status=gt__bridge_runtime_open(session,&c,&r->inner);
 if(status!=GT_OK){gt__secure_erase(r,sizeof(*r));gt__plugin_memory_free(NULL,r,sizeof(*r));return status;}*out=r;return GT_OK;
}
gt_ext_runtime_phase_t gt_ext_runtime_step(gt_ext_runtime_t *r){return r?ext_runtime_phase(gt__bridge_runtime_step(r->inner)):GT_EXT_RUNTIME_CLOSED;}
bool gt_ext_runtime_info(const gt_ext_runtime_t *r,gt_ext_runtime_info_t *out){
 if(out)memset(out,0,sizeof(*out));if(!r || !out)return false;gt_runtime_info_t i;if(!gt__bridge_runtime_info(r->inner,&i))return false;
 out->phase=ext_runtime_phase(i.phase);out->error=ext_runtime_error(i.error);out->cleanup_error=i.cleanup_error;out->session_end=i.session_end;out->session_error=i.session_error;out->dispose_error=i.dispose_error;out->engine_released=i.engine_released;out->steps=i.steps;out->jobs=i.jobs;out->module_bytes=i.module_bytes;return true;
}
void gt_ext_runtime_stop(gt_ext_runtime_t *r){if(r)gt__bridge_runtime_stop(r->inner);}
gt_ext_close_t gt_ext_runtime_close(gt_ext_runtime_t *r){return r?gt__bridge_runtime_close(r->inner):GT_EXT_CLOSE_ERROR;}
gt_status_t gt_ext_runtime_destroy(gt_ext_runtime_t **out){
 if(!out || !*out)return GT_ERR_INVALID_ARG;gt_ext_runtime_t *r=*out;gt_status_t status=gt__bridge_runtime_destroy(&r->inner);if(status!=GT_OK)return status;gt__secure_erase(r,sizeof(*r));gt__plugin_memory_free(NULL,r,sizeof(*r));*out=NULL;return GT_OK;
}
