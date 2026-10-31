/* Real OpenSSH and exact protocol peers exercise production controlled mkdir. */
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
static int checked_mkdir(LIBSSH2_SFTP *,const char *,unsigned,long);
static int checked_close(LIBSSH2_SFTP_HANDLE *);
#define libssh2_sftp_mkdir_ex checked_mkdir
#define libssh2_sftp_close_handle checked_close
#include "../src/sftp.c"
#undef libssh2_sftp_mkdir_ex
#undef libssh2_sftp_close_handle
static volatile sig_atomic_t stopping;
static bool cancel_created, cancel_closed;
static int failures,calls;
#define CHECK(c,m) do { if(!(c)){puts("FAIL: mkdir " m);failures++;} } while(0)
static int checked_mkdir(LIBSSH2_SFTP *s,const char *path,unsigned n,long mode) {
    calls++;CHECK(mode==0700,"private requested mode");
    int result=libssh2_sftp_mkdir_ex(s,path,n,mode);
    if(!result && cancel_created)stopping=1;
    return result;
}
static int checked_close(LIBSSH2_SFTP_HANDLE *h) {
    int result=libssh2_sftp_close_handle(h);
    if(!result && cancel_closed)stopping=1;
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
static void write_file(const char *path){FILE *f=fopen(path,"wb");if(!f||fwrite("data",1,4,f)!=4||fclose(f))abort();}
static void run_case(char **argv,const char *mode,bool peer){
    char parent[4096],target[8192],name[128];
    snprintf(parent,sizeof parent,"%s/%s-parent",argv[5],mode);
    if(peer&&!strcmp(mode,"root"))strcpy(parent,"/");
    snprintf(name,sizeof name,"%s",!strcmp(mode,"unicode")?"目录 ☃":"child");
    snprintf(target,sizeof target,"%s/%s",parent,name);
    bool conflict=!strncmp(mode,"conflict-",9),link=!strcmp(mode,"parent-link"),file=!strcmp(mode,"parent-file"),missing=!strcmp(mode,"parent-missing"),denied=!strcmp(mode,"permission");
    if(!peer){
        if(link){if(symlink(argv[5],parent))abort();}
        else if(file)write_file(parent);
        else if(!missing&&mkdir(parent,0700))abort();
        if(!strcmp(mode,"conflict-file"))write_file(target);
        if(!strcmp(mode,"conflict-link")&&symlink(argv[5],target))abort();
        if(!strcmp(mode,"conflict-directory")&&mkdir(target,0700))abort();
        if(denied&&chmod(parent,0500))abort();
    }
    gt_ssh_t *ssh=connect_fixture(argv);gt_status_t opened;
    gt_sftp_t *sftp=gt_ssh_sftp_controlled(ssh,3000,NULL,NULL,&opened);if(!sftp||opened)abort();
    gt_sftp_mkdir_result_t out={GT_SFTP_MKDIR_COMPLETE,99};
    CHECK(gt_sftp_mkdir_controlled(sftp,parent,name,&out,0,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_MKDIR_NOT_ATTEMPTED && !out.remote_status,"invalid budget clears result");
    const char *badparents[]={"","relative","//double","/trailing/","/./dot","/foo/../bar"};
    const char *badnames[]={"",".","..","two/parts"};
    for(size_t i=0;i<sizeof badparents/sizeof *badparents;i++)CHECK(gt_sftp_mkdir_controlled(sftp,badparents[i],name,&out,100,NULL,NULL)==GT_ERR_INVALID_ARG,"canonical absolute parent required");
    for(size_t i=0;i<sizeof badnames/sizeof *badnames;i++)CHECK(gt_sftp_mkdir_controlled(sftp,parent,badnames[i],&out,100,NULL,NULL)==GT_ERR_INVALID_ARG,"single non-dot name required");
    char longname[257];memset(longname,'x',256);longname[256]=0;
    CHECK(gt_sftp_mkdir_controlled(sftp,parent,longname,&out,100,NULL,NULL)==GT_ERR_INVALID_ARG,"name byte limit");
    char *longparent=malloc(65537);if(!longparent)abort();memset(longparent,'x',65536);longparent[0]='/';longparent[65536]=0;
    CHECK(gt_sftp_mkdir_controlled(sftp,longparent,name,&out,100,NULL,NULL)==GT_ERR_INVALID_ARG,"parent byte limit");
    longparent[65534]=0;CHECK(gt_sftp_mkdir_controlled(sftp,longparent,name,&out,100,NULL,NULL)==GT_ERR_INVALID_ARG,"joined path byte limit");free(longparent);
    stopping=1;CHECK(gt_sftp_mkdir_controlled(sftp,parent,name,&out,100,cancelled,NULL)==GT_ERR_CANCELLED && out.state==GT_SFTP_MKDIR_NOT_ATTEMPTED,"cancel before IO");stopping=0;
    if(failures){gt_sftp_close(sftp);gt_ssh_close(ssh);return;}
    if(!strcmp(mode,"jobs")){
        char source[8192],destination[8192];snprintf(source,sizeof source,"%s/source",parent);snprintf(destination,sizeof destination,"%s/destination",parent);write_file(source);
        CHECK(gt_sftp_upload(sftp,source,destination)!=0,"job submitted");
        CHECK(gt_sftp_mkdir_controlled(sftp,parent,name,&out,100,NULL,NULL)==GT_ERR_INVALID_ARG && out.state==GT_SFTP_MKDIR_NOT_ATTEMPTED,"dedicated owner required");
        gt_sftp_close(sftp);gt_ssh_close(ssh);return;
    }
    void *raw=NULL;int socket=-1;if(gt__ssh_borrow(ssh,&raw,&socket))abort();libssh2_session_set_timeout(raw,2345);
    int64_t calibration=now();for(int i=0;i<20;i++){struct timespec t={.tv_nsec=10000000};nanosleep(&t,NULL);}calibration=now()-calibration;
    int64_t bound=calibration*3/2+500;if(bound<3000)bound=3000;
    cancel_created=!strcmp(mode,"cancel-created");cancel_closed=!strcmp(mode,"late-cancel");calls=0;
    printf("READY %ld\n",(long)getpid());int64_t began=now();
    int status=gt_sftp_mkdir_controlled(sftp,parent,name,&out,!strcmp(mode,"timeout")?400:10000,cancelled,NULL);
    int64_t elapsed=now()-began;
    int wanted=peer?atoi(argv[7]):conflict||missing||denied?GT_ERR_IO:link||file?GT_ERR_UNSUPPORTED:cancel_created?GT_ERR_CANCELLED:GT_OK;
    gt_sftp_mkdir_state_t state=peer?(gt_sftp_mkdir_state_t)atoi(argv[8]):conflict?GT_SFTP_MKDIR_CONFLICT:link||file||missing?GT_SFTP_MKDIR_NOT_ATTEMPTED:denied?GT_SFTP_MKDIR_REJECTED:cancel_created?GT_SFTP_MKDIR_CREATED:GT_SFTP_MKDIR_COMPLETE;
    uint32_t remote=peer?(uint32_t)strtoul(argv[9],NULL,10):missing?2:denied?3:0;
    CHECK(status==wanted,"status preserved");CHECK(out.state==state,"side effect state preserved");CHECK(out.remote_status==remote,"raw remote status preserved");
    if(state==GT_SFTP_MKDIR_NOT_ATTEMPTED || state==GT_SFTP_MKDIR_CONFLICT)CHECK(!calls,"preflight never creates");else CHECK(calls>0,"creation attempted");
    if(status==GT_OK){CHECK(gt__ssh_borrow(ssh,&raw,&socket)==GT_OK,"success owner reusable");CHECK(libssh2_session_get_timeout(raw)==2345&&libssh2_session_get_blocking(raw)==1,"session options restored");}
    else CHECK(gt__ssh_borrow(ssh,&raw,&socket)!=GT_OK,"failed IO invalidates owner");
    if(!strcmp(mode,"timeout")||!strcmp(mode,"cancel")){CHECK(elapsed<bound,"bounded total time");if(!strcmp(mode,"timeout"))CHECK(elapsed>=398,"positive total budget consumed");}
    stopping=0;cancel_created=false;cancel_closed=false;
    if(!peer){
        struct stat st;
        if(state==GT_SFTP_MKDIR_COMPLETE||state==GT_SFTP_MKDIR_CREATED){CHECK(!lstat(target,&st)&&S_ISDIR(st.st_mode),"created directory retained");CHECK((st.st_mode&0777)==0700,"created permissions");}
        if(conflict){CHECK(!lstat(target,&st),"conflict retained");CHECK(!strcmp(mode,"conflict-file")?S_ISREG(st.st_mode):!strcmp(mode,"conflict-link")?S_ISLNK(st.st_mode):S_ISDIR(st.st_mode),"conflict type retained");}
        if(!strcmp(mode,"nested")){
            CHECK(gt_sftp_mkdir_controlled(sftp,target,"empty",&out,2000,NULL,NULL)==GT_OK&&out.state==GT_SFTP_MKDIR_COMPLETE,"nested empty directory");
            char source[8192],destination[8192],final[8192];snprintf(source,sizeof source,"%s/source",parent);write_file(source);snprintf(destination,sizeof destination,"%s/child/empty/.staged",parent);snprintf(final,sizeof final,"%s/child/empty/data",parent);
            int fd=open(source,O_RDONLY);if(fd<0)abort();gt_sftp_create_result_t created;
            CHECK(gt_sftp_create_controlled(sftp,destination,fd,4,&created,2000,NULL,NULL)==GT_OK,"real file staged in new child");close(fd);
            gt_sftp_publish_result_t published;CHECK(gt_sftp_publish_controlled(sftp,destination,final,false,4,&published,2000,NULL,NULL)==GT_OK,"real file published in new child");
            char bytes[4];FILE *f=fopen(final,"rb");CHECK(f&&fread(bytes,1,4,f)==4&&!memcmp(bytes,"data",4),"published file bytes");if(f)fclose(f);
        }
        if(denied&&chmod(parent,0700))abort();
    }
    began=now();gt_sftp_close(sftp);gt_ssh_close(ssh);CHECK(now()-began<bound,"bounded cleanup");
    if(!failures)printf("PASS mkdir %s\n",mode);
}
int main(int argc,char **argv){
    if(argc!=6&&argc!=10)return 2;setvbuf(stdout,NULL,_IONBF,0);signal(SIGUSR1,stop);
    if(argc==10)run_case(argv,argv[6],true);
    else{const char *cases[]={"normal","unicode","nested","late-cancel","cancel-created","conflict-file","conflict-link","conflict-directory","parent-link","parent-file","parent-missing","permission","jobs"};for(size_t i=0;i<sizeof cases/sizeof *cases;i++)run_case(argv,cases[i],false);}
    if(!failures)puts("SFTP MKDIR PASS");return failures?1:0;
}
