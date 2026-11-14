/* Owned Native cleanup probe: real TERM-ignore, never a user shell/service. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

int main(void) {
    alarm(8);
    signal(SIGTERM, SIG_IGN);
    int fd = open("ready", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, "R", 1) != 1 || close(fd)) {
        return 91;
    }
    for (;;) {
        pause();
    }
}
