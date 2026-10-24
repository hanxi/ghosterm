#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include "ipc/ssh-session-provider.h"
#include "internal.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static gt_session_authenticator_t *auth;static gt_ssh_t *owned;static gt_ssh_session_provider_t *provider;
static void cleanup(void){gt_session_authenticator_close(auth);auth=NULL;gt__ssh_session_provider_close(provider);provider=NULL;gt_ssh_close(owned);owned=NULL;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL deferred SSH:%d: %s\n",__LINE__,#x);cleanup();exit(1);}}while(0)
static gt_ssh_opts_t options;static uint64_t clock_value;static uint64_t requested;static unsigned cancelled;
static bool clock_ms(void *context,uint64_t *out){(void)context;*out=clock_value;return true;}
static uint64_t real_ms(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static void pause_one(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static gt_session_auth_info_t info(void){gt_session_auth_info_t out;CHECK(gt_session_authenticator_info(auth,&out));return out;}
static void until(gt_session_auth_phase_t phase){uint64_t end=real_ms()+5000;for(;;){gt_session_auth_phase_t state=gt_session_authenticator_step(auth);if(state==phase)return;CHECK(state!=GT_SESSION_AUTH_FAILED && state!=GT_SESSION_AUTH_CANCELLED && real_ms()<end);pause_one();}}
static gt_ssh_t *material(const char *key){gt_ssh_t *ssh=gt_ssh_new(&options);CHECK(ssh);if(key)CHECK(gt_ssh_auth_private_key(ssh,key,NULL)==GT_OK);return ssh;}
static void open_auth(void){owned=material(NULL);gt_session_auth_options_t config={.family=4,.timeout_ms=1000,.defer_credentials=true,.clock=clock_ms};CHECK(inet_pton(AF_INET,options.host,config.address)==1);CHECK(gt_session_authenticator_open(&owned,&config,&auth)==GT_OK && !owned);}
static char *read_key(const char *path){FILE *f=fopen(path,"rb");CHECK(f && !fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0 && n<1024*1024 && !fseek(f,0,SEEK_SET));char *out=calloc(1,(size_t)n+1);CHECK(out && fread(out,1,(size_t)n,f)==(size_t)n && !fclose(f));return out;}
static void requested_cb(void *context,uint64_t id,uint32_t attempt){(void)context;CHECK(id && attempt==1);requested=id;CHECK(gt__ssh_session_provider_adopt(provider,id,&owned)==GT_ERR_INVALID_ARG && owned);}
static void cancelled_cb(void *context,uint64_t id){(void)context;CHECK(id==requested);cancelled++;}
static void forbidden_hostkey(void *context,uint64_t id,const gt_ssh_connector_info_t *i){(void)context;(void)id;(void)i;CHECK(false);}
static gt_session_link_t open_provider(void){gt_ssh_session_provider_config_t config={.proxy="/fixture/proxy",.daemon="/fixture/daemon",.runtime="/fixture/runtime",.authentication_timeout_ms=100,.clock=clock_ms,.requested=requested_cb,.cancelled=cancelled_cb,.hostkey=forbidden_hostkey};CHECK(gt__ssh_session_provider_open(&config,NULL,&provider)==GT_OK);gt_session_link_t link;CHECK(gt__ssh_session_provider_link(provider,&link));gt_status_t error;CHECK(link.step(link.context,true,1,&error)==GT_SESSION_LINK_WAIT && error==GT_OK);return link;}
int main(int argc,char **argv){
  CHECK(argc==6 && getenv("GT_DEFER_BAD_KEY"));char known[2048];CHECK(snprintf(known,sizeof(known),"%s/deferred-known-hosts",argv[5])>0);unlink(known);
  options=(gt_ssh_opts_t){.host=argv[1],.port=(uint16_t)strtoul(argv[2],NULL,10),.username=argv[3],.known_hosts_path=known,.connect_timeout_ms=1000};
  char *key=read_key(argv[4]),*bad=read_key(getenv("GT_DEFER_BAD_KEY"));
  open_auth();until(GT_SESSION_AUTH_HOSTKEY);CHECK(!info().auth_methods && info().host==GT_SESSION_HOST_UNKNOWN);
  owned=material(key);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_ERR_INVALID_ARG && owned);gt_ssh_close(owned);owned=NULL;
  CHECK(gt_session_authenticator_decide(auth,GT_HOSTKEY_UNKNOWN)==GT_OK && info().phase==GT_SESSION_AUTH_HOSTKEY);
  CHECK(gt_session_authenticator_decide(auth,GT_HOSTKEY_ACCEPT)==GT_OK);until(GT_SESSION_AUTH_CREDENTIALS);CHECK(info().auth_methods&GT_SSH_AUTH_PUBLICKEY);
  gt_ssh_opts_t wrong=options;wrong.username="wrong-deferred-identity";owned=gt_ssh_new(&wrong);CHECK(owned && gt_ssh_auth_private_key(owned,key,NULL)==GT_OK);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_ERR_INVALID_ARG && owned);gt_ssh_close(owned);owned=NULL;
  owned=material(NULL);CHECK(gt_ssh_auth_password(owned,"fixture-wrong-password")==GT_OK);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_ERR_UNSUPPORTED && owned);gt_ssh_close(owned);owned=NULL;
  owned=material(bad);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_OK && !owned);until(GT_SESSION_AUTH_CREDENTIALS);CHECK(info().error==GT_ERR_AUTH && (info().auth_methods&GT_SSH_AUTH_PUBLICKEY));
  // All candidates, UI waits and retries use the original deadline.
  clock_value=1000;owned=material(key);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_ERR_IO && owned && info().deadline && info().phase==GT_SESSION_AUTH_FAILED);cleanup();
  clock_value=0;open_auth();until(GT_SESSION_AUTH_CREDENTIALS);CHECK(info().host==GT_SESSION_HOST_KNOWN);gt_session_authenticator_close(auth);auth=NULL;
  // The private cancel path must not confuse the appended phase with TAKEN.
  gt_ssh_connector_t *connector=NULL;owned=material(NULL);
  gt_ssh_connector_config_t deferred={.family=4,.timeout_ms=1000,.clock=clock_ms,.defer_credentials=true};CHECK(inet_pton(AF_INET,options.host,deferred.address)==1);
  CHECK(gt__ssh_connector_open(&owned,&deferred,NULL,&connector)==GT_OK && !owned);
  uint64_t end=real_ms()+5000;while(gt__ssh_connector_step(connector)!=GT_SSH_CONNECT_CREDENTIALS){CHECK(real_ms()<end);pause_one();}
  gt__ssh_connector_cancel(connector);gt_ssh_connector_info_t cancelled_info;CHECK(gt__ssh_connector_info(connector,&cancelled_info) && cancelled_info.phase==GT_SSH_CONNECT_CANCELLED && cancelled_info.error==GT_ERR_CANCELLED);gt__ssh_connector_close(connector);

  open_auth();until(GT_SESSION_AUTH_CREDENTIALS);owned=material(bad);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_OK && !owned);until(GT_SESSION_AUTH_CREDENTIALS);
  owned=material(key);CHECK(gt_session_authenticator_supply(auth,&owned)==GT_OK && !owned);until(GT_SESSION_AUTH_READY);CHECK(gt_session_authenticator_take(auth,&owned) && owned);gt_session_authenticator_close(auth);auth=NULL;
  void *session=NULL;int socket=-1;CHECK(gt__ssh_borrow(owned,&session,&socket)==GT_OK && (fcntl(socket,F_GETFL)&O_NONBLOCK));
  gt_session_link_t link=open_provider();CHECK(gt__ssh_session_provider_adopt(provider,requested+1,&owned)==GT_ERR_NOT_FOUND && owned);
  clock_value=100;CHECK(gt__ssh_session_provider_adopt(provider,requested,&owned)==GT_ERR_IO && owned && cancelled==1);gt__ssh_session_provider_close(provider);provider=NULL;
  clock_value=0;link=open_provider();CHECK(gt__ssh_session_provider_adopt(provider,requested,&owned)==GT_OK && !owned);
  gt_status_t error;CHECK(link.step(link.context,false,1,&error)==GT_SESSION_LINK_READY && error==GT_OK && cancelled==1);link.reset(link.context);gt__ssh_session_provider_close(provider);provider=NULL;
  memset(key,0,strlen(key));memset(bad,0,strlen(bad));free(key);free(bad);CHECK(!unlink(known));cleanup();
  puts("PASS deferred SSH: host verification/methods before material, same-connection retry, total deadline, identity/scope/ownership and authenticated adoption");return 0;
}
