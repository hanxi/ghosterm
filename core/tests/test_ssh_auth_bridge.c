/* Public SSH authentication translation; network behavior has real sshd tests. */
#include "ghosterm.h"
#include "ssh/connector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL auth ABI:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int marker;static gt_ssh_authenticator_t *auth;static gt_ssh_connector_info_t state;
static bool fail_open,taken;static unsigned closes;
gt_status_t gt__ssh_connector_open(gt_ssh_t **ssh,const gt_ssh_connector_config_t *c,const gt_allocator_t *a,gt_ssh_connector_t **out){(void)a;CHECK(ssh && *ssh==(void *)&marker && c->family==6 && c->address[15]==1 && c->scope==7 && c->timeout_ms==60000 && c->clock);uint64_t now;CHECK(c->clock(c->clock_context,&now));if(fail_open){*out=NULL;return GT_ERR_AUTH;}*ssh=NULL;*out=(void *)&marker;return GT_OK;}
gt_ssh_connector_phase_t gt__ssh_connector_step(gt_ssh_connector_t *c){CHECK(c==(void *)&marker);return state.phase;}
bool gt__ssh_connector_info(const gt_ssh_connector_t *c,gt_ssh_connector_info_t *out){CHECK(c==(void *)&marker);*out=state;return true;}
gt_status_t gt__ssh_connector_decide(gt_ssh_connector_t *c,gt_hostkey_decision_t d){CHECK(c==(void *)&marker && d==GT_HOSTKEY_REJECT);return GT_ERR_HOST_KEY;}
gt_status_t gt__ssh_connector_supply(gt_ssh_connector_t *c,gt_ssh_t **s){CHECK(c==(void *)&marker && s && *s==(void *)&marker);*s=NULL;return GT_OK;}
bool gt__ssh_connector_take(gt_ssh_connector_t *c,gt_ssh_t **out){CHECK(c==(void *)&marker);if(taken)return false;taken=true;*out=(void *)&marker;return true;}
void gt__ssh_connector_close(gt_ssh_connector_t *c){CHECK(c==(void *)&marker);closes++;}
int main(void){
  gt_ssh_auth_options_t options={.family=6,.scope=7};options.address[15]=1;gt_ssh_t *ssh=(void *)&marker;CHECK(gt_ssh_authenticator_open(&ssh,&options,&auth)==GT_OK && !ssh && auth);
  for(unsigned i=0;i<9;i++){state.phase=(gt_ssh_connector_phase_t)i;CHECK((unsigned)gt_ssh_authenticator_step(auth)==i);}
  state=(gt_ssh_connector_info_t){.phase=GT_SSH_CONNECT_HOSTKEY,.host=GT_SSH_HOST_UNKNOWN,.error=GT_ERR_HOST_KEY,.deadline=true,.auth_methods=3};strcpy(state.key_type,"ssh-ed25519");strcpy(state.fingerprint,"SHA256:fixture");gt_ssh_auth_info_t info;CHECK(gt_ssh_authenticator_info(auth,&info) && info.phase==GT_SSH_AUTH_PHASE_HOSTKEY && info.host==GT_SSH_AUTH_HOST_UNKNOWN && info.error==GT_ERR_HOST_KEY && info.deadline && info.auth_methods==3 && !strcmp(info.key_type,"ssh-ed25519") && !strcmp(info.fingerprint,"SHA256:fixture"));
  ssh=(void *)&marker;CHECK(gt_ssh_authenticator_supply(auth,&ssh)==GT_OK && !ssh);
  CHECK(gt_ssh_authenticator_decide(auth,GT_HOSTKEY_REJECT)==GT_ERR_HOST_KEY);CHECK(gt_ssh_authenticator_take(auth,&ssh) && ssh==(void *)&marker);CHECK(!gt_ssh_authenticator_take(auth,&ssh) && !ssh);gt_ssh_authenticator_close(auth);CHECK(closes==1);
  fail_open=true;ssh=(void *)&marker;auth=(void *)1;CHECK(gt_ssh_authenticator_open(&ssh,&options,&auth)==GT_ERR_AUTH && !auth && ssh==(void *)&marker);CHECK(gt_ssh_authenticator_open(NULL,&options,&auth)==GT_ERR_INVALID_ARG && !auth);CHECK(!gt_ssh_authenticator_info(NULL,&info) && !info.auth_methods);CHECK(gt_ssh_authenticator_step(NULL)==GT_SSH_AUTH_PHASE_FAILED && gt_ssh_authenticator_decide(NULL,GT_HOSTKEY_ACCEPT)==GT_ERR_INVALID_ARG);gt_ssh_authenticator_close(NULL);
  puts("PASS SSH authentication ABI, phases, hostkey, credential and connection ownership");return 0;
}
