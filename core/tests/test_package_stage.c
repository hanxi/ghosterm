/* Real fixed package -> private physical tree; no plugin code is executed. */
#define main stage_metadata_fixture_main
#include "test_package_check.c"
#undef main
#undef CHECK
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d %s\n", __func__, __LINE__, #x);                            \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#include "extension/package-stage.h"
#include "base/secure-random.h"

typedef struct {
    char root[4096];
    int root_fd, source, parent;
    gt_package_checked_t *checked;
    memory_t memory;
} stage_fixture_t;

static const char *stage_names[] = {"plugin.json",
                                    "README",
                                    "LICENSES/MIT.txt",
                                    "schemas/input.json",
                                    "executor/main.mjs",
                                    "node_modules/pkg/中文.mjs",
                                    "binary.dat",
                                    "empty"};

static void stage_fixture(stage_fixture_t *f, const char *temp, const char *kind) {
    memset(f, 0, sizeof(*f));
    f->memory.wiped = true;
    CHECK(snprintf(f->root, sizeof(f->root), "%s/package-stage-XXXXXX", temp) > 0 &&
          mkdtemp(f->root));
    f->root_fd = open(f->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(f->root_fd >= 0 && !mkdirat(f->root_fd, "source", 0700) &&
          !mkdirat(f->root_fd, "parent", 0700));
    f->source = openat(f->root_fd, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    f->parent = openat(f->root_fd, "parent", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(f->source >= 0 && f->parent >= 0);
    const char *dirs[] = {"LICENSES", "schemas", "executor", "node_modules", "node_modules/pkg"};
    for (size_t i = 0; i < 5; i++) {
        CHECK(!mkdirat(f->source, dirs[i], 0700));
    }
    char manifest_text[4096], side[1024];
    bool exec = !strcmp(kind, "exec"), portable = !strcmp(kind, "portable");
    CHECK(snprintf(side,
                   sizeof(side),
                   "\"executor\":{\"variants\":[{\"id\":\"selected\",\"runtime\":\"%s\","
                   "\"%s\":\"executor/main.mjs\",\"requires\":[\"session.rpc\"]%s}]}",
                   exec       ? "exec"
                   : portable ? "portable-js"
                              : "node",
                   exec ? "executable" : "entry",
                   exec       ? ",\"os\":[\"linux\"],\"arch\":[\"aarch64\"]"
                   : portable ? ""
                              : ",\"runtimeVersionRange\":\">=22.0.0 <27.0.0\"") > 0);
    CHECK(snprintf(
              manifest_text,
              sizeof(manifest_text),
              "{\"schemaVersion\":1,\"id\":\"org.ghosterm.stage\",\"pluginVersion\":\"1.0.0\","
              "\"sdkApiRange\":\">=0.1.0 <2.0.0\",\"bridgeProtocolRange\":[1],"
              "\"publisher\":\"fixture\",\"license\":\"MIT\",\"commands\":[{\"path\":[\"stage\"],"
              "\"title\":\"Stage\",\"argsSchema\":\"schemas/input.json\"}],%s,\"permissions\":[]}",
              side) > 0);
    unsigned char binary[GT_PACKAGE_CHUNK_MAX * 2 + 71];
    for (size_t i = 0; i < sizeof(binary); i++) {
        binary[i] = (unsigned char)(i * 37 + (i >> 8) + (i >> 16) * 17);
    }
    const void *texts[] = {manifest_text,
                           "stage fixture",
                           "MIT",
                           "{\"type\":\"object\"}",
                           "throw Error('NOT EXECUTED');",
                           "export const x=42;",
                           binary,
                           ""};
    char integrity[8192] = "{\"schemaVersion\":1,\"members\":[";
    size_t at = strlen(integrity);
    for (size_t i = 0; i < 8; i++) {
        size_t n = i == 6 ? sizeof(binary) : strlen(texts[i]);
        CHECK(!put(f->source, stage_names[i], texts[i], n));
        gt_sha256_t hash;
        char digest[65];
        gt__sha256_init(&hash);
        CHECK(gt__sha256_update(&hash, texts[i], n) && gt__sha256_final(&hash, digest));
        int written = snprintf(integrity + at,
                               sizeof(integrity) - at,
                               "%s{\"path\":\"%s\",\"bytes\":%zu,\"sha256\":\"%s\"}",
                               i ? "," : "",
                               stage_names[i],
                               n,
                               digest);
        CHECK(written > 0 && (size_t)written < sizeof(integrity) - at);
        at += (size_t)written;
    }
    strcat(integrity, "]}");
    CHECK(!put(f->source, "integrity.json", integrity, strlen(integrity)));
    gt_package_snapshot_t *snapshot = NULL;
    gt_package_capture_config_t capture = {f->source, 1024, 4096, 16777216, 67108864};
    CHECK(gt__package_capture_open(&capture, NULL, &snapshot) == GT_PACKAGE_OK);
    gt_package_status_t status;
    do {
        status = gt__package_capture_step(snapshot);
    } while (status == GT_PACKAGE_WAIT);
    CHECK(status == GT_PACKAGE_OK);
    char facts[1024];
    CHECK(snprintf(facts,
                   sizeof(facts),
                   "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                   "\"os\":\"linux\",\"arch\":\"aarch64\",\"runtimes\":[\"%s\"],"
                   "\"capabilities\":[\"session.rpc\"],\"programs\":{\"node\":{"
                   "\"state\":\"available\",\"version\":\"26.4.0\"}}}",
                   exec       ? "exec"
                   : portable ? "portable-js"
                              : "node") > 0);
    gt_package_check_config_t config = {.host_json = facts, .host_size = strlen(facts)};
    gt_package_check_report_t report;
    gt_package_check_status_t result =
        gt__package_check(&snapshot, &config, NULL, &f->checked, &report);
    if (result != GT_PACKAGE_CHECK_COMPATIBLE) {
        fprintf(stderr, "stage metadata %s: %u %s\n", kind, result, report.code);
    }
    CHECK(result == GT_PACKAGE_CHECK_COMPATIBLE);
    CHECK(!snapshot && f->checked);
}

static size_t stage_children(int fd, char *first) {
    int fresh = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fresh >= 0);
    DIR *dir = fdopendir(fresh);
    CHECK(dir);
    size_t count = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        if (!count && first) {
            strcpy(first, e->d_name);
        }
        count++;
    }
    CHECK(!closedir(dir));
    return count;
}

static void stage_finish(gt_package_stage_t *s) {
    gt_package_stage_status_t status;
    size_t steps = 0;
    do {
        status = gt__package_stage_step(s);
        CHECK(++steps < 1000);
    } while (status == GT_STAGE_WAIT);
    CHECK(status == GT_STAGE_OK);
}

static void stage_compare(gt_package_checked_t *checked, int fd, bool exec) {
    size_t count;
    uint64_t total;
    CHECK(gt__package_checked_stats(checked, &count, &total) && count == 9 && total > 32768);
    for (size_t i = 0; i < count; i++) {
        gt_package_member_t member;
        const uint8_t *bytes;
        size_t size;
        CHECK(gt__package_checked_at(checked, i, &member, &bytes, &size));
        int file_fd = openat(fd, member.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        CHECK(file_fd >= 0 && !fstat(file_fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
              st.st_size == (off_t)size);
        mode_t mode = exec && !strcmp(member.path, "executor/main.mjs") ? 0500 : 0400;
        CHECK((st.st_mode & 0777) == mode);
        size_t at = 0;
        unsigned char buffer[4096];
        for (;;) {
            ssize_t n = read(file_fd, buffer, sizeof(buffer));
            CHECK(n >= 0);
            if (!n) {
                break;
            }
            CHECK((size_t)n <= size - at && !memcmp(buffer, bytes + at, (size_t)n));
            at += (size_t)n;
        }
        CHECK(at == size && !close(file_fd));
    }
    const char *dirs[] = {
        ".", "executor", "node_modules", "node_modules/pkg", "schemas", "LICENSES"};
    for (size_t i = 0; i < 6; i++) {
        struct stat st;
        CHECK(!fstatat(fd, dirs[i], &st, AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode) &&
              (st.st_mode & 0777) == 0500);
    }
}

static void stage_clear(stage_fixture_t *f) {
    if (f->checked) {
        CHECK(gt__package_checked_destroy(&f->checked) == GT_PACKAGE_OK);
    }
    CHECK(!f->memory.live && f->memory.wiped && !stage_children(f->parent, NULL));
    CHECK(!close(f->parent) && !clear(f->source) && !close(f->source));
    CHECK(!unlinkat(f->root_fd, "parent", AT_REMOVEDIR) &&
          !unlinkat(f->root_fd, "source", AT_REMOVEDIR) && !close(f->root_fd) && !rmdir(f->root));
    CHECK(!gt__plugin_memory_info().used);
}

#ifdef GT_PACKAGE_STAGE_BOUNDARY
/* Only the included production owner's syscalls are intercepted. Fixture IO
 * and the independent configuration control use the real OS unchanged. */
static const char *stage_fault;
static bool stage_fault_seen;

static gt_status_t stage_entropy_fault(uint8_t *bytes, size_t size) {
    if (!strcmp(stage_fault, "entropy-unsupported") || !strcmp(stage_fault, "entropy-io")) {
        stage_fault_seen = true;
        gt__secure_erase(bytes, size);
        return !strcmp(stage_fault, "entropy-unsupported") ? GT_ERR_UNSUPPORTED : GT_ERR_IO;
    }
    return gt__secure_random(bytes, size);
}

static ssize_t stage_write_fault(int fd, const void *bytes, size_t n) {
    if (!strcmp(stage_fault, "short-write")) {
        stage_fault_seen = true;
        return write(fd, bytes, n > 1 ? n / 2 : n);
    }
    if (!stage_fault_seen && strcmp(stage_fault, "close-error")) {
        stage_fault_seen = true;
        if (!strcmp(stage_fault, "interrupted-write")) {
            errno = EINTR;
            return -1;
        }
        if (!strcmp(stage_fault, "io-write")) {
            errno = ENOSPC;
            return -1;
        }
        if (!strcmp(stage_fault, "zero-write")) {
            return 0;
        }
    }
    return write(fd, bytes, n);
}

static int stage_close_fault(int fd) {
    struct stat st;
    bool fail = !strcmp(stage_fault, "close-error") && !stage_fault_seen && !fstat(fd, &st) &&
                S_ISREG(st.st_mode);
    int result = close(fd);
    if (fail) {
        stage_fault_seen = true;
        errno = EIO;
        return -1;
    }
    return result;
}

#define write stage_write_fault
#define close stage_close_fault
#define gt__secure_random stage_entropy_fault
#include "extension/package-stage.c"
#undef gt__secure_random
#undef close
#undef write
#endif

int main(int argc, char **argv) {
    CHECK(argc == 3);
    const char *mode = argv[1];
    stage_fixture_t f;
    stage_fixture(&f,
                  argv[2],
                  !strcmp(mode, "exec")          ? "exec"
                  : !strcmp(mode, "unsupported") ? "portable"
                                                 : "node");
    atomic_bool stop = ATOMIC_VAR_INIT(false);
    gt_package_stage_t *s = NULL;
    gt_allocator_t allocator = {&f.memory, alloc, release};
#ifdef GT_PACKAGE_STAGE_BOUNDARY
    stage_fault = mode;
    CHECK(gt__package_stage_open(
              f.checked, f.parent, GT_PACKAGE_TOTAL_BYTES_MAX, &stop, &allocator, &s) ==
          GT_STAGE_OK);
    gt_package_stage_status_t result;
    do {
        result = gt__package_stage_step(s);
    } while (result == GT_STAGE_WAIT);
    CHECK(stage_fault_seen);
    bool success = !strcmp(mode, "short-write") || !strcmp(mode, "interrupted-write");
    gt_package_stage_status_t expected = success ? GT_STAGE_OK
                                         : !strcmp(mode, "entropy-unsupported")
                                             ? GT_STAGE_UNSUPPORTED
                                             : GT_STAGE_IO;
    CHECK(result == expected);
    if (success) {
        int fd;
        const char *name, *entry;
        CHECK(gt__package_stage_view(s, &fd, &name, &entry) == GT_STAGE_OK);
        stage_compare(f.checked, fd, false);
    } else {
        int fd = 42;
        const char *name = "stale", *entry = "stale";
        CHECK(gt__package_stage_step(s) == expected);
        CHECK(gt__package_stage_view(s, &fd, &name, &entry) == expected && fd == -1 && !name &&
              !entry);
    }
    CHECK(gt__package_stage_destroy(&s) == GT_STAGE_OK);
    stage_clear(&f);
    return 0;
#endif
    if (!strcmp(mode, "cancel-before")) {
        atomic_store(&stop, true);
        CHECK(gt__package_stage_open(
                  f.checked, f.parent, GT_PACKAGE_TOTAL_BYTES_MAX, &stop, &allocator, &s) ==
                  GT_STAGE_CANCELLED &&
              !s);
    } else if (!strcmp(mode, "limit")) {
        CHECK(gt__package_stage_open(f.checked, f.parent, 1, NULL, &allocator, &s) ==
                  GT_STAGE_LIMIT &&
              !s);
    } else if (!strcmp(mode, "unsupported")) {
        CHECK(gt__package_stage_open(
                  f.checked, f.parent, GT_PACKAGE_TOTAL_BYTES_MAX, NULL, &allocator, &s) ==
                  GT_STAGE_UNSUPPORTED &&
              !s);
    } else if (!strcmp(mode, "invalid")) {
        CHECK(gt__package_stage_open(f.checked, f.parent, 0, NULL, &allocator, &s) ==
              GT_STAGE_INVALID);
        CHECK(gt__package_stage_open(NULL, f.parent, 1, NULL, &allocator, &s) == GT_STAGE_INVALID);
        CHECK(gt__package_stage_open(f.checked, -1, 1, NULL, &allocator, &s) == GT_STAGE_INVALID);
        CHECK(gt__package_stage_open(f.checked, f.parent, 1, NULL, &allocator, NULL) ==
              GT_STAGE_INVALID);
        CHECK(!fchmod(f.parent, 0755));
        CHECK(gt__package_stage_open(
                  f.checked, f.parent, GT_PACKAGE_TOTAL_BYTES_MAX, NULL, &allocator, &s) ==
                  GT_STAGE_INVALID &&
              !s);
        CHECK(!fchmod(f.parent, 0700));
        CHECK(gt__package_stage_step(NULL) == GT_STAGE_INVALID &&
              gt__package_stage_destroy(NULL) == GT_STAGE_INVALID &&
              gt__package_stage_destroy(&s) == GT_STAGE_INVALID);
    } else if (!strcmp(mode, "allocation")) {
        bool completed = false;
        for (size_t fail = 1; fail < 64 && !completed; fail++) {
            f.memory.calls = 0;
            f.memory.fail = fail;
            gt_package_stage_status_t status = gt__package_stage_open(
                f.checked, f.parent, GT_PACKAGE_TOTAL_BYTES_MAX, NULL, &allocator, &s);
            if (status == GT_STAGE_OK) {
                do {
                    status = gt__package_stage_step(s);
                } while (status == GT_STAGE_WAIT);
                completed = status == GT_STAGE_OK;
                CHECK(completed || status == GT_STAGE_NOMEM);
                CHECK(gt__package_stage_destroy(&s) == GT_STAGE_OK);
            } else {
                CHECK(status == GT_STAGE_NOMEM && !s);
            }
            CHECK(!f.memory.live && f.memory.wiped && !stage_children(f.parent, NULL));
        }
        CHECK(completed);
    } else if (!strcmp(mode, "global-memory")) {
        gt_plugin_memory_info_t budget = gt__plugin_memory_info();
        size_t n = budget.limit - budget.used - 1;
        void *reservation = gt__plugin_memory_alloc(NULL, n);
        CHECK(reservation);
        CHECK(gt__package_stage_open(
                  f.checked, f.parent, GT_PACKAGE_TOTAL_BYTES_MAX, NULL, &allocator, &s) ==
                  GT_STAGE_NOMEM &&
              !s);
        gt__plugin_memory_free(NULL, reservation, n);
    } else {
        bool defaults = !strcmp(mode, "default");
        CHECK(gt__package_stage_open(f.checked,
                                     f.parent,
                                     GT_PACKAGE_TOTAL_BYTES_MAX,
                                     &stop,
                                     defaults ? NULL : &allocator,
                                     &s) == GT_STAGE_OK &&
              s);
        CHECK(!stage_children(f.parent, NULL));
        int fd = 42;
        const char *name = "stale", *entry = "stale";
        CHECK(gt__package_stage_view(s, &fd, &name, &entry) == GT_STAGE_WAIT && fd == -1 && !name &&
              !entry);
        if (!strcmp(mode, "frozen")) {
            CHECK(!put(f.source, "executor/main.mjs", "CHANGED", 7));
            CHECK(!put(f.source, "node_modules/pkg/中文.mjs", "CHANGED", 7));
            CHECK(!put(f.source, "new-file", "UNLISTED", 8));
        }
        if (!strcmp(mode, "retained")) {
            CHECK(gt__package_checked_destroy(&f.checked) == GT_PACKAGE_OK);
        }
        if (!strcmp(mode, "parent-close")) {
            CHECK(!close(f.parent));
            f.parent = -1;
        }
        if (!strcmp(mode, "cancel-mid")) {
            char root_name[256];
            bool partial = false;
            for (size_t i = 0; i < 100 && !partial; i++) {
                CHECK(gt__package_stage_step(s) == GT_STAGE_WAIT);
                CHECK(stage_children(f.parent, root_name) == 1);
                int root = openat(f.parent, root_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                struct stat st;
                partial = root >= 0 && !fstatat(root, "binary.dat", &st, AT_SYMLINK_NOFOLLOW) &&
                          st.st_size > 0 && st.st_size <= GT_PACKAGE_CHUNK_MAX;
                CHECK(root >= 0 && !close(root));
            }
            CHECK(partial);
            atomic_store(&stop, true);
            CHECK(gt__package_stage_step(s) == GT_STAGE_CANCELLED &&
                  gt__package_stage_step(s) == GT_STAGE_CANCELLED);
            CHECK(gt__package_stage_destroy(&s) == GT_STAGE_OK);
        } else {
            stage_finish(s);
            CHECK(gt__package_stage_view(s, &fd, &name, &entry) == GT_STAGE_OK && fd >= 0 &&
                  !strncmp(name, "runtime-", 8) && !strcmp(entry, "executor/main.mjs"));
            CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
            char root_name[48];
            strcpy(root_name, name);
            if (f.checked) {
                stage_compare(f.checked, fd, !strcmp(mode, "exec"));
            }
            if (!strcmp(mode, "parent-close")) {
                f.parent = openat(f.root_fd, "parent", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                CHECK(f.parent >= 0);
            }
            if (!strcmp(mode, "cancel-ready")) {
                atomic_store(&stop, true);
                CHECK(gt__package_stage_view(s, &fd, &name, &entry) == GT_STAGE_CANCELLED &&
                      fd == -1 && !name && !entry);
                CHECK(gt__package_stage_step(s) == GT_STAGE_CANCELLED);
            }
            if (!strcmp(mode, "replace-file")) {
                CHECK(!fchmod(fd, 0700) && !fchmodat(fd, "executor", 0700, 0));
                CHECK(!renameat(fd, entry, f.root_fd, "saved-file"));
                CHECK(!put(fd, entry, "FOREIGN", 7));
                CHECK(gt__package_stage_destroy(&s) == GT_STAGE_CHANGED);
                int root = openat(f.parent, root_name, O_RDONLY | O_DIRECTORY);
                char bytes[8] = {0};
                int foreign = openat(root, "executor/main.mjs", O_RDONLY);
                CHECK(foreign >= 0 && read(foreign, bytes, 7) == 7 && !strcmp(bytes, "FOREIGN"));
                CHECK(!close(foreign) && !clear(root) && !close(root) &&
                      !unlinkat(f.parent, root_name, AT_REMOVEDIR) &&
                      !unlinkat(f.root_fd, "saved-file", 0));
            } else if (!strcmp(mode, "replace-dir")) {
                CHECK(!fchmod(fd, 0700) && !fchmodat(fd, "executor", 0700, 0) &&
                      !renameat(fd, "executor", f.root_fd, "saved-dir"));
                CHECK(!symlinkat("../..", fd, "executor"));
                CHECK(gt__package_stage_destroy(&s) == GT_STAGE_CHANGED);
                int root = openat(f.parent, root_name, O_RDONLY | O_DIRECTORY);
                struct stat st;
                CHECK(root >= 0 && !fstatat(root, "executor", &st, AT_SYMLINK_NOFOLLOW) &&
                      S_ISLNK(st.st_mode) && !unlinkat(root, "executor", 0) && !close(root));
                CHECK(!unlinkat(f.parent, root_name, AT_REMOVEDIR));
                int saved = openat(f.root_fd, "saved-dir", O_RDONLY | O_DIRECTORY);
                CHECK(saved >= 0 && !fchmod(saved, 0700) && !clear(saved) && !close(saved) &&
                      !unlinkat(f.root_fd, "saved-dir", AT_REMOVEDIR));
            } else if (!strcmp(mode, "replace-root")) {
                CHECK(!fchmod(fd, 0700) && !renameat(f.parent, root_name, f.root_fd, "saved-root"));
                CHECK(!mkdirat(f.parent, root_name, 0700));
                CHECK(gt__package_stage_view(s, &fd, &name, &entry) == GT_STAGE_CHANGED &&
                      fd == -1);
                CHECK(gt__package_stage_destroy(&s) == GT_STAGE_CHANGED);
                CHECK(stage_children(f.parent, NULL) == 1 &&
                      !unlinkat(f.parent, root_name, AT_REMOVEDIR) &&
                      !unlinkat(f.root_fd, "saved-root", AT_REMOVEDIR));
            } else if (!strcmp(mode, "unknown-file")) {
                CHECK(!fchmod(fd, 0700) && !put(fd, "foreign", "KEEP", 4));
                CHECK(gt__package_stage_destroy(&s) == GT_STAGE_IO);
                int root = openat(f.parent, root_name, O_RDONLY | O_DIRECTORY);
                CHECK(root >= 0 && stage_children(root, NULL) == 1 &&
                      !unlinkat(root, "foreign", 0) && !close(root) &&
                      !unlinkat(f.parent, root_name, AT_REMOVEDIR));
            } else {
                CHECK(gt__package_stage_destroy(&s) == GT_STAGE_OK && !s);
            }
        }
    }
    CHECK(!s);
    stage_clear(&f);
    return 0;
}
