/* Exercise production receive with real peers and local syscall failures. */
#include "ghosterm.h"
#include "internal.h"
#include <fcntl.h>
#include <libssh2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
static ssize_t receive_write(int fd,const void *data,size_t count,off_t offset);
#define pwrite receive_write
#include "../src/sftp.c"
#undef pwrite
static const char *write_mode;
static int write_calls;
static ssize_t receive_write(int fd,const void *data,size_t count,off_t offset) {
    write_calls++;
    if(write_mode && !strcmp(write_mode,"write-zero"))return 0;
    if(write_mode && !strcmp(write_mode,"write-error")){errno=ENOSPC;return -1;}
    if(write_mode && !strcmp(write_mode,"eintr") && write_calls==1){errno=EINTR;return -1;}
    if(write_mode && !strcmp(write_mode,"short") && count>3)count=3;
    if(write_mode && !strcmp(write_mode,"local-growth") && write_calls==1 && pwrite(fd,"x",1,100000)!=1)abort();
    return pwrite(fd,data,count,offset);
}
static volatile sig_atomic_t stopping;
static int failures;
#define CHECK(c,m) do { if(!(c)){puts("FAIL: receive " m);failures++;} } while(0)
static void stop(int value){(void)value;stopping=1;}
static bool cancelled(void *unused){(void)unused;return stopping!=0;}
static int64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static gt_hostkey_decision_t trust(void *a,const char *b,uint16_t c,const char *d,const char *e){(void)a;(void)b;(void)c;(void)d;(void)e;return GT_HOSTKEY_ACCEPT;}
static gt_ssh_t *connect_fixture(char **argv){
    char known[4096];snprintf(known,sizeof known,"%s/known",argv[5]);
    gt_ssh_opts_t o={.host=argv[1],.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=known};
    gt_ssh_t *ssh=gt_ssh_new(&o);FILE *f=fopen(argv[4],"rb");if(!ssh||!f)abort();
    char key[16384];size_t n=fread(key,1,sizeof key-1,f);if(ferror(f)||!feof(f)||!n||fclose(f))abort();key[n]=0;
    int rc=gt_ssh_auth_private_key(ssh,key,NULL);volatile char *wipe=key;for(size_t i=0;i<sizeof key;i++)wipe[i]=0;
    if(rc)abort();gt_ssh_set_hostkey_policy(ssh,trust,NULL);if(gt_ssh_connect(ssh))abort();return ssh;
}
typedef struct {
    int fd, calls;
    uint64_t last, *published;
    bool unknown;
} receive_progress;
static void observed(const gt_sftp_progress_t *value, void *context) {
    receive_progress *p=context;
    CHECK(p && value,"progress borrowed context and value");
    if(!p || !value)return;
    CHECK(value->kind==GT_SFTP_PROGRESS_RECEIVE,"progress receive kind");
    CHECK(!p->calls ? value->bytes==0 : value->bytes>p->last,"progress initial zero and positive monotonic bytes");
    CHECK(*p->published==0,"progress is not publication");
    CHECK(value->total_known==!p->unknown,"progress known and unknown totals distinct");
    CHECK(value->total_known || value->total_bytes==0,"progress unknown total has no fabricated size");
    struct stat st;
    CHECK(!fstat(p->fd,&st) && st.st_size>=0 && (uint64_t)st.st_size>=value->bytes,"progress follows real destination writes");
    p->last=value->bytes;p->calls++;
}
static void run_case(char **argv,const char *mode,bool peer){
    size_t size=!strcmp(mode,"empty")?0:!strcmp(mode,"exact")?300:peer?257:70001;
    unsigned char *content=malloc(size+1);if(!content)abort();for(size_t i=0;i<size;i++)content[i]=(unsigned char)(i*31+7);
    char local[4096],remote[4096];snprintf(local,sizeof local,"%s/%s-target",argv[5],mode);snprintf(remote,sizeof remote,"%s/%s-source",argv[5],mode);
    bool link=!strcmp(mode,"link"),directory=!strcmp(mode,"directory"),limited=!strcmp(mode,"limit"),zero=!strcmp(mode,"zero-limit");
    if(!peer){
        if(link){if(symlink(local,remote))abort();}
        else if(directory){if(mkdir(remote,0700))abort();}
        else {FILE *f=fopen(remote,"wb");if(!f||fwrite(content,1,size,f)!=size||fclose(f))abort();}
    }
    int fd=open(local,O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0||lseek(fd,7,SEEK_SET)!=7)abort();
    gt_ssh_t *ssh=connect_fixture(argv);gt_status_t opened;gt_sftp_t *sftp=gt_ssh_sftp_controlled(ssh,3000,NULL,NULL,&opened);if(!sftp||opened)abort();
    uint64_t count=99;
    CHECK(gt_sftp_receive_controlled(sftp,remote,fd,300,&count,0,NULL,NULL)==GT_ERR_INVALID_ARG && count==0,"invalid budget clears count");
    CHECK(gt_sftp_receive_controlled(sftp,remote,fd,UINT64_MAX,&count,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"unrepresentable limit rejected");
    stopping=1;CHECK(gt_sftp_receive_controlled(sftp,remote,fd,300,&count,1000,cancelled,NULL)==GT_ERR_CANCELLED,"preflight cancel");stopping=0;
    int readonly=open(local,O_RDONLY);if(readonly<0)abort();
    CHECK(gt_sftp_receive_controlled(sftp,remote,readonly,300,&count,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"readonly target rejected");close(readonly);
    int pipefd[2];if(pipe(pipefd))abort();CHECK(gt_sftp_receive_controlled(sftp,remote,pipefd[1],300,&count,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"pipe target rejected");close(pipefd[0]);close(pipefd[1]);
    if(fcntl(fd,F_SETFL,O_APPEND))abort();CHECK(gt_sftp_receive_controlled(sftp,remote,fd,300,&count,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"append target rejected");if(fcntl(fd,F_SETFL,0))abort();
    if(pwrite(fd,"old",3,0)!=3)abort();CHECK(gt_sftp_receive_controlled(sftp,remote,fd,300,&count,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"nonempty target rejected");char old[3];CHECK(pread(fd,old,3,0)==3&&!memcmp(old,"old",3),"nonempty target preserved");if(ftruncate(fd,0))abort();
    void *raw=NULL;int socket=-1;if(gt__ssh_borrow(ssh,&raw,&socket))abort();libssh2_session_set_timeout(raw,2345);
    int64_t calibration=now();for(int i=0;i<20;i++){struct timespec t={.tv_nsec=10000000};nanosleep(&t,NULL);}calibration=now()-calibration;
    int64_t bound=calibration*3/2+500;if(bound<3000)bound=3000;
    write_mode=mode;write_calls=0;count=99;
    printf("READY %ld\n",(long)getpid());int64_t began=now();
    receive_progress progress={.fd=fd,.published=&count,.unknown=getenv("SFTP_RECEIVE_EXPECT_UNKNOWN")!=NULL};
    int status=gt_sftp_receive_observed(sftp,remote,fd,zero||!strcmp(mode,"empty")?0:limited?1:peer?300:size+1,&count,!strcmp(mode,"timeout")?400:10000,cancelled,NULL,observed,&progress);
    int64_t elapsed=now()-began;
    bool good=!strcmp(mode,"normal")||!strcmp(mode,"allow")||!strcmp(mode,"empty")||!strcmp(mode,"exact")||!strcmp(mode,"short")||!strcmp(mode,"eintr");
    if(good){
        CHECK(status==GT_OK && count==size,"complete bytes after close");
        CHECK(progress.calls>0 && progress.last==size,"progress successful final bytes including empty files");
        CHECK(size==0 || progress.calls>1,"progress positive IO observations delivered");
        unsigned char *actual=malloc(size+1);if(!actual)abort();CHECK(pread(fd,actual,size+1,0)==(ssize_t)size&&!memcmp(actual,content,size),"exact destination bytes");free(actual);
        CHECK(gt__ssh_borrow(ssh,&raw,&socket)==GT_OK,"successful owner reusable");
        CHECK(libssh2_session_get_timeout(raw)==2345&&libssh2_session_get_blocking(raw)==1,"session options restored");
        if(!strcmp(mode,"short"))CHECK(write_calls>100,"short writes exercised");
        if(!strcmp(mode,"eintr"))CHECK(write_calls>1,"EINTR exercised");
    }else{
        int wanted=!strcmp(mode,"cancel")?GT_ERR_CANCELLED:link||directory||!strcmp(mode,"unsupported")?GT_ERR_UNSUPPORTED:GT_ERR_IO;
        CHECK(status==wanted && count==0,"failure never publishes count");
        CHECK(gt__ssh_borrow(ssh,&raw,&socket)!=GT_OK,"failed IO invalidates owner");
        if(!strcmp(mode,"timeout")||!strcmp(mode,"cancel"))CHECK(elapsed<bound,"bounded cancellation or deadline");
        if(!strcmp(mode,"timeout"))CHECK(elapsed>=398,"one total budget consumed");
    }
    CHECK(lseek(fd,0,SEEK_CUR)==7,"caller offset and ownership preserved");
    struct stat st;CHECK(!fstat(fd,&st)&&st.st_size>=0&&(uint64_t)st.st_size<=(!strcmp(mode,"local-growth")?100001:peer?300:size+1),"destination never exceeds limit");
    if(zero||limited||!strcmp(mode,"empty"))CHECK(st.st_size==0,"zero or known oversized source never writes target");
    began=now();gt_sftp_close(sftp);gt_ssh_close(ssh);CHECK(now()-began<bound,"owner cleanup bounded");
    close(fd);free(content);if(!failures)printf("PASS receive %s\n",mode);
}
int main(int argc,char **argv){
    if(argc!=6&&argc!=7)return 2;setvbuf(stdout,NULL,_IONBF,0);signal(SIGUSR1,stop);
    if(argc==7)run_case(argv,argv[6],true);
    else {const char *cases[]={"normal","empty","short","eintr","write-zero","write-error","local-growth","limit","zero-limit","link","directory"};for(size_t i=0;i<11;i++)run_case(argv,cases[i],false);}
    if(!failures)puts("SFTP RECEIVE PASS");return failures?1:0;
}
