#define _POSIX_C_SOURCE 200809L
#include "extension/frame.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void delay(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t)) {
        CHECK(errno == EINTR);
    }
}

static void pair(bool socket_mode, gt_bridge_stream_t **a, gt_bridge_stream_t **b) {
    if (socket_mode) {
        int fd[2];
        CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
        int size = 4096;
        CHECK(!setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)));
        CHECK(!setsockopt(fd[1], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)));
        CHECK(gt__bridge_fd_open(fd[0], fd[0], NULL, a) == GT_OK);
        CHECK(gt__bridge_fd_open(fd[1], fd[1], NULL, b) == GT_OK);
    } else {
        int ab[2], ba[2];
        CHECK(!pipe(ab) && !pipe(ba));
        CHECK(gt__bridge_fd_open(ba[0], ab[1], NULL, a) == GT_OK);
        CHECK(gt__bridge_fd_open(ab[0], ba[1], NULL, b) == GT_OK);
    }
}

static void dispose(gt_bridge_stream_t *s) {
    CHECK(gt__bridge_stream_close(s, 0) == GT_BRIDGE_RELEASED);
    CHECK(gt__bridge_stream_close(s, 0) == GT_BRIDGE_RELEASED);
    uint8_t byte = 0;
    CHECK(gt__bridge_stream_write(s, &byte, 1, 0).state == GT_BRIDGE_CLOSED);
    CHECK(gt__bridge_stream_read(s, &byte, 1, 0).state == GT_BRIDGE_CLOSED);
    CHECK(gt__bridge_stream_destroy(s) == GT_OK);
}

