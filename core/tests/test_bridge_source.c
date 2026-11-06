/* Actual files -> private Source -> Session -> ResourceStore. Reuse the real
 * transport/RPC fixture; only filesystem syscall boundaries inject faults. */
#define GT_BRIDGE_TRANSFER_TEST_MAIN source_transfer_fixture_main
#include "test_bridge_transfer.c"
#include "extension/source.h"
static const char *source_fault;
static gt_bridge_session_t *source_stop_target;
static unsigned source_reads, source_opens, source_stats, source_closes;
static unsigned source_stats_at_read;
static size_t source_largest;
static int source_close_error = -1, source_swap_root = -1;
static int source_change_fd = -1;

static ssize_t source_fixture_pread(int fd, void *p, size_t n, off_t offset) {
    source_reads++;
    if (n > source_largest) {
        source_largest = n;
    }
    if (source_fault && (!strcmp(source_fault, "eintr") || !strcmp(source_fault, "again") ||
                         !strcmp(source_fault, "read-error"))) {
        errno = !strcmp(source_fault, "eintr")   ? EINTR
                : !strcmp(source_fault, "again") ? EAGAIN
                                                 : EIO;
        return -1;
    }
    if (source_fault && !strcmp(source_fault, "early-eof")) {
        return 0;
    }
    if (source_fault && !strcmp(source_fault, "short") && n > 2) {
        n = 2;
    }
    ssize_t result = pread(fd, p, n, offset);
    if (source_change_fd >= 0) {
        CHECK(!ftruncate(source_change_fd, 1));
        source_change_fd = -1;
    }
    if (source_fault && !strcmp(source_fault, "eof-extra") && !result) {
        *(uint8_t *)p = 1;
        return 1;
    }
    if (source_fault && !strcmp(source_fault, "digest") && result > 0) {
        ((uint8_t *)p)[0] ^= 1;
    }
    source_stats_at_read = source_stats;
    if (source_fault && !strcmp(source_fault, "stop-read")) {
        gt__bridge_session_stop(source_stop_target);
    }
    return result;
}

static int source_fixture_fstat(int fd, struct stat *st) {
    source_stats++;
    int result = fstat(fd, st);
    if (source_fault && !strcmp(source_fault, "stop-stat")) {
        gt__bridge_session_stop(source_stop_target);
    }
    if (source_fault && !strcmp(source_fault, "stat-error")) {
        errno = EIO;
        return -1;
    }
    return result;
}

static int source_fixture_openat(int fd, const char *name, int flags) {
    source_opens++;
    CHECK((flags & O_NOFOLLOW) && (flags & O_CLOEXEC) && (flags & O_NONBLOCK));
    if (source_fault && !strcmp(source_fault, "open-error")) {
        errno = EACCES;
        return -1;
    }
    if (source_fault && !strcmp(source_fault, "swap-open")) {
        CHECK(!renameat(fd, name, fd, "original") && !symlinkat("../outside", fd, name));
        source_fault = NULL;
    }
    int result = openat(fd, name, flags);
    if (source_swap_root >= 0 && result >= 0 && !strcmp(name, "input.bin")) {
        CHECK(!renameat(source_swap_root, "nested", source_swap_root, "moved"));
        source_swap_root = -1;
    }
    return result;
}

static int source_fixture_close(int fd) {
    source_closes++;
    int result = close(fd);
    if (fd == source_close_error && !result) {
        source_close_error = -1;
        errno = EIO;
        return -1;
    }
    return result;
}

#define pread source_fixture_pread
#define fstat source_fixture_fstat
#define openat source_fixture_openat
#define close source_fixture_close
#include "../src/extension/source.c"
#undef pread
#undef fstat
#undef openat
#undef close

typedef struct {
    char path[4096];
    int root;
    size_t size;
    gt_bridge_source_t *source;
} source_files_t;

static void source_caps(gt_bridge_session_config_t *c) {
    strcpy(c->required[c->required_count++], "file.readScoped");
    strcpy(c->available[c->available_count++], "file.readScoped");
}

static void source_pair_open(transfer_fixture_t *f, const char *mode) {
    memset(f, 0, sizeof(*f));
    transfer_store_open(&f->host, 1048576);
    transfer_store_open(&f->executor, 1048576);
    f->rpc.pair.clock.now = 1000;
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_bridge_stream_t *a = NULL, *b = NULL;
    CHECK(gt__bridge_fd_open(fd[0], fd[0], NULL, &a) == GT_OK &&
          gt__bridge_fd_open(fd[1], fd[1], NULL, &b) == GT_OK);
    gt_bridge_session_config_t h = session_config(&f->rpc.pair.clock), e = h;
    rpc_fixture_config(&h, &f->rpc.host);
    rpc_fixture_config(&e, &f->rpc.executor);
    source_caps(&h);
    source_caps(&e);
    if (mode && !strcmp(mode, "unsupported")) {
        h.required_count--;
        e.required_count--;
        h.available_count--;
        e.available_count--;
    }
    if (mode && !strcmp(mode, "ungranted")) {
        h.required_count--;
        e.required_count--;
    }
    h.resource_store = f->host.store;
    e.resource_store = f->executor.store;
    h.resource_scope_bytes = e.resource_scope_bytes = 1048576;
    h.budgets.resource_bytes = e.budgets.resource_bytes = 262144;
    gt_allocator_t allocator = {&f->rpc.pair.budget, session_allocate, session_release};
    CHECK(gt__bridge_session_open(&a, GT_BRIDGE_HOST, &h, &allocator, &f->rpc.pair.h) == GT_OK &&
          !a);
    CHECK(gt__bridge_session_open(&b, GT_BRIDGE_EXECUTOR, &e, &allocator, &f->rpc.pair.e) ==
              GT_OK &&
          !b);
    session_consent(&f->rpc.pair);
    if (mode && !strcmp(mode, "waiting")) {
        return;
    }
    session_allow(&f->rpc.pair);
    session_active(&f->rpc.pair);
    CHECK(gt__bridge_session_lease(f->rpc.pair.h, &f->rpc.hlease) &&
          gt__bridge_session_lease(f->rpc.pair.e, &f->rpc.elease));
    transfer_fixture_pump(f, 20);
}

