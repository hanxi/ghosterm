/* Real SFTP deletions plus deterministic acknowledgement/metadata fault hooks. */
#include "ghosterm.h"
#include "internal.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static LIBSSH2_SFTP_HANDLE *
checked_open(LIBSSH2_SFTP *, const char *, unsigned, unsigned long, long, int);
static int checked_unlock(pthread_mutex_t *);
static int checked_unlink(LIBSSH2_SFTP *, const char *, unsigned);
static int checked_rmdir(LIBSSH2_SFTP *, const char *, unsigned);
static int checked_stat(LIBSSH2_SFTP *, const char *, unsigned, int, LIBSSH2_SFTP_ATTRIBUTES *);
static unsigned long checked_error(LIBSSH2_SFTP *);
#define libssh2_sftp_open_ex checked_open
#define pthread_mutex_unlock checked_unlock
#define libssh2_sftp_unlink_ex checked_unlink
#define libssh2_sftp_rmdir_ex checked_rmdir
#define libssh2_sftp_stat_ex checked_stat
#define libssh2_sftp_last_error checked_error
#include "../src/sftp.c"
#undef libssh2_sftp_open_ex
#undef pthread_mutex_unlock
#undef libssh2_sftp_unlink_ex
#undef libssh2_sftp_rmdir_ex
#undef libssh2_sftp_stat_ex
#undef libssh2_sftp_last_error
static int failures, calls, target_checks;
static unsigned long override_error;
static bool stopping;
static atomic_bool upload_started, upload_release, release_on_unlock;
static gt_sftp_t *active_sftp;
static pthread_t controller;
static const char *mode;
static char watched[8192];
#define CHECK(c, m)                                                                                \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            puts("FAIL: remove " m);                                                               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static bool cancelled(void *unused) {
    (void)unused;
    return stopping;
}

static int64_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static gt_hostkey_decision_t
trust(void *a, const char *b, uint16_t c, const char *d, const char *e) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return GT_HOSTKEY_ACCEPT;
}

static gt_ssh_t *connect_fixture(char **argv) {
    char known[4096];
    snprintf(known, sizeof known, "%s/known", argv[5]);
    gt_ssh_opts_t o = {.host = argv[1],
                       .port = (uint16_t)atoi(argv[2]),
                       .username = argv[3],
                       .known_hosts_path = known};
    gt_ssh_t *ssh = gt_ssh_new(&o);
    FILE *f = fopen(argv[4], "rb");
    if (!ssh || !f) {
        abort();
    }
    char key[16384];
    size_t n = fread(key, 1, sizeof key - 1, f);
    if (ferror(f) || !feof(f) || !n || fclose(f)) {
        abort();
    }
    key[n] = 0;
    int rc = gt_ssh_auth_private_key(ssh, key, NULL);
    volatile char *wipe = key;
    for (size_t i = 0; i < sizeof key; i++) {
        wipe[i] = 0;
    }
    if (rc) {
        abort();
    }
    gt_ssh_set_hostkey_policy(ssh, trust, NULL);
    if (gt_ssh_connect(ssh)) {
        abort();
    }
    return ssh;
}

static void write_file(const char *path, const char *value) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(value, 1, strlen(value), f) != strlen(value) || fclose(f)) {
        abort();
    }
}

static bool bytes_equal(const char *path, const char *value) {
    char bytes[64];
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(bytes, 1, sizeof bytes, f);
    bool good = !ferror(f) && feof(f) && n == strlen(value) && !memcmp(bytes, value, n);
    return !fclose(f) && good;
}

static LIBSSH2_SFTP_HANDLE *checked_open(
    LIBSSH2_SFTP *s, const char *p, unsigned n, unsigned long flags, long permissions, int type) {
    if (!strcmp(mode, "jobs") && (flags & LIBSSH2_FXF_WRITE)) {
        atomic_store(&upload_started, true);
        while (!atomic_load(&upload_release)) {
            usleep(1000);
        }
    }
    return libssh2_sftp_open_ex(s, p, n, flags, permissions, type);
}

static int checked_unlock(pthread_mutex_t *mutex) {
    int result = pthread_mutex_unlock(mutex);
    if (active_sftp && pthread_equal(pthread_self(), controller) && mutex == &active_sftp->qlock &&
        atomic_exchange(&release_on_unlock, false)) {
        atomic_store(&upload_release, true);
    }
    return result;
}

