#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static gt_session_authenticator_t *auth;static gt_ssh_t *ssh;static char *key;
static void cleanup(void){gt_session_authenticator_close(auth);auth=NULL;gt_ssh_close(ssh);ssh=NULL;if(key){memset(key,0,strlen(key));free(key);key=NULL;}}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL public setup:%d: %s\n",__LINE__,#x);cleanup();exit(1);}}while(0)
static uint64_t now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static gt_session_auth_info_t info(void){gt_session_auth_info_t i;CHECK(gt_session_authenticator_info(auth,&i));return i;}
static void until(gt_session_auth_phase_t expected){uint64_t end=now()+5000;for(;;){uint64_t before=now();gt_session_auth_phase_t p=gt_session_authenticator_step(auth);CHECK(now()-before<100 && now()<end);if(p==expected)return;CHECK(p!=GT_SESSION_AUTH_FAILED);struct timespec t={0,1000000};nanosleep(&t,NULL);}}
static gt_hostkey_decision_t forbidden(void *c,const char *h,uint16_t p,const char *k,const char *f){(void)c;(void)h;(void)p;(void)k;(void)f;CHECK(false);return GT_HOSTKEY_REJECT;}
static void open_auth(char **argv,const char *known){gt_ssh_opts_t o={.host="setup-identity.invalid",.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=known,.connect_timeout_ms=1000};ssh=gt_ssh_new(&o);CHECK(ssh && gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK && gt_ssh_set_hostkey_policy(ssh,forbidden,NULL)==GT_OK);gt_session_auth_options_t c={.family=4,.timeout_ms=3000};CHECK(inet_pton(AF_INET,argv[1],c.address)==1 && gt_session_authenticator_open(&ssh,&c,&auth)==GT_OK && !ssh);}
int main(int argc,char **argv){
  if(argc==2){CHECK(!strcmp(argv[1],"--unsupported"));gt_ssh_opts_t o={.host="fixture",.username="fixture",.known_hosts_path="/dev/null"};ssh=gt_ssh_new(&o);CHECK(ssh);gt_session_auth_options_t c={.family=4,.address={127,0,0,1}};CHECK(gt_session_authenticator_open(&ssh,&c,&auth)==GT_ERR_UNSUPPORTED && ssh && !auth);gt_session_linux_info_t linux_info;CHECK(gt_session_linux_probe(ssh,NULL,&linux_info)==GT_ERR_UNSUPPORTED && !linux_info.arch[0]);cleanup();puts("PASS public setup capabilities explicitly unavailable without SSH");return 0;}
  CHECK(argc==6);FILE *f=fopen(argv[4],"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,f)==(size_t)n);fclose(f);char known[1200];CHECK(snprintf(known,sizeof(known),"%s/public-setup-known",argv[5])>0);
  open_auth(argv,known);gt_ssh_t *early=(void *)1;CHECK(!gt_session_authenticator_take(auth,&early) && !early);until(GT_SESSION_AUTH_HOSTKEY);CHECK(info().host==GT_SESSION_HOST_UNKNOWN && !strcmp(info().fingerprint,getenv("GT_TEST_HOSTKEY_SHA256")));CHECK(gt_session_authenticator_decide(auth,GT_HOSTKEY_UNKNOWN)==GT_OK && info().phase==GT_SESSION_AUTH_HOSTKEY);CHECK(gt_session_authenticator_decide(auth,GT_HOSTKEY_ACCEPT)==GT_OK);until(GT_SESSION_AUTH_READY);CHECK(info().auth_methods&GT_SSH_AUTH_PUBLICKEY);CHECK(gt_session_authenticator_take(auth,&ssh) && ssh);gt_ssh_t *again=(void *)1;CHECK(!gt_session_authenticator_take(auth,&again) && !again);gt_session_authenticator_close(auth);auth=NULL;
  gt_session_linux_info_t linux_info;gt_session_install_options_t install={.timeout_ms=3000};gt_status_t probed=gt_session_linux_probe(ssh,&install,&linux_info);
#ifdef __linux__
  CHECK(probed==GT_OK && linux_info.arch[0] && linux_info.glibc[0] && linux_info.home[0]);
#else
  CHECK(probed==GT_ERR_UNSUPPORTED && !linux_info.arch[0]);
#endif
  gt_ssh_close(ssh);ssh=NULL;open_auth(argv,known);until(GT_SESSION_AUTH_READY);CHECK(info().host==GT_SESSION_HOST_KNOWN);gt_session_authenticator_close(auth);auth=NULL;CHECK(!unlink(known));
  open_auth(argv,known);until(GT_SESSION_AUTH_HOSTKEY);CHECK(gt_session_authenticator_decide(auth,GT_HOSTKEY_REJECT)==GT_ERR_HOST_KEY && info().phase==GT_SESSION_AUTH_FAILED);gt_session_authenticator_close(auth);auth=NULL;
  f=fopen(known,"wb");CHECK(f && fprintf(f,"[setup-identity.invalid]:%s ssh-ed25519 AAAAAAAAAAAAAAAAAAAAAAAA\n",argv[2])>0 && !fclose(f));open_auth(argv,known);until(GT_SESSION_AUTH_FAILED);CHECK(info().error==GT_ERR_HOST_KEY && info().host==GT_SESSION_HOST_MISMATCH);CHECK(!unlink(known));cleanup();puts("PASS public setup real SSH: resolved identity, trust, take once, Linux capability probe and explicit refusal");return 0;
}
