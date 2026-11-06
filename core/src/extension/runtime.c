#define _POSIX_C_SOURCE 200809L
#include "extension/runtime.h"
#include <string.h>
#ifndef GT_HAVE_PLUGIN_JS
gt_status_t gt__bridge_runtime_open(gt_ext_session_t **s,const gt_runtime_config_t *c,gt_bridge_runtime_t **out){(void)s;(void)c;if(out)*out=NULL;return GT_ERR_UNSUPPORTED;}
gt_status_t gt__bridge_runtime_open_package(gt_ext_session_t **s,gt_package_checked_t *p,const gt_runtime_config_t *c,bool a,gt_bridge_runtime_t **out){(void)s;(void)p;(void)c;(void)a;if(out)*out=NULL;return GT_ERR_UNSUPPORTED;}
gt_runtime_phase_t gt__bridge_runtime_step(gt_bridge_runtime_t *r){(void)r;return GT_RUNTIME_CLOSED;}
bool gt__bridge_runtime_info(const gt_bridge_runtime_t *r,gt_runtime_info_t *out){(void)r;if(out)memset(out,0,sizeof(*out));return false;}
void gt__bridge_runtime_stop(gt_bridge_runtime_t *r){(void)r;}
gt_ext_close_t gt__bridge_runtime_close(gt_bridge_runtime_t *r){(void)r;return GT_EXT_CLOSE_ERROR;}
gt_status_t gt__bridge_runtime_destroy(gt_bridge_runtime_t **r){(void)r;return GT_ERR_UNSUPPORTED;}
#else
#include "extension/runtime-quickjs.inc"
#endif
