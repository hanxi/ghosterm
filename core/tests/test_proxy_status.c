#include "ipc/proxy-status.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL proxy status:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void) {
  const gt_status_t expected[]={GT_ERR_INVALID_ARG,GT_ERR_NOT_FOUND,GT_ERR_AUTH,GT_ERR_UNSUPPORTED,GT_ERR_IO,GT_ERR_CANCELLED,GT_ERR_UNSUPPORTED,GT_ERR_UNSUPPORTED,GT_ERR_PROTOCOL,GT_ERR_IO};
  const gt_activation_rc_t results[]={GT_ACTIVATION_INVALID,GT_ACTIVATION_NOT_FOUND,GT_ACTIVATION_AUTH,GT_ACTIVATION_UNSUPPORTED,GT_ACTIVATION_TIMEOUT,GT_ACTIVATION_CANCELLED,GT_ACTIVATION_INCOMPATIBLE,GT_ACTIVATION_FEATURES,GT_ACTIVATION_INSTANCE,GT_ACTIVATION_IO};
  CHECK(GT_ACTIVATION_OK==0);
  for(unsigned i=0;i<10;i++)CHECK(64+(int)results[i]==65+(int)i);
  for(unsigned i=0;i<10;i++){gt_status_t status=GT_OK;CHECK(gt__proxy_exit_status(65+(int)i,&status) && status==expected[i]);}
  const int unknown[]={-1,0,1,2,7,64,75,127,255};
  for(unsigned i=0;i<sizeof(unknown)/sizeof(*unknown);i++){gt_status_t status=GT_ERR_INTERNAL;CHECK(!gt__proxy_exit_status(unknown[i],&status) && status==GT_OK);}
  CHECK(!gt__proxy_exit_status(71,NULL));puts("PASS proxy exit status keeps typed failures and zero/absent unknown");return 0;
}
