#include <libssh2.h>
#include <libssh2_sftp.h>
static unsigned live;
LIBSSH2_SFTP_HANDLE *gt_deploy_test_open(LIBSSH2_SFTP *s,const char *p,unsigned int n,unsigned long flags,long mode,int kind) {
  LIBSSH2_SFTP_HANDLE *h=libssh2_sftp_open_ex(s,p,n,flags,mode,kind);if(h)live++;return h;
}
int gt_deploy_test_close(LIBSSH2_SFTP_HANDLE *h) {
  int rc=libssh2_sftp_close_handle(h);if(rc!=LIBSSH2_ERROR_EAGAIN && live)live--;return rc;
}
unsigned gt_deploy_live_handles(void){return live;}
