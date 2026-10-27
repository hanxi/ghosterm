#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include "deploy.h"
#include "internal.h"
#include "ipc/ssh-link.h"
#include "base/secure-random.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* The source archive stays in the app's verified bundle. The first five files
 * are the remote runtime inventory; old immutable releases remain reusable. */
static const char *inventory[GT_BUNDLE_FILES]={"ghosterm-sessiond","ghosterm-session-proxy","lib/libghostty-vt.so.0","LICENSE","SOURCE-INFO.json","sources.tar.gz"};
static bool hex(const char *s,size_t n){if(strlen(s)!=n)return false;for(size_t i=0;i<n;i++)if(!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f')))return false;return true;}
static bool token(const char *s,size_t cap){size_t n=strlen(s);if(!n || n>=cap || !strcmp(s,".") || !strcmp(s,".."))return false;for(size_t i=0;i<n;i++)if(!((s[i]>='a' && s[i]<='z') || (s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9') || s[i]=='-' || s[i]=='_' || s[i]=='.'))return false;return true;}
static bool version(const char *s,unsigned *a,unsigned *b,unsigned *c) {
  unsigned values[3]={0};unsigned n=0;
  while(*s && n<3){if(*s<'0' || *s>'9')return false;char *end;errno=0;unsigned long v=strtoul(s,&end,10);if(errno || v>65535)return false;values[n++]=(unsigned)v;if(!*end){s=end;break;}if(*end!='.')return false;s=end+1;if(!*s)return false;}
  if(*s || n<2)return false;
  *a=values[0];*b=values[1];*c=values[2];return true;
}
static bool fields(char *line,char **out,size_t count) {size_t i=0;char *save=NULL,*p=strtok_r(line,"\t\n",&save);while(p && i<count){out[i++]=p;p=strtok_r(NULL,"\t\n",&save);}return i==count && !p;}
gt_status_t gt__session_bundle_read(const char *directory,gt_session_bundle_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!directory || !out || strlen(directory)>=sizeof(out->directory))return GT_ERR_INVALID_ARG;
  char *absolute=realpath(directory,NULL);if(!absolute)return GT_ERR_INVALID_ARG;
  if(strlen(absolute)>=sizeof(out->directory)){free(absolute);return GT_ERR_INVALID_ARG;}
  char owned_directory[GT_DEPLOY_PATH];strcpy(owned_directory,absolute);free(absolute);
  int root=open(owned_directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(root<0)return GT_ERR_IO;
  int fd=openat(root,"manifest.tsv",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
  if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>8192){if(fd>=0)close(fd);close(root);return GT_ERR_INVALID_ARG;}
  FILE *f=fdopen(fd,"r");if(!f){close(fd);close(root);return GT_ERR_IO;}char line[2048],*parts[6];gt_session_bundle_t bundle={0};bool okay=fgets(line,sizeof(line),f) && fields(line,parts,6) && !strcmp(parts[0],"GTB1");
  if(okay){okay=token(parts[1],sizeof(bundle.release)) && (!strcmp(parts[2],"aarch64") || !strcmp(parts[2],"x86_64")) && strlen(parts[3])<sizeof(bundle.min_glibc) && hex(parts[5],64) && (!strcmp(parts[4],"/lib/ld-linux-aarch64.so.1") || !strcmp(parts[4],"/lib64/ld-linux-x86-64.so.2"));unsigned a,b,c;okay=okay && version(parts[3],&a,&b,&c);if(okay){strcpy(bundle.release,parts[1]);strcpy(bundle.arch,parts[2]);strcpy(bundle.min_glibc,parts[3]);strcpy(bundle.loader,parts[4]);strcpy(bundle.contract,parts[5]);}}
  for(unsigned i=0;i<GT_BUNDLE_FILES && okay;i++) {
    okay=fgets(line,sizeof(line),f) && fields(line,parts,3) && !strcmp(parts[0],inventory[i]) && hex(parts[2],64);
    if(!okay)break;
    char *end;errno=0;unsigned long long n=strtoull(parts[1],&end,10);okay=!errno && !*end && n>0 && n<=512u*1024u*1024u;
    fd=openat(root,inventory[i],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);okay=okay && fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=0 && (uint64_t)st.st_size==n;
    if(fd>=0)close(fd);
    if(okay){strcpy(bundle.files[i].name,inventory[i]);strcpy(bundle.files[i].sha256,parts[2]);bundle.files[i].size=n;}
  }
  if(fgetc(f)!=EOF)okay=false;
  fclose(f);close(root);if(!okay)return GT_ERR_INVALID_ARG;
  strcpy(bundle.directory,owned_directory);*out=bundle;return GT_OK;
}
#ifdef GT_HAVE_LIBSSH2
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <poll.h>
#include <time.h>
typedef struct {
  gt_ssh_t *ssh;LIBSSH2_SESSION *session;int fd,flags,blocking;long timeout;
  uint64_t started,previous,reported;gt_deploy_options_t options;gt_status_t error;
} job;
static bool clock_ms(uint64_t *out){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0)return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static bool alive(job *j) {
  uint64_t now;if(j->error!=GT_OK)return false;
  if(j->options.cancelled && j->options.cancelled(j->options.context)){j->error=GT_ERR_CANCELLED;return false;}
  if(!clock_ms(&now) || now<j->previous){j->error=GT_ERR_INTERNAL;return false;}j->previous=now;
  if(now-j->started>=j->options.timeout_ms){gt__set_error("session install: deadline expired");j->error=GT_ERR_IO;return false;}return true;
}
static bool wait_io(job *j) {
  if(!alive(j))return false;
  int directions=libssh2_session_block_directions(j->session);struct pollfd p={.fd=j->fd,.events=(short)(((directions&LIBSSH2_SESSION_BLOCK_OUTBOUND)?POLLOUT:0)|POLLIN)};
  int rc=poll(&p,1,10);if((rc<0 && errno!=EINTR) || (p.revents&(POLLHUP|POLLERR|POLLNVAL))){j->error=GT_ERR_IO;return false;}return alive(j);
}
static bool begin(job *j,gt_ssh_t *ssh,const gt_deploy_options_t *options) {
  memset(j,0,sizeof(*j));j->ssh=ssh;j->options=options?*options:(gt_deploy_options_t){.timeout_ms=30000};
  if(!j->options.timeout_ms || j->options.timeout_ms>120000){j->error=GT_ERR_INVALID_ARG;return false;}
  void *session=NULL;j->error=(gt_status_t)gt__ssh_borrow(ssh,&session,&j->fd);if(j->error!=GT_OK)return false;
  j->session=session;if(!clock_ms(&j->started)){j->error=GT_ERR_INTERNAL;return false;}j->previous=j->started;
  j->flags=fcntl(j->fd,F_GETFL);if(j->flags<0 || fcntl(j->fd,F_SETFL,j->flags|O_NONBLOCK)){j->error=GT_ERR_IO;return false;}
  j->blocking=libssh2_session_get_blocking(j->session);j->timeout=libssh2_session_get_timeout(j->session);libssh2_session_set_blocking(j->session,0);return true;
}
static void end(job *j) {
  if(j->error==GT_ERR_IO || j->error==GT_ERR_CANCELLED || j->error==GT_ERR_INTERNAL)gt__ssh_abort(j->ssh);
  libssh2_session_set_blocking(j->session,j->blocking);libssh2_session_set_timeout(j->session,j->timeout);(void)fcntl(j->fd,F_SETFL,j->flags);
}
static void progress(job *j,gt_deploy_phase_t phase,unsigned file,uint64_t bytes,uint64_t total) {
  uint64_t now;if(!j->options.progress || !clock_ms(&now))return;
  if(bytes && bytes!=total && now-j->reported<100)return;
  j->reported=now;j->options.progress(j->options.context,phase,file,bytes,total);
}
static bool run(job *j,const char *command,char *out,size_t capacity) {
  LIBSSH2_CHANNEL *ch=NULL;size_t used=0;bool eof=false;int rc;out[0]=0;
  while(alive(j) && !(ch=libssh2_channel_open_session(j->session))){if(libssh2_session_last_errno(j->session)!=LIBSSH2_ERROR_EAGAIN){j->error=GT_ERR_IO;break;}if(!wait_io(j))break;}
  if(!ch)return false;
  do {rc=libssh2_channel_exec(ch,command);if(rc!=LIBSSH2_ERROR_EAGAIN)break;}while(wait_io(j));
  if(rc!=0 && j->error==GT_OK)j->error=GT_ERR_UNSUPPORTED;
  while(alive(j)) {
    char bytes[4096];ssize_t a=libssh2_channel_read(ch,bytes,sizeof(bytes));
    if(a>0){if((size_t)a>=capacity-used){j->error=GT_ERR_PROTOCOL;break;}memcpy(out+used,bytes,(size_t)a);used+=(size_t)a;out[used]=0;}
    ssize_t b=libssh2_channel_read_stderr(ch,bytes,sizeof(bytes));gt__secure_erase(bytes,sizeof(bytes));
    if((a<0 && a!=LIBSSH2_ERROR_EAGAIN) || (b<0 && b!=LIBSSH2_ERROR_EAGAIN)){j->error=GT_ERR_IO;break;}
    if(!a && !b && libssh2_channel_eof(ch)){eof=true;break;}
    if(a<=0 && b<=0 && !wait_io(j))break;
  }
  if(j->error!=GT_OK)gt__ssh_abort(j->ssh);
  do {rc=libssh2_channel_free(ch);if(rc!=LIBSSH2_ERROR_EAGAIN)break;}while(wait_io(j));
  if(rc && j->error==GT_OK)j->error=GT_ERR_IO;
  return eof && j->error==GT_OK;
}
static bool linux_probe(job *j,gt_linux_probe_t *out) {
  char result[1024];progress(j,GT_DEPLOY_PROBE,0,0,0);
  if(!run(j,"uname -s; uname -m; { getconf GNU_LIBC_VERSION 2>/dev/null || printf 'unsupported-libc\\n'; }; id -u; printf '%s\\n' \"$HOME\"",result,sizeof(result)))return false;
  char *save=NULL,*parts[5];for(unsigned i=0;i<5;i++){parts[i]=strtok_r(i?NULL:result,"\n",&save);if(!parts[i]){j->error=GT_ERR_PROTOCOL;return false;}}
  if(strtok_r(NULL,"\n",&save) || strcmp(parts[0],"Linux") || (strcmp(parts[1],"aarch64") && strcmp(parts[1],"x86_64")) || strncmp(parts[2],"glibc ",6)){j->error=GT_ERR_UNSUPPORTED;return false;}
  char *endp;errno=0;unsigned long uid=strtoul(parts[3],&endp,10);unsigned a,b,c;
  if(errno || *endp || uid>UINT32_MAX || strlen(parts[2]+6)>=sizeof(out->glibc) || !version(parts[2]+6,&a,&b,&c) || parts[4][0]!='/' || strlen(parts[4])>=sizeof(out->home)){j->error=GT_ERR_PROTOCOL;return false;}
  strcpy(out->arch,parts[1]);strcpy(out->glibc,parts[2]+6);strcpy(out->home,parts[4]);out->uid=(uint32_t)uid;return true;
}
gt_status_t gt__ssh_probe_linux(gt_ssh_t *ssh,const gt_deploy_options_t *options,gt_linux_probe_t *out) {
  if(!out)return GT_ERR_INVALID_ARG;
  memset(out,0,sizeof(*out));job j;gt__ssh_io_lock(ssh);if(!begin(&j,ssh,options)){gt__ssh_io_unlock(ssh);return j.error;}
  (void)linux_probe(&j,out);end(&j);gt__ssh_io_unlock(ssh);return j.error;
}
static bool sf_result(job *j,LIBSSH2_SFTP *sf,int rc) {
  if(j->error!=GT_OK)return false;
  if(!rc)return true;
  j->error=libssh2_sftp_last_error(sf)==LIBSSH2_FX_OP_UNSUPPORTED?GT_ERR_UNSUPPORTED:GT_ERR_IO;return false;
}
static int stat_path(job *j,LIBSSH2_SFTP *sf,const char *path,LIBSSH2_SFTP_ATTRIBUTES *a) {
  int rc;do {rc=libssh2_sftp_stat_ex(sf,path,(unsigned)strlen(path),LIBSSH2_SFTP_LSTAT,a);if(rc!=LIBSSH2_ERROR_EAGAIN)break;}while(wait_io(j));return rc;
}
static bool directories(job *j,LIBSSH2_SFTP *sf,const char *path,uint32_t uid) {
  if(path[0]!='/' || strlen(path)>700){j->error=GT_ERR_INVALID_ARG;return false;}
  char current[768];size_t used=0;const char *p=path+1;current[used++]='/';current[used]=0;
  while(*p && alive(j)) {
    const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);
    if(!n || (n==1 && p[0]=='.') || (n==2 && !memcmp(p,"..",2)) || (slash && !slash[1])){j->error=GT_ERR_INVALID_ARG;return false;}
    memcpy(current+used,p,n);used+=n;current[used]=0;LIBSSH2_SFTP_ATTRIBUTES a={0};int rc=stat_path(j,sf,current,&a);
    if(rc && libssh2_sftp_last_error(sf)==LIBSSH2_FX_NO_SUCH_FILE){do{rc=libssh2_sftp_mkdir_ex(sf,current,(unsigned)used,0700);if(rc!=LIBSSH2_ERROR_EAGAIN)break;}while(wait_io(j));if(rc){j->error=GT_ERR_IO;return false;}rc=stat_path(j,sf,current,&a);}
    if(rc){if(j->error==GT_OK)j->error=GT_ERR_IO;return false;}
    if(!(a.flags&LIBSSH2_SFTP_ATTR_UIDGID) || !(a.flags&LIBSSH2_SFTP_ATTR_PERMISSIONS)){j->error=GT_ERR_UNSUPPORTED;return false;}
    if(!S_ISDIR((mode_t)a.permissions) || (a.uid!=uid && a.uid!=0) || (a.permissions&0022)){j->error=GT_ERR_AUTH;return false;}
    if(!slash)return true;
    current[used++]='/';current[used]=0;p=slash+1;
  }
  return j->error==GT_OK;
}
static bool pathcat(char *out,size_t capacity,const char *root,const char *name){int n=snprintf(out,capacity,"%s/%s",root,name);return n>0 && (size_t)n<capacity;}
static bool upload(job *j,LIBSSH2_SFTP *sf,const char *local,const char *remote,uint64_t size,unsigned index) {
  int fd=open(local,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){j->error=GT_ERR_IO;return false;}
  LIBSSH2_SFTP_HANDLE *file=NULL;
  while(alive(j) && !(file=libssh2_sftp_open_ex(sf,remote,(unsigned)strlen(remote),LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_EXCL,0600,LIBSSH2_SFTP_OPENFILE))){if(libssh2_session_last_errno(j->session)!=LIBSSH2_ERROR_EAGAIN){j->error=GT_ERR_IO;break;}if(!wait_io(j))break;}
  uint8_t bytes[32768];uint64_t sent=0;progress(j,GT_DEPLOY_UPLOAD,index,0,size);
  while(file && alive(j)) {
    ssize_t got=read(fd,bytes,sizeof(bytes));if(got<0){if(errno==EINTR)continue;j->error=GT_ERR_IO;break;}if(!got)break;
    size_t offset=0;while(offset<(size_t)got && alive(j)){ssize_t n=libssh2_sftp_write(file,(const char *)bytes+offset,(size_t)got-offset);if(n>0){offset+=(size_t)n;sent+=(size_t)n;progress(j,GT_DEPLOY_UPLOAD,index,sent,size);}else if(n==LIBSSH2_ERROR_EAGAIN || n==0){if(!wait_io(j))break;}else{j->error=GT_ERR_IO;break;}}
    gt__secure_erase(bytes,sizeof(bytes));
  }
  gt__secure_erase(bytes,sizeof(bytes));close(fd);
  if(file) {
    /* An SFTP handle is not freed by subsystem shutdown. Cancellation must
     * first break the network wait, then finish this exact close continuation. */
    if(j->error!=GT_OK)gt__ssh_abort(j->ssh);
    int rc;unsigned cleanup_attempts=0;
    do {
      rc=libssh2_sftp_close_handle(file);if(rc!=LIBSSH2_ERROR_EAGAIN)break;
      if(j->error==GT_OK){if(wait_io(j))continue;gt__ssh_abort(j->ssh);}
      if(++cleanup_attempts>=8)break;
    }while(true);
    if(rc && j->error==GT_OK)j->error=GT_ERR_IO;
  }
  if(sent!=size && j->error==GT_OK)j->error=GT_ERR_IO;
  return j->error==GT_OK;
}
static bool inventory_safe(job *j,LIBSSH2_SFTP *sf,const gt_session_bundle_t *b,const char *root,uint32_t uid) {
  for(unsigned i=0;i<GT_BUNDLE_UPLOAD_FILES+2;i++) {
    const char *name=i<GT_BUNDLE_UPLOAD_FILES?inventory[i]:i==GT_BUNDLE_UPLOAD_FILES?"":"lib";char path[GT_DEPLOY_PATH];
    if(*name){if(!pathcat(path,sizeof(path),root,name)){j->error=GT_ERR_INVALID_ARG;return false;}}else{if(strlen(root)>=sizeof(path)){j->error=GT_ERR_INVALID_ARG;return false;}strcpy(path,root);}
    LIBSSH2_SFTP_ATTRIBUTES a={0};if(stat_path(j,sf,path,&a)){if(j->error==GT_OK)j->error=GT_ERR_IO;return false;}
    if(!(a.flags&LIBSSH2_SFTP_ATTR_UIDGID) || !(a.flags&LIBSSH2_SFTP_ATTR_PERMISSIONS) || (i<GT_BUNDLE_UPLOAD_FILES && !(a.flags&LIBSSH2_SFTP_ATTR_SIZE))){j->error=GT_ERR_UNSUPPORTED;return false;}
    if((a.uid!=uid && a.uid!=0) || (a.permissions&0022) || (i<GT_BUNDLE_UPLOAD_FILES?(!S_ISREG((mode_t)a.permissions) || a.filesize!=b->files[i].size):!S_ISDIR((mode_t)a.permissions))){j->error=GT_ERR_AUTH;return false;}
  }
  return true;
}
static bool verify(job *j,const gt_session_bundle_t *b,const char *stage) {
  char command[8192]="cd ";size_t used=3;
  if(!gt__ssh_posix_path(command,sizeof(command),&used,stage)){j->error=GT_ERR_INVALID_ARG;return false;}
  int n=snprintf(command+used,sizeof(command)-used," && printf '%%s\\n'");if(n<0 || (size_t)n>=sizeof(command)-used){j->error=GT_ERR_INTERNAL;return false;}used+=(size_t)n;
  for(unsigned i=0;i<GT_BUNDLE_UPLOAD_FILES;i++){n=snprintf(command+used,sizeof(command)-used," '%s  %s'",b->files[i].sha256,inventory[i]);if(n<0 || (size_t)n>=sizeof(command)-used){j->error=GT_ERR_INTERNAL;return false;}used+=(size_t)n;}
  n=snprintf(command+used,sizeof(command)-used," | /usr/bin/sha256sum -c - >/dev/null && printf 'GT_VERIFY_OK\\n'");if(n<0 || (size_t)n>=sizeof(command)-used){j->error=GT_ERR_INTERNAL;return false;}
  char reply[512];progress(j,GT_DEPLOY_VERIFY,0,0,0);if(!run(j,command,reply,sizeof(reply)))return false;
  if(strcmp(reply,"GT_VERIFY_OK\n")){j->error=GT_ERR_PROTOCOL;return false;}return true;
}
static bool executable_probe(job *j,const gt_session_bundle_t *bundle,const char *directory) {
  char command[2048]="",executable[GT_DEPLOY_PATH];size_t used=0;
  if(!pathcat(executable,sizeof(executable),directory,"ghosterm-sessiond") || !gt__ssh_posix_path(command,sizeof(command),&used,executable) || used+sizeof(" --version")>sizeof(command)){j->error=GT_ERR_INVALID_ARG;return false;}
  memcpy(command+used," --version",sizeof(" --version"));char reply[512],expected[160];
  snprintf(expected,sizeof(expected),"ghosterm-sessiond GTSD/1 INLINE/1 %s\n",bundle->contract);
  if(!run(j,command,reply,sizeof(reply)))return false;
  if(strcmp(reply,expected)){j->error=GT_ERR_UNSUPPORTED;return false;}
  used=0;command[0]=0;
  if(!pathcat(executable,sizeof(executable),directory,"ghosterm-session-proxy") || !gt__ssh_posix_path(command,sizeof(command),&used,executable) || used+sizeof(" --version")>sizeof(command)){j->error=GT_ERR_INVALID_ARG;return false;}
  memcpy(command+used," --version",sizeof(" --version"));
  if(!run(j,command,reply,sizeof(reply)))return false;
  if(strcmp(reply,"ghosterm-session-proxy GTSD/1 DEPLOY/1\n")){j->error=GT_ERR_UNSUPPORTED;return false;}return true;
}
gt_status_t gt__ssh_deploy_linux(gt_ssh_t *ssh,const gt_session_bundle_t *b,const char *root,const gt_deploy_options_t *options,gt_deploy_result_t *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!out || !b || !root || !token(b->release,sizeof(b->release)) || !hex(b->contract,64))return GT_ERR_INVALID_ARG;
  for(unsigned i=0;i<GT_BUNDLE_FILES;i++)if(strcmp(b->files[i].name,inventory[i]) || !hex(b->files[i].sha256,64))return GT_ERR_INVALID_ARG;
  gt__ssh_io_lock(ssh);job j;if(!begin(&j,ssh,options)){gt__ssh_io_unlock(ssh);return j.error;}
  gt_linux_probe_t probe;LIBSSH2_SFTP *sf=NULL;char releases[GT_DEPLOY_PATH],final[GT_DEPLOY_PATH],stage[GT_DEPLOY_PATH];
  if(!linux_probe(&j,&probe))goto done;
  unsigned a,bv,c,x,y,z;if(strcmp(probe.arch,b->arch) || !version(probe.glibc,&a,&bv,&c) || !version(b->min_glibc,&x,&y,&z) || a<x || (a==x && (bv<y || (bv==y && c<z)))){j.error=GT_ERR_UNSUPPORTED;goto done;}
  if(!pathcat(releases,sizeof(releases),root,"releases") || !pathcat(final,sizeof(final),releases,b->release)){j.error=GT_ERR_INVALID_ARG;goto done;}
  while(alive(&j) && !(sf=libssh2_sftp_init(j.session))){if(libssh2_session_last_errno(j.session)!=LIBSSH2_ERROR_EAGAIN){j.error=GT_ERR_UNSUPPORTED;break;}if(!wait_io(&j))break;}
  if(!sf || !directories(&j,sf,releases,probe.uid))goto done;
  LIBSSH2_SFTP_ATTRIBUTES attributes={0};int rc=stat_path(&j,sf,final,&attributes);
  if(!rc){out->reused=true;if(!inventory_safe(&j,sf,b,final,probe.uid) || !verify(&j,b,final) || !executable_probe(&j,b,final))goto done;}
  else {
    if(libssh2_sftp_last_error(sf)!=LIBSSH2_FX_NO_SUCH_FILE){j.error=GT_ERR_IO;goto done;}
    uint8_t nonce[12];if(gt__secure_random(nonce,sizeof(nonce))!=GT_OK){j.error=GT_ERR_INTERNAL;goto done;}
    char leaf[64]=".stage-";for(unsigned i=0;i<sizeof(nonce);i++)snprintf(leaf+7+i*2,3,"%02x",nonce[i]);gt__secure_erase(nonce,sizeof(nonce));
    if(!pathcat(stage,sizeof(stage),root,leaf) || !directories(&j,sf,stage,probe.uid)){j.error=GT_ERR_IO;goto done;}
    char lib[GT_DEPLOY_PATH];if(!pathcat(lib,sizeof(lib),stage,"lib") || !directories(&j,sf,lib,probe.uid))goto done;
    for(unsigned i=0;i<GT_BUNDLE_UPLOAD_FILES;i++){char local[GT_DEPLOY_PATH],remote[GT_DEPLOY_PATH];if(!pathcat(local,sizeof(local),b->directory,inventory[i]) || !pathcat(remote,sizeof(remote),stage,inventory[i])){j.error=GT_ERR_INVALID_ARG;goto done;}if(!upload(&j,sf,local,remote,b->files[i].size,i))goto done;}
    if(!verify(&j,b,stage))goto done;
    for(unsigned i=0;i<2;i++){char remote[GT_DEPLOY_PATH];(void)pathcat(remote,sizeof(remote),stage,inventory[i]);LIBSSH2_SFTP_ATTRIBUTES a2={.flags=LIBSSH2_SFTP_ATTR_PERMISSIONS,.permissions=0700};do{rc=libssh2_sftp_stat_ex(sf,remote,(unsigned)strlen(remote),LIBSSH2_SFTP_SETSTAT,&a2);if(rc!=LIBSSH2_ERROR_EAGAIN)break;}while(wait_io(&j));if(!sf_result(&j,sf,rc))goto done;}
    if(!executable_probe(&j,b,stage))goto done;
    progress(&j,GT_DEPLOY_ACTIVATE,0,0,0);
    char command[4096]="",publisher[GT_DEPLOY_PATH],reply[128];size_t used=0;
    if(!pathcat(publisher,sizeof(publisher),stage,"ghosterm-session-proxy") || !gt__ssh_posix_path(command,sizeof(command),&used,publisher) || used+sizeof(" publish --from ")>sizeof(command)){j.error=GT_ERR_INVALID_ARG;goto done;}
    memcpy(command+used," publish --from ",sizeof(" publish --from "));used+=strlen(" publish --from ");
    if(!gt__ssh_posix_path(command,sizeof(command),&used,stage) || used+sizeof(" --to ")>sizeof(command)){j.error=GT_ERR_INVALID_ARG;goto done;}
    memcpy(command+used," --to ",sizeof(" --to "));used+=strlen(" --to ");
    if(!gt__ssh_posix_path(command,sizeof(command),&used,final)){j.error=GT_ERR_INVALID_ARG;goto done;}
    out->publication_unknown=true;
    if(!run(&j,command,reply,sizeof(reply)))goto done;
    if(strcmp(reply,"GT_PUBLISH_OK\n")){j.error=GT_ERR_IO;goto done;}
    out->publication_unknown=false;
  }
  out->published=true;
  /* Return immutable version paths; there is no mutable PATH/symlink switch.
   * Future connections select their exact bundle. Existing instances are never
   * replaced by deployment, and reuse always verifies the entire inventory. */
  if(!pathcat(out->daemon,sizeof(out->daemon),final,"ghosterm-sessiond") || !pathcat(out->proxy,sizeof(out->proxy),final,"ghosterm-session-proxy")){j.error=GT_ERR_INTERNAL;goto done;}
  progress(&j,GT_DEPLOY_DONE,0,0,0);
done:
  if(j.error!=GT_OK)gt__ssh_abort(ssh);
  if(sf){int status;do{status=libssh2_sftp_shutdown(sf);if(status!=LIBSSH2_ERROR_EAGAIN)break;}while(wait_io(&j));if(status && j.error==GT_OK)j.error=GT_ERR_IO;}
  end(&j);gt__ssh_io_unlock(ssh);return j.error;
}
#else
gt_status_t gt__ssh_probe_linux(gt_ssh_t *s,const gt_deploy_options_t *o,gt_linux_probe_t *p){(void)s;(void)o;if(p)memset(p,0,sizeof(*p));return GT_ERR_UNSUPPORTED;}
gt_status_t gt__ssh_deploy_linux(gt_ssh_t *s,const gt_session_bundle_t *b,const char *r,const gt_deploy_options_t *o,gt_deploy_result_t *p){(void)s;(void)b;(void)r;(void)o;if(p)memset(p,0,sizeof(*p));return GT_ERR_UNSUPPORTED;}
#endif
