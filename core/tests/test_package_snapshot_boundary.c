/* Compile production capture against deterministic POSIX boundary failures.
 * Main physical tests separately consume the actual archive implementation. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *boundary_mode;
static int boundary_root;
static bool boundary_read_done,boundary_emitted,boundary_destroy_error;
static bool boundary_case(const char *mode,int root){
 const char *cases[]={"bad-utf8","duplicate-entry","case-entry","read-error","read-truncated","read-grown","namespace-change","readdir-error","destroy-error"};
 for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++)if(!strcmp(mode,cases[i])){boundary_mode=mode;boundary_root=root;return true;}return false;
}
static struct dirent *boundary_readdir(DIR *d){
 struct dirent *e=readdir(d);static struct dirent fake;
 if(e && !strcmp(e->d_name,"payload.bin") && !strcmp(boundary_mode,"bad-utf8")){memset(&fake,0,sizeof(fake));strcpy(fake.d_name,"bad\xe0\x80\xaf");return &fake;}
 if(!e && !boundary_emitted){
  boundary_emitted=true;
  if(!strcmp(boundary_mode,"readdir-error")){errno=EIO;return NULL;}
  if(!strcmp(boundary_mode,"duplicate-entry") || !strcmp(boundary_mode,"case-entry")){memset(&fake,0,sizeof(fake));strcpy(fake.d_name,!strcmp(boundary_mode,"case-entry")?"PAYLOAD.BIN":"payload.bin");return &fake;}
 }return e;
}
static ssize_t boundary_read(int fd,void *p,size_t n){
 if(!strcmp(boundary_mode,"read-error")){errno=EIO;return -1;}
 if(!strcmp(boundary_mode,"read-truncated"))return 0;
 ssize_t got=read(fd,p,n);
 if(got>0 && !boundary_read_done){
  boundary_read_done=true;
  if(!strcmp(boundary_mode,"read-grown")){int f=openat(boundary_root,"payload.bin",O_WRONLY|O_APPEND|O_CLOEXEC);if(f<0 || write(f,"x",1)!=1 || close(f))abort();}
  if(!strcmp(boundary_mode,"namespace-change")){int f=openat(boundary_root,"late",O_WRONLY|O_CREAT|O_CLOEXEC,0600);if(f<0 || close(f))abort();}
 }return got;
}
static int boundary_close(int fd){int rc=close(fd);if(boundary_destroy_error){boundary_destroy_error=false;errno=EIO;return -1;}return rc;}
#define readdir boundary_readdir
#define read boundary_read
#define close boundary_close
#include "../src/extension/package-snapshot.c"
#undef readdir
#undef read
#undef close
static gt_package_status_t boundary_expected(const char *mode){
 if(!strcmp(mode,"destroy-error"))return GT_PACKAGE_OK;
 if(!strcmp(mode,"read-error") || !strcmp(mode,"readdir-error"))return GT_PACKAGE_IO;
 if(!strcmp(mode,"read-truncated") || !strcmp(mode,"read-grown") || !strcmp(mode,"namespace-change"))return GT_PACKAGE_CHANGED;
 return GT_PACKAGE_INVALID;
}
#define GT_PACKAGE_BOUNDARY 1
#include "test_package_snapshot.c"