static uint32_t random_step(uint32_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

static void conformance(bool socket_mode) {
    gt_bridge_stream_t *a, *b;
    pair(socket_mode, &a, &b);
    uint8_t bytes[4096];
    CHECK(gt__bridge_stream_read(b, bytes, sizeof(bytes), 0).state == GT_BRIDGE_WAIT);
    gt_bridge_writer_t *w;
    gt_bridge_decoder_t *d;
    CHECK(gt__bridge_writer_new(GT_BRIDGE_FRAME_MAX, NULL, &w) == GT_OK);
    CHECK(gt__bridge_decoder_new(GT_BRIDGE_FRAME_MAX, NULL, &d) == GT_OK);
    uint8_t *payload = malloc(GT_BRIDGE_FRAME_MAX);
    CHECK(payload);
    for (size_t i = 0; i < GT_BRIDGE_FRAME_MAX; i++) {
        payload[i] = (uint8_t)(i * 17u);
    }
    CHECK(gt__bridge_writer_queue(w, payload, GT_BRIDGE_FRAME_MAX) == GT_OK);
    memset(payload, 0, GT_BRIDGE_FRAME_MAX); /* writer owns an immutable copy */
    bool waited = false;
    size_t accepted = 0;
    for (int i = 0; i < 100; i++) {
        gt_bridge_io_t r = gt__bridge_writer_step(w, a, 0);
        if (r.state == GT_BRIDGE_WAIT) {
            waited = true;
            break;
        }
        CHECK(r.state == GT_BRIDGE_DATA && r.count <= GT_BRIDGE_IO_QUANTUM);
        accepted += r.count;
    }
    CHECK(waited && accepted > 0 && gt__bridge_writer_pending(w) > 0);
    size_t pending = gt__bridge_writer_pending(w);
    CHECK(gt__bridge_writer_queue(w, "cancel", 6) == GT_ERR_INVALID_ARG);
    CHECK(gt__bridge_writer_pending(w) == pending);
    uint32_t seed = 173;
    size_t received = 0;
    bool complete = false;
    for (int turn = 0; turn < 20000 && !complete; turn++) {
        if (gt__bridge_writer_pending(w)) {
            gt_bridge_io_t r = gt__bridge_writer_step(w, a, 0);
            CHECK(r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT);
            if (r.state == GT_BRIDGE_DATA) {
                accepted += r.count;
            }
        }
        gt_bridge_io_t r =
            gt__bridge_stream_read(b, bytes, random_step(&seed) % sizeof(bytes) + 1, 0);
        CHECK(r.state == GT_BRIDGE_DATA || r.state == GT_BRIDGE_WAIT);
        if (r.state == GT_BRIDGE_WAIT) {
            continue;
        }
        received += r.count;
        size_t used;
        CHECK(gt__bridge_decoder_feed(d, bytes, r.count, &used) == GT_OK && used == r.count);
        const uint8_t *view;
        size_t n;
        complete = gt__bridge_decoder_view(d, &view, &n);
        if (complete) {
            CHECK(n == GT_BRIDGE_FRAME_MAX);
            for (size_t i = 0; i < n; i++) {
                CHECK(view[i] == (uint8_t)(i * 17u));
            }
        }
    }
    CHECK(complete && accepted == GT_BRIDGE_FRAME_MAX + 4 && received == accepted);
    CHECK(gt__bridge_writer_pending(w) == 0);
    CHECK(gt__bridge_decoder_consume(d) == GT_OK);
    /* Same framing in the reverse direction, independent of the original pane. */
    CHECK(gt__bridge_writer_queue(w, "reply", 5) == GT_OK);
    CHECK(gt__bridge_writer_step(w, b, 100).state == GT_BRIDGE_DATA);
    gt_bridge_io_t r = gt__bridge_stream_read(a, bytes, sizeof(bytes), 100);
    CHECK(r.state == GT_BRIDGE_DATA && r.count == 9 && !memcmp(bytes, "\0\0\0\5reply", 9));
    CHECK(gt__bridge_stream_write(a, "TAIL", 4, 0).count == 4);
    CHECK(gt__bridge_stream_close(a, 0) == GT_BRIDGE_RELEASED);
    r = gt__bridge_stream_read(b, bytes, sizeof(bytes), 100);
    CHECK(r.state == GT_BRIDGE_DATA && r.count == 4 && !memcmp(bytes, "TAIL", 4));
    CHECK(gt__bridge_stream_read(b, bytes, sizeof(bytes), 100).state == GT_BRIDGE_EOF);
    CHECK(gt__bridge_decoder_finish(d) == GT_OK);
    gt__bridge_writer_free(w);
    gt__bridge_decoder_free(d);
    free(payload);
    dispose(a);
    dispose(b);
}

static void prefix(uint8_t *p, size_t n) {
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}

static void codec(void) {
    enum {
        FRAMES = 120
    };

    size_t sizes[FRAMES], total = 0;
    uint32_t seed = 17113;
    for (size_t i = 0; i < FRAMES; i++) {
        sizes[i] = i == 0 ? GT_BRIDGE_FRAME_MAX : random_step(&seed) % 2048 + 1;
        total += sizes[i] + 4;
    }
    uint8_t *wire = malloc(total);
    CHECK(wire);
    size_t offset = 0;
    for (size_t i = 0; i < FRAMES; i++) {
        prefix(wire + offset, sizes[i]);
        offset += 4;
        for (size_t j = 0; j < sizes[i]; j++) {
            wire[offset++] = (uint8_t)(i + j);
        }
    }
    gt_bridge_decoder_t *d;
    CHECK(gt__bridge_decoder_new(GT_BRIDGE_FRAME_MAX, NULL, &d) == GT_OK);
    offset = 0;
    size_t frame = 0;
    while (offset < total) {
        size_t n = random_step(&seed) % 511 + 1;
        if (n > total - offset) {
            n = total - offset;
        }
        size_t used;
        CHECK(gt__bridge_decoder_feed(d, wire + offset, n, &used) == GT_OK && used > 0 &&
              used <= n);
        offset += used;
        const uint8_t *view;
        size_t size;
        if (gt__bridge_decoder_view(d, &view, &size)) {
            CHECK(frame < FRAMES && size == sizes[frame]);
            for (size_t j = 0; j < size; j++) {
                CHECK(view[j] == (uint8_t)(frame + j));
            }
            CHECK(gt__bridge_decoder_feed(d, "x", 1, &used) == GT_ERR_INVALID_ARG && !used);
            if (frame == FRAMES - 1) {
                CHECK(gt__bridge_decoder_finish(d) == GT_OK);
            }
            CHECK(gt__bridge_decoder_consume(d) == GT_OK);
            frame++;
        }
    }
    CHECK(frame == FRAMES && gt__bridge_decoder_finish(d) == GT_OK);
    size_t used = 99;
    CHECK(gt__bridge_decoder_feed(d, wire, 1, &used) == GT_ERR_INVALID_ARG && !used);
    gt__bridge_decoder_free(d);
    free(wire);
    /* Exhaust all truncation points of both header and body. */
    const uint8_t small[] = {0, 0, 0, 5, 'h', 'e', 'l', 'l', 'o'};
    for (size_t cut = 0; cut <= sizeof(small); cut++) {
        CHECK(gt__bridge_decoder_new(5, NULL, &d) == GT_OK);
        CHECK(gt__bridge_decoder_feed(d, small, cut, &used) == GT_OK && used == cut);
        gt_status_t expected = cut == 0 || cut == sizeof(small) ? GT_OK : GT_ERR_PROTOCOL;
        CHECK(gt__bridge_decoder_finish(d) == expected);
        CHECK(gt__bridge_decoder_finish(d) == expected);
        gt__bridge_decoder_free(d);
    }
    CHECK(gt__bridge_decoder_new(5, NULL, &d) == GT_OK);
    CHECK(gt__bridge_decoder_feed(d, small, sizeof(small), &used) == GT_OK);
    CHECK(gt__bridge_decoder_consume(d) == GT_OK);
    CHECK(gt__bridge_decoder_feed(d, small, 1, &used) == GT_OK);
    CHECK(gt__bridge_decoder_finish(d) == GT_ERR_PROTOCOL);
    gt__bridge_decoder_free(d);
}

static void limits(void) {
    const uint8_t bad[][4] = {
        {0, 0, 0, 0}, {0, 1, 0, 1}, {255, 255, 255, 255}, {'W', 'e', 'l', 'c'}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        gt_bridge_decoder_t *d;
        CHECK(gt__bridge_decoder_new(GT_BRIDGE_FRAME_MAX, NULL, &d) == GT_OK);
        size_t used;
        CHECK(gt__bridge_decoder_feed(d, bad[i], 4, &used) == GT_ERR_PROTOCOL && used == 4);
        const uint8_t valid[] = {0, 0, 0, 1, 'x'};
        CHECK(gt__bridge_decoder_feed(d, valid, sizeof(valid), &used) == GT_ERR_PROTOCOL && !used);
        CHECK(gt__bridge_decoder_finish(d) == GT_ERR_PROTOCOL);
        gt__bridge_decoder_free(d);
    }
    gt_bridge_decoder_t *d;
    CHECK(gt__bridge_decoder_new(0, NULL, &d) == GT_ERR_INVALID_ARG && !d);
    CHECK(gt__bridge_decoder_new(GT_BRIDGE_FRAME_MAX + 1, NULL, &d) == GT_ERR_INVALID_ARG && !d);
    CHECK(gt__bridge_decoder_new(4, NULL, &d) == GT_OK);
    size_t used;
    const uint8_t over[] = {0, 0, 0, 5};
    CHECK(gt__bridge_decoder_feed(d, over, 4, &used) == GT_ERR_PROTOCOL);
    gt__bridge_decoder_free(d);
    gt_bridge_writer_t *w;
    CHECK(gt__bridge_writer_new(4, NULL, &w) == GT_OK);
    CHECK(gt__bridge_writer_queue(w, "large", 5) == GT_ERR_INVALID_ARG);
    CHECK(gt__bridge_writer_queue(w, "", 0) == GT_ERR_INVALID_ARG);
    CHECK(!gt__bridge_writer_pending(w));
    gt__bridge_writer_free(w);
}

typedef struct {
    size_t live, calls, fail, allocations;
} budget_t;

typedef union {
    max_align_t alignment;
    size_t size;
} allocation_header_t;

static void *budget_allocate(void *ctx, size_t n) {
    budget_t *b = ctx;
    if (++b->calls == b->fail) {
        return NULL;
    }
    allocation_header_t *p = malloc(sizeof(*p) + n);
    CHECK(p);
    p->size = n;
    b->live += n;
    b->allocations++;
    return p + 1;
}

static void budget_release(void *ctx, void *p, size_t n) {
    budget_t *b = ctx;
    allocation_header_t *allocation = (allocation_header_t *)p - 1;
    CHECK(allocation->size == n && b->live >= n && b->allocations);
    b->live -= n;
    b->allocations--;
    free(allocation);
}

static void allocation(void) {
    budget_t b = {0};
    gt_allocator_t a = {&b, budget_allocate, budget_release};
    for (size_t fail = 1; fail <= 2; fail++) {
        b.calls = 0;
        b.fail = fail;
        gt_bridge_decoder_t *d;
        gt_bridge_writer_t *w;
        CHECK(gt__bridge_decoder_new(64, &a, &d) == GT_ERR_IO && !d && !b.live);
        b.calls = 0;
        CHECK(gt__bridge_writer_new(64, &a, &w) == GT_ERR_IO && !w && !b.live);
    }
    b.calls = 0;
    b.fail = 0;
    gt_bridge_decoder_t *d;
    gt_bridge_writer_t *w;
    CHECK(gt__bridge_decoder_new(64, &a, &d) == GT_OK);
    CHECK(gt__bridge_writer_new(64, &a, &w) == GT_OK);
    size_t calls = b.calls;
    const uint8_t frame[] = {0, 0, 0, 1, 'x'};
    for (int i = 0; i < 1000; i++) {
        size_t used;
        CHECK(gt__bridge_decoder_feed(d, frame, sizeof(frame), &used) == GT_OK);
        CHECK(gt__bridge_decoder_consume(d) == GT_OK && b.calls == calls);
    }
    gt__bridge_decoder_free(d);
    gt__bridge_writer_free(w);
    CHECK(!b.live && !b.allocations);
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    int flags = fcntl(fd[0], F_GETFL), fdflags = fcntl(fd[0], F_GETFD);
    b.fail = b.calls + 1;
    gt_bridge_stream_t *s;
    CHECK(gt__bridge_fd_open(fd[0], fd[0], &a, &s) == GT_ERR_IO && !s);
    CHECK(fcntl(fd[0], F_GETFL) == flags && fcntl(fd[0], F_GETFD) == fdflags);
    b.fail = 0;
    CHECK(gt__bridge_fd_open(fd[0], fd[0], &a, &s) == GT_OK);
    CHECK(fcntl(fd[0], F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(fd[0], F_GETFD) & FD_CLOEXEC);
    CHECK(gt__bridge_stream_destroy(s) == GT_ERR_INVALID_ARG && b.live);
    dispose(s);
    close(fd[1]);
    CHECK(!b.live && !b.allocations);
}

typedef struct {
    gt_bridge_stream_t *stream;
    pthread_t owner;
    bool interrupt;
} wake_t;

static void noop(int sig) {
    (void)sig;
}

static void *wake_later(void *arg) {
    wake_t *w = arg;
    if (w->interrupt) {
        for (int i = 0; i < 150; i++) {
            delay(2);
            CHECK(!pthread_kill(w->owner, SIGUSR1));
        }
    } else {
        delay(30);
        gt__bridge_stream_stop(w->stream);
    }
    return NULL;
}

static void stop_case(void) {
    for (int mode = 0; mode < 2; mode++) {
        gt_bridge_stream_t *a, *b;
        pair(mode != 0, &a, &b);
        uint8_t bytes[GT_BRIDGE_IO_QUANTUM] = {0};
        /* Exercise both a blocked reader and a backpressured writer. */
        for (int writing = 0; writing < 2; writing++) {
            if (writing) {
                pair(mode != 0, &a, &b);
                for (int i = 0;; i++) {
                    CHECK(i < 1000);
                    gt_bridge_io_t r = gt__bridge_stream_write(a, bytes, sizeof(bytes), 0);
                    if (r.state == GT_BRIDGE_WAIT) {
                        break;
                    }
                    CHECK(r.state == GT_BRIDGE_DATA);
                }
            }
            wake_t wake = {.stream = a};
            pthread_t thread;
            CHECK(!pthread_create(&thread, NULL, wake_later, &wake));
            int64_t start = now();
            gt_bridge_io_t r = writing ? gt__bridge_stream_write(a, bytes, sizeof(bytes), 5000)
                                       : gt__bridge_stream_read(a, bytes, sizeof(bytes), 5000);
            CHECK(r.state == GT_BRIDGE_CLOSED && r.error == GT_ERR_CANCELLED &&
                  now() - start < 1000);
            CHECK(!pthread_join(thread, NULL));
            CHECK(gt__bridge_stream_write(a, bytes, 1, 0).state == GT_BRIDGE_CLOSED);
            dispose(a);
            dispose(b);
        }
    }
}

static void deadline(void) {
    struct sigaction action = {0}, old;
    action.sa_handler = noop;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1, &action, &old));
    gt_bridge_stream_t *a, *b;
    pair(true, &a, &b);
    wake_t wake = {.owner = pthread_self(), .interrupt = true};
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, wake_later, &wake));
    uint8_t byte;
    int64_t start = now();
    CHECK(gt__bridge_stream_read(a, &byte, 1, 100).state == GT_BRIDGE_WAIT);
    int64_t elapsed = now() - start;
    CHECK(elapsed >= 75 && elapsed < 250); /* EINTR must not restart 100ms each time. */
    CHECK(!pthread_join(thread, NULL));
    CHECK(gt__bridge_stream_write(b, "x", 1, 0).count == 1);
    CHECK(gt__bridge_stream_read(a, &byte, 1, 0).state == GT_BRIDGE_DATA && byte == 'x');
    dispose(a);
    dispose(b);
    CHECK(!sigaction(SIGUSR1, &old, NULL));
}

