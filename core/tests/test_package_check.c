#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/package-check.h"
#include "extension/memory.h"
#include "base/sha256.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d %s\n", mode, __LINE__, #x);                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    size_t calls, fail, live;
    void *preflight_owner;
    bool wiped;
    atomic_bool *late_stop;
} memory_t;

static void *alloc(void *p, size_t n) {
    memory_t *m = p;
    if (++m->calls == m->fail) {
        return NULL;
    }
    void *out = malloc(n);
    if (out) {
        if (m->calls == 1) {
            m->preflight_owner = out;
        }
        m->live++;
        memset(out, 0xa5, n);
    }
    return out;
}

static void release(void *p, void *bytes, size_t n) {
    memory_t *m = p;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char *)bytes)[i]) {
            m->wiped = false;
        }
    }
    bool preflight = bytes == m->preflight_owner;
    m->live--;
    free(bytes);
    if (preflight && m->late_stop) {
        atomic_store(m->late_stop, true);
    }
}

static char *file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *p = malloc(1048578);
    if (!p) {
        fclose(f);
        return NULL;
    }
    *size = fread(p, 1, 1048577, f);
    bool ok = !ferror(f);
    if (fclose(f)) {
        ok = false;
    }
    if (!ok) {
        free(p);
        return NULL;
    }
    p[*size] = 0;
    return p;
}

