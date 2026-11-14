#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/process-owner.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) {
        abort();
    }
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

static void delay(void) {
    struct timespec value = {0, 1000000};
    (void)nanosleep(&value, NULL);
}

static bool gone(pid_t pid) {
    return kill(pid, 0) < 0 && errno == ESRCH;
}

static int child(const char *directory, bool descendants) {
    // A failed mutation cannot leave a permanent process; this alarm is outside
    // the two-second assertion budget and is never accepted as owner cleanup.
    alarm(5);
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGTERM, &ignore, NULL)) {
        return 91;
    }
    pid_t descendant = 0;
    if (descendants) {
        int ready[2];
        if (pipe(ready)) {
            return 91;
        }
        descendant = fork();
        if (descendant == 0) {
            alarm(5);
            close(ready[0]);
            if (write(ready[1], "R", 1) != 1) {
                _exit(91);
            }
            close(ready[1]);
            for (;;) {
                pause();
            }
        }
        close(ready[1]);
        char byte = 0;
        if (descendant < 0 || read(ready[0], &byte, 1) != 1 || byte != 'R') {
            return 91;
        }
        close(ready[0]);
    }
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/ready", directory) >= (int)sizeof(path)) {
        return 91;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    char text[128];
    int size = snprintf(text,
                        sizeof(text),
                        "%ld %ld %ld %ld\n",
                        (long)getpid(),
                        (long)getppid(),
                        (long)getpgrp(),
                        (long)descendant);
    if (fd < 0 || write(fd, text, (size_t)size) != size || close(fd)) {
        return 91;
    }
    for (;;) {
        pause();
    }
}

static int
crash(const char *executable, const char *temporary, bool descendants, int signal_number) {
    char directory[4096], ready[4096];
    if (snprintf(directory, sizeof(directory), "%s/guard-XXXXXX", temporary) >=
            (int)sizeof(directory) ||
        !mkdtemp(directory) ||
        snprintf(ready, sizeof(ready), "%s/ready", directory) >= (int)sizeof(ready)) {
        return 2;
    }
    pid_t owner = fork();
    if (owner == 0) {
        int cwd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        gt_ext_process_t *process = NULL;
        const char *args[] = {
            executable, "child", directory, descendants ? "descendants" : "single"};
        if (cwd < 0 ||
            gt__ext_process_spawn(executable, args, 4, cwd, NULL, 0, &process) != GT_OK) {
            _exit(92);
        }
        close(cwd);
        for (;;) {
            pause();
        }
    }
    if (owner < 0) {
        rmdir(directory);
        return 2;
    }
    long pid = 0, monitor = 0, group = 0, descendant = 0;
    uint64_t end = now() + 2000;
    while (now() < end) {
        FILE *file = fopen(ready, "r");
        if (file) {
            int count = fscanf(file, "%ld %ld %ld %ld", &pid, &monitor, &group, &descendant);
            fclose(file);
            if (count == 4) {
                break;
            }
        }
        delay();
    }
    bool started = pid > 0 && monitor > 0 && monitor != owner && group == pid &&
                   (!descendants || descendant > 0);
    // Only our direct fork child is signalled; its PID stays reserved until waitpid.
    (void)kill(owner, signal_number);
    int status;
    while (waitpid(owner, &status, 0) < 0 && errno == EINTR) {
    }
    bool cleaned = false;
    end = now() + 2000;
    while (started && now() < end) {
        cleaned =
            gone((pid_t)pid) && gone((pid_t)monitor) && (!descendant || gone((pid_t)descendant));
        if (cleaned) {
            break;
        }
        delay();
    }
    if (!started || !cleaned) {
        fprintf(stderr,
                started ? "ASSERT: parent death retained owned executable/monitor/group\n"
                        : "SETUP: owned executable did not publish readiness\n");
        // Cleanup grace for the fixture's independent alarm. No stale PID/PGID
        // signal, and no extension of the assertion's two-second deadline.
        end = now() + 5000;
        while (now() < end &&
               ((pid > 0 && !gone((pid_t)pid)) || (monitor > 0 && !gone((pid_t)monitor)) ||
                (descendant > 0 && !gone((pid_t)descendant)))) {
            delay();
        }
    }
    unlink(ready);
    rmdir(directory);
    return !started ? 2 : cleaned ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "child")) {
        return child(argv[2], !strcmp(argv[3], "descendants"));
    }
    if (argc != 3) {
        return 2;
    }
    char executable[4096];
    if (!realpath(argv[0], executable)) {
        return 2;
    }
    bool descendants = !strcmp(argv[1], "descendants");
    int number = !strcmp(argv[1], "term") ? SIGTERM : SIGKILL;
    return crash(executable, argv[2], descendants, number);
}
