#define _POSIX_C_SOURCE 200809L
#include "io/backend.h"
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static void child(void) {
  struct termios t;CHECK(!tcgetattr(STDIN_FILENO,&t));t.c_lflag&=(tcflag_t)~(ICANON|ECHO);t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;CHECK(!tcsetattr(STDIN_FILENO,TCSANOW,&t));
  for(;;) {
    struct winsize w;CHECK(!ioctl(STDIN_FILENO,TIOCGWINSZ,&w));printf("SIZE %u %u %u %u\n",w.ws_col,w.ws_row,w.ws_xpixel,w.ws_ypixel);CHECK(!fflush(stdout));
    char c;CHECK(read(STDIN_FILENO,&c,1)==1);if(c=='q')return;CHECK(c=='s');
  }
}
static void expect(gt_io_backend_t *p,unsigned cols,unsigned rows,unsigned x,unsigned y) {
  char text[256]={0};size_t used=0;struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
  while(!strchr(text,'\n')) {
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);
    int n=p->ops->read(p,(uint8_t *)text+used,sizeof(text)-used-1,20);if(n==GT_TRANSPORT_READ_TIMEOUT)continue;CHECK(n>0);used+=(size_t)n;CHECK(used<sizeof(text)-1);
  }
  unsigned c,r,a,b;CHECK(sscanf(text,"SIZE %u %u %u %u",&c,&r,&a,&b)==4 && c==cols && r==rows && a==x && b==y);
}
static gt_pty_opts_t options(const char *exe,const char *const *args){return (gt_pty_opts_t){.executable=exe,.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24,.px_width=320,.px_height=200};}
static void flow(const char *exe) {
  const char *args[]={"child",NULL};gt_pty_opts_t o=options(exe,args);gt_io_backend_t *p=gt__pty_open(&o);CHECK(p);expect(p,80,24,320,200);
  CHECK(p->ops->resize(p,97,31,65535,65535)==GT_OK && p->ops->write(p,(const uint8_t *)"s",1)==GT_OK);expect(p,97,31,65535,65535);
  const uint32_t values[][2]={{65536,1},{1,65536},{UINT32_MAX,0},{0,UINT32_MAX}};
  for(unsigned i=0;i<4;i++) {
    CHECK(p->ops->resize(p,99,41,values[i][0],values[i][1])==GT_ERR_UNSUPPORTED);
    CHECK(p->ops->write(p,(const uint8_t *)"s",1)==GT_OK);expect(p,97,31,65535,65535);
  }
  CHECK(p->ops->resize(p,0,31,0,0)==GT_ERR_INVALID_ARG && p->ops->resize(p,97,0,0,0)==GT_ERR_INVALID_ARG);
  CHECK(p->ops->write(p,(const uint8_t *)"s",1)==GT_OK);expect(p,97,31,65535,65535);
  CHECK(p->ops->resize(p,1,1,0,0)==GT_OK && p->ops->write(p,(const uint8_t *)"s",1)==GT_OK);expect(p,1,1,0,0);p->ops->close(p);
}
static void defaults(const char *exe) {
  const char *args[]={"child",NULL};gt_pty_opts_t o=options(exe,args);o.cols=o.rows=0;o.px_width=0;o.px_height=65535;
  gt_io_backend_t *p=gt__pty_open(&o);CHECK(p);expect(p,80,24,0,65535);p->ops->close(p);
}
static void *deny(void *ctx,size_t n){(void)n;(*(unsigned *)ctx)++;return NULL;}
static void refund(void *ctx,void *p,size_t n){(void)ctx;(void)p;(void)n;CHECK(false);}
static void admission(const char *exe) {
  const char *args[]={"child",NULL};gt_pty_opts_t o=options(exe,args);unsigned calls=0;gt_allocator_t a={&calls,deny,refund};
  const uint32_t values[][2]={{65536,1},{1,65536},{UINT32_MAX,0},{0,UINT32_MAX}};
  for(unsigned i=0;i<4;i++) {
    o.px_width=values[i][0];o.px_height=values[i][1];CHECK(!gt__pty_open_with_allocator(&o,&a) && !calls);
    CHECK(!gt__pty_open(&o));
  }
}
int main(int argc,char **argv) {
  CHECK(argc==2 && argv[0][0]=='/');if(!strcmp(argv[1],"child")){child();return 0;}
  if(!strcmp(argv[1],"flow"))flow(argv[0]);else if(!strcmp(argv[1],"defaults"))defaults(argv[0]);else if(!strcmp(argv[1],"admission"))admission(argv[0]);else CHECK(false);
  printf("PASS native PTY dimensions %s\n",argv[1]);return 0;
}
