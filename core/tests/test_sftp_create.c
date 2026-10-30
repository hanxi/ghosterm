/* Exclusive streaming creation through the public C ABI and real SSH peers. */
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

static volatile sig_atomic_t stopped;
static gt_sftp_create_result_t *active;
static int mutate_fd=-1;
static bool cancel_after_complete;
static int failures;
#define CHECK(c,m) do { if(!(c)){puts("FAIL: " m);failures++;} } while(0)
static void stop(int value){(void)value;stopped=1;}
static bool cancelled(void *unused){
    (void)unused;
    if(mutate_fd>=0 && active->state==GT_SFTP_CREATE_CREATED){
        if(ftruncate(mutate_fd,0))abort();mutate_fd=-1;
    }
    return stopped!=0 || (cancel_after_complete && active && active->state==GT_SFTP_CREATE_COMPLETE);
}
static int64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static gt_hostkey_decision_t trust(void *a,const char *b,uint16_t c,const char *d,const char *e){
    (void)a;(void)b;(void)c;(void)d;(void)e;return GT_HOSTKEY_ACCEPT;
}
static gt_ssh_t *connect_fixture(char **argv){
    char known[4096];snprintf(known,sizeof known,"%s/known",argv[5]);
    gt_ssh_opts_t o={.host=argv[1],.port=(uint16_t)atoi(argv[2]),.username=argv[3],.known_hosts_path=known};
    gt_ssh_t *ssh=gt_ssh_new(&o);FILE *f=fopen(argv[4],"rb");if(!ssh||!f)abort();
    char key[16384];size_t n=fread(key,1,sizeof key-1,f);if(ferror(f)||!feof(f)||!n||fclose(f))abort();key[n]=0;
    int rc=gt_ssh_auth_private_key(ssh,key,NULL);volatile char *wipe=key;for(size_t i=0;i<sizeof key;i++)wipe[i]=0;
    if(rc)abort();gt_ssh_set_hostkey_policy(ssh,trust,NULL);if(gt_ssh_connect(ssh))abort();return ssh;
}
static void run_case(char **argv,const char *mode,bool peer){
    cancel_after_complete=!strcmp(mode,"normal") || !strcmp(mode,"empty");
    bool empty=!strcmp(mode,"empty");size_t size=empty?0:peer?4096:3*1024*1024+17;
    unsigned char *content=malloc(size?size:1);if(!content)abort();
    for(size_t i=0;i<size;i++)content[i]=(unsigned char)(i*31+7);
    char local[4096],remote[4096];snprintf(local,sizeof local,"%s/%s-source",argv[5],mode);
    snprintf(remote,sizeof remote,"%s/%s-target",argv[5],mode);
    int fd=open(local,O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0||write(fd,content,size)!=(ssize_t)size)abort();
    if(lseek(fd,7,SEEK_SET)!=7)abort();
    const char old[]="existing bytes";
    bool existing=!strcmp(mode,"existing"),link=!strcmp(mode,"symlink"),directory=!strcmp(mode,"directory");
    if(existing){FILE *f=fopen(remote,"wb");if(!f||fwrite(old,1,sizeof old,f)!=sizeof old||fclose(f))abort();}
    if(link&&symlink(local,remote))abort();
    if(directory&&mkdir(remote,0700))abort();
    gt_ssh_t *ssh=connect_fixture(argv);gt_status_t opened;
    gt_sftp_t *sftp=gt_ssh_sftp_controlled(ssh,3000,NULL,NULL,&opened);if(!sftp||opened)abort();
    void *raw=NULL;int socket=-1;if(gt__ssh_borrow(ssh,&raw,&socket))abort();libssh2_session_set_timeout(raw,2345);
    gt_sftp_create_result_t out={GT_SFTP_CREATE_COMPLETE,99};active=&out;
    CHECK(gt_sftp_create_controlled(sftp,remote,fd,size,&out,0,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_CREATE_NOT_ATTEMPTED && !out.bytes_acked,"create invalid budget clears outcome");
    stopped=1;
    CHECK(gt_sftp_create_controlled(sftp,remote,fd,size,&out,1000,cancelled,NULL)==GT_ERR_CANCELLED && out.state==GT_SFTP_CREATE_NOT_ATTEMPTED,"create preflight cancellation has no IO");
    stopped=0;
    CHECK(gt_sftp_create_controlled(sftp,remote,fd,size+1,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_CREATE_NOT_ATTEMPTED,"create source size preflight");
    int pipefd[2];if(pipe(pipefd))abort();
    CHECK(gt_sftp_create_controlled(sftp,remote,pipefd[0],0,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_CREATE_NOT_ATTEMPTED,"create rejects nonregular source");close(pipefd[0]);close(pipefd[1]);
    int writeonly=open(local,O_WRONLY);if(writeonly<0)abort();
    CHECK(gt_sftp_create_controlled(sftp,remote,writeonly,size,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_CREATE_NOT_ATTEMPTED,"create rejects unreadable source");close(writeonly);
    if(!strcmp(mode,"source-change"))mutate_fd=fd;
    int64_t calibration=now();for(int i=0;i<20;i++){struct timespec t={.tv_nsec=10000000};nanosleep(&t,NULL);}calibration=now()-calibration;
    int64_t limit=calibration*3/2+500;if(limit<3000)limit=3000;
    printf("READY %ld\n",(long)getpid());
    int64_t begin=now();
    uint32_t budget=strstr(mode,"timeout")?400:10000;
    int status=gt_sftp_create_controlled(sftp,remote,fd,size,&out,budget,cancelled,NULL);
    int64_t elapsed=now()-begin;
    bool good=!strcmp(mode,"normal")||empty;
    if(good){
        CHECK(status==GT_OK && out.state==GT_SFTP_CREATE_COMPLETE && out.bytes_acked==size,"create complete after all bytes and close");
        CHECK(gt__ssh_borrow(ssh,&raw,&socket)==GT_OK,"create success retains owner");
        CHECK(libssh2_session_get_timeout(raw)==2345 && libssh2_session_get_blocking(raw)==1,"create restores session options");
        struct stat st;CHECK(!stat(remote,&st)&&(st.st_mode&0777)==0600,"create private mode");
        FILE *f=fopen(remote,"rb");unsigned char *actual=malloc(size+1);if(!f||!actual)abort();
        size_t n=fread(actual,1,size+1,f);CHECK(n==size&&!ferror(f)&&feof(f)&&!memcmp(actual,content,size),"create exact multi-chunk bytes");fclose(f);free(actual);
    }else{
        CHECK(status==(strstr(mode,"cancel")?GT_ERR_CANCELLED:GT_ERR_IO),"create failure status");
        gt_sftp_create_state_t want=GT_SFTP_CREATE_CREATED;
        if(existing||link||directory||!strcmp(mode,"open-deny"))want=GT_SFTP_CREATE_REJECTED;
        else if(strstr(mode,"open-"))want=GT_SFTP_CREATE_UNKNOWN;
        CHECK(out.state==want,"create failure preserves creation outcome");
        CHECK(gt__ssh_borrow(ssh,&raw,&socket)!=GT_OK,"create failed IO invalidates owner");
        if(strstr(mode,"cancel")||strstr(mode,"timeout"))CHECK(elapsed<limit,"create operation is bounded");
        if(strstr(mode,"timeout"))CHECK(elapsed>=398,"create consumes one total budget");
        if(existing){char data[sizeof old+1];FILE *f=fopen(remote,"rb");if(!f)abort();size_t n=fread(data,1,sizeof data,f);CHECK(n==sizeof old&&!memcmp(data,old,sizeof old),"create never overwrites existing target");fclose(f);}
        if(link){struct stat st;CHECK(!lstat(remote,&st)&&S_ISLNK(st.st_mode),"create does not replace symlink");CHECK(!fstat(fd,&st)&&(uint64_t)st.st_size==size,"create does not truncate symlink source");}
        if(directory){struct stat st;CHECK(!stat(remote,&st)&&S_ISDIR(st.st_mode),"create does not replace directory");}
        if(peer&&out.state==GT_SFTP_CREATE_CREATED)CHECK(out.bytes_acked<=size,"create acknowledged count is bounded");
    }
    CHECK(lseek(fd,0,SEEK_CUR)==7,"create preserves caller fd offset and ownership");
    begin=now();gt_sftp_close(sftp);gt_ssh_close(ssh);CHECK(now()-begin<limit,"create owner release bounded");
    close(fd);free(content);active=NULL;
    if(!failures)printf("PASS create %s\n",mode);
}
int main(int argc,char **argv){
    if(argc!=6&&argc!=7)return 2;setvbuf(stdout,NULL,_IONBF,0);signal(SIGUSR1,stop);
    if(argc==7)run_case(argv,argv[6],true);
    else {const char *cases[]={"normal","empty","existing","symlink","directory","source-change"};for(size_t i=0;i<6;i++)run_case(argv,cases[i],false);}
    if(!failures)puts("SFTP CREATE PASS");return failures?1:0;
}