static unsigned long checked_error(LIBSSH2_SFTP *s) {
    return override_error ? override_error : libssh2_sftp_last_error(s);
}

static int
checked_stat(LIBSSH2_SFTP *s, const char *path, unsigned n, int type, LIBSSH2_SFTP_ATTRIBUTES *a) {
    int result = libssh2_sftp_stat_ex(s, path, n, type, a);
    if (result == 0 && type == LIBSSH2_SFTP_LSTAT && !strcmp(path, watched)) {
        target_checks++;
        if (!strcmp(mode, "missing-mtime")) {
            a->flags &= ~LIBSSH2_SFTP_ATTR_ACMODTIME;
        }
        if (!strcmp(mode, "missing-size")) {
            a->flags &= ~LIBSSH2_SFTP_ATTR_SIZE;
        }
        if (!strcmp(mode, "missing-type")) {
            a->flags &= ~LIBSSH2_SFTP_ATTR_PERMISSIONS;
        }
        if (target_checks == 1 && !strcmp(mode, "second-change")) {
            write_file(path, "changed bytes");
        }
        if (!strcmp(mode, "cancel-before-delete")) {
            stopping = true;
        }
    }
    return result;
}

static int remove_call(LIBSSH2_SFTP *s, const char *path, unsigned n, bool directory) {
    calls++;
    if (!strcmp(mode, "unsupported")) {
        override_error = 8;
        return LIBSSH2_ERROR_SFTP_PROTOCOL;
    }
    if (!strcmp(mode, "unknown-code")) {
        override_error = 222;
        return LIBSSH2_ERROR_SFTP_PROTOCOL;
    }
    if (!strcmp(mode, "timeout")) {
        return LIBSSH2_ERROR_EAGAIN;
    }
    if (!strcmp(mode, "cancel-in-delete")) {
        stopping = true;
        return LIBSSH2_ERROR_EAGAIN;
    }
    int result = directory ? libssh2_sftp_rmdir_ex(s, path, n) : libssh2_sftp_unlink_ex(s, path, n);
    if (result == 0 && !strcmp(mode, "lost-reply")) {
        return LIBSSH2_ERROR_SOCKET_RECV;
    }
    if (result == 0 && !strcmp(mode, "late-cancel")) {
        stopping = true;
    }
    return result;
}

static int checked_unlink(LIBSSH2_SFTP *s, const char *p, unsigned n) {
    return remove_call(s, p, n, false);
}

static int checked_rmdir(LIBSSH2_SFTP *s, const char *p, unsigned n) {
    return remove_call(s, p, n, true);
}

