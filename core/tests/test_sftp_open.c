/* Real SSH/SFTP admission; protocol barriers are driven by test-sftp-open.py. */
#include "ghosterm.h"
#include "internal.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal) { (void)signal; stopped=1; }
static bool cancelled(void *unused) { (void)unused; return stopped!=0; }
static int failures;
#define CHECK(value, message) do { if (!(value)) { puts("FAIL: " message); failures++; } } while (0)
static int64_t now(void) {
  struct timespec value; if (clock_gettime(CLOCK_MONOTONIC,&value)) exit(2);
  return (int64_t)value.tv_sec*1000+value.tv_nsec/1000000;
}
static gt_hostkey_decision_t trust(void *u,const char *h,uint16_t p,const char *t,const char *f) {
  (void)u;(void)h;(void)p;(void)t;(void)f;return GT_HOSTKEY_ACCEPT;
}
static char *read_key(const char *path) {
  FILE *f=fopen(path,"rb"); if(!f)return NULL;
  char *key=calloc(1024*1024+1,1);size_t n=key?fread(key,1,1024*1024,f):0;
  bool ok=key && n>0 && !ferror(f) && feof(f);fclose(f);
  if(!ok){free(key);return NULL;}return key;
}
int main(int argc,char **argv) {
  setvbuf(stdout,NULL,_IONBF,0);
  if(argc!=6 && argc!=7)return 2;
  signal(SIGUSR1,stop);
  const char *mode=argc==7?argv[6]:"normal";
  char kh[4096];snprintf(kh,sizeof kh,"%s/sftp-open-known",argv[5]);
  gt_ssh_opts_t opts={.host=argv[1],.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=kh};
  gt_ssh_t *ssh=gt_ssh_new(&opts);char *key=read_key(argv[4]);
  if(!ssh || !key)return 2;
  CHECK(gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK,"sftp fixture key");
  memset(key,0,strlen(key));free(key);
  gt_ssh_set_hostkey_policy(ssh,trust,NULL);
  if(gt_ssh_connect(ssh)!=GT_OK){puts("FAIL: sftp fixture authentication");gt_ssh_close(ssh);return 1;}
  /* Calibrate the same ten-millisecond owner polling load before timing IO. */
  int64_t calibration=now();
  for(int i=0;i<20;i++){struct timespec delay={.tv_nsec=10*1000*1000};nanosleep(&delay,NULL);}
  calibration=now()-calibration;
  int64_t allowance=calibration*3/2+500;
  const int64_t admission_bound=allowance>3000?allowance:3000;
  const int64_t close_bound=allowance>1500?allowance:1500;
  gt_status_t status=GT_OK;gt_sftp_t *sftp=NULL;
  if(!strcmp(mode,"normal") || !strcmp(mode,"admit")) {
    sftp=gt_ssh_sftp_controlled(ssh,0,NULL,NULL,&status);
    CHECK(!sftp && status==GT_ERR_INVALID_ARG,"sftp zero budget");
    stopped=1;sftp=gt_ssh_sftp_controlled(ssh,2000,cancelled,NULL,&status);
    CHECK(!sftp && status==GT_ERR_CANCELLED,"sftp preflight cancellation");
    stopped=0;sftp=gt_ssh_sftp_controlled(ssh,3000,cancelled,NULL,&status);
    CHECK(sftp && status==GT_OK,"sftp normal admission after preflight rejection");
    if(sftp && !strcmp(mode,"normal")) {
      char path[4096];
      CHECK(gt_sftp_realpath(sftp,".",path,sizeof path)==GT_OK && path[0]=='/',"sftp real home");
      gt_sftp_entry_t *entries=NULL;size_t count=0;
      CHECK(gt_sftp_list(sftp,argv[5],&entries,&count)==GT_OK,"sftp real listing");
      bool found=false;
      for(size_t i=0;i<count;i++) {
        const char *name=entries[i].full_path?strrchr(entries[i].full_path,'/'):NULL;
        if(name && !strcmp(name+1,"clientkey.pub"))found=true;
      }
      CHECK(found,"sftp listing contains real fixture file");gt_sftp_list_free(entries,count);
    }
    gt_sftp_close(sftp);
  } else {
    printf("READY %ld\n",(long)getpid());
    int64_t start=now();
    sftp=gt_ssh_sftp_controlled(ssh,!strcmp(mode,"timeout")?400:5000,cancelled,NULL,&status);
    int64_t elapsed=now()-start;
    CHECK(sftp==NULL,"sftp fault must not succeed");
    CHECK(status==(!strcmp(mode,"cancel")?GT_ERR_CANCELLED:GT_ERR_IO),"sftp fault status");
    CHECK(elapsed<admission_bound,"sftp admission bounded");
    if(!strcmp(mode,"timeout"))CHECK(elapsed>=398,"sftp budget not shortened");
    void *session=NULL;int socket=-1;
    CHECK(gt__ssh_borrow(ssh,&session,&socket)!=GT_OK,"sftp partial admission invalidates SSH");
    if(sftp)gt_sftp_close(sftp);
  }
  int64_t closing=now();gt_ssh_close(ssh);
  CHECK(now()-closing<close_bound,"sftp owner close bounded after admission");
  if(!failures)puts("SFTP OPEN PASS");
  return failures?1:0;
}