static void quote(const char *p) {
    putchar('"');
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 32) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "invalid";
    gt_package_checked_t *checked = NULL;
    gt_package_check_report_t report;
    if (!strcmp(mode, "disabled")) {
        CHECK(gt__package_check(NULL, NULL, NULL, &checked, &report) ==
                  GT_PACKAGE_CHECK_UNSUPPORTED &&
              !checked && report.status == GT_PACKAGE_CHECK_UNSUPPORTED &&
              !strcmp(report.code, "ENGINE_UNAVAILABLE"));
        return 0;
    }
    CHECK(!strcmp(mode, "probe") && (argc == 7 || argc == 8));
    int fd = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(fd >= 0);
    gt_package_capture_config_t capture = {fd,
                                           GT_PACKAGE_MEMBERS_MAX,
                                           GT_PACKAGE_ENTRIES_MAX,
                                           GT_PACKAGE_MEMBER_BYTES_MAX,
                                           GT_PACKAGE_TOTAL_BYTES_MAX};
    gt_package_snapshot_t *snapshot = NULL;
    CHECK(gt__package_capture_open(&capture, NULL, &snapshot) == GT_PACKAGE_OK);
    const uint8_t *view = (void *)1;
    size_t view_size = 99;
    CHECK(!gt__package_snapshot_view(snapshot, 0, &view, &view_size) && !view && !view_size);
    CHECK(gt__package_check(&snapshot, NULL, NULL, &checked, &report) == GT_PACKAGE_CHECK_INVALID &&
          snapshot && !checked && !strcmp(report.code, "ARGUMENT"));
    gt_package_status_t state = GT_PACKAGE_WAIT;
    for (size_t i = 0; i < 50000 && state == GT_PACKAGE_WAIT; i++) {
        state = gt__package_capture_step(snapshot);
    }
    CHECK(state == GT_PACKAGE_OK);
    size_t host_size = 0, registry_size = 0;
    char *host = NULL, *registry = NULL;
    if (strcmp(argv[3], "-")) {
        host = file(argv[3], &host_size);
        CHECK(host);
    }
    if (strcmp(argv[4], "-")) {
        registry = file(argv[4], &registry_size);
        CHECK(registry);
    }
    const char *option = argc == 8 ? argv[7] : "";
    atomic_bool stop;
    atomic_init(&stop, !strcmp(option, "cancel"));
    memory_t m = {.wiped = true, .late_stop = !strcmp(option, "late-cancel") ? &stop : NULL};
    if (!strncmp(option, "fail=", 5)) {
        m.fail = (size_t)strtoull(option + 5, NULL, 10);
    }
    gt_allocator_t a = {&m, alloc, release};
    const gt_package_check_config_t config = {.host_json = host,
                                              .host_size = host_size,
                                              .registry_json = registry,
                                              .registry_size = registry_size,
                                              .stop = &stop};
    const gt_package_check_status_t expected = (gt_package_check_status_t)strtol(argv[5], NULL, 10);
    const char *expected_code = argv[6];
    const gt_package_check_status_t status =
        gt__package_check(&snapshot, &config, &a, &checked, &report);
    if (status != expected || strcmp(report.code, expected_code)) {
        fprintf(stderr,
                "actual status=%u code=%s expected=%u %s\n",
                (unsigned)status,
                report.code,
                (unsigned)expected,
                expected_code);
    }
    CHECK(status == expected && report.status == status && !strcmp(report.code, expected_code));
    gt_package_check_info_t info;
    memset(&info, 0xff, sizeof(info));
    if (status == GT_PACKAGE_CHECK_COMPATIBLE) {
        CHECK(checked && !snapshot && gt__package_checked_info(checked, &info));
        CHECK(!info.authorization_granted && !info.publisher_authenticated);
        CHECK(info.protocol_count && info.requires_count <= 16 &&
              info.optional_missing_count <= 16 && info.process_program_count <= 16);
        gt_package_member_t member;
        CHECK(gt__package_checked_member(checked, "manifest.json", &member, &view, &view_size));
        gt_sha256_t hash;
        char hex[65];
        gt__sha256_init(&hash);
        CHECK(gt__sha256_update(&hash, view, view_size) && gt__sha256_final(&hash, hex) &&
              !strcmp(hex, info.manifest_digest));
        CHECK(gt__package_checked_member(checked, "integrity.json", &member, &view, &view_size));
        gt__sha256_init(&hash);
        CHECK(gt__sha256_update(&hash, view, view_size) && gt__sha256_final(&hash, hex) &&
              !strcmp(hex, info.package_digest));
        if (!strcmp(option, "association=true")) {
            CHECK(info.association_required);
        }
        CHECK(gt__package_checked_member(checked, info.entry, &member, &view, &view_size));
        CHECK(view || !view_size);
        CHECK(!gt__package_checked_member(checked, "../escape", &member, &view, &view_size) &&
              !view && !view_size && !member.path);
        CHECK(gt__package_checked_destroy(&checked) == GT_PACKAGE_OK && !checked);
    } else {
        size_t count;
        uint64_t bytes;
        CHECK(!checked && snapshot && gt__package_snapshot_count(snapshot, &count, &bytes));
        CHECK(!gt__package_checked_info(checked, &info) && !info.id[0]);
        CHECK(gt__package_snapshot_destroy(&snapshot) == GT_PACKAGE_OK && !snapshot);
    }
    CHECK(!m.live && m.wiped && !gt__plugin_memory_info().used);
    free(host);
    free(registry);
    CHECK(close(fd) == 0);
    printf("{\"status\":%u,\"code\":", (unsigned)status);
    quote(report.code);
    printf(",\"allocationCalls\":%zu", m.calls);
    if (status == GT_PACKAGE_CHECK_COMPATIBLE) {
        printf(",\"id\":");
        quote(info.id);
        printf(",\"version\":");
        quote(info.version);
        printf(",\"entry\":");
        quote(info.entry);
        printf(",\"runtime\":");
        quote(info.runtime);
        printf(",\"packageDigest\":");
        quote(info.package_digest);
        printf(",\"manifestDigest\":");
        quote(info.manifest_digest);
        printf(",\"executor\":%s,\"associationRequired\":%s,\"authorizationGranted\":false,"
               "\"publisherAuthenticated\":false,\"protocols\":[",
               info.executor ? "true" : "false",
               info.association_required ? "true" : "false");
        for (size_t i = 0; i < info.protocol_count; i++) {
            printf("%s%u", i ? "," : "", info.protocols[i]);
        }
        printf("],\"optionalMissing\":[");
        for (size_t i = 0; i < info.optional_missing_count; i++) {
            if (i) {
                putchar(',');
            }
            quote(info.optional_missing[i]);
        }
        printf("],\"processPrograms\":[");
        for (size_t i = 0; i < info.process_program_count; i++) {
            if (i) {
                putchar(',');
            }
            quote(info.process_programs[i]);
        }
        putchar(']');
    }
    puts("}");
    return 0;
}
