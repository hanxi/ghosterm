#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ghosterm.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
static uint64_t channel_now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static size_t channel_fds(void){DIR *d=opendir("/dev/fd");CHECK(d);size_t n=0;struct dirent *e;while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,".."))n++;CHECK(!closedir(d));return n;}
static gt_ext_channel_info_t channel_info(gt_ext_channel_t *c){gt_ext_channel_info_t i;CHECK(gt_ext_channel_info(c,&i));return i;}
static void channel_drop(gt_ext_channel_t **c){uint64_t end=channel_now()+2000;for(;;){gt_ext_close_t close=gt_ext_channel_close(*c,0);if(close==GT_EXT_RELEASED)break;CHECK(close==GT_EXT_DRAINING && channel_now()<end);}CHECK(gt_ext_channel_close(*c,0)==GT_EXT_RELEASED && gt_ext_channel_destroy(c)==GT_OK && !*c);}
static gt_ext_channel_t *channel_fd(int fd){gt_ext_channel_t *c=NULL;CHECK(gt_ext_channel_open_fd(fd,fd,&c)==GT_OK && c);return c;}
#ifdef GT_HAVE_LIBSSH2
static void channel_read_exact(gt_ext_channel_t *c,uint8_t *p,size_t n){size_t at=0;uint64_t end=channel_now()+10000;while(at<n){gt_ext_io_t io=gt_ext_channel_read(c,p+at,n-at,5);CHECK((io.state==GT_EXT_DATA || io.state==GT_EXT_WAIT) && channel_now()<end);if(io.state==GT_EXT_DATA)at+=io.count;}}
#endif
static void channel_write_exact(gt_ext_channel_t *c,const uint8_t *p,size_t n){size_t at=0;uint64_t end=channel_now()+10000;while(at<n || channel_info(c).buffered){if(at<n){gt_ext_io_t io=gt_ext_channel_write(c,p+at,n-at);CHECK(io.state==GT_EXT_DATA || io.state==GT_EXT_WAIT);at+=io.count;}CHECK(gt_ext_channel_step(c,5)==GT_EXT_READY && channel_now()<end);}}
static void channel_native(const char *mode,const char *root){
  int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
  if(!strcmp(mode,"disabled")){gt_ext_channel_t *c=(gt_ext_channel_t *)1;int flags=fcntl(pair[0],F_GETFL);CHECK(gt_ext_channel_open_fd(pair[0],pair[0],&c)==GT_ERR_UNSUPPORTED && !c && fcntl(pair[0],F_GETFL)==flags);CHECK(!close(pair[0]) && !close(pair[1]));return;}
  if(!strcmp(mode,"unsupported-ssh")){gt_ssh_opts_t opts={.host="route-hop-0.invalid",.port=22,.username="fixture"};gt_ssh_t *owned=gt_ssh_new(&opts);CHECK(owned);gt_ext_ssh_hop_t hop={.authentication={.family=4,.address={127,0,0,1},.timeout_ms=1000,.defer_credentials=true}};gt_ext_channel_t *c=NULL;CHECK(gt_ext_channel_open_ssh(&owned,&hop,1,"fixed",NULL,&c)==GT_ERR_UNSUPPORTED && !c && owned);gt_ssh_close(owned);CHECK(!close(pair[0]) && !close(pair[1]));return;}
  if(!strcmp(mode,"rejected-fd")){
    CHECK(!close(pair[0]) && !close(pair[1]));CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair));int flags=fcntl(pair[0],F_GETFL),fdflags=fcntl(pair[0],F_GETFD);gt_ext_channel_t *c=(gt_ext_channel_t *)1;CHECK(gt_ext_channel_open_fd(pair[0],pair[0],&c)==GT_ERR_INVALID_ARG && !c && flags==fcntl(pair[0],F_GETFL) && fdflags==fcntl(pair[0],F_GETFD));CHECK(!close(pair[0]) && !close(pair[1]));char path[4096];CHECK(snprintf(path,sizeof(path),"%s/ext-channel-XXXXXX",root)>0);int file=mkstemp(path);CHECK(file>=0);CHECK(gt_ext_channel_open_fd(file,file,&c)==GT_ERR_INVALID_ARG && !c && fcntl(file,F_GETFD)>=0);CHECK(!close(file) && !unlink(path));return;
  }
  if(!strcmp(mode,"capacity")){
    CHECK(!close(pair[0]) && !close(pair[1]));gt_ext_channel_t *all[GT_EXT_CHANNEL_LIMIT]={0};int peer[GT_EXT_CHANNEL_LIMIT];for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++){CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));all[i]=channel_fd(pair[0]);peer[i]=pair[1];}
    CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));int flags=fcntl(pair[0],F_GETFL);gt_ext_channel_t *extra=NULL;CHECK(gt_ext_channel_open_fd(pair[0],pair[0],&extra)==GT_ERR_IO && !extra && flags==fcntl(pair[0],F_GETFL));channel_drop(&all[0]);extra=channel_fd(pair[0]);channel_drop(&extra);CHECK(!close(pair[1]));for(size_t i=0;i<GT_EXT_CHANNEL_LIMIT;i++){if(all[i])channel_drop(&all[i]);CHECK(!close(peer[i]));}return;
  }
  int small=1024;CHECK(!setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));gt_ext_channel_t *c=channel_fd(pair[0]);CHECK(fcntl(pair[0],F_GETFD)&FD_CLOEXEC);CHECK(fcntl(pair[0],F_GETFL)&O_NONBLOCK);
  if(!strcmp(mode,"invalid")){
    gt_ext_channel_t *out=(gt_ext_channel_t *)1;CHECK(gt_ext_channel_open_fd(-1,-1,&out)==GT_ERR_INVALID_ARG && !out);CHECK(!gt_ext_channel_info(NULL,NULL));gt_ext_channel_info_t i;memset(&i,1,sizeof(i));CHECK(!gt_ext_channel_info(NULL,&i) && !i.challenge && !i.accepted);CHECK(gt_ext_channel_step(NULL,0)==GT_EXT_FAILED && gt_ext_channel_step(c,-1)==GT_EXT_FAILED);CHECK(gt_ext_channel_read(c,NULL,1,0).error==GT_ERR_INVALID_ARG && gt_ext_channel_read(c,&i,1,-1).error==GT_ERR_INVALID_ARG && gt_ext_channel_write(c,"",0).error==GT_ERR_INVALID_ARG);CHECK(gt_ext_channel_decide(c,0,1,GT_HOSTKEY_ACCEPT)==GT_ERR_UNSUPPORTED);CHECK(gt_ext_channel_destroy(&c)==GT_ERR_INVALID_ARG && c && gt_ext_channel_close(c,-1)==GT_EXT_CLOSE_ERROR);gt_ext_channel_stop(NULL);
  }else if(!strcmp(mode,"quantum")){uint8_t p[GT_EXT_IO_MAX*2]={0};gt_ext_io_t io=gt_ext_channel_write(c,p,sizeof(p));CHECK(io.state==GT_EXT_DATA && io.count==GT_EXT_IO_MAX && channel_info(c).accepted==GT_EXT_IO_MAX && channel_info(c).buffered==GT_EXT_IO_MAX && !channel_info(c).sent);
  }else if(!strcmp(mode,"eof")){
    CHECK(write(pair[1],"tail",4)==4 && !shutdown(pair[1],SHUT_WR));uint8_t p[4];for(size_t i=0;i<4;i++){gt_ext_io_t io=gt_ext_channel_read(c,p+i,1,20);CHECK(io.state==GT_EXT_DATA && io.count==1);}CHECK(!memcmp(p,"tail",4) && gt_ext_channel_read(c,p,4,20).state==GT_EXT_EOF);CHECK(gt_ext_channel_write(c,"x",1).state==GT_EXT_DATA && gt_ext_channel_step(c,20)==GT_EXT_READY);CHECK(read(pair[1],p,1)==1 && p[0]=='x');
  }else if(!strcmp(mode,"close-queued")){
    CHECK(gt_ext_channel_write(c,"never sent",10).count==10 && !channel_info(c).sent);CHECK(gt_ext_channel_close(c,0)==GT_EXT_RELEASED && channel_info(c).accepted==10 && !channel_info(c).sent && !channel_info(c).buffered);char p;CHECK(!read(pair[1],&p,1));CHECK(gt_ext_channel_write(c,"x",1).state==GT_EXT_IO_CLOSED && gt_ext_channel_read(c,&p,1,0).state==GT_EXT_IO_CLOSED);
  }else if(!strcmp(mode,"copy") || !strcmp(mode,"partial")){
    uint8_t expected[GT_EXT_IO_MAX],input[GT_EXT_IO_MAX],output[GT_EXT_IO_MAX];for(size_t i=0;i<sizeof(input);i++)input[i]=expected[i]=(uint8_t)(i%251);
    gt_ext_io_t io=gt_ext_channel_write(c,input,sizeof(input));CHECK(io.state==GT_EXT_DATA && io.count==sizeof(input) && !channel_info(c).sent && channel_info(c).buffered==sizeof(input));memset(input,0xff,sizeof(input));CHECK(gt_ext_channel_write(c,input,1).state==GT_EXT_WAIT);CHECK(gt_ext_channel_step(c,0)==GT_EXT_READY);CHECK(channel_info(c).buffered>0 && channel_info(c).buffered<sizeof(input));
    int flags=fcntl(pair[1],F_GETFL);CHECK(!fcntl(pair[1],F_SETFL,flags|O_NONBLOCK));size_t at=0;uint64_t end=channel_now()+1000;while(at<sizeof(output)){ssize_t n=read(pair[1],output+at,sizeof(output)-at);if(n<0)CHECK(errno==EAGAIN);else{CHECK(n>0);at+=(size_t)n;}CHECK(gt_ext_channel_step(c,0)==GT_EXT_READY && channel_now()<end);}CHECK(!memcmp(output,expected,sizeof(output)) && !channel_info(c).buffered && channel_info(c).sent==sizeof(input) && channel_info(c).accepted==sizeof(input));CHECK(gt_ext_channel_read(c,output,1,0).state==GT_EXT_WAIT);
  }else if(!strcmp(mode,"failure-stop")){CHECK(!close(pair[1]));pair[1]=-1;CHECK(gt_ext_channel_write(c,"x",1).state==GT_EXT_DATA && gt_ext_channel_step(c,10)==GT_EXT_FAILED && channel_info(c).error==GT_ERR_IO);gt_ext_channel_stop(c);CHECK(gt_ext_channel_step(c,0)==GT_EXT_FAILED && channel_info(c).phase==GT_EXT_FAILED && channel_info(c).error==GT_ERR_IO);char p;CHECK(gt_ext_channel_read(c,&p,1,0).error==GT_ERR_IO && gt_ext_channel_write(c,"x",1).error==GT_ERR_IO);
  }else if(!strcmp(mode,"stop")){CHECK(gt_ext_channel_write(c,"x",1).state==GT_EXT_DATA);gt_ext_channel_stop(c);gt_ext_channel_stop(c);CHECK(channel_info(c).phase==GT_EXT_STOPPED && gt_ext_channel_step(c,0)==GT_EXT_STOPPED && channel_info(c).buffered==1 && !channel_info(c).sent);char p;CHECK(gt_ext_channel_read(c,&p,1,0).state==GT_EXT_IO_CLOSED && gt_ext_channel_write(c,"x",1).state==GT_EXT_IO_CLOSED);
  }else CHECK(false);
  channel_drop(&c);if(pair[1]>=0)CHECK(!close(pair[1]));
}
static void channel_executor(void){
  CHECK(!isatty(0) && !isatty(1));gt_ext_channel_t *c=NULL;CHECK(gt_ext_channel_open_fd(0,1,&c)==GT_OK);channel_write_exact(c,(const uint8_t *)"ready",5);uint8_t p[GT_EXT_IO_MAX];
  for(;;){gt_ext_io_t io=gt_ext_channel_read(c,p,sizeof(p),20);if(io.state==GT_EXT_EOF)break;CHECK(io.state==GT_EXT_DATA || io.state==GT_EXT_WAIT);if(io.state==GT_EXT_DATA)channel_write_exact(c,p,io.count);}
  channel_drop(&c);
}
#ifdef GT_HAVE_LIBSSH2
extern uint16_t gt_ext_test_port;extern unsigned gt_ext_test_dials;
static char *channel_key;static size_t channel_key_size;
static char *channel_wrong_key;static uint64_t channel_fake_now;
static bool channel_fake_clock(void *ctx,uint64_t *out){(void)ctx;*out=channel_fake_now;return true;}
static gt_ssh_t *channel_material(char **argv,size_t i,bool secret){char identity[64];CHECK(snprintf(identity,sizeof(identity),"route-hop-%zu.invalid",i)>0);gt_ssh_opts_t opts={.host=identity,.port=(uint16_t)atoi(argv[6+i]),.username=argv[3],.known_hosts_path=argv[4]};gt_ssh_t *s=gt_ssh_new(&opts);CHECK(s);if(secret)CHECK(gt_ssh_auth_private_key(s,channel_key,NULL)==GT_OK);return s;}
static void channel_network(int argc,char **argv){
  size_t count=(size_t)atoi(argv[2]);CHECK(count>=2 && count<=3 && argc==(int)(6+count));gt_ext_test_port=(uint16_t)atoi(argv[6]);uint8_t prefix[4];CHECK(read(0,prefix,4)==4);channel_key_size=((size_t)prefix[0]<<24)|((size_t)prefix[1]<<16)|((size_t)prefix[2]<<8)|prefix[3];CHECK(channel_key_size && channel_key_size<8192);channel_key=calloc(1,channel_key_size+1);CHECK(channel_key);size_t used=0;while(used<channel_key_size){ssize_t n=read(0,channel_key+used,channel_key_size-used);CHECK(n>0);used+=(size_t)n;}
  bool retry=!strcmp(argv[1],"credentials-retry"),expiry=!strcmp(argv[1],"expired-before-supply");size_t wrong_size=0;
  if(retry){CHECK(read(0,prefix,4)==4);wrong_size=((size_t)prefix[0]<<24)|((size_t)prefix[1]<<16)|((size_t)prefix[2]<<8)|prefix[3];CHECK(wrong_size && wrong_size<8192);channel_wrong_key=calloc(1,wrong_size+1);CHECK(channel_wrong_key);used=0;while(used<wrong_size){ssize_t n=read(0,channel_wrong_key+used,wrong_size-used);CHECK(n>0);used+=(size_t)n;}}
  channel_fake_now=1000;gt_ssh_t *owned[GT_EXT_MAX_HOPS]={0};gt_ext_ssh_hop_t hops[GT_EXT_MAX_HOPS]={0};bool deferred=retry || expiry || !strcmp(argv[1],"credentials") || !strcmp(argv[1],"unknown-accept") || !strcmp(argv[1],"unknown-reject");for(size_t i=0;i<count;i++){owned[i]=channel_material(argv,i,!deferred);hops[i].authentication=(gt_ssh_auth_options_t){.family=i?0:4,.address={127,0,0,1},.timeout_ms=5000,.defer_credentials=deferred};if(i)hops[i].via_host="127.0.0.1";if(expiry)hops[i].authentication.clock=channel_fake_clock;}
  gt_ext_ssh_options_t opts={100,5000,262144};gt_ext_channel_t *c=NULL;CHECK(gt_ext_channel_open_ssh(owned,hops,count,argv[5],expiry?&opts:NULL,&c)==GT_OK && c);for(size_t i=0;i<count;i++)CHECK(!owned[i] && channel_info(c).decisions[i]==GT_HOSTKEY_UNKNOWN);uint64_t end=channel_now()+12000,last_challenge=0,bad_challenge=0;unsigned supplies=0,decisions=0;
  for(;;){gt_ext_phase_t phase=gt_ext_channel_step(c,5);gt_ext_channel_info_t info=channel_info(c);CHECK(channel_now()<end);
    if(info.challenge){CHECK(info.challenge>last_challenge);last_challenge=info.challenge;gt_ssh_auth_info_t a=info.authentication[info.hop];CHECK(gt_ext_channel_decide(c,info.hop+1,info.challenge,GT_HOSTKEY_ACCEPT)==GT_ERR_INVALID_ARG);
      if(a.phase==GT_SSH_AUTH_PHASE_HOSTKEY){CHECK(a.host==GT_SSH_AUTH_HOST_UNKNOWN && a.fingerprint[0]);CHECK(gt_ext_channel_decide(c,info.hop,info.challenge,GT_HOSTKEY_UNKNOWN)==GT_OK && channel_info(c).challenge==info.challenge);gt_hostkey_decision_t decision=!strcmp(argv[1],"unknown-reject")?GT_HOSTKEY_REJECT:GT_HOSTKEY_ACCEPT;CHECK(gt_ext_channel_decide(c,info.hop,info.challenge,decision)==(decision==GT_HOSTKEY_REJECT?GT_ERR_HOST_KEY:GT_OK));CHECK(gt_ext_channel_decide(c,info.hop,info.challenge,GT_HOSTKEY_ACCEPT)!=GT_OK);decisions++;}
      else{CHECK(a.phase==GT_SSH_AUTH_PHASE_CREDENTIALS && (a.host==GT_SSH_AUTH_HOST_KNOWN || (a.host==GT_SSH_AUTH_HOST_UNKNOWN && info.decisions[info.hop]==GT_HOSTKEY_ACCEPT)));gt_ssh_t *material=channel_material(argv,info.hop,true);CHECK(gt_ext_channel_supply(c,info.hop,info.challenge+1,&material)==GT_ERR_INVALID_ARG && material);
        if(expiry){channel_fake_now+=100;CHECK(gt_ext_channel_supply(c,info.hop,info.challenge,&material)==GT_ERR_IO && material);gt_ssh_close(material);break;}
        if(retry && !info.hop){if(!bad_challenge){gt_ssh_close(material);material=channel_material(argv,0,false);CHECK(gt_ssh_auth_private_key(material,channel_wrong_key,NULL)==GT_OK);bad_challenge=info.challenge;}else{CHECK(a.error==GT_ERR_AUTH && info.challenge>bad_challenge && gt_ext_channel_supply(c,0,bad_challenge,&material)==GT_ERR_INVALID_ARG && material);}}
        CHECK(gt_ext_channel_supply(c,info.hop,info.challenge,&material)==GT_OK && !material);supplies++;}
    }
    if(phase==GT_EXT_READY)break;if(phase==GT_EXT_FAILED || phase==GT_EXT_STOPPED)break;
  }
  gt_ext_channel_info_t info=channel_info(c);
  if(expiry)CHECK(info.phase==GT_EXT_FAILED && info.error==GT_ERR_IO && info.setup_expired && info.failure_stage==GT_EXT_STAGE_AUTH && !supplies);
  else if(!strcmp(argv[1],"wrong-pin"))CHECK(info.phase==GT_EXT_FAILED && info.error==GT_ERR_HOST_KEY && info.authentication[count-1].host==GT_SSH_AUTH_HOST_MISMATCH && !info.challenge);
  else if(!strcmp(argv[1],"unknown-reject"))CHECK(info.phase==GT_EXT_FAILED && info.error==GT_ERR_HOST_KEY && decisions==1);
  else if(!strcmp(argv[1],"forward-deny"))CHECK(info.phase==GT_EXT_FAILED && info.failure_stage==GT_EXT_STAGE_FORWARD && info.error==GT_ERR_IO);
  else{
    CHECK(info.phase==GT_EXT_READY && !info.challenge && info.hops==count);if(deferred)CHECK(supplies==count+(retry?1u:0u));if(!strcmp(argv[1],"unknown-accept"))CHECK(decisions==1);uint8_t ready[5];channel_read_exact(c,ready,5);CHECK(!memcmp(ready,"ready",5));
    uint8_t p[GT_EXT_IO_MAX],copy[GT_EXT_IO_MAX];for(size_t at=0;at<262144;at+=sizeof(p)){for(size_t i=0;i<sizeof(p);i++)p[i]=(uint8_t)((at+i)%251);channel_write_exact(c,p,sizeof(p));channel_read_exact(c,copy,sizeof(copy));CHECK(!memcmp(copy,p,sizeof(p)));}info=channel_info(c);CHECK(info.accepted==262144 && info.sent==262144 && info.received==262149 && !info.exit_status_known);
  }
  CHECK(gt_ext_test_dials==1);channel_drop(&c);memset(channel_key,0,channel_key_size);free(channel_key);if(channel_wrong_key){memset(channel_wrong_key,0,wrong_size);free(channel_wrong_key);}printf("PASS ext_channel_%s_%zu\n",argv[1],count);
}
#endif
int main(int argc,char **argv){CHECK(argc>=2);if(!strcmp(argv[1],"--executor")){channel_executor();return 0;}
#ifdef GT_HAVE_LIBSSH2
  if(argc>3){channel_network(argc,argv);return 0;}
#endif
  CHECK(argc==3);size_t before=channel_fds();channel_native(argv[1],argv[2]);CHECK(channel_fds()==before);printf("PASS ext_channel_%s\n",argv[1]);return 0;
}
