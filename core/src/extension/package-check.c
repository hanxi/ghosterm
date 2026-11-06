#define _POSIX_C_SOURCE 200809L
#include "extension/memory.h"
#include "extension/package-check.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
struct gt_package_checked {gt_allocator_t allocator;gt_package_snapshot_t *snapshot;gt_package_check_info_t info;atomic_size_t references;size_t allocation_size,command_count;gt_package_command_t commands[];};
static gt_package_check_status_t check_report(gt_package_check_report_t *r,gt_package_check_status_t status,const char *code){if(r){memset(r,0,sizeof(*r));r->status=status;strncpy(r->code,code,64);}return status;}
#ifndef GT_HAVE_PLUGIN_JS
gt_package_check_status_t gt__package_check(gt_package_snapshot_t **s,const gt_package_check_config_t *c,const gt_allocator_t *a,gt_package_checked_t **out,gt_package_check_report_t *report){(void)s;(void)c;(void)a;if(out)*out=NULL;return check_report(report,GT_PACKAGE_CHECK_UNSUPPORTED,"ENGINE_UNAVAILABLE");}
#else
#include "extension/package-check-js.inc"
#endif
bool gt__package_checked_info(const gt_package_checked_t *p,gt_package_check_info_t *out){if(out)memset(out,0,sizeof(*out));if(!p || !out)return false;*out=p->info;return true;}
bool gt__package_checked_member(const gt_package_checked_t *p,const char *path,gt_package_member_t *info,const uint8_t **bytes,size_t *size){
 if(info)memset(info,0,sizeof(*info));if(bytes)*bytes=NULL;if(size)*size=0;size_t index;
 if(!p || !info || !bytes || !size || gt__package_snapshot_find(p->snapshot,path,&index)!=GT_PACKAGE_OK)return false;
 return gt__package_snapshot_member(p->snapshot,index,info) && gt__package_snapshot_view(p->snapshot,index,bytes,size);
}
bool gt__package_checked_retain(gt_package_checked_t *p,gt_package_checked_t **out){
 if(out)*out=NULL;if(!p || !out)return false;size_t n=atomic_load_explicit(&p->references,memory_order_relaxed);
 while(n && n<SIZE_MAX){if(atomic_compare_exchange_weak_explicit(&p->references,&n,n+1,memory_order_relaxed,memory_order_relaxed)){*out=p;return true;}}return false;
}
gt_package_status_t gt__package_checked_destroy(gt_package_checked_t **out){if(!out || !*out)return GT_PACKAGE_INVALID;gt_package_checked_t *p=*out;*out=NULL;if(atomic_fetch_sub_explicit(&p->references,1,memory_order_acq_rel)!=1)return GT_PACKAGE_OK;gt_package_status_t status=gt__package_snapshot_destroy(&p->snapshot);gt_allocator_t a=p->allocator;size_t bytes=p->allocation_size;gt__secure_erase(p,bytes);gt__plugin_memory_free(&a,p,bytes);return status;}

size_t gt__package_checked_command_count(const gt_package_checked_t *p){return p?p->command_count:0;}
bool gt__package_checked_command(const gt_package_checked_t *p,size_t index,gt_package_command_t *out){if(out)memset(out,0,sizeof(*out));if(!p || !out || index>=p->command_count)return false;*out=p->commands[index];return true;}
