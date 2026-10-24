#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include "ipc/ssh-link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static void cleanup(void);
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);cleanup();exit(1);}}while(0)
static gt_ssh_link_t *owner;
static void cleanup(void){gt__ssh_link_close(owner);owner=NULL;}
static uint64_t now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static gt_hostkey_decision_t accept(void *c,const char *h,uint16_t p,const char *k,const char *f){(void)c;(void)h;(void)p;(void)k;(void)f;return GT_HOSTKEY_ACCEPT;}
static uint8_t byte(size_t n,unsigned slot){return (uint8_t)(n*31+slot*17);}
int main(int argc,char **argv) {
  if(argc==2 && !strcmp(argv[1],"--unsupported")) {
    gt_ssh_opts_t o={.host="unused",.username="unused"};gt_ssh_t *ssh=gt_ssh_new(&o);CHECK(ssh);
    CHECK(gt__ssh_link_open(&ssh,NULL,&owner)==GT_ERR_UNSUPPORTED && ssh && !owner);
    gt_stream_ops_t ops;CHECK(gt__ssh_link_stream(NULL,0,"/p","/r","/d",&ops)==GT_ERR_UNSUPPORTED && gt__ssh_link_step(NULL)==GT_ERR_UNSUPPORTED);
    gt_ssh_close(ssh);puts("PASS SSH owner explicitly unsupported without backend");return 0;
  }
  CHECK(argc==6 && !atexit(cleanup));
  FILE *f=fopen(argv[4],"rb");CHECK(f && !fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>0 && !fseek(f,0,SEEK_SET));char *key=calloc(1,(size_t)size+1);CHECK(key && fread(key,1,(size_t)size,f)==(size_t)size);fclose(f);
  char known[2048],proxy[2048],absent[2048];CHECK(snprintf(known,sizeof(known),"%s/known",argv[5])>0 && snprintf(proxy,sizeof(proxy),"%s/proxy' ; echo BAD",argv[5])>0 && snprintf(absent,sizeof(absent),"%s/absent",argv[5])>0);
  f=fopen(proxy,"w");CHECK(f && fputs("#!/bin/sh\ndd if=/dev/zero bs=4096 count=64 >&2 2>/dev/null\ncat\n",f)>=0 && !fclose(f) && !chmod(proxy,0700));
  gt_ssh_opts_t opts={.host=argv[1],.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=known,.connect_timeout_ms=3000};gt_ssh_t *ssh=gt_ssh_new(&opts);CHECK(ssh && gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK && gt_ssh_set_hostkey_policy(ssh,accept,NULL)==GT_OK && gt_ssh_connect(ssh)==GT_OK);memset(key,0,(size_t)size);free(key);
  CHECK(gt__ssh_link_open(&ssh,NULL,&owner)==GT_OK && !ssh);gt_stream_ops_t ops[2];
  CHECK(gt__ssh_link_stream(owner,2,proxy,argv[5],absent,&ops[0])==GT_ERR_INVALID_ARG);
  CHECK(gt__ssh_link_stream(owner,0,"/bad\npath",argv[5],absent,&ops[0])==GT_ERR_INVALID_ARG);
  for(unsigned i=0;i<2;i++)CHECK(gt__ssh_link_stream(owner,i,proxy,argv[5],absent,&ops[i])==GT_OK);
  size_t sent[2]={0},received[2]={0},total=4*1024*1024+123;bool blocked=false;uint64_t start=now(),end=start+20000;uint8_t bytes[4093];
  while(received[0]<total || received[1]<total) {
    CHECK(now()<end && gt__ssh_link_step(owner)==GT_OK);
    for(unsigned i=0;i<2;i++) {
      if(sent[i]<total){size_t n=total-sent[i];if(n>sizeof(bytes))n=sizeof(bytes);for(size_t j=0;j<n;j++)bytes[j]=byte(sent[i]+j,i);gt_stream_result_t r=ops[i].write(ops[i].context,bytes,n);CHECK(r.kind==GT_STREAM_DATA || r.kind==GT_STREAM_WAIT);if(r.kind==GT_STREAM_DATA)sent[i]+=r.length;else blocked=true;}
      if(i || now()-start>=300){gt_stream_result_t r=ops[i].read(ops[i].context,bytes,sizeof(bytes));CHECK(r.kind==GT_STREAM_DATA || r.kind==GT_STREAM_WAIT);if(r.kind==GT_STREAM_DATA){for(size_t j=0;j<r.length;j++)CHECK(bytes[j]==byte(received[i]+j,i));received[i]+=r.length;}}
    }
  }
  CHECK(blocked && received[0]==total && received[1]==total);
  for(unsigned i=0;i<2;i++){gt_ssh_link_info_t info;CHECK(gt__ssh_link_info(owner,i,&info) && info.stderr_bytes==64*4096);ops[i].close(ops[i].context);}
  end=now()+5000;
  for(;;){CHECK(now()<end && gt__ssh_link_step(owner)==GT_OK);gt_ssh_link_info_t a,b;CHECK(gt__ssh_link_info(owner,0,&a) && gt__ssh_link_info(owner,1,&b));if(a.phase==GT_SSH_LINK_ENDED && b.phase==GT_SSH_LINK_ENDED){CHECK(!a.has_exit_code && !b.has_exit_code);break;}}
  CHECK(gt__ssh_link_stream(owner,0,absent,argv[5],absent,&ops[0])==GT_OK);end=now()+5000;
  for(;;){CHECK(now()<end && gt__ssh_link_step(owner)==GT_OK);gt_ssh_link_info_t info;CHECK(gt__ssh_link_info(owner,0,&info));if(info.phase==GT_SSH_LINK_ENDED){/* 远端命令不存在：exec 失败码是 **shell 约定**，不是我们的数——bash 对
 * `exec /absent` 给 126（builtin 失败），zsh 给 127（command not found）。
 * 本机 shell 是 zsh、CI runner 是 bash，写死 127 就只在其中一边绿。
 * 要守的不变量是「退出码如实上报、不折叠成 0，且 stderr 不被吞」。 */
if(!(info.has_exit_code&&(info.exit_code==126||info.exit_code==127)&&info.stderr_bytes>0))fprintf(stderr,"归因 absent-command: has_exit_code=%d exit_code=%d stderr_bytes=%llu error=%d\n",(int)info.has_exit_code,info.exit_code,(unsigned long long)info.stderr_bytes,(int)info.error);CHECK(info.has_exit_code && (info.exit_code==126||info.exit_code==127) && info.stderr_bytes>0);break;}}
  ops[0].close(ops[0].context);gt__ssh_link_cancel(owner);CHECK(gt__ssh_link_step(owner)==GT_ERR_CANCELLED && ops[1].read(ops[1].context,bytes,1).kind==GT_STREAM_ERROR);
  cleanup();owner=NULL;puts("PASS real SSH multiplexed streams, binary fidelity, fairness, stderr, backpressure, quoting and cancellation");return 0;
}