/* Controlled provider faults test facade checks and delayed release. Real FD
 * conformance above remains the evidence for actual IO and backpressure. */
typedef struct {
    gt_bridge_stream_t base;
    int calls, wakes, closes;
    bool destroyed;
    gt_bridge_io_t value;
} fake_t;

static gt_bridge_io_t fake_read(gt_bridge_stream_t *s, void *p, size_t n, int ms) {
    (void)p;
    (void)n;
    (void)ms;
    fake_t *f = (fake_t *)s;
    f->calls++;
    return f->value;
}

static gt_bridge_io_t fake_write(gt_bridge_stream_t *s, const void *p, size_t n, int ms) {
    return fake_read(s, (void *)p, n, ms);
}

static void fake_wake(gt_bridge_stream_t *s) {
    ((fake_t *)s)->wakes++;
}

static gt_bridge_close_t fake_close(gt_bridge_stream_t *s, int ms) {
    (void)ms;
    return ++((fake_t *)s)->closes == 1 ? GT_BRIDGE_DRAINING : GT_BRIDGE_RELEASED;
}

static void fake_destroy(gt_bridge_stream_t *s) {
    ((fake_t *)s)->destroyed = true;
}

static const gt_bridge_stream_ops_t fake_ops = {
    fake_read, fake_write, fake_wake, fake_close, fake_destroy};

