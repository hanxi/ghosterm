#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "io/backend.h"
#include "allocation-test.h"
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
static double now(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec/1e9;}
static void ready(gt_io_backend_t *p) {
  char bytes[8192]={0};size_t used=0;double end=now()+5;
  while(!strstr(bytes,"READY")) {
    assert(now()<end);int n=p->ops->read(p,(uint8_t *)bytes+used,sizeof(bytes)-used-1,50);
    if(n==GT_TRANSPORT_READ_TIMEOUT)continue;
    assert(n>0);used+=(size_t)n;assert(used<sizeof(bytes)-1);
  }
}
static gt_io_backend_t *open_pty(allocation_test_budget_t *b,const char *script) {
  const char *args[]={"-c",script,NULL};gt_pty_opts_t opts={.executable="/bin/sh",.cmd_kind=GT_PTY_CMD_SHELL,.argv=args,.cols=80,.rows=24};
  gt_allocator_t a={b,allocation_test_allocate,allocation_test_release};return gt__pty_open_with_allocator(&opts,&a);
}
static void thread_case(allocation_test_budget_t *b) {
  const char *args[]={"-c","printf READY; read x",NULL};
  gt_pty_opts_t opts={.executable="/bin/sh",.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
  gt_allocator_t a={b,allocation_test_allocate,allocation_test_release};
  size_t page=(size_t)sysconf(_SC_PAGESIZE);gt_thread_options_t options={1024*1024,page};
  gt_io_backend_t *p=gt__pty_open_with_options(&opts,&a,&options);assert(p);ready(p);
  pthread_t thread;assert(gt__pty_waiter_thread(p,&thread)==GT_OK && !pthread_equal(thread,pthread_self()));
#ifdef __APPLE__
  size_t actual=pthread_get_stacksize_np(thread);uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(thread);
  assert(actual>=options.stack_size && actual<=options.stack_size+page);
  mach_vm_address_t guard=top-actual-1,address=guard;mach_vm_size_t size=0;
  vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
  assert(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
  assert(address<=guard && size>=page && address+size>guard && info.protection==VM_PROT_NONE);
  if(object)assert(mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS);
#endif
  assert(b->count==1 && b->live);p->ops->close(p);assert(!b->live && !b->count);
}
int main(int argc,char **argv) {
  assert(argc==2);allocation_test_budget_t b={.limit=32*1024*1024};
  if(!strcmp(argv[1],"thread")){thread_case(&b);return 0;}
  if(!strcmp(argv[1],"invalid")) {
    gt_allocator_t bad={&b,NULL,allocation_test_release};
    assert(!gt__pty_open_with_allocator(NULL,&bad) && !b.calls);
    gt_allocator_t valid={&b,allocation_test_allocate,allocation_test_release};gt_thread_options_t wrong={1024*1024,0};
    assert(!gt__pty_open_with_options(NULL,&valid,&wrong) && !b.calls);
    long minimum=sysconf(_SC_THREAD_STACK_MIN),page=sysconf(_SC_PAGESIZE);
    if(minimum>page && page>0) {
      wrong=(gt_thread_options_t){(size_t)page,(size_t)page};
      assert(!gt__pty_open_with_options(NULL,&valid,&wrong) && !b.calls);
    }
    pthread_t thread;gt_io_backend_t fake={0};
    assert(gt__pty_waiter_thread(NULL,&thread)==GT_ERR_INVALID_ARG);
    assert(gt__pty_waiter_thread(&fake,NULL)==GT_ERR_INVALID_ARG);
    assert(gt__pty_waiter_thread(&fake,&thread)==GT_ERR_UNSUPPORTED);
    return 0;
  }
  if(!strcmp(argv[1],"failures")) {
    gt_io_backend_t *p=open_pty(&b,"printf READY; read x");assert(p);ready(p);
    size_t calls=b.calls;assert(calls>=5 && b.count==1 && b.live);p->ops->close(p);assert(!b.live && !b.count);
    for(size_t i=1;i<=calls;i++) {
      b.fail=b.calls+i;assert(!open_pty(&b,"printf READY; read x"));assert(!b.live && !b.count);
    }
    b.fail=0;b.limit=1;assert(!open_pty(&b,"printf READY; read x") && !b.live && !b.count);return 0;
  }
  bool pressure=!strcmp(argv[1],"pressure");assert(pressure || !strcmp(argv[1],"flow"));
  gt_io_backend_t *p=open_pty(&b,pressure?"stty -echo -icanon min 1 time 0; printf READY; sleep 30":"stty -echo -icanon min 1 time 0; printf READY; sleep 1; cat");
  assert(p);ready(p);assert(b.count==1);size_t fixed=b.live,n=1024*1024;
  uint8_t *payload=malloc(5*n);assert(payload);memset(payload,'A',5*n);
  assert(p->ops->write(p,payload,n)==GT_OK && b.count==2 && b.live>fixed+n/2);
  if(pressure) {
    size_t before=b.live;memset(payload,'B',n);b.fail=b.calls+1;
    assert(p->ops->write(p,payload,n)==GT_ERR_IO && b.count==2 && b.live==before);
    b.fail=0;size_t calls=b.calls;
    assert(p->ops->write(p,payload,5*n)==GT_ERR_IO && b.calls==calls && b.count==2 && b.live==before);
    assert(p->ops->write(p,payload,8192)==GT_OK && b.count==2 && b.live>fixed);
  } else {
    memset(payload,'B',n);assert(p->ops->write(p,payload,n)==GT_OK);
    uint8_t bytes[8192];size_t read=0;double end=now()+8;
    while(read<2*n) {
      assert(now()<end);int count=p->ops->read(p,bytes,sizeof(bytes),50);
      if(count==GT_TRANSPORT_READ_TIMEOUT)continue;
      assert(count>0 && (size_t)count<=2*n-read);
      for(int i=0;i<count;i++)assert(bytes[i]==(read+(size_t)i<n?'A':'B'));
      read+=(size_t)count;
    }
    assert(b.count==1 && b.live==fixed);
  }
  free(payload);p->ops->close(p);assert(!b.live && !b.count);return 0;
}
