/* Pinned dependency regression: real SSH, tracked allocations, no private API. */
#define _POSIX_C_SOURCE 200809L
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct allocation {
    void *pointer;
    bool admission;
    struct allocation *next;
} allocation;
typedef struct {
    size_t live;
    bool partial;
    bool admission;
    allocation *first;
} ledger;

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }

static void *tracked_alloc(size_t size, void **abstract) {
    ledger *book = *abstract;
    void *pointer = malloc(size);
    if (pointer) {
        allocation *entry = malloc(sizeof *entry);
        if (!entry) abort();
        *entry = (allocation){pointer, book->admission, book->first};
        book->first = entry;
        book->live++;
        if (book->admission && size == 16384) book->partial = true;
    }
    return pointer;
}

static void tracked_free(void *pointer, void **abstract) {
    if (!pointer) return;
    ledger *book = *abstract;
    allocation **entry = &book->first;
    while (*entry && (*entry)->pointer != pointer) entry = &(*entry)->next;
    if (!*entry || !book->live) abort();
    allocation *old = *entry;
    *entry = old->next;
    free(old);
    book->live--;
    free(pointer);
}

static void *tracked_realloc(void *pointer, size_t size, void **abstract) {
    if (!pointer) return tracked_alloc(size, abstract);
    if (!size) { tracked_free(pointer, abstract); return NULL; }
    ledger *book = *abstract;
    allocation *entry = book->first;
    while (entry && entry->pointer != pointer) entry = entry->next;
    if (!entry) abort();
    void *next = realloc(pointer, size);
    if (next) entry->pointer = next;
    return next;
}

static int64_t now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tick(void) {
    struct timespec ts = {.tv_nsec = 1000000};
    nanosleep(&ts, NULL);
}

static int fail(const char *why) {
    fprintf(stderr, "FAIL: %s\n", why);
    return 1;
}