static void lifecycle(void) {
    fake_t f = {0};
    CHECK(gt__bridge_stream_init(&f.base, &fake_ops) == GT_OK);
    uint8_t byte;
    CHECK(gt__bridge_stream_close(&f.base, 0) == GT_BRIDGE_DRAINING);
    CHECK(gt__bridge_stream_destroy(&f.base) == GT_ERR_INVALID_ARG && !f.destroyed);
    CHECK(gt__bridge_stream_write(&f.base, "x", 1, 0).state == GT_BRIDGE_CLOSED && f.calls == 0);
    CHECK(gt__bridge_stream_read(&f.base, &byte, 1, 0).state == GT_BRIDGE_CLOSED && f.calls == 0);
    gt__bridge_stream_stop(&f.base);
    CHECK(f.wakes == 1);
    CHECK(gt__bridge_stream_close(&f.base, 0) == GT_BRIDGE_RELEASED);
    CHECK(gt__bridge_stream_close(&f.base, 0) == GT_BRIDGE_RELEASED && f.closes == 2);
    CHECK(gt__bridge_stream_destroy(&f.base) == GT_OK && f.destroyed);
    /* Short write, WAIT, then cancellation must never consume or replay a tail. */
    fake_t a = {0}, b = {0};
    CHECK(gt__bridge_stream_init(&a.base, &fake_ops) == GT_OK);
    CHECK(gt__bridge_stream_init(&b.base, &fake_ops) == GT_OK);
    gt_bridge_writer_t *w;
    CHECK(gt__bridge_writer_new(32, NULL, &w) == GT_OK);
    CHECK(gt__bridge_writer_queue(w, "abc", 3) == GT_OK);
    a.value = (gt_bridge_io_t){GT_BRIDGE_DATA, 2, GT_OK};
    CHECK(gt__bridge_writer_step(w, &a.base, 0).count == 2 && gt__bridge_writer_pending(w) == 5);
    a.value = (gt_bridge_io_t){GT_BRIDGE_WAIT, 0, GT_OK};
    CHECK(gt__bridge_writer_step(w, &a.base, 0).state == GT_BRIDGE_WAIT &&
          gt__bridge_writer_pending(w) == 5);
    CHECK(gt__bridge_writer_step(w, &b.base, 0).error == GT_ERR_INVALID_ARG && b.calls == 0);
    gt__bridge_stream_stop(&a.base);
    CHECK(gt__bridge_writer_step(w, &a.base, 0).state == GT_BRIDGE_CLOSED && a.calls == 2);
    CHECK(gt__bridge_writer_step(w, &b.base, 0).state == GT_BRIDGE_CLOSED && b.calls == 0);
    CHECK(gt__bridge_stream_close(&a.base, 0) == GT_BRIDGE_DRAINING);
    CHECK(gt__bridge_stream_close(&a.base, 0) == GT_BRIDGE_RELEASED);
    gt__bridge_writer_free(w);
}

