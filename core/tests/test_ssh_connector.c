#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ssh/connector.h"
#include "internal.h"
#include "base/secure-random.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef GT_CONNECT_ERASE_TEST
static unsigned erased;
void gt_connect_test_erase(void *p,size_t n) {
  static const char *labels[]={"fixture-password-A","fixture-password-B","fixture-key-A","fixture-key-B","fixture-phrase-A","fixture-phrase-B"};
  for(unsigned i=0;i<6;i++)if(n==strlen(labels[i]) && !memcmp(p,labels[i],n))erased|=1u<<i;
  gt__secure_erase(p,n);
}
#endif
static gt_ssh_connector_t *active;static gt_ssh_t *owned;
static void cleanup(void){gt_ssh_connector_t *c=active;gt_ssh_t *s=owned;active=NULL;owned=NULL;gt__ssh_connector_close(c);gt_ssh_close(s);}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);cleanup();exit(1);}} while(0)
typedef union {max_align_t align;size_t n;} block;
typedef struct {uint64_t now;bool clock_fail;size_t live;bool refuse;} fixture;
static void *admit(void *ctx,size_t n){fixture *f=ctx;if(f->refuse)return NULL;block *p=malloc(sizeof(*p)+n);CHECK(p);p->n=n;f->live++;return p+1;}
static void refund(void *ctx,void *p,size_t n){fixture *f=ctx;block *b=(block *)p-1;CHECK(f->live && b->n==n);for(size_t i=0;i<n;i++)CHECK(!((uint8_t *)p)[i]);f->live--;free(b);}
static bool fake_clock(void *ctx,uint64_t *out){fixture *f=ctx;*out=f->now;return !f->clock_fail;}
static bool real_clock(void *ctx,uint64_t *out){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;*out=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;return true;}
static uint64_t now(void){uint64_t n;CHECK(real_clock(NULL,&n));return n;}
/* 量一遍「real-deadline 那条轮询」在**当前负载下**的真实耗时（30 次 1ms sleep，
 * 与被测循环一模一样的步长），用来给上界做标定——共享 runner 会把每次唤醒
 * 整倍往后挤，固定 500ms 预算测到的是调度抖动而不是「到期没人管」。 */
