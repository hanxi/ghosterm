/* Exact-size allocation accounting and deterministic failure injection. */
#ifndef GT_TEST_ALLOCATION_H
#define GT_TEST_ALLOCATION_H
#include <assert.h>
#include <stdlib.h>
#include "base/allocator.h"
typedef struct {
  struct {void *pointer;size_t bytes;} slots[8];
  size_t calls,fail,live,count,limit;
} allocation_test_budget_t;
static void *allocation_test_allocate(void *context,size_t n) {
  allocation_test_budget_t *b=context;b->calls++;
  if(b->calls==b->fail || n>b->limit-b->live)return NULL;
  assert(b->count<8);void *p=malloc(n);assert(p);
  b->slots[b->count].pointer=p;b->slots[b->count++].bytes=n;b->live+=n;return p;
}
static void allocation_test_release(void *context,void *p,size_t n) {
  allocation_test_budget_t *b=context;size_t i=0;
  while(i<b->count && b->slots[i].pointer!=p)i++;
  assert(i<b->count && b->slots[i].bytes==n && b->live>=n);
  b->live-=n;b->slots[i]=b->slots[--b->count];free(p);
}
#endif
