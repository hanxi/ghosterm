#include "memory.h"
#include "base/secure-random.h"
#include <stdlib.h>
static void *allocate(void *context,size_t n) {
  gt_daemon_memory_t *m=context;size_t used=atomic_load(&m->used);
  do {if(!n || n>m->limit || used>m->limit-n)return NULL;}
  while(!atomic_compare_exchange_weak(&m->used,&used,used+n));
  void *p=malloc(n);if(!p)atomic_fetch_sub(&m->used,n);return p;
}
static void release(void *context,void *p,size_t n) {
  gt_daemon_memory_t *m=context;gt__secure_erase(p,n);free(p);atomic_fetch_sub(&m->used,n);
}
void gt__daemon_memory_init(gt_daemon_memory_t *m,size_t limit){m->limit=limit;atomic_init(&m->used,0);}
gt_allocator_t gt__daemon_memory_allocator(gt_daemon_memory_t *m){return (gt_allocator_t){m,allocate,release};}