static uint64_t measure_step_cost(void){
  struct timespec t={0,1000000};uint64_t a=now();
  for(int i=0;i<30;i++)nanosleep(&t,NULL);
  return now()-a;
}
static gt_ssh_connector_info_t info(void){gt_ssh_connector_info_t i;CHECK(gt__ssh_connector_info(active,&i));return i;}
static gt_hostkey_decision_t forbidden_decider(void *ctx,const char *h,uint16_t port,const char *type,const char *fingerprint){(void)ctx;(void)h;(void)port;(void)type;(void)fingerprint;CHECK(false);return GT_HOSTKEY_UNKNOWN;}
static void setup(uint16_t port,const char *known,const char *key) {
  gt_ssh_opts_t options={.host="connector-identity.invalid",.port=port,.username=getenv("GT_CONNECT_TEST_USER")?getenv("GT_CONNECT_TEST_USER"):"fixture",.known_hosts_path=known,.connect_timeout_ms=1000};
  owned=gt_ssh_new(&options);CHECK(owned && gt_ssh_set_hostkey_policy(owned,forbidden_decider,NULL)==GT_OK);
  CHECK(key?gt_ssh_auth_private_key(owned,key,NULL)==GT_OK:gt_ssh_auth_password(owned,"connector-fixture-password")==GT_OK);
}
static gt_ssh_connector_config_t config(fixture *f){return (gt_ssh_connector_config_t){.family=4,.address={127,0,0,1},.timeout_ms=100,.clock_context=f,.clock=fake_clock};}
static int listener(uint16_t *port) {
  int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(UINT32_C(0x7f000001))};CHECK(!bind(fd,(struct sockaddr *)&a,sizeof(a)) && !listen(fd,4));socklen_t n=sizeof(a);CHECK(!getsockname(fd,(struct sockaddr *)&a,&n));*port=ntohs(a.sin_port);CHECK(!fcntl(fd,F_SETFL,O_NONBLOCK));return fd;
}
static void unit(const char *mode) {
  fixture f={0};gt_allocator_t a={&f,admit,refund};uint16_t port;int server=listener(&port);setup(port,"/dev/null",NULL);gt_ssh_connector_config_t c=config(&f);
  if(!strcmp(mode,"allocation")) {f.refuse=true;CHECK(gt__ssh_connector_open(&owned,&c,&a,&active)==GT_ERR_IO && !active && owned);cleanup();CHECK(!f.live);close(server);return;}
  CHECK(gt__ssh_connector_open(&owned,&c,&a,&active)==GT_OK && !owned);int accepted=-1;
  for(unsigned i=0;i<100;i++){(void)gt__ssh_connector_step(active);if(accepted<0)accepted=accept(server,NULL,NULL);if(info().phase==GT_SSH_CONNECT_NEGOTIATING && accepted>=0)break;CHECK(info().phase!=GT_SSH_CONNECT_FAILED);}
  CHECK(accepted>=0 && info().phase==GT_SSH_CONNECT_NEGOTIATING);
  int client=-1;for(int fd=3;fd<256;fd++){struct sockaddr_in peer;socklen_t n=sizeof(peer);if(!getpeername(fd,(struct sockaddr *)&peer,&n) && n==sizeof(peer) && peer.sin_family==AF_INET && ntohs(peer.sin_port)==port){client=fd;break;}}
  CHECK(client>=0 && (fcntl(client,F_GETFL)&O_NONBLOCK) && (fcntl(client,F_GETFD)&FD_CLOEXEC));
  if(!strcmp(mode,"deadline")){f.now=100;CHECK(gt__ssh_connector_step(active)==GT_SSH_CONNECT_FAILED && info().deadline && info().error==GT_ERR_IO);}
  else if(!strcmp(mode,"cancel")){uint64_t started=now();gt__ssh_connector_cancel(active);CHECK(now()-started<100 && info().phase==GT_SSH_CONNECT_CANCELLED && info().error==GT_ERR_CANCELLED);}
  else if(!strcmp(mode,"clock")){f.now=10;(void)gt__ssh_connector_step(active);f.now=9;CHECK(gt__ssh_connector_step(active)==GT_SSH_CONNECT_FAILED && info().error==GT_ERR_INTERNAL);}
  else{CHECK(!strcmp(mode,"real-deadline"));cleanup();setup(port,"/dev/null",NULL);c.clock=real_clock;c.timeout_ms=30;
    /* 时间戳必须在 open **之前**取。连接器记的是它自己启动那一刻，原先在 open
     * 返回之后才取，两者之间被 runner 挤掉 1ms 就量到 elapsed=29，而 `>=30` 的
     * 下界零余量（CI 实测 0.04 秒内即红；本机空载与 8 路忙循环下都是 30/31，
     * 所以不是忙循环问题，是取点问题）。取早了 elapsed 只会变大，代价是下界白捡
     * 一段 open_cost（实测 0~1ms）的松弛——那段松弛由下面的 open_cost 断言钉住，
     * 而到期算术本身由 fake-clock 的 ssh_connector_deadline 逐毫秒盯着。 */
    /* 标定必须在 started 之前：它自己就要跑 30 次 1ms，放在后面等于把标定算进
     * elapsed（实测 44ms），那条上界就再也不量轮询了。 */
    uint64_t calib=measure_step_cost(),budget=calib*4+60;if(budget<500)budget=500;
    uint64_t started=now();
    CHECK(gt__ssh_connector_open(&owned,&c,&a,&active)==GT_OK);
    uint64_t open_cost=now()-started;
    /* 连接器记的是**它自己**那一刻，所以「不早于 30ms」这条下界天然带着
     * open_cost 的松弛。把它钉在预算一半以内，松弛就不可能自己长大到把真实回归
     * 整段吞掉；超了就是环境不可用，宁可红。 */
    CHECK(open_cost<budget/2);
    while(gt__ssh_connector_step(active)!=GT_SSH_CONNECT_FAILED){CHECK(now()-started<budget);struct timespec t={0,1000000};nanosleep(&t,NULL);}
    fprintf(stderr,"real-deadline: elapsed=%llu（open %llu）/ 预算 %llu（轮询标定 %llu）\n",(unsigned long long)(now()-started),(unsigned long long)open_cost,(unsigned long long)budget,(unsigned long long)calib);
    CHECK(info().deadline && now()-started>=30 && now()-started<budget);}
  cleanup();CHECK(!f.live);close(accepted);close(server);
}
static void until(gt_ssh_connector_phase_t phase){uint64_t deadline=now()+5000;int previous=-1;for(;;){gt_ssh_connector_phase_t p=gt__ssh_connector_step(active);if(getenv("GT_CONNECT_TRACE") && (int)p!=previous){fprintf(stderr,"connector phase=%d target=%d\n",p,phase);previous=(int)p;}if(p==phase)return;CHECK(p!=GT_SSH_CONNECT_FAILED && p!=GT_SSH_CONNECT_CANCELLED && p!=GT_SSH_CONNECT_READY && p!=GT_SSH_CONNECT_HOSTKEY && now()<deadline);struct timespec t={0,1000000};nanosleep(&t,NULL);}}
static void open_real(uint16_t port,const char *known,const char *key,const char *host){setup(port,known,key);gt_ssh_connector_config_t c={.family=4,.timeout_ms=3000,.clock=real_clock};CHECK(inet_pton(AF_INET,host,c.address)==1 && gt__ssh_connector_open(&owned,&c,NULL,&active)==GT_OK && !owned);}
static void real_flow(int argc,char **argv) {
  CHECK(argc==6 && !setenv("GT_CONNECT_TEST_USER",argv[3],1));FILE *f=fopen(argv[4],"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));char *key=calloc(1,(size_t)n+1);CHECK(key && fread(key,1,(size_t)n,f)==(size_t)n);fclose(f);
  char directory[1024],known[1100];CHECK(snprintf(directory,sizeof(directory),"%s/connector-XXXXXX",argv[5])>0 && mkdtemp(directory));CHECK(snprintf(known,sizeof(known),"%s/known_hosts",directory)>0);uint16_t port=(uint16_t)atoi(argv[2]);
  open_real(port,known,key,argv[1]);until(GT_SSH_CONNECT_HOSTKEY);CHECK(info().host==GT_SSH_HOST_UNKNOWN && !strncmp(info().fingerprint,"SHA256:",7) && info().key_type[0]);if(getenv("GT_TEST_HOSTKEY_SHA256"))CHECK(!strcmp(info().fingerprint,getenv("GT_TEST_HOSTKEY_SHA256")));
  CHECK(gt__ssh_connector_decide(active,GT_HOSTKEY_UNKNOWN)==GT_OK && info().phase==GT_SSH_CONNECT_HOSTKEY && !info().auth_methods);
  CHECK(gt__ssh_connector_decide(active,GT_HOSTKEY_ACCEPT)==GT_OK);until(GT_SSH_CONNECT_READY);CHECK(info().auth_methods&GT_SSH_AUTH_PUBLICKEY);CHECK(gt__ssh_connector_take(active,&owned) && owned);gt_ssh_t *again=(void *)1;CHECK(!gt__ssh_connector_take(active,&again) && !again);
  void *session;int fd;CHECK(gt__ssh_borrow(owned,&session,&fd)==GT_OK && (fcntl(fd,F_GETFL)&O_NONBLOCK) && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
  if(getenv("GT_CONNECT_TRACE"))fprintf(stderr,"connector exec begin\n");
  gt_status_t status;gt_ssh_exec_t *command=gt_ssh_exec(owned,"printf connector-ok",&status);CHECK(command && status==GT_OK);char output[32]={0};size_t used=0;uint64_t deadline=now()+5000;
  for(;;){bool stderr_stream=false;int rc=gt_ssh_exec_read(command,(uint8_t *)output+used,sizeof(output)-used-1,50,&stderr_stream);if(rc==GT_EXEC_READ_EOF)break;CHECK(rc!=GT_EXEC_READ_ERROR && !stderr_stream && now()<deadline);if(rc>0)used+=(size_t)rc;}
  CHECK(!strcmp(output,"connector-ok"));if(getenv("GT_CONNECT_TRACE"))fprintf(stderr,"connector exec close\n");gt_ssh_exec_close(command);cleanup();
  open_real(port,known,key,argv[1]);until(GT_SSH_CONNECT_READY);CHECK(info().host==GT_SSH_HOST_KNOWN);cleanup();
  /* Wrong OpenSSH key passphrase must permit replacement on this connection.
   * libssh2/OpenSSL reports FILE for this in-memory parse failure. */
  const char *encrypted_path=getenv("GT_CONNECT_ENCRYPTED_KEY"),*phrase=getenv("GT_CONNECT_PASSPHRASE");
  CHECK(encrypted_path && phrase);
  f=fopen(encrypted_path,"rb");CHECK(f && !fseek(f,0,SEEK_END));long encrypted_size=ftell(f);
  CHECK(encrypted_size>0 && !fseek(f,0,SEEK_SET));char *encrypted=calloc(1,(size_t)encrypted_size+1);
  CHECK(encrypted && fread(encrypted,1,(size_t)encrypted_size,f)==(size_t)encrypted_size && !fclose(f));
  setup(port,known,key);CHECK(gt_ssh_auth_private_key(owned,encrypted,"wrong-fixture-passphrase")==GT_OK);
  gt_ssh_connector_config_t retry={.family=4,.timeout_ms=10000,.clock=real_clock,.defer_credentials=true};
  CHECK(inet_pton(AF_INET,argv[1],retry.address)==1 && gt__ssh_connector_open(&owned,&retry,NULL,&active)==GT_OK);
  until(GT_SSH_CONNECT_CREDENTIALS);CHECK(info().error==GT_ERR_AUTH && info().host==GT_SSH_HOST_KNOWN);
  setup(port,known,key);CHECK(gt_ssh_auth_private_key(owned,encrypted,phrase)==GT_OK);
  CHECK(gt__ssh_connector_supply(active,&owned)==GT_OK && !owned);
  until(GT_SSH_CONNECT_READY);CHECK(gt__ssh_connector_take(active,&owned) && owned);cleanup();
  gt__secure_erase(encrypted,(size_t)encrypted_size);free(encrypted);
  CHECK(!unlink(known));
  fixture expired={0};setup(port,known,key);gt_ssh_connector_config_t expires=config(&expired);CHECK(inet_pton(AF_INET,argv[1],expires.address)==1 && gt__ssh_connector_open(&owned,&expires,NULL,&active)==GT_OK);until(GT_SSH_CONNECT_HOSTKEY);expired.now=100;
  CHECK(gt__ssh_connector_decide(active,GT_HOSTKEY_UNKNOWN)==GT_ERR_IO && info().deadline && info().phase==GT_SSH_CONNECT_FAILED);cleanup();
  open_real(port,known,key,argv[1]);until(GT_SSH_CONNECT_HOSTKEY);CHECK(gt__ssh_connector_decide(active,GT_HOSTKEY_REJECT)==GT_ERR_HOST_KEY && info().phase==GT_SSH_CONNECT_FAILED);cleanup();
  open_real(port,known,key,argv[1]);until(GT_SSH_CONNECT_HOSTKEY);uint64_t started=now();gt__ssh_connector_cancel(active);CHECK(now()-started<100 && info().phase==GT_SSH_CONNECT_CANCELLED);cleanup();
  open_real(port,known,key,argv[1]);until(GT_SSH_CONNECT_HOSTKEY);CHECK(gt__ssh_connector_decide(active,GT_HOSTKEY_ACCEPT)==GT_OK);cleanup();
  open_real(port,known,NULL,argv[1]);until(GT_SSH_CONNECT_FAILED);CHECK(info().error==GT_ERR_AUTH);cleanup();
  f=fopen(known,"wb");CHECK(f && fprintf(f,"[connector-identity.invalid]:%u ssh-ed25519 AAAAAAAAAAAAAAAAAAAAAAAA\\n",port)>0 && !fclose(f));
  open_real(port,known,key,argv[1]);until(GT_SSH_CONNECT_FAILED);CHECK(info().error==GT_ERR_HOST_KEY && info().host==GT_SSH_HOST_MISMATCH);cleanup();
  f=fopen(known,"wb");CHECK(f && fprintf(f,"[%s]:%u ssh-ed25519 AAAAAAAAAAAAAAAAAAAAAAAA\n",argv[1],port)>0 && !fclose(f));
  gt_ssh_opts_t legacy={.host=argv[1],.port=port,.username=argv[3],.known_hosts_path=known,.connect_timeout_ms=1000};owned=gt_ssh_new(&legacy);CHECK(owned && gt_ssh_auth_private_key(owned,key,NULL)==GT_OK);
  CHECK(gt_ssh_handshake(owned)==GT_ERR_HOST_KEY);CHECK(gt_ssh_handshake(owned)!=GT_OK && gt_ssh_authenticate(owned)==GT_ERR_PROTOCOL);cleanup();
  memset(key,0,(size_t)n);free(key);CHECK(!unlink(known));
  /* Stable lock and required backup belong to this isolated fixture. */
  DIR *files=opendir(directory);CHECK(files);struct dirent *entry;
  while((entry=readdir(files))) {
    if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
    CHECK(!strncmp(entry->d_name,".ghosterm-",10));
    char extra[1400];CHECK(snprintf(extra,sizeof extra,"%s/%s",directory,entry->d_name)<(int)sizeof extra);
    CHECK(!unlink(extra));
  }
  CHECK(!closedir(files) && !rmdir(directory));
}
int main(int argc,char **argv){if(argc==6){real_flow(argc,argv);puts("PASS nonblocking SSH real authentication");return 0;}CHECK(argc==2);
  if(!strcmp(argv[1],"unsupported")){CHECK(gt__ssh_connector_open(NULL,NULL,NULL,&active)==GT_ERR_UNSUPPORTED && !active);}
#ifdef GT_CONNECT_ERASE_TEST
  else if(!strcmp(argv[1],"secrets")) {
    gt_ssh_opts_t o={.host="fixture",.username="fixture",.known_hosts_path="/dev/null"};owned=gt_ssh_new(&o);CHECK(owned);
    CHECK(gt_ssh_auth_password(owned,"fixture-password-A")==GT_OK && gt_ssh_auth_password(owned,"fixture-password-B")==GT_OK);
    CHECK(gt_ssh_auth_private_key(owned,"fixture-key-A","fixture-phrase-A")==GT_OK && gt_ssh_auth_private_key(owned,"fixture-key-B","fixture-phrase-B")==GT_OK);cleanup();CHECK(erased==63);
  }
#endif
  else {unit(argv[1]);}
  cleanup();printf("PASS SSH connector %s\n",argv[1]);return 0;}
