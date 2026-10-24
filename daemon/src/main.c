#define _POSIX_C_SOURCE 200809L
#include "host.h"
#include "memory.h"
#include "vt-state.h"
#include "vt-snapshot.h"
#include "session-contract.h"
#include "startup-wire.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define MIB (1024u*1024u)
_Static_assert(ATOMIC_BOOL_LOCK_FREE==2 && ATOMIC_INT_LOCK_FREE==2,"Signal state requires lock-free atomics");
static atomic_bool stopping;
static atomic_int signal_write=ATOMIC_VAR_INIT(-1);
static bool cancelled(void *context){(void)context;return atomic_load_explicit(&stopping,memory_order_relaxed);}
static bool detached_context(void) {
  if(getsid(0)!=getpid() || getpgrp()!=getpid() || gt__unix_peer_check(3,geteuid())!=GT_ENDPOINT_OK)return false;
  int null=open("/dev/null",O_RDONLY|O_CLOEXEC);if(null<0)return false;
  struct stat expected;bool okay=!fstat(null,&expected);
  for(int i=0;i<3 && okay;i++){struct stat actual;okay=!fstat(i,&actual) && S_ISCHR(actual.st_mode) && actual.st_rdev==expected.st_rdev;}
  close(null);return okay && !fcntl(3,F_SETFD,FD_CLOEXEC) && !chdir("/");
}
static void stop(int signal) {
  (void)signal;int saved=errno;atomic_store_explicit(&stopping,true,memory_order_relaxed);
  int fd=atomic_load_explicit(&signal_write,memory_order_relaxed);
  if(fd>=0){uint8_t b=1;ssize_t notified=write(fd,&b,1);(void)notified;}errno=saved;
}
static bool signals(int fds[2]) {
  if(pipe(fds))return false;
  for(unsigned i=0;i<2;i++) {
    int flags=fcntl(fds[i],F_GETFL);
    if(flags<0 || fcntl(fds[i],F_SETFL,flags|O_NONBLOCK) || fcntl(fds[i],F_SETFD,FD_CLOEXEC))return false;
  }
  atomic_store(&signal_write,fds[1]);struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
  return !sigaction(SIGTERM,&action,NULL) && !sigaction(SIGINT,&action,NULL);
}
static gt_vt_config_t vt_config(void) {
  return (gt_vt_config_t){.budget_bytes=512*MIB,.state_bytes=32*MIB,.frozen_bytes=32*MIB,
    .encoder_bytes=32*MIB,.checkpoint_bytes=8*MIB,.continuation_bytes=MIB,
    .history_bytes=4*MIB,.history_lines=10000,.image_bytes=4*MIB,.cell_width=10,.cell_height=20,
    .light={{255,255,255},{0,0,0}},.dark={{0x12,0x12,0x12},{255,255,255}},.initial_dark=true};
}
int main(int argc,char **argv) {
  if(argc==2 && !strcmp(argv[1],"--version")){puts("ghosterm-sessiond GTSD/1 INLINE/1 " GT_SESSION_CONTRACT_HEX);return 0;}
  if(argc==2 && !strcmp(argv[1],"--help")){puts("Usage: ghosterm-sessiond serve --runtime-dir ABSOLUTE_DIRECTORY\nRuns the current candidate host in the foreground; SIGINT/SIGTERM stops sessions.");return 0;}
  bool detached=argc==4 && !strcmp(argv[1],"serve-detached");
  if(argc!=4 || (!detached && strcmp(argv[1],"serve")) || strcmp(argv[2],"--runtime-dir")) {
    fputs("Usage: ghosterm-sessiond serve --runtime-dir ABSOLUTE_DIRECTORY\n",stderr);return 2;
  }
  if(detached && !detached_context())return 2;
  int wake[2]={-1,-1};if(!signals(wake)){fputs("sessiond: signal setup failed\n",stderr);return 1;}
  gt_daemon_memory_t memory;gt__daemon_memory_init(&memory,64*MIB);gt_allocator_t domain=gt__daemon_memory_allocator(&memory);
  gt_vt_factory_t *factory=NULL;gt_vt_config_t vt=vt_config();gt_session_host_t *host=NULL;
  gt_status_t error=gt__vt_factory_open(&vt,&domain,&factory);
  gt_session_host_config_t config={.directory=argv[3],.sessions=32,.connections=64,
    .snapshot_limit=8*MIB,.snapshot_format=GT_VT_SNAPSHOT_FORMAT};
  long page=sysconf(_SC_PAGESIZE);
  if(error==GT_OK && page<=0)error=GT_ERR_IO;
  if(error==GT_OK) {
    config.threads=(gt_thread_options_t){2*MIB,(size_t)page};config.state=gt__vt_factory_state(factory);config.encoder=gt__vt_snapshot_encoder(factory);
    error=gt__protocol_identity_new(gt_session_contract,0,NULL,&config.identity);
    if(error==GT_OK)error=gt__session_host_open(&config,&domain,&host);
  }
  /* No service round/CREATE until the private parent has verified our identity
   * and we have confirmed its ACK. Failed startup only shuts an unused host. */
  if(detached && !gt__startup_serve(3,error,&config.identity,5000,NULL,cancelled))error=GT_ERR_CANCELLED;
  while(error==GT_OK && !atomic_load_explicit(&stopping,memory_order_relaxed))error=gt__session_host_poll(host,wake[0],10);
  while(host) {
    gt_runtime_rc_t closed=gt__session_host_close(&host);
    if(closed==GT_RUNTIME_OK)break;
    if(closed!=GT_RUNTIME_BUSY){error=GT_ERR_IO;break;}
    (void)poll(NULL,0,1);
  }
  if(factory && !host && gt__vt_factory_close(factory)!=GT_RUNTIME_OK)error=GT_ERR_IO;
  if(atomic_load(&memory.used))error=GT_ERR_IO;
  /* Handlers remain installed until exit; block them before closing wake fds. */
  sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGTERM);sigaddset(&blocked,SIGINT);(void)pthread_sigmask(SIG_BLOCK,&blocked,NULL);
  atomic_store(&signal_write,-1);close(wake[0]);close(wake[1]);
  if(error!=GT_OK){fprintf(stderr,"sessiond: host failed (%d)\n",(int)error);return 1;}
  return 0;
}
