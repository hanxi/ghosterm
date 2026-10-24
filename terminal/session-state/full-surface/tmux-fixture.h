#include <fcntl.h>
static char tmux_socket[4096];
static const char *tmux_program;
static int tmux_command(const char *socket_path,const char *command) {
  pid_t pid=fork();
  if(pid==0) {
    int nullfd=open("/dev/null",O_WRONLY);
    if(nullfd>=0) {dup2(nullfd,STDOUT_FILENO);dup2(nullfd,STDERR_FILENO);close(nullfd);}
    execl(tmux_program,"tmux","-S",socket_path,command,(char*)NULL);_exit(127);
  }
  if(pid<0)return 127;
  int status;pid_t waited;
  do {waited=waitpid(pid,&status,0);}while(waited<0 && errno==EINTR);
  return waited==pid && WIFEXITED(status)?WEXITSTATUS(status):127;
}
static void cleanup_tmux(void) {
  if(!tmux_socket[0])return;
  (void)tmux_command(tmux_socket,"kill-server");
  tmux_socket[0]=0;
}
static void save_tmux_fixture(const char *dir) {
  tmux_program=getenv("GHOSTERM_P0_TMUX");REQUIRE(tmux_program!=NULL && access(tmux_program,X_OK)==0);
  REQUIRE(snprintf(tmux_socket,sizeof tmux_socket,"%s/tmux.sock",dir)<100);
  REQUIRE(atexit(cleanup_tmux)==0);
  int fd;struct winsize size={.ws_col=80,.ws_row=24};
  pid_t pid=forkpty(&fd,NULL,NULL,&size);REQUIRE(pid>=0);
  if(pid==0) {
    alarm(15);REQUIRE(setenv("TERM","xterm-256color",1)==0);REQUIRE(setenv("LC_ALL","C",1)==0);
    execl(tmux_program,"tmux","-S",tmux_socket,"-f","/dev/null","new-session","-s","ghosterm-p0","/bin/cat",(char*)NULL);_exit(127);
  }
  tui_pid=pid;REQUIRE(atexit(cleanup_tui)==0);
  GhosttyTerminal authority=new_terminal();Replies replies={0};enable_replies(authority,&replies);
  Bytes startup=pump_tui(fd,authority,&replies);free(startup.data);
  Bytes initial=capture(authority);save_bytes(dir,"tmux.initial",initial.data,initial.len);
  const char edit[]="GHOSTERM_TMUX_REAL\r";
  fd_write_all(fd,(const uint8_t*)edit,sizeof edit-1);
  Bytes delta=pump_tui(fd,authority,&replies);save_bytes(dir,"tmux.delta",delta.data,delta.len);
  GhosttyFormatterTerminalOptions opts={.size=sizeof opts,.emit=GHOSTTY_FORMATTER_FORMAT_PLAIN,.trim=true};GhosttyFormatter formatter=NULL;
  REQUIRE(ghostty_formatter_terminal_new(NULL,&formatter,authority,opts)==GHOSTTY_SUCCESS);
  uint8_t text[8192]={0};size_t written=0;
  REQUIRE(ghostty_formatter_format_buf(formatter,text,sizeof text-1,&written)==GHOSTTY_SUCCESS);
  REQUIRE(written<sizeof text);text[written]=0;REQUIRE(strstr((char*)text,"GHOSTERM_TMUX_REAL")!=NULL);ghostty_formatter_free(formatter);
  Bytes final=capture(authority);save_bytes(dir,"tmux.final",final.data,final.len);
  GhosttyTerminalModeConfig mode={.mode=GHOSTTY_MODE_BRACKETED_PASTE};
  REQUIRE(ghostty_terminal_get(authority,GHOSTTY_TERMINAL_DATA_MODE,&mode)==GHOSTTY_SUCCESS);
  REQUIRE(mode.value);
  fd_write_all(fd,(const uint8_t*)"\x02" "d",2);
  double deadline=tui_now()+5;int status=0;pid_t waited=0;
  while(tui_now()<deadline) {
    waited=waitpid(pid,&status,WNOHANG);if(waited==pid)break;
    REQUIRE(waited==0 || (waited<0 && errno==EINTR));
    struct pollfd pfd={.fd=fd,.events=POLLIN};int ready=poll(&pfd,1,50);
    if(ready>0) {uint8_t buf[4096];(void)read(fd,buf,sizeof buf);}
  }
  REQUIRE(waited==pid);tui_pid=-1;REQUIRE(WIFEXITED(status) && WEXITSTATUS(status)==0);
  char saved_socket[sizeof tmux_socket];memcpy(saved_socket,tmux_socket,sizeof tmux_socket);
  close(fd);cleanup_tmux();
  /* A dead server may leave its socket file behind. Query the actual server. */
  REQUIRE(tmux_command(saved_socket,"has-session")==1);
  free(initial.data);free(delta.data);free(final.data);ghostty_terminal_free(authority);
  puts("PASS full-surface/real tmux fixture captured, detached, and server cleaned");
}
