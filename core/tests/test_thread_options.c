#include "base/thread-options.h"
#include <assert.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
static void *observe(void *context) {
  const gt_thread_options_t *o=context;
#ifdef __APPLE__
  size_t actual=pthread_get_stacksize_np(pthread_self()),page=(size_t)sysconf(_SC_PAGESIZE);
  uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());
  assert(actual>=o->stack_size && actual<=o->stack_size+page);
  char local;assert((uintptr_t)&local<top && (uintptr_t)&local>=top-actual);
  mach_vm_address_t guard=top-actual-1,address=guard;mach_vm_size_t size=0;
  vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
  assert(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
  assert(address<=guard && size>=o->guard_size && address+size>guard && info.protection==VM_PROT_NONE);
  if(object)assert(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
#else
  (void)o;
#endif
  return context;
}
int main(int argc,char **argv) {
  assert(argc==2);size_t page=(size_t)sysconf(_SC_PAGESIZE);gt_thread_options_t o={1024*1024,page};
  if(!strcmp(argv[1],"invalid")) {
    gt_thread_options_t bad[]={{0,page},{1024*1024,0},{1024*1024+1,page},{1024*1024,page+1},{SIZE_MAX-(SIZE_MAX%page),page}};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
      pthread_attr_t a;assert(!gt__thread_options_valid(&bad[i]));assert(gt__thread_attributes(&a,&bad[i])==EINVAL);
    }
    assert(gt__thread_options_valid(NULL));return 0;
  }
  if(!strcmp(argv[1],"attributes")) {
    pthread_attr_t a;assert(!gt__thread_attributes(&a,&o));size_t stack=0,guard=0;
    assert(!pthread_attr_getstacksize(&a,&stack) && stack==o.stack_size);
    assert(!pthread_attr_getguardsize(&a,&guard) && guard==o.guard_size);
    assert(!pthread_attr_destroy(&a));return 0;
  }
  assert(!strcmp(argv[1],"live"));pthread_t t;assert(!gt__thread_create(&t,observe,&o,&o));
  void *value=NULL;assert(!pthread_join(t,&value) && value==&o);return 0;
}
