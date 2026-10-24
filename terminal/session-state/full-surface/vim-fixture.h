#include <time.h>
static double tui_now(void) {
  struct timespec ts; REQUIRE(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
  return (double)ts.tv_sec+(double)ts.tv_nsec/1000000000.0;
}
static Bytes pump_tui(int fd,GhosttyTerminal authority,Replies *replies) {
  Bytes bytes={0};
  double deadline=tui_now()+5, quiet=tui_now()+0.3;
  bool any=false;
  while(tui_now()<deadline) {
    struct pollfd pfd={.fd=fd,.events=POLLIN};
    int ready=poll(&pfd,1,50);
    if(ready<0 && errno==EINTR)continue;
    REQUIRE(ready>=0);
    if(ready==0) { if(any && tui_now()>=quiet)return bytes; continue; }
    uint8_t buf[4096]; ssize_t n=read(fd,buf,sizeof buf);
    if(n<0 && errno==EINTR)continue;
    REQUIRE(n>0 && (size_t)n<=1024*1024-bytes.len);
    uint8_t *grown=realloc(bytes.data,bytes.len+(size_t)n);REQUIRE(grown!=NULL);bytes.data=grown;
    memcpy(bytes.data+bytes.len,buf,(size_t)n);bytes.len+=(size_t)n;
    ghostty_terminal_vt_write(authority,buf,(size_t)n);
    bool failed=false;
    REQUIRE(ghostty_terminal_get(authority,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed)==GHOSTTY_SUCCESS && !failed);
    if(replies->len) {fd_write_all(fd,replies->data,replies->len);*replies=(Replies){0};}
    any=true;quiet=tui_now()+0.3;
  }
  REQUIRE(false);return bytes;
}
static void save_vim_fixture(const char *dir) {
  int fd; struct winsize size={.ws_col=80,.ws_row=24};
  char file[4096]; REQUIRE(snprintf(file,sizeof file,"%s/vim-buffer",dir)<(int)sizeof file);
  pid_t pid=forkpty(&fd,NULL,NULL,&size);REQUIRE(pid>=0);
  if(pid==0) {
    alarm(15);
    REQUIRE(setenv("TERM","xterm-256color",1)==0);
    REQUIRE(setenv("LANG","C",1)==0);
    REQUIRE(setenv("LC_ALL","C",1)==0);
    execl("/usr/bin/vim","vim","-u","NONE","-U","NONE","-i","NONE","-n","-N","--noplugin","-T","xterm-256color","--",file,(char*)NULL);
    _exit(127);
  }
  tui_pid=pid;REQUIRE(atexit(cleanup_tui)==0);
  GhosttyTerminal authority=new_terminal();Replies replies={0};enable_replies(authority,&replies);
  Bytes startup=pump_tui(fd,authority,&replies);free(startup.data);
  Bytes initial=capture(authority);save_bytes(dir,"vim.initial",initial.data,initial.len);
  const char edit[]="iGHOSTERM_VIM_REAL\x1b";
  fd_write_all(fd,(const uint8_t*)edit,sizeof edit-1);
  Bytes delta=pump_tui(fd,authority,&replies);save_bytes(dir,"vim.delta",delta.data,delta.len);
  GhosttyFormatterTerminalOptions opts={.size=sizeof opts,.emit=GHOSTTY_FORMATTER_FORMAT_PLAIN,.trim=true};
  GhosttyFormatter formatter=NULL;
  REQUIRE(ghostty_formatter_terminal_new(NULL,&formatter,authority,opts)==GHOSTTY_SUCCESS);
  uint8_t text[8192]={0};size_t written=0;
  REQUIRE(ghostty_formatter_format_buf(formatter,text,sizeof text-1,&written)==GHOSTTY_SUCCESS);
  REQUIRE(written<sizeof text);text[written]=0;REQUIRE(strstr((char*)text,"GHOSTERM_VIM_REAL")!=NULL);
  ghostty_formatter_free(formatter);
  Bytes final=capture(authority);save_bytes(dir,"vim.final",final.data,final.len);
  fd_write_all(fd,(const uint8_t*)":qa!\r",5);
  double deadline=tui_now()+5;int status=0;pid_t waited=0;
  while(tui_now()<deadline) {
    waited=waitpid(pid,&status,WNOHANG);
    if(waited==pid)break;
    REQUIRE(waited==0 || (waited<0 && errno==EINTR));
    struct pollfd pfd={.fd=fd,.events=POLLIN};int ready=poll(&pfd,1,50);
    if(ready>0) {uint8_t buf[4096];(void)read(fd,buf,sizeof buf);}
  }
  REQUIRE(waited==pid);tui_pid=-1;REQUIRE(WIFEXITED(status) && WEXITSTATUS(status)==0);
  close(fd);free(initial.data);free(delta.data);free(final.data);ghostty_terminal_free(authority);
  puts("PASS full-surface/real vim edit fixture captured and child exited cleanly");
}
