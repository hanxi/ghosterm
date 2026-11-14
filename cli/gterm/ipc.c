#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "ipc.h"
#include "stdio.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The permit is an ephemeral same-user capability, never a TTY/argv token.
 * Same-account hostile native code is outside this boundary. Session package
 * matching/consent remain mandatory after this transport admission. */
static const char socket_name[] = "channel.sock", permit_name[] = "permit";
static const unsigned char magic[8] = {'G', 'T', 'E', 'R', 'M', 'I', 'P', '2'};
static const unsigned char acknowledged[8] = {'G', 'T', 'E', 'R', 'M', 'O', 'K', '2'};

typedef struct {
    unsigned char bytes[56]; /* magic, public instance (16), private permit (32) */
} permit_t;

typedef struct {
    int directory, listener, peer, cwd;
    struct stat socket, permit;
    bool has_socket, has_permit;
} endpoint_t;

static int error(void) {
    fputs("gterm: local IPC unavailable or admission failed\n", stderr);
    return 5;
}

static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL), descriptor = fcntl(fd, F_GETFD);
    return flags >= 0 && descriptor >= 0 && !fcntl(fd, F_SETFL, flags | O_NONBLOCK) &&
           !fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC);
}

static int private_fd(int fd) {
    if (fd < 0 || fd > 2) {
        return fd;
    }
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return copy;
}

static bool same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool entry(int dir, const char *name, const struct stat *expected) {
    struct stat info;
    return !fstatat(dir, name, &info, AT_SYMLINK_NOFOLLOW) && same(&info, expected);
}

static bool peer_user(int fd) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uid_t user;
    gid_t group;
    return !getpeereid(fd, &user, &group) && user == geteuid();
#elif defined(__linux__)
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) &&
           size == sizeof(credentials) && credentials.uid == geteuid();
#else
    (void)fd;
    return false;
#endif
}