static void source_make(int root, const char *name, size_t size) {
    int fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    uint8_t p[16384];
    size_t at = 0;
    while (at < size) {
        size_t n = size - at;
        if (n > sizeof(p)) {
            n = sizeof(p);
        }
        for (size_t i = 0; i < n; i++) {
            p[i] = size == 3 ? (uint8_t)"abc"[at + i] : (uint8_t)((at + i) % 251);
        }
        ssize_t written = write(fd, p, n);
        CHECK(written > 0);
        at += (size_t)written;
    }
    CHECK(!close(fd));
}

static void source_files_open(source_files_t *files, size_t size) {
    memset(files, 0, sizeof(*files));
    CHECK(
        snprintf(files->path, sizeof(files->path), "%s/plugin-source-unit-XXXXXX", transfer_root) >
            0 &&
        mkdtemp(files->path));
    files->root = open(files->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(files->root >= 0);
    files->size = size;
    CHECK(!mkdirat(files->root, "root", 0700) && !mkdirat(files->root, "root/nested", 0700));
    source_make(files->root, "root/nested/input.bin", size);
    source_make(files->root, "outside", 3);
}

/* Only used for this fixture's own mkdtemp tree, never a user supplied path. */
static void source_remove_tree(int fd) {
    DIR *dir = fdopendir(dup(fd));
    CHECK(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        struct stat st;
        CHECK(!fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW));
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            CHECK(child >= 0);
            source_remove_tree(child);
            CHECK(!close(child) && !unlinkat(fd, entry->d_name, AT_REMOVEDIR));
        } else {
            CHECK(!unlinkat(fd, entry->d_name, 0));
        }
    }
    CHECK(!closedir(dir));
}

static gt_bridge_source_config_t source_config(transfer_fixture_t *f, source_files_t *files) {
    return (gt_bridge_source_config_t){f->rpc.pair.h,
                                       f->rpc.hlease,
                                       files->root,
                                       "root/nested/input.bin",
                                       "application/octet-stream",
                                       262144};
}

static gt_bridge_source_info_t source_info(gt_bridge_source_t *s) {
    gt_bridge_source_info_t out;
    CHECK(gt__bridge_source_info(s, &out));
    return out;
}

static void source_ready(gt_bridge_source_t *s) {
    for (unsigned i = 0; i < 1000 && source_info(s).phase == GT_BRIDGE_SOURCE_HASHING; i++) {
        gt__bridge_source_step(s);
    }
    CHECK(source_info(s).phase == GT_BRIDGE_SOURCE_READY);
}

static void source_finish(transfer_fixture_t *f, gt_bridge_source_t *s) {
    for (unsigned i = 0; i < 100000 && !source_terminal(s); i++) {
        gt__bridge_source_step(s);
        gt__bridge_session_step(f->rpc.pair.h, 0);
        gt__bridge_session_step(f->rpc.pair.e, 0);
    }
    CHECK(source_terminal(s));
}

static void source_files_close(transfer_fixture_t *f, source_files_t *files) {
    source_fault = NULL;
    source_swap_root = -1;
    if (files->source) {
        gt_bridge_source_status_t status = gt__bridge_source_close(files->source);
        CHECK(status == GT_BRIDGE_SOURCE_OK || status == GT_BRIDGE_SOURCE_WAIT);
        if (status == GT_BRIDGE_SOURCE_WAIT) {
            source_finish(f, files->source);
        }
        CHECK(gt__bridge_source_destroy(files->source) == GT_BRIDGE_SOURCE_OK);
        files->source = NULL;
    }
    source_remove_tree(files->root);
    CHECK(!close(files->root) && !rmdir(files->path));
    transfer_fixture_close(f);
}

static void source_no_snapshot(transfer_fixture_t *f) {
    gt_bridge_snapshot_t snapshot;
    CHECK(gt__bridge_session_take_resource(f->rpc.pair.e, &f->rpc.elease, &snapshot) ==
          GT_BRIDGE_RESOURCE_WAIT);
    gt_bridge_resource_usage_t usage;
    CHECK(gt__bridge_resource_usage(f->executor.store, &usage) && !usage.resources &&
          !usage.reserved_bytes);
}

