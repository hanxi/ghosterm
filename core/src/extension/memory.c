#include "extension/memory.h"
#include "base/secure-random.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
static gt_plugin_memory_info_t memory={GT_PLUGIN_MEMORY_DEFAULT,0,0};

gt_plugin_memory_info_t gt__plugin_memory_info(void){
 pthread_mutex_lock(&memory_lock);gt_plugin_memory_info_t out=memory;pthread_mutex_unlock(&memory_lock);return out;
}
bool gt__plugin_memory_limit(size_t limit){
 pthread_mutex_lock(&memory_lock);bool ok=limit && !memory.used;
 if(ok)memory=(gt_plugin_memory_info_t){limit,0,0};pthread_mutex_unlock(&memory_lock);return ok;
}
static void uncharge(size_t n){pthread_mutex_lock(&memory_lock);memory.used-=n;pthread_mutex_unlock(&memory_lock);}
void *gt__plugin_memory_alloc(const gt_allocator_t *a,size_t n){
 if(!n || (a && (!a->allocate || !a->release)))return NULL;
 pthread_mutex_lock(&memory_lock);bool ok=n<=memory.limit-memory.used;
 if(ok){memory.used+=n;if(memory.used>memory.peak)memory.peak=memory.used;}
 pthread_mutex_unlock(&memory_lock);if(!ok)return NULL;
 void *p=a?a->allocate(a->context,n):malloc(n);if(!p)uncharge(n);return p;
}
void *gt__plugin_memory_calloc(size_t count,size_t n){
 if(count && n>SIZE_MAX/count)return NULL;size_t bytes=count*n;
 void *p=gt__plugin_memory_alloc(NULL,bytes);if(p)memset(p,0,bytes);return p;
}
void gt__plugin_memory_free(const gt_allocator_t *a,void *p,size_t n){
 if(!p)return;gt__secure_erase(p,n);if(a)a->release(a->context,p,n);else free(p);uncharge(n);
}