static bool wait_fd(int fd, short events, uint64_t end, volatile sig_atomic_t *cancelled) {
    while (!*cancelled) {
        uint64_t current = now();
        if (!current || current >= end) {
            return false;
        }
        struct pollfd p = {fd, events, 0};
        int result = poll(&p, 1, (int)(end - current > 25 ? 25 : end - current));
        if (result > 0) {
            return !(p.revents & POLLNVAL) && (p.revents & (events | POLLHUP | POLLERR));
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

static bool exchange(int fd,
                     void *buffer,
                     size_t size,
                     bool sending,
                     uint64_t end,
                     volatile sig_atomic_t *cancelled) {
    size_t at = 0;
    while (at < size && !*cancelled) {
        if (!wait_fd(fd, sending ? POLLOUT : POLLIN, end, cancelled)) {
            return false;
        }
        ssize_t n = sending ? write(fd, (char *)buffer + at, size - at)
                            : read(fd, (char *)buffer + at, size - at);
        if (n > 0) {
            at += (size_t)n;
        } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            return false;
        }
    }
    return at == size && !*cancelled;
}

static bool open_directory(endpoint_t *e, const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    size_t length = strlen(path);
    if (length < 2 || length > 4096 || path[length - 1] == '/' || strchr(path, '\n') ||
        strchr(path, '\r')) {
        return false;
    }
    for (const char *p = path + 1; *p;) {
        const char *end = strchr(p, '/');
        size_t size = end ? (size_t)(end - p) : strlen(p);
        if (!size || (size == 1 && p[0] == '.') || (size == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    e->directory = private_fd(open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info;
    if (e->directory < 0 || fstat(e->directory, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0777) != 0700) {
        return false;
    }
    e->cwd = private_fd(open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return e->cwd >= 0 && !fchdir(e->directory);
}

static bool instance_matches(const permit_t *permit, const char *instance) {
    if (!instance || strlen(instance) != 32) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    unsigned difference = 0;
    for (size_t i = 0; i < 16; i++) {
        difference |=
            (unsigned char)instance[i * 2] ^ (unsigned char)hex[permit->bytes[8 + i] >> 4];
        difference |=
            (unsigned char)instance[i * 2 + 1] ^ (unsigned char)hex[permit->bytes[8 + i] & 15];
    }
    return difference == 0;
}

/* Private IPC prelude only; Core wire v1 starts after ACK. The expected literal
 * command/argv are not an authority grant and do not replace package/consent. */
static bool invocation(
    int fd, int argc, char **argv, bool sending, uint64_t end, volatile sig_atomic_t *cancelled) {
    if (argc < 1 || argc > 33) {
        return false;
    }
    unsigned char number[4] = {0, 0, 0, (unsigned char)argc};
    if (!exchange(fd, number, sizeof(number), sending, end, cancelled) || number[0] || number[1] ||
        number[2] || number[3] != argc) {
        return false;
    }
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        size_t size = strlen(argv[i]);
        if (size > 4096 || (i == 0 && size == 0) || size + 1 > 16384 - total) {
            return false;
        }
        total += size + 1;
        unsigned char bytes[4096];
        number[0] = number[1] = 0;
        number[2] = (unsigned char)(size >> 8);
        number[3] = (unsigned char)size;
        if (!exchange(fd, number, sizeof(number), sending, end, cancelled) || number[0] ||
            number[1] || (((size_t)number[2] << 8) | number[3]) != size) {
            return false;
        }
        if (sending) {
            memcpy(bytes, argv[i], size);
        }
        if (!exchange(fd, bytes, size, sending, end, cancelled) ||
            (!sending && memcmp(bytes, argv[i], size))) {
            return false;
        }
    }
    return true;
}

static bool
remove_entry(endpoint_t *e, const char *name, const struct stat *identity, bool *present) {
    if (!*present) {
        return true;
    }
    if (!entry(e->directory, name, identity) || unlinkat(e->directory, name, 0)) {
        return false;
    }
    *present = false;
    return true;
}

static bool cleanup(endpoint_t *e) {
    bool ok = true;
    if (e->peer >= 0 && close(e->peer)) {
        ok = false;
    }
    if (e->listener >= 0 && close(e->listener)) {
        ok = false;
    }
    if (!remove_entry(e, permit_name, &e->permit, &e->has_permit)) {
        ok = false;
    }
    if (!remove_entry(e, socket_name, &e->socket, &e->has_socket)) {
        ok = false;
    }
    if (e->cwd >= 0) {
        if (fchdir(e->cwd) || close(e->cwd)) {
            ok = false;
        }
    }
    if (e->directory >= 0 && close(e->directory)) {
        ok = false;
    }
    return ok;
}

static int local_socket(void) {
    int fd = private_fd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd >= 0 && !nonblocking(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool ignore_pipe(void) {
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    return !sigemptyset(&action.sa_mask) && !sigaction(SIGPIPE, &action, NULL);
}

static bool create_permit(endpoint_t *e, permit_t *permit) {
    memcpy(permit->bytes, magic, sizeof(magic));
    int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random < 0) {
        return false;
    }
    size_t used = sizeof(magic);
    while (used < sizeof(*permit)) {
        ssize_t n = read(random, permit->bytes + used, sizeof(*permit) - used);
        if (n > 0) {
            used += (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            close(random);
            return false;
        }
    }
    if (close(random)) {
        return false;
    }
    int fd = openat(
        e->directory, permit_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok = !fstat(fd, &e->permit);
    e->has_permit = ok;
    if (ok) {
        ok = write(fd, permit, sizeof(*permit)) == sizeof(*permit);
    }
    if (close(fd)) {
        ok = false;
    }
    return ok;
}

static bool read_permit(endpoint_t *e, permit_t *permit) {
    int fd = openat(e->directory, permit_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat info;
    bool ok = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
              (info.st_mode & 0777) == 0600 && info.st_nlink == 1 &&
              info.st_size == sizeof(*permit);
    if (ok) {
        ok = read(fd, permit, sizeof(*permit)) == sizeof(*permit) &&
             entry(e->directory, permit_name, &info) &&
             !memcmp(permit->bytes, magic, sizeof(magic));
    }
    if (close(fd)) {
        ok = false;
    }
    return ok;
}

static struct sockaddr_un address(void) {
    struct sockaddr_un value = {0};
    value.sun_family = AF_UNIX;
    memcpy(value.sun_path, socket_name, sizeof(socket_name));
    return value;
}

int gterm_ipc_listen(const char *directory,
                     int argc,
                     char **argv,
                     volatile sig_atomic_t *cancelled) {
    endpoint_t e = {.directory = -1, .listener = -1, .peer = -1, .cwd = -1};
    permit_t permit = {0};
    int result = 5;
    if (!open_directory(&e, directory) || !ignore_pipe()) {
        goto done;
    }
    /* Fail closed for stale entries. Never replace another endpoint. */
    struct stat existing;
    if (!fstatat(e.directory, socket_name, &existing, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        goto done;
    }
    if (!fstatat(e.directory, permit_name, &existing, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        goto done;
    }
    e.listener = local_socket();
    struct sockaddr_un a = address();
    mode_t old = umask(0077);
    int bound = e.listener < 0 ? -1 : bind(e.listener, (struct sockaddr *)&a, sizeof(a));
    umask(old);
    if (bound || fstatat(e.directory, socket_name, &e.socket, AT_SYMLINK_NOFOLLOW)) {
        goto done;
    }
    e.has_socket = true;
    if (listen(e.listener, 1) || !create_permit(&e, &permit)) {
        goto done;
    }
    fputs("gterm: local endpoint ready instance=", stderr);
    for (size_t i = 8; i < 24; i++) {
        fprintf(stderr, "%02x", permit.bytes[i]);
    }
    fputc('\n', stderr);
    int boundary = gterm_stdio_invocation_index(argc, argv);
    uint64_t end = now();
    if (!end) {
        goto done;
    }
    end += 30000;
    for (unsigned attempts = 0; attempts < 16 && !*cancelled; attempts++) {
        if (!wait_fd(e.listener, POLLIN, end, cancelled)) {
            break;
        }
        e.peer = private_fd(accept(e.listener, NULL, NULL));
        if (e.peer < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }
        permit_t received = {0};
        uint64_t admission = now() + 1000;
        if (admission > end) {
            admission = end;
        }
        bool ok = nonblocking(e.peer) && peer_user(e.peer) &&
                  exchange(e.peer, &received, sizeof(received), false, admission, cancelled);
        unsigned difference = 0;
        for (size_t i = 0; i < sizeof(permit); i++) {
            difference |= received.bytes[i] ^ permit.bytes[i];
        }
        memset(&received, 0, sizeof(received));
        ok = ok && !difference && boundary >= 0 &&
             invocation(
                 e.peer, argc - boundary - 2, argv + boundary + 2, false, admission, cancelled) &&
             entry(e.directory, socket_name, &e.socket) &&
             remove_entry(&e, permit_name, &e.permit, &e.has_permit);
        if (ok) {
            close(e.listener);
            e.listener = -1;
            if (!remove_entry(&e, socket_name, &e.socket, &e.has_socket)) {
                break;
            }
            unsigned char ack[8];
            memcpy(ack, acknowledged, sizeof(ack));
            if (!exchange(e.peer, ack, sizeof(ack), true, admission, cancelled)) {
                break;
            }
            if (fchdir(e.cwd) || dup2(e.peer, STDIN_FILENO) != STDIN_FILENO ||
                dup2(e.peer, STDOUT_FILENO) != STDOUT_FILENO) {
                break;
            }
            close(e.peer);
            e.peer = -1;
            memset(&permit, 0, sizeof(permit));
            result = gterm_stdio(argc, argv, cancelled);
            goto done;
        }
        close(e.peer);
        e.peer = -1;
    }
done:
    memset(&permit, 0, sizeof(permit));
    if (!cleanup(&e)) {
        fputs("gterm: local IPC cleanup incomplete\n", stderr);
        return 5;
    }
    if (*cancelled) {
        return 130;
    }
    return result == 5 ? error() : result;
}

typedef struct {
    unsigned char bytes[16384];
    size_t at, used;
    bool eof;
} pending_t;

static bool read_pending(int fd, pending_t *p) {
    ssize_t n = read(fd, p->bytes, sizeof(p->bytes));
    if (n > 0) {
        p->at = 0;
        p->used = (size_t)n;
        return true;
    }
    if (!n) {
        p->eof = true;
        return true;
    }
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

static bool write_pending(int fd, pending_t *p) {
    ssize_t n = write(fd, p->bytes + p->at, p->used - p->at);
    if (n > 0) {
        p->at += (size_t)n;
        if (p->at == p->used) {
            p->at = p->used = 0;
        }
        return true;
    }
    return n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
}

static int pump(int socket, volatile sig_atomic_t *cancelled) {
    pending_t input = {0}, output = {0};
    bool half_closed = false;
    uint64_t end = now();
    if (!end) {
        return 5;
    }
    end += 600000;
    while (!*cancelled && now() && now() < end) {
        if (output.eof && !output.used) {
            return 0;
        }
        if (input.eof && !input.used && !half_closed) {
            if (shutdown(socket, SHUT_WR)) {
                return 5;
            }
            half_closed = true;
        }
        struct pollfd p[3] = {
            {STDIN_FILENO, !input.used && !input.eof ? POLLIN : 0, 0},
            {socket, (!output.used && !output.eof ? POLLIN : 0) | (input.used ? POLLOUT : 0), 0},
            {STDOUT_FILENO, output.used ? POLLOUT : 0, 0}};
        for (size_t i = 0; i < 3; i++) {
            if (!p[i].events) {
                p[i].fd = -1;
            }
        }
        int ready = poll(p, 3, 25);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 5;
        }
        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && !read_pending(STDIN_FILENO, &input)) {
            return 5;
        }
        if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) && !output.used && !output.eof &&
            !read_pending(socket, &output)) {
            return 5;
        }
        if ((p[1].revents & POLLOUT) && input.used && !write_pending(socket, &input)) {
            return 5;
        }
        if ((p[2].revents & (POLLOUT | POLLHUP | POLLERR)) && output.used &&
            !write_pending(STDOUT_FILENO, &output)) {
            return 5;
        }
        if ((p[0].revents | p[1].revents | p[2].revents) & POLLNVAL) {
            return 5;
        }
    }
    return *cancelled ? 130 : 5;
}

int gterm_ipc_attach(const char *directory,
                     const char *instance,
                     int argc,
                     char **argv,
                     volatile sig_atomic_t *cancelled) {
    endpoint_t e = {.directory = -1, .listener = -1, .peer = -1, .cwd = -1};
    permit_t permit = {0};
    int result = 5;
    struct stat input, output, socket_info;
    if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO) || fstat(STDIN_FILENO, &input) ||
        fstat(STDOUT_FILENO, &output) || (!S_ISFIFO(input.st_mode) && !S_ISSOCK(input.st_mode)) ||
        (!S_ISFIFO(output.st_mode) && !S_ISSOCK(output.st_mode))) {
        return error();
    }
    if (!open_directory(&e, directory) || !ignore_pipe() || !read_permit(&e, &permit) ||
        !instance_matches(&permit, instance)) {
        goto done;
    }
    if (fstatat(e.directory, socket_name, &socket_info, AT_SYMLINK_NOFOLLOW) ||
        !S_ISSOCK(socket_info.st_mode) || socket_info.st_uid != geteuid() ||
        (socket_info.st_mode & 0077)) {
        goto done;
    }
    e.peer = local_socket();
    if (e.peer < 0) {
        goto done;
    }
    struct sockaddr_un a = address();
    uint64_t end = now();
    if (!end) {
        goto done;
    }
    end += 5000;
    if (connect(e.peer, (struct sockaddr *)&a, sizeof(a))) {
        if (errno != EINPROGRESS || !wait_fd(e.peer, POLLOUT, end, cancelled)) {
            goto done;
        }
        int failure = 0;
        socklen_t size = sizeof(failure);
        if (getsockopt(e.peer, SOL_SOCKET, SO_ERROR, &failure, &size) || failure) {
            goto done;
        }
    }
    if (!peer_user(e.peer) || !entry(e.directory, socket_name, &socket_info) ||
        !exchange(e.peer, &permit, sizeof(permit), true, end, cancelled) ||
        !invocation(e.peer, argc, argv, true, end, cancelled)) {
        goto done;
    }
    memset(&permit, 0, sizeof(permit));
    unsigned char ack[8];
    if (!exchange(e.peer, ack, sizeof(ack), false, end, cancelled) ||
        memcmp(ack, acknowledged, sizeof(ack)) || fchdir(e.cwd) || !nonblocking(STDIN_FILENO) ||
        !nonblocking(STDOUT_FILENO)) {
        goto done;
    }
    result = pump(e.peer, cancelled);
done:
    memset(&permit, 0, sizeof(permit));
    if (!cleanup(&e)) {
        fputs("gterm: local IPC cleanup incomplete\n", stderr);
        return 5;
    }
    if (*cancelled) {
        return 130;
    }
    return result == 5 ? error() : result;
}
