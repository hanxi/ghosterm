/* Linked only into the short-IO test binary. Production socket adapter has no
 * fault switch. Force one-byte readiness reads/writes and application tails. */
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <stddef.h>
static size_t shortened;
ssize_t gt_test_send(int fd,const void *bytes,size_t n,int flags){if(n>1 && n<=72){n=1;shortened++;}return send(fd,bytes,n,flags);}
ssize_t gt_test_recv(int fd,void *bytes,size_t n,int flags){if(n>1 && n<=72){n=1;shortened++;}return recv(fd,bytes,n,flags);}
size_t gt_test_short_calls(void){return shortened;}
