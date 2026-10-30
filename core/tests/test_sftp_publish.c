/* Production publication, real OpenSSH and controllable protocol peers. */
#include "ghosterm.h"
#include "internal.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int checked_rename(LIBSSH2_SFTP *,const char *,unsigned,const char *,unsigned,long);
#define libssh2_sftp_rename_ex checked_rename
#include "../src/sftp.c"
#undef libssh2_sftp_rename_ex
static volatile sig_atomic_t stopping;
static bool late_cancel;
static int failures, calls;
#define CHECK(c,m) do { if(!(c)){puts("FAIL: publish " m);failures++;} } while(0)
static int checked_rename(LIBSSH2_SFTP *s,const char *from,unsigned n,const char *to,unsigned m,long flags) {
    calls++;
    CHECK(flags==0,"only non-overwrite rename flags");
    int result=libssh2_sftp_rename_ex(s,from,n,to,m,flags);
    if(result==0 && late_cancel)stopping=1;
    return result;
}
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
static void write_file(const char *path,const char *value){FILE *f=fopen(path,"wb");if(!f||fwrite(value,1,strlen(value),f)!=strlen(value)||fclose(f))abort();}
static bool bytes_equal(const char *path,const char *value){char bytes[64];FILE *f=fopen(path,"rb");if(!f)return false;size_t n=fread(bytes,1,sizeof bytes,f);bool good=!ferror(f)&&feof(f)&&n==strlen(value)&&!memcmp(bytes,value,n);return !fclose(f)&&good;}
static void run_case(char **argv,const char *mode,bool peer){
    char source[4096],target[4096];snprintf(source,sizeof source,"%s/%s-source",argv[5],mode);snprintf(target,sizeof target,"%s/%s-target",argv[5],mode);
    bool dir=!strcmp(mode,"directory"),empty=!strcmp(mode,"empty"),link=!strcmp(mode,"link");
    bool conflict=!strncmp(mode,"conflict-",9),wrong=!strcmp(mode,"wrong-size"),missing=!strcmp(mode,"missing");
    uint64_t size=dir||empty?0:4;
    if(!peer){
        if(dir){if(mkdir(source,0700))abort();char child[8192];snprintf(child,sizeof child,"%s/empty",source);if(mkdir(child,0700))abort();snprintf(child,sizeof child,"%s/data",source);write_file(child,"tree");}
        else if(link){if(symlink(target,source))abort();}
        else if(!missing)write_file(source,empty?"":"data");
        if(!strcmp(mode,"conflict-file"))write_file(target,"original");
        if(!strcmp(mode,"conflict-link")&&symlink(source,target))abort();
        if(!strcmp(mode,"conflict-directory")&&mkdir(target,0700))abort();
    }
    gt_ssh_t *ssh=connect_fixture(argv);gt_status_t opened;
    gt_sftp_t *sftp=gt_ssh_sftp_controlled(ssh,3000,NULL,NULL,&opened);if(!sftp||opened)abort();
    gt_sftp_publish_result_t out={GT_SFTP_PUBLISH_PUBLISHED,99};
    CHECK(gt_sftp_publish_controlled(sftp,source,target,dir,size,&out,0,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_PUBLISH_NOT_ATTEMPTED && !out.remote_status,"invalid budget clears result");if(failures){gt_sftp_close(sftp);gt_ssh_close(ssh);return;}
    CHECK(gt_sftp_publish_controlled(sftp,source,source,dir,size,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"same path rejected");if(failures){gt_sftp_close(sftp);gt_ssh_close(ssh);return;}
    CHECK(gt_sftp_publish_controlled(sftp,source,target,false,UINT64_MAX,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"invalid size rejected");if(failures){gt_sftp_close(sftp);gt_ssh_close(ssh);return;}
    CHECK(gt_sftp_publish_controlled(sftp,source,target,true,1,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG,"directory size rejected");if(failures){gt_sftp_close(sftp);gt_ssh_close(ssh);return;}
    stopping=1;CHECK(gt_sftp_publish_controlled(sftp,source,target,dir,size,&out,1000,cancelled,NULL)==GT_ERR_CANCELLED && out.state==GT_SFTP_PUBLISH_NOT_ATTEMPTED,"preflight cancellation");stopping=0;if(failures){gt_sftp_close(sftp);gt_ssh_close(ssh);return;}
    if(!strcmp(mode,"jobs")) {
        char uploaded[8192];snprintf(uploaded,sizeof uploaded,"%s-job",source);
        CHECK(gt_sftp_upload(sftp,source,uploaded)!=0,"job fixture submitted");
        CHECK(gt_sftp_publish_controlled(sftp,source,target,false,size,&out,1000,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_PUBLISH_NOT_ATTEMPTED,"job owner rejects publication");
        gt_sftp_close(sftp);gt_ssh_close(ssh);
        CHECK(bytes_equal(source,"data"),"job rejection preserves source");
        if(!failures)puts("PASS publish jobs");return;
    }
    void *raw=NULL;int socket=-1;if(gt__ssh_borrow(ssh,&raw,&socket))abort();libssh2_session_set_timeout(raw,2345);
    int64_t calibration=now();for(int i=0;i<20;i++){struct timespec t={.tv_nsec=10000000};nanosleep(&t,NULL);}calibration=now()-calibration;
    int64_t bound=calibration*3/2+500;if(bound<3000)bound=3000;
    late_cancel=!strcmp(mode,"late-cancel");calls=0;printf("READY %ld\n",(long)getpid());int64_t began=now();
    int status=gt_sftp_publish_controlled(sftp,source,target,dir,wrong?size+1:size,&out,!strcmp(mode,"timeout")?400:10000,cancelled,NULL);
    int64_t elapsed=now()-began;
    int wanted=peer?atoi(argv[7]):conflict||wrong||missing?GT_ERR_IO:link?GT_ERR_UNSUPPORTED:GT_OK;
    gt_sftp_publish_state_t state=peer?(gt_sftp_publish_state_t)atoi(argv[8]):conflict?GT_SFTP_PUBLISH_CONFLICT:wrong||missing||link?GT_SFTP_PUBLISH_NOT_ATTEMPTED:GT_SFTP_PUBLISH_PUBLISHED;
    uint32_t remote=peer?(uint32_t)strtoul(argv[9],NULL,10):missing?2:0;
    CHECK(status==wanted,"status preserved");CHECK(out.state==state,"mutation outcome preserved");CHECK(out.remote_status==remote,"raw remote status preserved");
    if(state==GT_SFTP_PUBLISH_NOT_ATTEMPTED||state==GT_SFTP_PUBLISH_CONFLICT)CHECK(calls==0,"no rename on preflight failure");
    else CHECK(calls>0,"rename attempted");
    if(status==GT_OK){
        CHECK(gt__ssh_borrow(ssh,&raw,&socket)==GT_OK,"successful owner reusable");
        CHECK(libssh2_session_get_timeout(raw)==2345&&libssh2_session_get_blocking(raw)==1,"session options restored");
    }else CHECK(gt__ssh_borrow(ssh,&raw,&socket)!=GT_OK,"failed IO invalidates owner");
    if(!strcmp(mode,"timeout")||!strcmp(mode,"cancel")){CHECK(elapsed<bound,"bounded deadline and cancellation");if(!strcmp(mode,"timeout"))CHECK(elapsed>=398,"one total budget consumed");}
    if(!peer){
        struct stat st;
        if(status==GT_OK){
            CHECK(lstat(source,&st)<0,"source moved");
            if(dir){char child[8192];snprintf(child,sizeof child,"%s/data",target);CHECK(bytes_equal(child,"tree"),"directory content preserved");snprintf(child,sizeof child,"%s/empty",target);CHECK(!lstat(child,&st)&&S_ISDIR(st.st_mode),"empty directory preserved");}
            else CHECK(bytes_equal(target,empty?"":"data"),"published bytes");
        }else if(!missing){CHECK(!lstat(source,&st),"failed publication keeps source");if(!link)CHECK(bytes_equal(source,"data"),"source bytes preserved");}
        if(!strcmp(mode,"conflict-file"))CHECK(bytes_equal(target,"original"),"existing target bytes preserved");
        if(!strcmp(mode,"conflict-link"))CHECK(!lstat(target,&st)&&S_ISLNK(st.st_mode),"existing target link preserved");
        if(!strcmp(mode,"conflict-directory"))CHECK(!lstat(target,&st)&&S_ISDIR(st.st_mode),"existing target directory preserved");
    }
    began=now();gt_sftp_close(sftp);gt_ssh_close(ssh);CHECK(now()-began<bound,"owner cleanup bounded");stopping=0;
    if(!failures)printf("PASS publish %s\n",mode);
}
int main(int argc,char **argv){
    if(argc!=6&&argc!=10)return 2;setvbuf(stdout,NULL,_IONBF,0);signal(SIGUSR1,stop);
    if(argc==10)run_case(argv,argv[6],true);
    else{const char *cases[]={"normal","empty","directory","late-cancel","conflict-file","conflict-link","conflict-directory","link","wrong-size","missing","jobs"};for(size_t i=0;i<11;i++)run_case(argv,cases[i],false);}
    if(!failures)puts("SFTP PUBLISH PASS");return failures?1:0;
}