static int directory_probe(LIBSSH2_SESSION *session, LIBSSH2_SFTP *sftp,
                           const char *mode, int64_t start) {
    int result;
    if (!strcmp(mode, "raw-empty")) {
        LIBSSH2_SFTP_HANDLE *directory;
        while (!(directory = libssh2_sftp_opendir(sftp, "/fixture"))) {
            if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN ||
                now() - start > 5000) return fail("dependency directory open");
            tick();
        }
        char name[1024];
        LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
        while ((result = libssh2_sftp_readdir(directory, name, sizeof name, &attrs)) == LIBSSH2_ERROR_EAGAIN) {
            if (now() - start > 5000) return fail("dependency directory read bounded");
            tick();
        }
        if (result >= 0) return fail("dependency empty name became EOF or entry");
        while ((result = libssh2_sftp_closedir(directory)) == LIBSSH2_ERROR_EAGAIN) {
            if (now() - start > 5000) return fail("dependency directory close bounded");
            tick();
        }
        if (result) return fail("dependency directory close");
    } else {
        char path[1024];
        memset(path, 'X', sizeof path);
        while ((result = libssh2_sftp_realpath(sftp, ".", path, sizeof path)) == LIBSSH2_ERROR_EAGAIN) {
            if (now() - start > 5000) return fail("dependency realpath bounded");
            tick();
        }
        if (!strcmp(mode, "raw-path")) {
            if (result != LIBSSH2_ERROR_SFTP_PROTOCOL)
                return fail("dependency NAME length was accepted");
            for (size_t i = 0; i < sizeof path; i++)
                if (path[i] != 'X') return fail("dependency NAME copied before bounds check");
        } else if (result != 8 || memcmp(path, "/fixture", 8)) {
            return fail("dependency normal realpath");
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 7) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGUSR1, stop);
    signal(SIGPIPE, SIG_IGN);
    if (libssh2_init(0)) return 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2]))
    };
    if (fd < 0 || inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 ||
        connect(fd, (void *)&address, sizeof address)) return 2;
    ledger memory = {0};
    LIBSSH2_SESSION *session = libssh2_session_init_ex(
        tracked_alloc, tracked_free, tracked_realloc, &memory);
    if (!session) return 2;
    libssh2_session_set_timeout(session, 3000);
    if (libssh2_session_handshake(session, fd))
        return fail("dependency fixture handshake");
    int authentication;
    if (!strcmp(argv[4], "-")) {
        /* Device tests receive the ephemeral fixture key over adb stdin. It is
         * never copied to a device file or included in process arguments. */
        char pem[65536];
        size_t length = fread(pem, 1, sizeof pem, stdin);
        if (!length || length == sizeof pem || ferror(stdin))
            return fail("dependency fixture key input");
        authentication = libssh2_userauth_publickey_frommemory(
            session, argv[3], strlen(argv[3]), NULL, 0, pem, length, NULL);
        volatile char *wipe = pem;
        for (size_t index = 0; index < sizeof pem; index++) wipe[index] = 0;
    } else {
        authentication = libssh2_userauth_publickey_fromfile(
            session, argv[3], NULL, argv[4], NULL);
    }
    if (authentication) return fail("dependency fixture authentication");
    memory.admission = true;
    libssh2_session_set_blocking(session, 0);
    bool cancel = !strcmp(argv[6], "cancel");
    bool timeout = !strcmp(argv[6], "timeout");
    bool directory = !strncmp(argv[6], "raw-", 4);
    bool normal = !strcmp(argv[6], "admit") || directory;
    printf("READY %ld\n", (long)getpid());
    int64_t start = now();
    LIBSSH2_SFTP *sftp = NULL;
    int error = 0;
    for (;;) {
        sftp = libssh2_sftp_init(session);
        if (sftp) break;
        error = libssh2_session_last_errno(session);
        if (error != LIBSSH2_ERROR_EAGAIN) break;
        /* A cancellation sent before the fragment arrives must still exercise
         * the partial buffer, instead of trivially skipping init altogether. */
        if (cancel && stopped && memory.partial) break;
        if (timeout && now() - start >= 400 && memory.partial) break;
        if (now() - start > 5000)
            return fail("dependency init did not return expected state");
        tick();
    }
    if (normal && !sftp) return fail("normal dependency init");
    if (!normal && sftp) return fail("fault dependency init succeeded");
    if ((cancel || timeout) && !memory.partial)
        return fail("partial allocation not reached");
    if (!normal && !cancel && !timeout && error != LIBSSH2_ERROR_SOCKET_RECV)
        return fail("EOF must report socket receive error");
    if (now() - start > 3000) return fail("dependency init exceeded bound");
    if (directory && directory_probe(session, sftp, argv[6], start)) return 1;
    if (sftp) {
        int rc;
        while ((rc = libssh2_sftp_shutdown(sftp)) == LIBSSH2_ERROR_EAGAIN) {
            if (now() - start > 5000) return fail("normal dependency shutdown bounded");
            tick();
        }
        if (rc) return fail("normal dependency shutdown");
    }
    shutdown(fd, SHUT_RDWR);
    int rc;
    while ((rc = libssh2_session_free(session)) == LIBSSH2_ERROR_EAGAIN) {
        if (now() - start > 5000) return fail("dependency owner release bounded");
        tick();
    }
    close(fd);
    libssh2_exit();
    if (rc) return fail("dependency session free");
    size_t leaked = 0;
    for (allocation *entry = memory.first; entry; entry = entry->next)
        if (entry->admission) leaked++;
    if (leaked) {
        fprintf(stderr, "FAIL: SFTP init leaked %zu allocations\n", leaked);
        return 1;
    }
    /* Earlier dependency allocations are reported, never attributed to init. */
    if (memory.live)
        fprintf(stderr, "PRE-INIT outstanding allocations: %zu\n", memory.live);
    while (memory.first) {
        allocation *entry = memory.first;
        memory.first = entry->next;
        free(entry->pointer);
        free(entry);
    }
    puts("SFTP OPEN PASS");
    return 0;
}