static void run_case(char **argv, const char *selected) {
    mode = selected;
    calls = 0;
    target_checks = 0;
    stopping = false;
    override_error = 0;
    active_sftp = NULL;
    controller = pthread_self();
    atomic_store(&upload_started, false);
    atomic_store(&upload_release, false);
    atomic_store(&release_on_unlock, false);
    char name[128], child[16384], parent_link[8192];
    snprintf(name, sizeof name, "remove-%s", mode);
    snprintf(watched, sizeof watched, "%s/%s", argv[5], name);
    bool dir = !strcmp(mode, "directory") || !strcmp(mode, "nonempty"),
         link = !strcmp(mode, "symlink"), missing = !strcmp(mode, "absent");
    if (dir) {
        if (mkdir(watched, 0700)) {
            abort();
        }
        if (!strcmp(mode, "nonempty")) {
            snprintf(child, sizeof child, "%s/keep", watched);
            write_file(child, "child");
        }
    } else if (link) {
        snprintf(child, sizeof child, "%s/referent", argv[5]);
        write_file(child, "safe bytes");
        if (symlink(child, watched)) {
            abort();
        }
    } else if (!missing) {
        write_file(watched, "safe bytes");
    }
    struct stat st = {0};
    if (!missing && lstat(watched, &st)) {
        abort();
    }
    int64_t mtime = missing ? (int64_t)time(NULL) : (int64_t)st.st_mtime;
    const char *parent = argv[5];
    if (!strcmp(mode, "parent-link")) {
        snprintf(parent_link, sizeof parent_link, "%s/parent-link", argv[5]);
        if (symlink(argv[5], parent_link)) {
            abort();
        }
        parent = parent_link;
    }
    gt_ssh_t *ssh = connect_fixture(argv);
    gt_status_t opened;
    gt_sftp_t *sftp = gt_ssh_sftp_controlled(ssh, 3000, NULL, NULL, &opened);
    if (!sftp || opened) {
        abort();
    }
    active_sftp = sftp;
    gt_sftp_remove_result_t out = {GT_SFTP_REMOVE_REMOVED, 99};
    CHECK(gt_sftp_remove_controlled(
              sftp, parent, name, dir, dir ? 0 : 10, mtime, &out, 0, NULL, NULL) ==
                  GT_ERR_INVALID_ARG &&
              out.state == GT_SFTP_REMOVE_NOT_ATTEMPTED && !out.remote_status,
          "invalid budget clears result");
    CHECK(gt_sftp_remove_controlled(
              sftp, parent, name, false, UINT64_MAX, mtime, &out, 1000, NULL, NULL) ==
              GT_ERR_INVALID_ARG,
          "size range rejected");
    CHECK(gt_sftp_remove_controlled(sftp, parent, name, true, 1, mtime, &out, 1000, NULL, NULL) ==
              GT_ERR_INVALID_ARG,
          "directory size rejected");
    CHECK(gt_sftp_remove_controlled(sftp, parent, "..", false, 10, mtime, &out, 1000, NULL, NULL) ==
              GT_ERR_INVALID_ARG,
          "dot parent rejected");
    CHECK(gt_sftp_remove_controlled(
              sftp, parent, "x/y", false, 10, mtime, &out, 1000, NULL, NULL) == GT_ERR_INVALID_ARG,
          "single leaf required");
    CHECK(gt_sftp_remove_controlled(
              sftp, "relative", name, false, 10, mtime, &out, 1000, NULL, NULL) ==
              GT_ERR_INVALID_ARG,
          "canonical parent required");
    CHECK(gt_sftp_remove_controlled(sftp, parent, name, false, 10, -1, &out, 1000, NULL, NULL) ==
              GT_ERR_UNSUPPORTED,
          "unknown time is unsupported");
    if (failures) {
        gt_sftp_close(sftp);
        gt_ssh_close(ssh);
        return;
    }
    void *raw = NULL;
    int socket = -1;
    if (gt__ssh_borrow(ssh, &raw, &socket)) {
        abort();
    }
    libssh2_session_set_timeout(raw, 2345);
    if (!strcmp(mode, "jobs")) {
        char uploaded[16384];
        snprintf(uploaded, sizeof uploaded, "%s-job", watched);
        CHECK(gt_sftp_upload(sftp, watched, uploaded) != 0, "job fixture admitted");
        int64_t ready = now() + 1000;
        while (!atomic_load(&upload_started) && now() < ready) {
            usleep(1000);
        }
        if (!atomic_load(&upload_started)) {
            abort();
        }
    }
    if (!strcmp(mode, "pre-cancel")) {
        stopping = true;
    }
    int64_t calibration = now();
    for (int i = 0; i < 5; i++) {
        struct timespec t = {.tv_nsec = 10000000};
        nanosleep(&t, NULL);
    }
    calibration = now() - calibration;
    int64_t bound = calibration * 3 / 2 + 200;
    if (bound < 1000) {
        bound = 1000;
    }
    if (!strcmp(mode, "jobs")) {
        atomic_store(&release_on_unlock, true);
    }
    int64_t started = now();
    int status = gt_sftp_remove_controlled(sftp,
                                           parent,
                                           name,
                                           dir,
                                           dir                           ? 0
                                           : link                        ? (uint64_t)st.st_size
                                           : !strcmp(mode, "wrong-size") ? 11
                                                                         : 10,
                                           !strcmp(mode, "wrong-time") ? mtime + 1 : mtime,
                                           &out,
                                           !strcmp(mode, "timeout") ? 40 : 3000,
                                           cancelled,
                                           NULL);
    int64_t elapsed = now() - started;
    int wanted = GT_OK;
    gt_sftp_remove_state_t state = GT_SFTP_REMOVE_REMOVED;
    uint32_t remote = 0;
    if (!strcmp(mode, "jobs")) {
        wanted = GT_ERR_INVALID_ARG;
        state = GT_SFTP_REMOVE_NOT_ATTEMPTED;
    } else if (missing) {
        state = GT_SFTP_REMOVE_ABSENT;
        remote = 2;
    } else if (!strcmp(mode, "pre-cancel") || !strcmp(mode, "cancel-before-delete")) {
        wanted = GT_ERR_CANCELLED;
        state = GT_SFTP_REMOVE_NOT_ATTEMPTED;
    } else if (!strcmp(mode, "cancel-in-delete")) {
        wanted = GT_ERR_CANCELLED;
        state = GT_SFTP_REMOVE_UNKNOWN;
    } else if (!strcmp(mode, "wrong-size") || !strcmp(mode, "wrong-time") || link ||
               !strcmp(mode, "second-change")) {
        wanted = GT_ERR_IO;
        state = GT_SFTP_REMOVE_CHANGED;
    } else if (!strncmp(mode, "missing-", 8) || !strcmp(mode, "parent-link")) {
        wanted = GT_ERR_UNSUPPORTED;
        state = GT_SFTP_REMOVE_NOT_ATTEMPTED;
    } else if (!strcmp(mode, "nonempty")) {
        wanted = GT_ERR_IO;
        state = GT_SFTP_REMOVE_REJECTED;
        remote = 4;
    } else if (!strcmp(mode, "unsupported")) {
        wanted = GT_ERR_UNSUPPORTED;
        state = GT_SFTP_REMOVE_REJECTED;
        remote = 8;
    } else if (!strcmp(mode, "unknown-code")) {
        wanted = GT_ERR_IO;
        state = GT_SFTP_REMOVE_UNKNOWN;
        remote = 222;
    } else if (!strcmp(mode, "lost-reply") || !strcmp(mode, "timeout")) {
        wanted = GT_ERR_IO;
        state = GT_SFTP_REMOVE_UNKNOWN;
    }
    CHECK(status == wanted, "status preserved");
    CHECK(out.state == state, "outcome preserved");
    CHECK(out.remote_status == remote, "remote status preserved");
    if (state == GT_SFTP_REMOVE_NOT_ATTEMPTED || state == GT_SFTP_REMOVE_CHANGED ||
        state == GT_SFTP_REMOVE_ABSENT) {
        CHECK(calls == 0, "no mutation before admission");
    } else {
        CHECK(calls > 0, "deletion entered");
    }
    if (status == GT_OK || !strcmp(mode, "pre-cancel") || !strcmp(mode, "jobs")) {
        CHECK(gt__ssh_borrow(ssh, &raw, &socket) == GT_OK,
              "successful or untouched owner reusable");
        if (strcmp(mode, "jobs")) {
            CHECK(libssh2_session_get_timeout(raw) == 2345 &&
                      libssh2_session_get_blocking(raw) == 1,
                  "session options restored");
        }
    } else {
        CHECK(gt__ssh_borrow(ssh, &raw, &socket) != GT_OK, "failed IO invalidates owner");
    }
    if (!strcmp(mode, "timeout")) {
        CHECK(elapsed >= 38, "one total timeout consumed");
        CHECK(elapsed < bound, "timeout bounded");
    }
    bool gone = state == GT_SFTP_REMOVE_REMOVED || missing || !strcmp(mode, "lost-reply");
    CHECK((lstat(watched, &st) < 0) == gone, "actual filesystem result retained");
    if (!gone && !dir && !link) {
        CHECK(bytes_equal(watched, !strcmp(mode, "second-change") ? "changed bytes" : "safe bytes"),
              "preserved file bytes");
    }
    if (!strcmp(mode, "nonempty")) {
        CHECK(bytes_equal(child, "child"), "nonrecursive directory removal preserves children");
    }
    if (link) {
        CHECK(bytes_equal(child, "safe bytes"), "symlink referent preserved");
    }
    stopping = false;
    override_error = 0;
    started = now();
    gt_sftp_close(sftp);
    active_sftp = NULL;
    gt_ssh_close(ssh);
    CHECK(now() - started < bound, "cleanup bounded");
    if (!failures) {
        printf("PASS remove %s\n", mode);
    }
}

int main(int argc, char **argv) {
    if (argc != 6 && argc != 7) {
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *cases[] = {
        "normal",           "jobs",         "directory",   "nonempty",      "absent",
        "symlink",          "wrong-size",   "wrong-time",  "second-change", "missing-mtime",
        "missing-size",     "missing-type", "parent-link", "pre-cancel",    "cancel-before-delete",
        "cancel-in-delete", "late-cancel",  "lost-reply",  "unsupported",   "unknown-code",
        "timeout"};
    if (argc == 7) {
        run_case(argv, argv[6]);
    } else {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            run_case(argv, cases[i]);
        }
    }
    if (!failures) {
        puts("SFTP REMOVE PASS");
    }
    return failures ? 1 : 0;
}