static void source_normal(const char *mode) {
    transfer_fixture_t f;
    source_pair_open(&f, NULL);
    source_files_t files;
    size_t size =
        !strcmp(mode, "empty") ? 0
        : !strcmp(mode, "large") || !strcmp(mode, "backpressure") || !strcmp(mode, "rpc-progress")
            ? 262144
            : 3;
    source_files_open(&files, size);
    gt_bridge_source_config_t cfg = source_config(&f, &files);
    if (!strcmp(mode, "literal")) {
        CHECK(!renameat(
            files.root, cfg.relative_path, files.root, "root/nested/$(touch PWNED) 中文.bin"));
        cfg.relative_path = "./root//nested/./$(touch PWNED) 中文.bin";
    }
    if (!strcmp(mode, "hardlink")) {
        CHECK(!linkat(files.root, "outside", files.root, "root/nested/link", 0));
        cfg.relative_path = "root/nested/link";
    }
    char deep[128];
    if (!strcmp(mode, "max-depth")) {
        int at = dup(files.root);
        CHECK(at >= 0);
        size_t n = 0;
        for (unsigned i = 0; i < 31; i++) {
            CHECK(!mkdirat(at, "d", 0700));
            int next = openat(at, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            CHECK(next >= 0 && !close(at));
            at = next;
            deep[n++] = 'd';
            deep[n++] = '/';
        }
        source_make(at, "f", 3);
        CHECK(!close(at));
        deep[n++] = 'f';
        deep[n] = 0;
        cfg.relative_path = deep;
    }
    CHECK(gt__bridge_source_open(&cfg, NULL, &files.source) == GT_BRIDGE_SOURCE_OK && files.source);
    gt_bridge_source_t *s = files.source;
    CHECK(source_info(s).hashed == 0 && !source_info(s).transfer_handle &&
          source_info(s).phase == GT_BRIDGE_SOURCE_HASHING);
    CHECK(gt__bridge_source_send(s) == GT_BRIDGE_SOURCE_WAIT);
    source_no_snapshot(&f);
    CHECK((fcntl(s->file, F_GETFD) & FD_CLOEXEC) &&
          (fcntl(s->file, F_GETFL) & O_ACCMODE) == O_RDONLY &&
          (fcntl(s->file, F_GETFL) & O_NONBLOCK));
    for (size_t i = 0; i < s->count; i++) {
        CHECK(fcntl(s->directory[i], F_GETFD) & FD_CLOEXEC);
    }
    if (!strcmp(mode, "short")) {
        source_fault = "short";
    }
    source_ready(s);
    CHECK(source_info(s).hashed == size && s->hash_eof && source_info(s).descriptor.size_known &&
          source_info(s).descriptor.size == size &&
          !strcmp(source_info(s).descriptor.sha256, transfer_metadata(size, false).sha256));
    CHECK(!lseek(s->file, 0, SEEK_CUR) && !source_info(s).transfer_handle);
    source_no_snapshot(&f);
    CHECK(gt__bridge_source_send(s) == GT_BRIDGE_SOURCE_OK);
    CHECK(!source_info(s).transfer.confirmed);
    if (!strcmp(mode, "backpressure") || !strcmp(mode, "rpc-progress")) {
        /* No receiver progress: bounded local producer plus source buffer. */
        for (unsigned i = 0; i < 200; i++) {
            gt__bridge_source_step(s);
            gt__bridge_session_step(f.rpc.pair.h, 0);
        }
        gt_bridge_source_info_t before = source_info(s);
        CHECK(before.read_bytes <= GT_BRIDGE_SOURCE_QUANTUM &&
              before.accepted <= GT_BRIDGE_TRANSFER_WINDOW + GT_BRIDGE_TRANSFER_CHUNK &&
              !before.transfer.confirmed);
        unsigned reads = source_reads;
        for (unsigned i = 0; i < 20; i++) {
            gt__bridge_source_step(s);
        }
        CHECK(source_reads == reads && source_info(s).accepted == before.accepted &&
              source_info(s).read_bytes == before.read_bytes);
        if (!strcmp(mode, "rpc-progress")) {
            uint64_t h = rpc_fixture_start(&f.rpc, false, "true", 60000);
            gt_bridge_rpc_request_t q = rpc_fixture_take(&f.rpc, false);
            rpc_fixture_reply(&f.rpc, false, &q);
            CHECK(rpc_fixture_complete(&f.rpc, false, h).state == GT_BRIDGE_RPC_SUCCEEDED);
            CHECK(source_info(s).phase == GT_BRIDGE_SOURCE_SENDING &&
                  source_info(s).read_bytes < size);
        }
    }
    source_finish(&f, s);
    source_fault = NULL;
    gt_bridge_source_info_t out = source_info(s);
    CHECK(out.phase == GT_BRIDGE_SOURCE_SUCCEEDED && out.error == GT_BRIDGE_SOURCE_OK &&
          out.transfer.confirmed && out.transfer.state == GT_BRIDGE_TRANSFER_SUCCEEDED &&
          out.read_bytes == size && out.accepted == size && s->send_eof &&
          source_largest <= GT_BRIDGE_SOURCE_QUANTUM);
    gt_bridge_snapshot_t snapshot = transfer_take(&f, false);
    CHECK(!strcmp(snapshot.descriptor.display_name, s->names[s->count - 1]));
    transfer_verify(&f, false, &snapshot, size, true);
    /* Source changes after confirmed publication cannot rewrite that result. */
    CHECK(!unlinkat(files.root, cfg.relative_path, 0));
    CHECK(gt__bridge_source_step(s) == GT_BRIDGE_SOURCE_SUCCEEDED);
    CHECK(fcntl(files.root, F_GETFD) >= 0);
    source_files_close(&f, &files);
}

static void source_invalid(const char *mode) {
    transfer_fixture_t f;
    source_pair_open(&f, mode);
    source_files_t files;
    source_files_open(&files, 3);
    gt_bridge_source_config_t cfg = source_config(&f, &files);
    gt_bridge_source_status_t expected = GT_BRIDGE_SOURCE_INVALID;
    char path[5000];
    if (!strcmp(mode, "absolute")) {
        cfg.relative_path = "/etc/passwd";
    } else if (!strcmp(mode, "parent")) {
        cfg.relative_path = "root/../outside";
        expected = GT_BRIDGE_SOURCE_DENIED;
    } else if (!strcmp(mode, "utf8")) {
        cfg.relative_path = "root/\xc0\xaf";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "control")) {
        cfg.relative_path = "root/a\nb";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "c1")) {
        cfg.relative_path = "root/a\xc2\x9b";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "long-name")) {
        memset(path, 'x', 256);
        path[256] = 0;
        cfg.relative_path = path;
        expected = GT_BRIDGE_SOURCE_LIMIT;
    } else if (!strcmp(mode, "long-path")) {
        memset(path, 'x', 4097);
        path[4097] = 0;
        cfg.relative_path = path;
    } else if (!strcmp(mode, "deep")) {
        size_t n = 0;
        for (unsigned i = 0; i < 33; i++) {
            path[n++] = 'x';
            if (i != 32) {
                path[n++] = '/';
            }
        }
        path[n] = 0;
        cfg.relative_path = path;
        expected = GT_BRIDGE_SOURCE_LIMIT;
    } else if (!strcmp(mode, "missing")) {
        cfg.relative_path = "root/missing";
        expected = GT_BRIDGE_SOURCE_NOT_FOUND;
    } else if (!strcmp(mode, "mime")) {
        cfg.mime = "application/JSON; charset=utf8";
    } else if (!strcmp(mode, "limit")) {
        cfg.byte_limit = 2;
        expected = GT_BRIDGE_SOURCE_LIMIT;
    } else if (!strcmp(mode, "negotiated-limit")) {
        source_make(files.root, "root/big", 262145);
        cfg.relative_path = "root/big";
        cfg.byte_limit = 1048576;
        expected = GT_BRIDGE_SOURCE_LIMIT;
    } else if (!strcmp(mode, "root-file")) {
        cfg.root_fd = openat(files.root, cfg.relative_path, O_RDONLY | O_CLOEXEC);
        CHECK(cfg.root_fd >= 0);
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "symlink")) {
        CHECK(!symlinkat("../outside", files.root, "root/link"));
        cfg.relative_path = "root/link";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "symlink-dir")) {
        CHECK(!symlinkat("nested", files.root, "root/link"));
        cfg.relative_path = "root/link/input.bin";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "directory")) {
        cfg.relative_path = "root/nested";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "fifo")) {
        CHECK(!mkfifoat(files.root, "root/fifo", 0600));
        cfg.relative_path = "root/fifo";
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "unsupported") || !strcmp(mode, "ungranted")) {
        expected = GT_BRIDGE_SOURCE_UNSUPPORTED;
    } else if (!strcmp(mode, "scope")) {
        cfg.lease.epoch++;
        expected = GT_BRIDGE_SOURCE_DENIED;
    } else if (!strcmp(mode, "revoked")) {
        gt__bridge_session_revoke(f.rpc.pair.h);
        expected = GT_BRIDGE_SOURCE_CLOSED;
    } else if (!strcmp(mode, "waiting")) {
        expected = GT_BRIDGE_SOURCE_WAIT;
    } else if (!strcmp(mode, "open-error")) {
        source_fault = "open-error";
        expected = GT_BRIDGE_SOURCE_DENIED;
    } else {
        CHECK(false);
    }
    unsigned reads = source_reads, opens = source_opens, stats = source_stats;
    CHECK(gt__bridge_source_open(&cfg, NULL, &files.source) == expected && !files.source &&
          source_reads == reads);
    if (!strcmp(mode, "unsupported") || !strcmp(mode, "ungranted") || !strcmp(mode, "scope") ||
        !strcmp(mode, "revoked") || !strcmp(mode, "waiting")) {
        CHECK(opens == source_opens && stats == source_stats);
    }
    if (!strcmp(mode, "root-file")) {
        CHECK(!close(cfg.root_fd));
    }
    source_files_close(&f, &files);
}

