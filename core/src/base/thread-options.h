/* Private POSIX thread settings. Stack capacity must be admitted by caller. */
#ifndef GT_PRIVATE_THREAD_OPTIONS_H
#define GT_PRIVATE_THREAD_OPTIONS_H
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
typedef struct {size_t stack_size,guard_size;} gt_thread_options_t;
/* NULL retains OS defaults. Explicit settings require positive page multiples,
 * a guard, and nonoverflowing combined size. pthread validates minimum stack.
 * OS rounding/bookkeeping may exceed requested stack; host admission must
 * include its verified platform overhead. This is not a memory allocator. */
static inline bool gt__thread_options_valid(const gt_thread_options_t *o) {
  if(!o)return true;
  long page=sysconf(_SC_PAGESIZE);
  return page>0 && o->stack_size>0 && o->guard_size>0 &&
    o->stack_size<=SIZE_MAX-o->guard_size &&
    !(o->stack_size%(size_t)page) && !(o->guard_size%(size_t)page);
}
static inline int gt__thread_attributes(pthread_attr_t *attr,const gt_thread_options_t *o) {
  if(!gt__thread_options_valid(o))return EINVAL;
  int rc=pthread_attr_init(attr);if(rc)return rc;
  if(o) {
    rc=pthread_attr_setstacksize(attr,o->stack_size);
    if(!rc)rc=pthread_attr_setguardsize(attr,o->guard_size);
    if(rc)pthread_attr_destroy(attr);
  }
  return rc;
}
static inline int gt__thread_create(pthread_t *thread,void *(*run)(void *),void *context,const gt_thread_options_t *o) {
  if(!o)return pthread_create(thread,NULL,run,context);
  pthread_attr_t attr;int rc=gt__thread_attributes(&attr,o);if(rc)return rc;
  rc=pthread_create(thread,&attr,run,context);(void)pthread_attr_destroy(&attr);return rc;
}
#endif
