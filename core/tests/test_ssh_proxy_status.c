#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include "ipc/ssh-link.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static gt_ssh_link_t *owner;static gt_ssh_t *pending;static char *key;
static void close_link(void){gt_ssh_link_t *l=owner;gt_ssh_t *s=pending;owner=NULL;pending=NULL;gt__ssh_link_close(l);gt_ssh_close(s);}
static void cleanup(void){close_link();if(key){memset(key,0,strlen(key));free(key);key=NULL;}}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL SSH proxy status:%d: %s\n",__LINE__,#x);cleanup();exit(1);}}while(0)
static uint64_t now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static gt_hostkey_decision_t accept(void *c,const char *h,uint16_t p,const char *t,const char *f){(void)c;(void)h;(void)p;(void)t;(void)f;return GT_HOSTKEY_ACCEPT;}
int main(int argc,char **argv) {
  CHECK(argc==6);FILE *f=fopen(argv[4],"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,f)==(size_t)n);fclose(f);
  char known[2048],proxy[2048];CHECK(snprintf(known,sizeof(known),"%s/proxy-status-known",argv[5])>0 && snprintf(proxy,sizeof(proxy),"%s/proxy-status.sh",argv[5])>0);
  const struct {int code;gt_status_t error;} cases[]={
    {65,GT_ERR_INVALID_ARG},{66,GT_ERR_NOT_FOUND},{67,GT_ERR_AUTH},{68,GT_ERR_UNSUPPORTED},
    {69,GT_ERR_IO},{70,GT_ERR_CANCELLED},{71,GT_ERR_UNSUPPORTED},{72,GT_ERR_UNSUPPORTED},
    {73,GT_ERR_PROTOCOL},{74,GT_ERR_IO},{0,GT_OK},{127,GT_OK},{71,GT_OK}};
  for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) {
    bool detached=i==12;f=fopen(proxy,"wb");CHECK(f && fprintf(f,"#!/bin/sh\n%sexit %d\n",detached?"sleep 0.1\n":"",cases[i].code)>0 && !fclose(f) && !chmod(proxy,0700));
    gt_ssh_opts_t o={.host=argv[1],.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=known,.connect_timeout_ms=1000};pending=gt_ssh_new(&o);CHECK(pending && gt_ssh_auth_private_key(pending,key,NULL)==GT_OK && gt_ssh_set_hostkey_policy(pending,accept,NULL)==GT_OK && gt_ssh_connect(pending)==GT_OK);
    CHECK(gt__ssh_link_open(&pending,NULL,&owner)==GT_OK && !pending);gt_stream_ops_t stream;CHECK(gt__ssh_link_stream(owner,0,proxy,argv[5],"/fixture/daemon",&stream)==GT_OK);uint64_t end=now()+5000;gt_status_t status=GT_OK;gt_ssh_link_info_t info;
    for(;;) {
      status=gt__ssh_link_step(owner);CHECK(gt__ssh_link_info(owner,0,&info) && now()<end);
      if(detached && info.phase==GT_SSH_LINK_READY){stream.close(stream.context);detached=false;}
      if(status!=GT_OK || info.phase==GT_SSH_LINK_ENDED)break;
      struct timespec t={0,1000000};nanosleep(&t,NULL);
    }
    CHECK(status==cases[i].error);
    if(cases[i].error!=GT_OK){CHECK(info.phase==GT_SSH_LINK_FAILED && info.error==cases[i].error && info.has_exit_code && info.exit_code==cases[i].code);CHECK(gt__ssh_link_step(owner)==cases[i].error);uint8_t b;CHECK(stream.read(stream.context,&b,1).kind==GT_STREAM_ERROR);}
    else CHECK(info.phase==GT_SSH_LINK_ENDED && info.has_exit_code==(cases[i].code!=0));
    stream.close(stream.context);close_link();
  }
  cleanup();puts("PASS real SSH proxy typed failures, persistent reason and absent/zero status unknown");return 0;
}