static void source_failure(const char *mode) {
    transfer_fixture_t f;
    source_pair_open(&f, NULL);
    source_files_t files;
    source_files_open(&files, !strcmp(mode, "change-backpressure") ? 262144 : 3);
    gt_bridge_source_config_t cfg = source_config(&f, &files);
    source_stop_target = f.rpc.pair.h;
    if (!strcmp(mode, "swap-open")) {
        cfg.relative_path = "root/victim";
        source_make(files.root, "root/victim", 3);
        source_fault = mode;
        CHECK(gt__bridge_source_open(&cfg, NULL, &files.source) == GT_BRIDGE_SOURCE_CHANGED &&
              !files.source);
        source_files_close(&f, &files);
        return;
    }
    if (!strcmp(mode, "move-open")) {
        source_swap_root = openat(files.root, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(source_swap_root >= 0);
        int borrowed = source_swap_root;
        CHECK(gt__bridge_source_open(&cfg, NULL, &files.source) == GT_BRIDGE_SOURCE_CHANGED &&
              !files.source);
        CHECK(!close(borrowed));
        source_files_close(&f, &files);
        return;
    }
    if (!strcmp(mode, "stop-open")) {
        source_fault = "stop-stat";
        unsigned opens = source_opens;
        CHECK(gt__bridge_source_open(&cfg, NULL, &files.source) == GT_BRIDGE_SOURCE_CLOSED &&
              !files.source && source_opens == opens);
        source_files_close(&f, &files);
        return;
    }
    CHECK(gt__bridge_source_open(&cfg, NULL, &files.source) == GT_BRIDGE_SOURCE_OK);
    gt_bridge_source_t *s = files.source;
    gt_bridge_source_status_t expected = GT_BRIDGE_SOURCE_CHANGED;
    bool sending = !strcmp(mode, "digest") || !strcmp(mode, "cancel") ||
                   !strcmp(mode, "close-pending") || !strcmp(mode, "stop-send") ||
                   !strcmp(mode, "lost") || !strcmp(mode, "change-backpressure") ||
                   !strcmp(mode, "second-eof") || !strcmp(mode, "second-error") ||
                   !strcmp(mode, "queued-change") || !strcmp(mode, "failure-close");
    if (sending || !strcmp(mode, "change-ready")) {
        source_ready(s);
        if (sending) {
            CHECK(gt__bridge_source_send(s) == GT_BRIDGE_SOURCE_OK);
            if (strcmp(mode, "queued-change")) {
                transfer_fixture_pump(&f, 20);
            }
        }
    }
    if (!strcmp(mode, "eintr") || !strcmp(mode, "again")) {
        source_fault = mode;
        unsigned reads = source_reads;
        CHECK(gt__bridge_source_step(s) == GT_BRIDGE_SOURCE_HASHING && source_reads == reads + 1 &&
              !source_info(s).hashed && !source_info(s).transfer_handle);
        source_fault = NULL;
        source_ready(s);
        source_files_close(&f, &files);
        return;
    }
    int change_fd = -1;
    if (!strcmp(mode, "mid-read")) {
        change_fd = openat(files.root, cfg.relative_path, O_WRONLY | O_CLOEXEC);
        CHECK(change_fd >= 0);
        source_change_fd = change_fd;
    } else if (!strcmp(mode, "read-error") || !strcmp(mode, "stat-error") ||
               !strcmp(mode, "second-error")) {
        source_fault = !strcmp(mode, "second-error") ? "read-error" : mode;
        expected = GT_BRIDGE_SOURCE_IO;
    } else if (!strcmp(mode, "second-eof")) {
        CHECK(gt__bridge_source_step(s) == GT_BRIDGE_SOURCE_SENDING &&
              source_info(s).read_bytes == 3);
        CHECK(gt__bridge_source_step(s) == GT_BRIDGE_SOURCE_SENDING &&
              source_info(s).accepted == 3);
        source_fault = "eof-extra";
    } else if (!strcmp(mode, "early-eof") || !strcmp(mode, "digest")) {
        source_fault = mode;
    } else if (!strcmp(mode, "eof-extra")) {
        CHECK(gt__bridge_source_step(s) == GT_BRIDGE_SOURCE_HASHING && source_info(s).hashed == 3);
        source_fault = mode;
    } else if (!strcmp(mode, "stop-read") || !strcmp(mode, "stop-stat") ||
               !strcmp(mode, "stop-send")) {
        source_fault = !strcmp(mode, "stop-send") ? "stop-read" : mode;
        expected = GT_BRIDGE_SOURCE_CLOSED;
    } else if (!strcmp(mode, "cancel")) {
        gt__bridge_source_cancel(s);
        expected = GT_BRIDGE_SOURCE_CANCELLED;
    } else if (!strcmp(mode, "close-pending")) {
        CHECK(gt__bridge_source_close(s) == GT_BRIDGE_SOURCE_WAIT && source_info(s).closed);
        source_finish(&f, s);
        CHECK(source_info(s).phase == GT_BRIDGE_SOURCE_STOPPED);
        source_files_close(&f, &files);
        return;
    } else if (!strcmp(mode, "lost")) {
        CHECK(gt__bridge_session_close(f.rpc.pair.e, 0) == GT_BRIDGE_RELEASED);
        for (unsigned i = 0; i < 100 && session_info(f.rpc.pair.h).end == GT_BRIDGE_END_NONE; i++) {
            gt__bridge_session_step(f.rpc.pair.h, 0);
        }
        gt__bridge_source_step(s);
        CHECK(source_info(s).phase == GT_BRIDGE_SOURCE_UNCERTAIN &&
              source_info(s).error == GT_BRIDGE_SOURCE_UNKNOWN &&
              !source_info(s).transfer.confirmed);
        source_files_close(&f, &files);
        return;
    } else if (!strcmp(mode, "rename")) {
        CHECK(!renameat(files.root, "root/nested", files.root, "root/moved"));
    } else if (!strcmp(mode, "replace")) {
        CHECK(!renameat(files.root, cfg.relative_path, files.root, "saved"));
        source_make(files.root, cfg.relative_path, 3);
    } else if (!strcmp(mode, "unlink")) {
        CHECK(!unlinkat(files.root, cfg.relative_path, 0));
    } else if (!strcmp(mode, "change-ready") || !strcmp(mode, "change-backpressure") ||
               !strcmp(mode, "grow") || !strcmp(mode, "truncate") || !strcmp(mode, "edit") ||
               !strcmp(mode, "queued-change") || !strcmp(mode, "failure-close")) {
        if (!strcmp(mode, "change-backpressure")) {
            for (unsigned i = 0; i < 100; i++) {
                gt__bridge_source_step(s);
                gt__bridge_session_step(f.rpc.pair.h, 0);
            }
            CHECK(source_info(s).read_bytes < files.size && source_info(s).accepted < files.size);
        }
        int fd = openat(files.root, cfg.relative_path, O_WRONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        if (!strcmp(mode, "truncate")) {
            CHECK(!ftruncate(fd, 1));
        } else if (!strcmp(mode, "grow")) {
            CHECK(pwrite(fd, "x", 1, 3) == 1);
        } else {
            CHECK(pwrite(fd, "z", 1, 0) == 1);
        }
        CHECK(!close(fd));
    } else {
        CHECK(false);
    }
    unsigned reads_before_action = source_reads;
    if (!strcmp(mode, "change-ready")) {
        CHECK(gt__bridge_source_send(s) == GT_BRIDGE_SOURCE_CHANGED);
    } else if (!sending) {
        gt__bridge_source_step(s);
        CHECK(source_terminal(s));
    } else if (!strcmp(mode, "failure-close")) {
        CHECK(gt__bridge_source_step(s) == GT_BRIDGE_SOURCE_CANCELLING &&
              source_info(s).error == GT_BRIDGE_SOURCE_CHANGED);
        CHECK(gt__bridge_source_close(s) == GT_BRIDGE_SOURCE_WAIT &&
              source_info(s).error == GT_BRIDGE_SOURCE_CHANGED);
        source_finish(&f, s);
    } else {
        source_finish(&f, s);
    }
    if (change_fd >= 0) {
        CHECK(!close(change_fd));
    }
    if (!strcmp(mode, "stop-stat")) {
        CHECK(source_reads == reads_before_action);
    }
    gt_bridge_source_info_t out = source_info(s);
    if (expected == GT_BRIDGE_SOURCE_CLOSED && sending) {
        CHECK(out.phase == GT_BRIDGE_SOURCE_UNCERTAIN && out.error == GT_BRIDGE_SOURCE_UNKNOWN);
    } else {
        CHECK(out.phase ==
                  (expected == GT_BRIDGE_SOURCE_CLOSED || expected == GT_BRIDGE_SOURCE_CANCELLED
                       ? GT_BRIDGE_SOURCE_STOPPED
                       : GT_BRIDGE_SOURCE_FAILED) &&
              out.error == expected);
    }
    if (!strcmp(mode, "queued-change")) {
        CHECK(!out.transfer.confirmed && !f.rpc.pair.h->resources->out.started);
    }
    if (!strcmp(mode, "stop-read")) {
        CHECK(!out.hashed && !out.read_bytes && !out.accepted &&
              source_stats == source_stats_at_read);
        for (size_t i = 0; i < sizeof(s->buffer); i++) {
            CHECK(!s->buffer[i]);
        }
    }
    source_fault = NULL;
    if (!strcmp(mode, "stop-send")) {
        CHECK(!source_info(s).accepted && !f.rpc.pair.e->resources->snapshot_count);
        CHECK(gt__bridge_session_close(f.rpc.pair.h, 0) == GT_BRIDGE_RELEASED);
        for (unsigned i = 0; i < 100 && session_info(f.rpc.pair.e).end == GT_BRIDGE_END_NONE; i++) {
            gt__bridge_session_step(f.rpc.pair.e, 0);
        }
        CHECK(session_info(f.rpc.pair.e).end != GT_BRIDGE_END_NONE);
    }
    source_no_snapshot(&f);
    source_files_close(&f, &files);
}

static void source_lifetime(const char *mode) {
    transfer_fixture_t f;
    source_pair_open(&f, NULL);
    source_files_t files;
    source_files_open(&files, 3);
    gt_bridge_source_config_t cfg = source_config(&f, &files);
    session_budget_t budget = {0};
    gt_allocator_t allocator = {&budget, session_allocate, session_release};
    if (!strcmp(mode, "allocation")) {
        budget.fail = 1;
        CHECK(gt__bridge_source_open(&cfg, &allocator, &files.source) == GT_BRIDGE_SOURCE_IO &&
              !files.source && !budget.live);
        budget.fail = 0;
    }
    CHECK(gt__bridge_source_open(&cfg, &allocator, &files.source) == GT_BRIDGE_SOURCE_OK &&
          budget.live);
    size_t live = budget.live;
    if (!strcmp(mode, "close-error")) {
        source_close_error = files.source->file;
        unsigned before = source_closes;
        CHECK(gt__bridge_source_destroy(files.source) == GT_BRIDGE_SOURCE_IO &&
              source_info(files.source).closed && budget.live == live);
        unsigned after = source_closes;
        CHECK(after > before && gt__bridge_source_destroy(files.source) == GT_BRIDGE_SOURCE_OK &&
              source_closes == after && !budget.live);
        files.source = NULL;
    } else if (!strcmp(mode, "close-root")) {
        int borrowed = files.root;
        CHECK(!close(borrowed));
        source_ready(files.source);
        files.root = open(files.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(files.root >= 0);
    } else {
        source_ready(files.source);
        CHECK(budget.calls == (!strcmp(mode, "allocation") ? 2u : 1u));
    }
    source_files_close(&f, &files);
    CHECK(!budget.live);
}

typedef struct {
    transfer_network_t link;
    source_files_t files;
} source_network_t;

static void source_network_config(source_network_t *state, gt_bridge_session_config_t *cfg) {
    transfer_network_config(&state->link, cfg);
    source_caps(cfg);
}

static void source_network_drive(source_network_t *state, bool host) {
    transfer_network_t *n = &state->link;
    gt_bridge_session_t *s = n->session;
    CHECK(gt__bridge_session_lease(s, &n->lease));
    if (!state->files.source) {
        source_files_open(&state->files, 262144);
        gt_bridge_source_config_t cfg = {s,
                                         n->lease,
                                         state->files.root,
                                         "root/nested/input.bin",
                                         "application/octet-stream",
                                         262144};
        CHECK(gt__bridge_source_open(&cfg, NULL, &state->files.source) == GT_BRIDGE_SOURCE_OK);
    }
    gt_bridge_source_phase_t phase = gt__bridge_source_step(state->files.source);
    CHECK(phase <= GT_BRIDGE_SOURCE_RECEIPT || phase == GT_BRIDGE_SOURCE_SUCCEEDED);
    if (phase == GT_BRIDGE_SOURCE_READY) {
        CHECK(gt__bridge_source_send(state->files.source) == GT_BRIDGE_SOURCE_OK);
    }
    gt_bridge_source_info_t info = source_info(state->files.source);
    n->file = info.transfer_handle;
    n->confirmed = info.phase == GT_BRIDGE_SOURCE_SUCCEEDED;
    if (n->confirmed) {
        CHECK(info.hashed == 262144 && info.read_bytes == 262144 && info.accepted == 262144 &&
              info.transfer.confirmed);
    }
    gt_bridge_rpc_request_t q;
    if (gt__bridge_session_take_request(s, &n->lease, &q) == GT_BRIDGE_RPC_OK) {
        CHECK(gt__bridge_session_check_request(s, &n->lease, q.ticket) == GT_BRIDGE_RPC_OK);
        char copy[GT_BRIDGE_RPC_BODY_MAX + 1];
        int size = snprintf(copy,
                            sizeof(copy),
                            "{\"kind\":\"success\",\"value\":%.*s}",
                            (int)q.input_size,
                            q.input);
        CHECK(size > 0 && (size_t)size < sizeof(copy) &&
              gt__bridge_session_respond(s, &n->lease, q.ticket, copy, (size_t)size) ==
                  GT_BRIDGE_RPC_OK);
        if (host) {
            CHECK(!strcmp(q.method, "fixture.ask"));
            n->peer_ready = true;
            n->done_ticket = q.ticket;
        } else {
            CHECK(!strcmp(q.method, "fixture.echo"));
            n->work.effects++;
        }
    }
    if (host) {
        if (n->request) {
            gt_bridge_rpc_result_t r;
            CHECK(gt__bridge_session_request_result(s, &n->lease, n->request, &r));
            if (r.confirmed) {
                char expected[64];
                CHECK(snprintf(expected, sizeof(expected), "{\"n\":%u}", n->completed) > 0 &&
                      r.state == GT_BRIDGE_RPC_SUCCEEDED &&
                      strstr((const char *)r.outcome, expected));
                CHECK(gt__bridge_session_forget_request(s, &n->lease, n->request) ==
                      GT_BRIDGE_RPC_OK);
                n->request = 0;
                n->completed++;
            }
        }
        if (!n->request && n->completed < 80) {
            char input[64];
            int size = snprintf(input, sizeof(input), "{\"n\":%u}", n->completed);
            CHECK(size > 0 &&
                  gt__bridge_session_request(
                      s, &n->lease, "fixture.echo", input, (size_t)size, 60000, &n->request) ==
                      GT_BRIDGE_RPC_OK);
        }
    }
    if (!n->snapshot) {
        gt_bridge_snapshot_t snapshot;
        gt_bridge_resource_status_t status =
            gt__bridge_session_take_resource(s, &n->lease, &snapshot);
        CHECK(status == GT_BRIDGE_RESOURCE_OK || status == GT_BRIDGE_RESOURCE_WAIT);
        if (status == GT_BRIDGE_RESOURCE_OK) {
            CHECK(snapshot.descriptor.size_known && snapshot.descriptor.size == 262144 &&
                  !strcmp(snapshot.descriptor.sha256, transfer_metadata(262144, false).sha256));
            uint8_t p[16384];
            for (size_t at = 0; at < 262144;) {
                size_t count = 0;
                CHECK(gt__bridge_resource_read(n->store.store,
                                               snapshot.owner,
                                               &n->lease,
                                               &snapshot.ref,
                                               at,
                                               p,
                                               sizeof(p),
                                               &count) == GT_BRIDGE_RESOURCE_OK &&
                      count);
                for (size_t i = 0; i < count; i++) {
                    CHECK(p[i] == (uint8_t)((at + i) % 251));
                }
                at += count;
            }
            CHECK(gt__bridge_resource_release(
                      n->store.store, snapshot.owner, &n->lease, &snapshot.ref) ==
                  GT_BRIDGE_RESOURCE_OK);
            n->snapshot = true;
        }
    }
    if (!host && n->confirmed && n->snapshot && !n->done_request) {
        CHECK(gt__bridge_session_request(
                  s, &n->lease, "fixture.ask", "true", 4, 60000, &n->done_request) ==
              GT_BRIDGE_RPC_OK);
    }
}

static void source_network_close(source_network_t *state) {
    if (state->files.source) {
        CHECK(gt__bridge_source_destroy(state->files.source) == GT_BRIDGE_SOURCE_OK);
        source_remove_tree(state->files.root);
        CHECK(!close(state->files.root) && !rmdir(state->files.path));
    }
    session_dispose(state->link.session);
    transfer_store_close(&state->link.store);
}

static void source_network_executor(void) {
    source_network_t state = {0};
    transfer_network_t *n = &state.link;
    gt_bridge_session_config_t cfg;
    source_network_config(&state, &cfg);
    gt_bridge_stream_t *stream = NULL;
    CHECK(!isatty(0) && !isatty(1) && gt__bridge_fd_open(0, 1, NULL, &stream) == GT_OK);
    CHECK(gt__bridge_session_open(&stream, GT_BRIDGE_EXECUTOR, &cfg, NULL, &n->session) == GT_OK);
    uint64_t deadline = session_now() + 60000;
    for (;;) {
        gt_bridge_session_phase_t phase = gt__bridge_session_step(n->session, 2);
        if (phase >= GT_BRIDGE_SESSION_ENDED) {
            break;
        }
        if (phase == GT_BRIDGE_SESSION_ACTIVE) {
            source_network_drive(&state, false);
        }
        CHECK(session_now() < deadline);
    }
    if (n->file) {
        CHECK(n->confirmed && n->snapshot && n->work.effects == 80 && n->done_request);
        gt_bridge_rpc_result_t r;
        CHECK(gt__bridge_session_request_result(n->session, &n->lease, n->done_request, &r) &&
              r.confirmed && r.state == GT_BRIDGE_RPC_SUCCEEDED);
    } else {
        CHECK(!state.files.source && !n->work.effects);
    }
    source_network_close(&state);
}
#ifdef GT_HAVE_LIBSSH2
static void source_network_ssh(int argc, char **argv) {
    CHECK(argc >= 8);
    size_t count = (size_t)atoi(argv[2]);
    CHECK(count >= 2 && count <= 3 && argc == (int)(6 + count));
    session_port = (uint16_t)atoi(argv[6]);
    session_dials = 0;
    uint8_t prefix[4];
    CHECK(exact_read(0, prefix, 4));
    size_t n = ((size_t)prefix[0] << 24) | ((size_t)prefix[1] << 16) | ((size_t)prefix[2] << 8) |
               prefix[3];
    CHECK(n && n < 8192);
    private_key = calloc(1, n + 1);
    CHECK(private_key && exact_read(0, private_key, n));
    gt_ssh_t *material[3] = {0};
    gt_bridge_route_hop_t hops[3] = {0};
    for (size_t i = 0; i < count; i++) {
        char identity[64];
        CHECK(snprintf(identity, sizeof(identity), "route-hop-%zu.invalid", i) > 0);
        gt_ssh_opts_t o = {.host = identity,
                           .port = (uint16_t)atoi(argv[6 + i]),
                           .username = argv[3],
                           .known_hosts_path = argv[4]};
        material[i] = gt_ssh_new(&o);
        CHECK(material[i] && gt_ssh_auth_private_key(material[i], private_key, NULL) == GT_OK);
        hops[i].connector = (gt_ssh_connector_config_t){.family = i ? 0 : 4,
                                                        .address = {127, 0, 0, 1},
                                                        .timeout_ms = 5000,
                                                        .clock = session_clock};
        if (i) {
            hops[i].via_host = "127.0.0.1";
        }
    }
    gt_bridge_stream_t *route = NULL;
    CHECK(gt__bridge_route_open(material, hops, count, argv[5], NULL, NULL, &route) == GT_OK);
    uint64_t deadline = session_now() + 60000;
    for (;;) {
        gt_bridge_route_phase_t p = gt__bridge_route_step(route, 10);
        if (p == GT_BRIDGE_ROUTE_READY) {
            break;
        }
        CHECK(p < GT_BRIDGE_ROUTE_FAILED && session_now() < deadline);
    }
    gt_bridge_route_info_t info;
    CHECK(gt__bridge_route_info(route, &info));
    source_network_t state = {0};
    gt_bridge_session_config_t cfg;
    source_network_config(&state, &cfg);
    cfg.peer.kind = GT_BRIDGE_PEER_SSH;
    strcpy(cfg.peer.identity, info.authentication[count - 1].fingerprint);
    CHECK(gt__bridge_session_open(&route, GT_BRIDGE_HOST, &cfg, NULL, &state.link.session) ==
              GT_OK &&
          !route);
    bool decided = false;
    for (;;) {
        gt_bridge_session_phase_t phase = gt__bridge_session_step(state.link.session, 2);
        gt_bridge_session_info_t session = session_info(state.link.session);
        if (phase == GT_BRIDGE_SESSION_CONSENT && !decided) {
            CHECK(gt__bridge_session_decide(state.link.session,
                                            session.consent_token,
                                            !strcmp(argv[1], "ssh-deny")
                                                ? GT_BRIDGE_DECISION_DENY
                                                : GT_BRIDGE_DECISION_ALLOW) == GT_OK);
            decided = true;
        }
        if (phase == GT_BRIDGE_SESSION_ACTIVE) {
            source_network_drive(&state, true);
            rpc_in_t *done = state.link.done_ticket
                                 ? rpc_ticket(state.link.session->rpc, state.link.done_ticket)
                                 : NULL;
            if (state.link.confirmed && state.link.snapshot && state.link.completed == 80 &&
                state.link.peer_ready && done && !done->needs_send && !done->reserved &&
                !gt__bridge_writer_pending(state.link.session->writer)) {
                gt__bridge_session_revoke(state.link.session);
            }
        }
        if (phase == GT_BRIDGE_SESSION_ENDED) {
            break;
        }
        CHECK(session.end == GT_BRIDGE_END_NONE || session.end == GT_BRIDGE_END_DENIED ||
              session.end == GT_BRIDGE_END_REVOKED);
        CHECK(session_now() < deadline);
    }
    CHECK(decided && session_dials == 1);
    if (!strcmp(argv[1], "ssh-deny")) {
        CHECK(!state.files.source && !state.link.file && !state.link.completed);
    } else {
        CHECK(state.link.confirmed && state.link.snapshot && state.link.completed == 80 &&
              state.link.peer_ready);
    }
    source_network_close(&state);
    gt__secure_erase(private_key, n);
    free(private_key);
    private_key = NULL;
    printf("PASS bridge_source_%s_%zu\n", argv[1], count);
}
#endif
int main(int argc, char **argv) {
    CHECK(argc >= 3);
    transfer_root = argv[argc - 1];
    if (!strcmp(argv[1], "--executor")) {
        CHECK(argc == 3);
        source_network_executor();
        return 0;
    }
#ifdef GT_HAVE_LIBSSH2
    if (argc > 3) {
        source_network_ssh(argc - 1, argv);
        return 0;
    }
#endif
    CHECK(argc == 3);
    size_t fds = session_fds();
    const char *mode = argv[1];
    char word[80];
    CHECK(snprintf(word, sizeof(word), " %s ", mode) > 0);
    const char
        *normal =
            " roundtrip large empty short literal hardlink max-depth backpressure rpc-progress ",
        *invalid = " absolute parent utf8 control c1 long-name long-path deep missing mime limit "
                   "negotiated-limit root-file symlink symlink-dir directory fifo unsupported "
                   "ungranted scope revoked waiting open-error ",
        *lifetime = " allocation close-error close-root ";
    if (strstr(normal, word)) {
        source_normal(mode);
    } else if (strstr(invalid, word)) {
        source_invalid(mode);
    } else if (strstr(lifetime, word)) {
        source_lifetime(mode);
    } else {
        source_failure(mode);
    }
    CHECK(session_fds() == fds);
    printf("PASS bridge_source_%s\n", mode);
    return 0;
}
