#define _DARWIN_C_SOURCE 1
#include "session-main-stack.h"
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#define REQUIRE(c) do {if(!(c))return 1;}while(0)
static void *nonmain(void *context) {
    int *result=context;p0_main_stack_info info={1,1,1};
    *result=p0_main_stack_measure(&info) || info.stack || info.guard || info.capacity;return NULL;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    bool all=!strcmp(argv[1],"all");
    if(all || !strcmp(argv[1],"measure")) {
        p0_main_stack_info info;struct rlimit limit;
        REQUIRE(!p0_main_stack_measure(NULL) && p0_main_stack_measure(&info) && !getrlimit(RLIMIT_STACK,&limit));
        REQUIRE(info.stack==pthread_get_stacksize_np(pthread_self()) && info.stack==limit.rlim_cur);
        REQUIRE(info.guard>0 && info.capacity==info.stack+info.guard && info.capacity<=128u*1024u*1024u);
    }
    if(all || !strcmp(argv[1],"admission")) {
        p0_main_stack_info info;REQUIRE(p0_main_stack_measure(&info));
        REQUIRE(p0_main_stack_admitted(&info,info.capacity));
        REQUIRE(!p0_main_stack_admitted(&info,info.stack) && !p0_main_stack_admitted(&info,0));
        p0_main_stack_info bad=info;bad.capacity--;REQUIRE(!p0_main_stack_admitted(&bad,bad.capacity));
        bad=info;bad.guard--;REQUIRE(!p0_main_stack_admitted(&bad,bad.capacity));
    }
    if(all || !strcmp(argv[1],"thread")) {
        pthread_t thread;int result=-1;REQUIRE(!pthread_create(&thread,NULL,nonmain,&result));
        REQUIRE(!pthread_join(thread,NULL) && result==0);
    }
    if(!all && strcmp(argv[1],"measure") && strcmp(argv[1],"admission") && strcmp(argv[1],"thread"))return 2;
    puts("PASS main-stack measurement/admission/current-main-thread baselines");return 0;
}
