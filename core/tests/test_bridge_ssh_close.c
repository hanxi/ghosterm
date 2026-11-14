/* Deterministic libssh2 disposal continuations. Only the external calls at the
 * close boundary are replaced; the complete production stream owner is built.
 * The sentinel channel is never dereferenced: invalid reuse is an ordinary
 * assertion failure, independently of allocator reuse or sanitizer behavior. */
#define _POSIX_C_SOURCE 200809L
#include "extension/backends/ssh_exec.h"
#include "internal.h"
#include <libssh2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static struct {
    unsigned aborts, reads, writes, disposals, late_calls;
    bool parent_pending, invalid, blocked;
} fixture;

static void close_abort(gt_ssh_t *);
static gt_status_t close_dispose(gt_ssh_t **, bool *);
static ssize_t close_read(LIBSSH2_CHANNEL *, int, char *, size_t);
static ssize_t close_write(LIBSSH2_CHANNEL *, int, const char *, size_t);

#define gt__ssh_abort close_abort
#define gt__ssh_dispose_step close_dispose
#define libssh2_channel_read_ex close_read
#define libssh2_channel_write_ex close_write
#include "../src/extension/backends/ssh_exec.c"
#undef gt__ssh_abort
#undef gt__ssh_dispose_step
#undef libssh2_channel_read_ex
#undef libssh2_channel_write_ex

static void close_abort(gt_ssh_t *ssh) {
    CHECK(ssh == (gt_ssh_t *)&fixture);
    fixture.aborts++;
}

static gt_status_t close_dispose(gt_ssh_t **owned, bool *released) {
    CHECK(owned && *owned == (gt_ssh_t *)&fixture && released);
    fixture.disposals++;
    /* session_free can free the channel then return EAGAIN; alternatively the
     * entire session is gone but disposal of the enclosing jump is pending. */
    fixture.invalid = true;
    if (fixture.disposals < 4) {
        *released = false;
    } else {
        *owned = NULL;
        *released = true;
    }
    return GT_OK;
}

static ssize_t close_read(LIBSSH2_CHANNEL *channel, int stream, char *p, size_t n) {
    CHECK(channel == (LIBSSH2_CHANNEL *)&fixture && !stream && p && n);
    fixture.reads++;
    if (fixture.invalid) {
        fixture.late_calls++;
    }
    return LIBSSH2_ERROR_SOCKET_RECV;
}

static ssize_t close_write(LIBSSH2_CHANNEL *channel, int stream, const char *p, size_t n) {
    CHECK(channel == (LIBSSH2_CHANNEL *)&fixture && !stream && n == 1 && *p == 'x');
    fixture.writes++;
    if (fixture.invalid) {
        fixture.late_calls++;
    }
    return LIBSSH2_ERROR_SOCKET_SEND;
}

int main(int argc, char **argv) {
    CHECK(argc == 2 && (!strcmp(argv[1], "session") || !strcmp(argv[1], "parent") ||
                        !strcmp(argv[1], "blocked")));
    fixture.parent_pending = !strcmp(argv[1], "parent");
    fixture.blocked = !strcmp(argv[1], "blocked");
    gt_plugin_memory_info_t before = gt__plugin_memory_info();
    gt_allocator_t allocator = {NULL, allocate, release};
    ssh_stream_t *s = gt__plugin_memory_alloc(&allocator, sizeof(*s));
    CHECK(s);
    memset(s, 0, sizeof(*s));
    s->allocator = allocator;
    CHECK(!pipe(s->wake) && gt__bridge_stream_init(&s->base, &ops) == GT_OK);
    s->ssh = (gt_ssh_t *)&fixture;
    s->session = (LIBSSH2_SESSION *)&fixture;
    s->channel = (LIBSSH2_CHANNEL *)&fixture;
    s->info.phase = GT_BRIDGE_SSH_READY;
    if (fixture.blocked) {
        s->tx[0] = 'x';
        s->tx_size = 1;
        s->blocked = PUMP_WRITE;
    }
    for (unsigned i = 0; i < 3; i++) {
        CHECK(gt__bridge_stream_close(&s->base, 0) == GT_BRIDGE_DRAINING);
        CHECK(!fixture.late_calls);
        CHECK(fixture.reads == 1 && fixture.writes == (fixture.blocked ? 1u : 0u));
        CHECK(fixture.aborts == 1 && s->ssh && !s->base.closed);
        if (fixture.parent_pending && !i) {
            /* Same postcondition as ssh->session==NULL and ssh->wire!=NULL:
             * libssh2's session/channel are gone, outer owner is still live. */
            s->session = NULL;
        }
    }
    CHECK(gt__bridge_stream_close(&s->base, 0) == GT_BRIDGE_RELEASED && !s->ssh && !s->session &&
          !s->channel && s->info.phase == GT_BRIDGE_SSH_CLOSED);
    CHECK(gt__bridge_stream_close(&s->base, 0) == GT_BRIDGE_RELEASED);
    CHECK(fixture.disposals == 4 && !fixture.late_calls && fixture.aborts == 1);
    CHECK(gt__bridge_stream_destroy(&s->base) == GT_OK);
    CHECK(gt__plugin_memory_info().used == before.used);
    printf("PASS bridge_ssh_close_%s\n", argv[1]);
    return 0;
}
