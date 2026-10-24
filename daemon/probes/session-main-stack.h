/* Darwin P0 process bootstrap accounting, outside the portable core ABI.
 * Reserve the reported main stack and its complete contiguous VM guard region.
 * These are virtual capacity promises, not resident-memory measurements. */
#ifndef GT_P0_MAIN_STACK_H
#define GT_P0_MAIN_STACK_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
typedef struct {size_t stack,guard,capacity;} p0_main_stack_info;
static inline bool p0_main_stack_measure(p0_main_stack_info *out) {
    if(!out)return false;*out=(p0_main_stack_info){0};
    if(!pthread_main_np())return false;
    size_t stack=pthread_get_stacksize_np(pthread_self());
    uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());char local;
    if(!stack || stack>=top || (uintptr_t)&local>=top || (uintptr_t)&local<top-stack)return false;
    mach_vm_address_t address=top-stack-1;mach_vm_size_t guard=0;
    vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    kern_return_t rc=mach_vm_region(mach_task_self(),&address,&guard,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object);
    bool released=!object || mach_port_deallocate(mach_task_self(),object)==KERN_SUCCESS;
    if(rc!=KERN_SUCCESS || !released || !guard || address>top-stack-1 || guard>UINT64_MAX-address || address+guard!=top-stack || info.protection!=VM_PROT_NONE)return false;
    if(guard>SIZE_MAX-stack || stack+(size_t)guard>128u*1024u*1024u)return false;
    *out=(p0_main_stack_info){stack,(size_t)guard,stack+(size_t)guard};return true;
}
static inline bool p0_main_stack_admitted(const p0_main_stack_info *expected,size_t committed) {
    p0_main_stack_info actual;
    return expected && expected->guard<=SIZE_MAX-expected->stack && expected->capacity==expected->stack+expected->guard && p0_main_stack_measure(&actual) && actual.stack==expected->stack && actual.guard==expected->guard && actual.capacity==expected->capacity && committed==actual.capacity;
}
#endif