static void invalid(void) {
    fake_t f = {0};
    CHECK(gt__bridge_stream_init(&f.base, &fake_ops) == GT_OK);
    const gt_bridge_io_t bad[] = {{GT_BRIDGE_DATA, 0, GT_OK},
                                  {GT_BRIDGE_DATA, 2, GT_OK},
                                  {GT_BRIDGE_WAIT, 1, GT_OK},
                                  {GT_BRIDGE_WAIT, 0, GT_ERR_IO},
                                  {GT_BRIDGE_ERROR, 0, GT_OK},
                                  {GT_BRIDGE_EOF, 0, GT_OK},
                                  {(gt_bridge_io_state_t)99, 0, GT_OK},
                                  {GT_BRIDGE_DATA, 1, GT_ERR_IO}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        f.value = bad[i];
        CHECK(gt__bridge_stream_write(&f.base, "x", 1, 0).error == GT_ERR_INTERNAL);
    }
    uint8_t byte;
    CHECK(gt__bridge_stream_read(&f.base, &byte, 1, -1).error == GT_ERR_INVALID_ARG);
    CHECK(gt__bridge_stream_write(&f.base, NULL, 1, 0).error == GT_ERR_INVALID_ARG);
    CHECK(gt__bridge_stream_close(&f.base, -1) == GT_BRIDGE_CLOSE_ERROR);
    CHECK(gt__bridge_stream_init(NULL, &fake_ops) == GT_ERR_INVALID_ARG);
    int fd[2];
    CHECK(!pipe(fd));
    gt_bridge_stream_t *s;
    CHECK(gt__bridge_fd_open(fd[1], fd[0], NULL, &s) == GT_ERR_INVALID_ARG && !s);
    CHECK(gt__bridge_fd_open(fd[0], fd[0], NULL, &s) == GT_ERR_INVALID_ARG && !s);
    CHECK(fcntl(fd[0], F_GETFD) >= 0 && fcntl(fd[1], F_GETFD) >= 0);
    close(fd[0]);
    close(fd[1]);
    CHECK(!socketpair(AF_UNIX, SOCK_DGRAM, 0, fd));
    CHECK(gt__bridge_fd_open(fd[0], fd[0], NULL, &s) == GT_ERR_INVALID_ARG && !s);
    close(fd[0]);
    close(fd[1]);
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    struct linger linger = {1, 5};
    CHECK(!setsockopt(fd[0], SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)));
    CHECK(gt__bridge_fd_open(fd[0], fd[0], NULL, &s) == GT_ERR_INVALID_ARG && !s);
    close(fd[0]);
    close(fd[1]);
    /* SIGPIPE disposition is deliberately default: a closed pipe must return IO
   * rather than kill the host process or alter its global signal handler. */
    struct sigaction action = {0}, old;
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGPIPE, &action, &old));
    gt_bridge_stream_t *a, *b;
    pair(false, &a, &b);
    dispose(b);
    gt_bridge_io_t r = gt__bridge_stream_write(a, "x", 1, 0);
    CHECK(r.state == GT_BRIDGE_ERROR && r.error == GT_ERR_IO);
    struct sigaction after;
    CHECK(!sigaction(SIGPIPE, NULL, &after) && after.sa_handler == SIG_DFL);
    dispose(a);
    CHECK(!sigaction(SIGPIPE, &old, NULL));
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "pipe")) {
        conformance(false);
    } else if (!strcmp(argv[1], "socket")) {
        conformance(true);
    } else if (!strcmp(argv[1], "codec")) {
        codec();
    } else if (!strcmp(argv[1], "limits")) {
        limits();
    } else if (!strcmp(argv[1], "allocation")) {
        allocation();
    } else if (!strcmp(argv[1], "stop")) {
        stop_case();
    } else if (!strcmp(argv[1], "deadline")) {
        deadline();
    } else if (!strcmp(argv[1], "lifecycle")) {
        lifecycle();
    } else if (!strcmp(argv[1], "invalid")) {
        invalid();
    } else {
        CHECK(false);
    }
    printf("PASS bridge_stream_%s\n", argv[1]);
    return 0;
}
