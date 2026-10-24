#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "secure-random.h"
#include <errno.h>
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/random.h>
#endif
#include <unistd.h>
void gt__secure_erase(void *bytes,size_t n) {
  volatile unsigned char *p=bytes;while(n--)*p++=0;
}
gt_status_t gt__secure_random(uint8_t *bytes,size_t n) {
  if(!bytes || !n || n>256)return GT_ERR_INVALID_ARG;
#if defined(__linux__)
  ssize_t count=getrandom(bytes,n,GRND_NONBLOCK);if(count==(ssize_t)n)return GT_OK;
  int error=count<0?errno:0;gt__secure_erase(bytes,n);return error==ENOSYS?GT_ERR_UNSUPPORTED:GT_ERR_IO;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  if(!getentropy(bytes,n))return GT_OK;
  gt__secure_erase(bytes,n);return GT_ERR_IO;
#else
  gt__secure_erase(bytes,n);return GT_ERR_UNSUPPORTED;
#endif
}
