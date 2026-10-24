/* Public setup translation, separate from real sshd/deployment evidence. */
#include "ghosterm.h"
#include "ipc/ssh-connector.h"
#include "session/deploy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL setup ABI:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int marker;static gt_session_authenticator_t *auth;static gt_ssh_connector_info_t state;
static bool fail_open,taken,stop,fail_catalog;static unsigned closes,progresses;static gt_status_t probe_error;
static gt_deploy_phase_t progress_phase;
gt_status_t gt__ssh_connector_open(gt_ssh_t **ssh,const gt_ssh_connector_config_t *c,const gt_allocator_t *a,gt_ssh_connector_t **out){(void)a;CHECK(ssh && *ssh==(void *)&marker && c->family==6 && c->address[15]==1 && c->scope==7 && c->timeout_ms==60000 && c->clock);uint64_t now;CHECK(c->clock(c->clock_context,&now));if(fail_open){*out=NULL;return GT_ERR_AUTH;}*ssh=NULL;*out=(void *)&marker;return GT_OK;}
gt_ssh_connector_phase_t gt__ssh_connector_step(gt_ssh_connector_t *c){CHECK(c==(void *)&marker);return state.phase;}
bool gt__ssh_connector_info(const gt_ssh_connector_t *c,gt_ssh_connector_info_t *out){CHECK(c==(void *)&marker);*out=state;return true;}
gt_status_t gt__ssh_connector_decide(gt_ssh_connector_t *c,gt_hostkey_decision_t d){CHECK(c==(void *)&marker && d==GT_HOSTKEY_REJECT);return GT_ERR_HOST_KEY;}
gt_status_t gt__ssh_connector_supply(gt_ssh_connector_t *c,gt_ssh_t **s){CHECK(c==(void *)&marker && s && *s==(void *)&marker);*s=NULL;return GT_OK;}
bool gt__ssh_connector_take(gt_ssh_connector_t *c,gt_ssh_t **out){CHECK(c==(void *)&marker);if(taken)return false;taken=true;*out=(void *)&marker;return true;}
void gt__ssh_connector_close(gt_ssh_connector_t *c){CHECK(c==(void *)&marker);closes++;}
static bool cancelled(void *context){CHECK(context==&marker);return stop;}
static void report(void *context,gt_session_install_phase_t phase,unsigned file,uint64_t bytes,uint64_t total){CHECK(context==&marker && (unsigned)phase==(unsigned)progress_phase && file==3 && bytes==7 && total==11);progresses++;}
static void callbacks(const gt_deploy_options_t *c){CHECK(c->timeout_ms==120000 && c->cancelled && c->progress && c->cancelled(c->context)==stop);for(unsigned i=0;i<5;i++){progress_phase=(gt_deploy_phase_t)i;c->progress(c->context,progress_phase,3,7,11);}}
gt_status_t gt__ssh_probe_linux(gt_ssh_t *ssh,const gt_deploy_options_t *c,gt_linux_probe_t *out){CHECK(ssh==(void *)&marker);callbacks(c);memset(out,0,sizeof(*out));strcpy(out->arch,"aarch64");strcpy(out->glibc,"2.39");strcpy(out->home,"/home/fixture");out->uid=501;return probe_error;}
gt_status_t gt__session_bundle_read(const char *directory,gt_session_bundle_t *out){CHECK(!strcmp(directory,"/trusted/catalog"));memset(out,0,sizeof(*out));strcpy(out->release,"release-one");return fail_catalog?GT_ERR_PROTOCOL:GT_OK;}
gt_status_t gt__ssh_deploy_linux(gt_ssh_t *ssh,const gt_session_bundle_t *b,const char *root,const gt_deploy_options_t *c,gt_deploy_result_t *out){CHECK(ssh==(void *)&marker && !strcmp(b->release,"release-one") && !strcmp(root,"/owned/root"));callbacks(c);memset(out,0,sizeof(*out));out->publication_unknown=true;strcpy(out->daemon,"/owned/release/daemon");strcpy(out->proxy,"/owned/release/proxy");return GT_ERR_IO;}
int main(void){
  gt_session_auth_options_t options={.family=6,.scope=7};options.address[15]=1;gt_ssh_t *ssh=(void *)&marker;CHECK(gt_session_authenticator_open(&ssh,&options,&auth)==GT_OK && !ssh && auth);
  for(unsigned i=0;i<9;i++){state.phase=(gt_ssh_connector_phase_t)i;CHECK((unsigned)gt_session_authenticator_step(auth)==i);}
  state=(gt_ssh_connector_info_t){.phase=GT_SSH_CONNECT_HOSTKEY,.host=GT_SSH_HOST_UNKNOWN,.error=GT_ERR_HOST_KEY,.deadline=true,.auth_methods=3};strcpy(state.key_type,"ssh-ed25519");strcpy(state.fingerprint,"SHA256:fixture");gt_session_auth_info_t info;CHECK(gt_session_authenticator_info(auth,&info) && info.phase==GT_SESSION_AUTH_HOSTKEY && info.host==GT_SESSION_HOST_UNKNOWN && info.error==GT_ERR_HOST_KEY && info.deadline && info.auth_methods==3 && !strcmp(info.key_type,"ssh-ed25519") && !strcmp(info.fingerprint,"SHA256:fixture"));
  ssh=(void *)&marker;CHECK(gt_session_authenticator_supply(auth,&ssh)==GT_OK && !ssh);
  CHECK(gt_session_authenticator_decide(auth,GT_HOSTKEY_REJECT)==GT_ERR_HOST_KEY);CHECK(gt_session_authenticator_take(auth,&ssh) && ssh==(void *)&marker);CHECK(!gt_session_authenticator_take(auth,&ssh) && !ssh);gt_session_authenticator_close(auth);CHECK(closes==1);
  fail_open=true;ssh=(void *)&marker;auth=(void *)1;CHECK(gt_session_authenticator_open(&ssh,&options,&auth)==GT_ERR_AUTH && !auth && ssh==(void *)&marker);CHECK(gt_session_authenticator_open(NULL,&options,&auth)==GT_ERR_INVALID_ARG && !auth);CHECK(!gt_session_authenticator_info(NULL,&info) && !info.auth_methods);CHECK(gt_session_authenticator_step(NULL)==GT_SESSION_AUTH_FAILED && gt_session_authenticator_decide(NULL,GT_HOSTKEY_ACCEPT)==GT_ERR_INVALID_ARG);gt_session_authenticator_close(NULL);
  gt_session_install_options_t install={.context=&marker,.cancelled=cancelled,.progress=report};gt_session_linux_info_t linux_info;CHECK(gt_session_linux_probe((void *)&marker,&install,&linux_info)==GT_OK && !strcmp(linux_info.arch,"aarch64") && !strcmp(linux_info.glibc,"2.39") && !strcmp(linux_info.home,"/home/fixture") && linux_info.uid==501 && progresses==5);
  stop=true;probe_error=GT_ERR_CANCELLED;CHECK(gt_session_linux_probe((void *)&marker,&install,&linux_info)==GT_ERR_CANCELLED && !linux_info.arch[0] && !linux_info.uid && progresses==10);
  gt_session_install_result_t result;CHECK(gt_session_linux_install((void *)&marker,"/trusted/catalog","/owned/root",&install,&result)==GT_ERR_IO && result.publication_unknown && !result.published && !result.reused && !strcmp(result.daemon,"/owned/release/daemon") && !strcmp(result.proxy,"/owned/release/proxy") && progresses==15);
  fail_catalog=true;CHECK(gt_session_linux_install((void *)&marker,"/trusted/catalog","/owned/root",&install,&result)==GT_ERR_PROTOCOL && !result.publication_unknown && !result.daemon[0] && progresses==15);CHECK(gt_session_linux_install(NULL,NULL,NULL,NULL,&result)==GT_ERR_INVALID_ARG && !result.published);
  puts("PASS setup ABI identity, SSH ownership, Linux probe, progress/cancel and ambiguous publication translation");return 0;
}
