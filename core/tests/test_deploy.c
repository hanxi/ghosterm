#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include "ghosterm.h"
#include "session/deploy.h"
#include "ipc/ssh-link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
static gt_ssh_t *ssh;
static void handles_closed(void) {
#ifdef GT_DEPLOY_SHIM
  extern unsigned gt_deploy_live_handles(void);if(gt_deploy_live_handles()){fprintf(stderr,"FAIL deployment leaked SFTP handles\n");exit(1);}
#endif
}
static void cleanup(void){gt_ssh_close(ssh);ssh=NULL;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s (%s)\n",__FILE__,__LINE__,#x,gt_last_error());cleanup();exit(1);}}while(0)
static gt_hostkey_decision_t accept(void *c,const char *h,uint16_t p,const char *k,const char *f){(void)c;(void)h;(void)p;(void)k;(void)f;return GT_HOSTKEY_ACCEPT;}
static void catalog(const char *root) {
  char directory[1024];CHECK(snprintf(directory,sizeof(directory),"%s/catalog-XXXXXX",root)>0 && mkdtemp(directory));
  static const char *names[]={"ghosterm-sessiond","ghosterm-session-proxy","lib/libghostty-vt.so.0","LICENSE","SOURCE-INFO.json"};
  char path[1200],manifest[1200],first[1200];CHECK(snprintf(path,sizeof(path),"%s/lib",directory)>0 && !mkdir(path,0700));
  for(unsigned i=0;i<5;i++){CHECK(snprintf(path,sizeof(path),"%s/%s",directory,names[i])>0);FILE *f=fopen(path,"w");CHECK(f && fputc('x',f)!=EOF && !fclose(f));}
  CHECK(snprintf(first,sizeof(first),"%s/%s",directory,names[0])>0 && snprintf(manifest,sizeof(manifest),"%s/manifest.tsv",directory)>0);
  for(unsigned mode=0;mode<5;mode++) {
    FILE *f=fopen(manifest,"w");CHECK(f);CHECK(fprintf(f,"GTB1\trelease-one\taarch64\t2.34\t/lib/ld-linux-aarch64.so.1\t%064d\n",0)>0);
    for(unsigned i=0;i<5;i++)CHECK(fprintf(f,"%s\t%u\t%064d\n",mode==1 && !i?"evil":names[i],mode==4 && !i?2u:1u,0)>0);
    if(mode==2)CHECK(fputs("extra\n",f)>=0);
    CHECK(!fclose(f));
    if(mode==3){CHECK(!unlink(first) && !symlink("LICENSE",first));}
    gt_session_bundle_t b;CHECK(gt__session_bundle_read(directory,&b)==(mode?GT_ERR_INVALID_ARG:GT_OK));
    if(mode==3){CHECK(!unlink(first));f=fopen(first,"w");CHECK(f && fputc('x',f)!=EOF && !fclose(f));}
  }
  unlink(manifest);for(unsigned i=0;i<5;i++){CHECK(snprintf(path,sizeof(path),"%s/%s",directory,names[i])>0);unlink(path);}CHECK(snprintf(path,sizeof(path),"%s/lib",directory)>0);rmdir(path);rmdir(directory);puts("PASS fixed bundle catalog and malformed inventory refusals");
}
static bool stop_now(void *c){(void)c;return true;}
static bool upload_cancelled;
static bool stop_upload(void *c){(void)c;return upload_cancelled;}
static void progress(void *c,gt_deploy_phase_t phase,unsigned file,uint64_t bytes,uint64_t total){(void)c;(void)file;(void)total;if(phase==GT_DEPLOY_UPLOAD && bytes)upload_cancelled=true;}
int main(int argc,char **argv) {
  if(argc==3 && !strcmp(argv[1],"--catalog")){catalog(argv[2]);return 0;}
  if(argc==2 && !strcmp(argv[1],"--unsupported")){gt_linux_probe_t p;gt_deploy_result_t r;CHECK(gt__ssh_probe_linux(NULL,NULL,&p)==GT_ERR_UNSUPPORTED && gt__ssh_deploy_linux(NULL,NULL,NULL,NULL,&r)==GT_ERR_UNSUPPORTED);puts("PASS deployment explicitly unsupported without SSH");return 0;}
  CHECK(argc==8);FILE *file=fopen(argv[4],"rb");CHECK(file && !fseek(file,0,SEEK_END));long n=ftell(file);CHECK(n>0 && !fseek(file,0,SEEK_SET));char *key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,file)==(size_t)n);fclose(file);
  gt_ssh_opts_t options={.host=argv[1],.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=argv[7],.connect_timeout_ms=3000};ssh=gt_ssh_new(&options);CHECK(ssh && gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK && gt_ssh_set_hostkey_policy(ssh,accept,NULL)==GT_OK && gt_ssh_connect(ssh)==GT_OK);memset(key,0,(size_t)n);free(key);
  gt_session_bundle_t bundle;CHECK(gt__session_bundle_read(argv[5],&bundle)==GT_OK);gt_linux_probe_t probe;gt_deploy_options_t deploy={.timeout_ms=120000};CHECK(gt__ssh_probe_linux(ssh,&deploy,&probe)==GT_OK);
  printf("PROBE %s glibc %s uid %u\n",probe.arch,probe.glibc,probe.uid);fflush(stdout);
  const char *mode=getenv("GT_DEPLOY_CASE");if(!mode)mode="flow";
  if(!strcmp(mode,"probe")){handles_closed();cleanup();puts("PASS Linux probe control");return 0;}
  gt_status_t expected=GT_OK;
  if(!strcmp(mode,"arch")){strcpy(bundle.arch,!strcmp(probe.arch,"aarch64")?"x86_64":"aarch64");expected=GT_ERR_UNSUPPORTED;}
  else if(!strcmp(mode,"glibc")){strcpy(bundle.min_glibc,"65535.0");expected=GT_ERR_UNSUPPORTED;}
  else if(!strcmp(mode,"contract")){bundle.contract[0]=bundle.contract[0]=='a'?'b':'a';expected=GT_ERR_UNSUPPORTED;}
  else if(!strcmp(mode,"cancel")){deploy.cancelled=stop_now;expected=GT_ERR_CANCELLED;}
  else if(!strcmp(mode,"cancel-upload")){deploy.cancelled=stop_upload;deploy.progress=progress;expected=GT_ERR_CANCELLED;}
  else if(!strcmp(mode,"symlink") || !strcmp(mode,"permissions"))expected=GT_ERR_AUTH;
  if(expected!=GT_OK){gt_deploy_result_t r;gt_status_t status=gt__ssh_deploy_linux(ssh,&bundle,argv[6],&deploy,&r);CHECK(status==expected && !r.published);handles_closed();cleanup();puts("PASS Linux deployment refusal/cancellation");return 0;}
  gt_deploy_result_t result;CHECK(gt__ssh_deploy_linux(ssh,&bundle,argv[6],&deploy,&result)==GT_OK && result.published && !result.publication_unknown);printf("DAEMON %s\nPROXY %s\n",result.daemon,result.proxy);fflush(stdout);
  CHECK(gt__ssh_deploy_linux(ssh,&bundle,argv[6],&deploy,&result)==GT_OK && result.reused && result.published);
  char original=bundle.files[0].sha256[0];bundle.files[0].sha256[0]=original=='a'?'b':'a';CHECK(gt__ssh_deploy_linux(ssh,&bundle,argv[6],&deploy,&result)==GT_ERR_PROTOCOL && !result.published);bundle.files[0].sha256[0]=original;
  handles_closed();cleanup();puts("PASS real Linux probe, SFTP deployment, executable relocation, verified reuse and tamper rejection");return 0;
}
