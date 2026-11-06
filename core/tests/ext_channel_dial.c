/* Public-API test's real connector can dial only the first managed SSH hop. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <sys/socket.h>
int ext_channel_test_connect(int, const struct sockaddr *, socklen_t);
#define connect ext_channel_test_connect
#include "../src/ssh.c"
#undef connect
#include <arpa/inet.h>
uint16_t gt_ext_test_port;
unsigned gt_ext_test_dials;

int ext_channel_test_connect(int fd, const struct sockaddr *p, socklen_t n) {
    gt_ext_test_dials++;
    if (p->sa_family != AF_INET ||
        ntohs(((const struct sockaddr_in *)p)->sin_port) != gt_ext_test_port) {
        errno = EACCES;
        return -1;
    }
    return connect(fd, p, n);
}
