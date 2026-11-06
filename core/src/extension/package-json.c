#include "extension/package-json.h"
#include "extension/memory.h"
#include "base/secure-random.h"
#include <stdlib.h>
#include <string.h>
#define GT_JSON_TAG gt_package_json
#define GT_JSON_DOC gt_package_json_t
#define GT_JSON_ERROR gt_package_json_error_t
#define GT_JSON_NODES GT_PACKAGE_JSON_NODES
#define GT_JSON_BYTES GT_PACKAGE_JSON_BYTES
#define GT_JSON_DEPTH GT_PACKAGE_JSON_DEPTH
#define GT_JSON_KEYS 32768u
#define GT_JSON_WORK 1048576u
#define GT_JSON_LIMIT GT_PACKAGE_JSON_LIMIT
#define GT_JSON_SYNTAX GT_PACKAGE_JSON_SYNTAX
#define GT_JSON_UTF8 GT_PACKAGE_JSON_UTF8
#define GT_JSON_DUPLICATE GT_PACKAGE_JSON_DUPLICATE
#define GT_JSON_DYNAMIC 1
#define GT_JSON_MEMORY GT_PACKAGE_JSON_MEMORY
#include "base/json-reader.inc"
bool gt__package_json_open(const gt_allocator_t *allocator,gt_package_json_t **out){
 if(out)*out=NULL;if(!out)return false;gt_allocator_t a=allocator?*allocator:(gt_allocator_t){NULL,allocate,release};if(!a.allocate || !a.release)return false;
 gt_package_json_t *d=gt__plugin_memory_alloc(&a,sizeof(*d));if(!d)return false;memset(d,0,sizeof(*d));d->allocator=a;*out=d;return true;
}
static gt_package_json_error_t json_check(gt_package_json_t *d,const void *bytes,size_t size,bool integers){
 gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));d->allocator=a;d->input=bytes;d->input_size=size;
 if(!bytes && size)return d->error=GT_PACKAGE_JSON_SYNTAX;if(size>GT_PACKAGE_JSON_BYTES)return d->error=GT_PACKAGE_JSON_LIMIT;
 if(!value(d,0,0,0,0))return d->error;space(d);if(d->at!=size)return d->error=GT_PACKAGE_JSON_SYNTAX;
 if(integers)for(uint16_t id=1;id<=d->used;id++)if(d->nodes[id].kind==J_NUMBER){uint64_t n;if(!node_uint(d,id,&n) || n>UINT64_C(9007199254740991))return d->error=GT_PACKAGE_JSON_NUMBER;}
 d->valid=true;return GT_PACKAGE_JSON_OK;
}
gt_package_json_error_t gt__package_json_check(gt_package_json_t *d,const void *bytes,size_t size,bool integers){
 if(!d)return GT_PACKAGE_JSON_SYNTAX;gt_package_json_error_t result=json_check(d,bytes,size,integers);
 /* No parsed views escape lexical preflight. Release even on parse/OOM failure,
  * before another document or JS engine starts; a large input is never cached. */
 json_buffers_free(d);return result;
}
void gt__package_json_destroy(gt_package_json_t **out){if(out && *out){gt_package_json_t *d=*out;*out=NULL;gt_allocator_t a=d->allocator;json_buffers_free(d);gt__plugin_memory_free(&a,d,sizeof(*d));}}
