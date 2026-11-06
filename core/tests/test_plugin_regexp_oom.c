/* Raw pinned regexp compiler; no JS evaluator, package checker or Core owner. */
#include <libregexp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL plugin-regexp:%d %s\n", __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    size_t calls, fail, live;
} Memory;

typedef union {
    max_align_t alignment;
    size_t size;
} Allocation;
#ifdef GT_REGEXP_BOUNDARY
extern unsigned gt_test_force_regexp_oom, gt_test_regexp_injected, gt_test_regexp_scans;
extern unsigned gt_test_force_group_oom;
#endif
bool lre_check_stack_overflow(void *opaque, size_t size) {
    (void)opaque;
    (void)size;
    return false;
}

int lre_check_timeout(void *opaque) {
    (void)opaque;
    return 0;
}

void *lre_realloc(void *opaque, void *ptr, size_t size) {
    Memory *m = opaque;
    if (!size) {
        if (ptr) {
            m->live--;
            free((Allocation *)ptr - 1);
        }
        return NULL;
    }
    if (++m->calls == m->fail || size > SIZE_MAX - sizeof(Allocation)) {
        return NULL;
    }
    Allocation *a = malloc(sizeof(*a) + size);
    if (!a) {
        return NULL;
    }
    a->size = size;
    if (ptr) {
        Allocation *old = (Allocation *)ptr - 1;
        memcpy(a + 1, ptr, old->size < size ? old->size : size);
        free(old);
    } else {
        m->live++;
    }
    return a + 1;
}

static int compile(const char *pattern, int flags, size_t fail, size_t *calls, bool injected) {
    Memory m = {.fail = fail};
    char error[128] = {0};
    int length = -1;
    uint8_t *bytes =
        lre_compile(&length, error, sizeof(error), pattern, strlen(pattern), flags, &m);
    bool ok = false;
    if (bytes) {
        ok = !fail && !injected && length > 0 && !lre_check_bytecode(bytes, length);
        lre_realloc(&m, bytes, 0);
    } else {
        ok = length == 0 && (injected || (fail && m.calls >= fail)) && strstr(error, "memory");
    }
    if (!ok || m.live) {
        fprintf(stderr,
                "pattern=%s fail=%zu calls=%zu length=%d live=%zu error=%s\n",
                pattern,
                fail,
                m.calls,
                length,
                m.live,
                error);
    }
    CHECK(ok && !m.live);
#ifdef GT_REGEXP_BOUNDARY
    CHECK(!gt_test_regexp_scans);
#endif
    *calls = m.calls;
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    size_t calls = 0, total = 0;
    if (!strcmp(argv[1], "control")) {
        return compile("literal", 0, 0, &calls, false);
    }
#ifdef GT_REGEXP_BOUNDARY
    if (!strcmp(argv[1], "boundary")) {
        gt_test_force_regexp_oom = 1;
        CHECK(!compile("[a-z]+", 0, 0, &calls, true));
        CHECK(gt_test_regexp_injected == 1 && !gt_test_force_regexp_oom);
        return 0;
    }
    if (!strcmp(argv[1], "group-boundary")) {
        gt_test_force_group_oom = 1;
        CHECK(!compile("(a)", 0, 0, &calls, true));
        CHECK(gt_test_regexp_injected == 1 && !gt_test_force_group_oom);
        return 0;
    }
#endif
    CHECK(!strcmp(argv[1], "sweep"));
    const char *patterns[] = {"[a-zA-Z0-9_]{1,32}",
                              "((?:a|bc|[a-z]){1,10})+?",
                              "^(?:0|[1-9][0-9]*)(?:\\.(?:0|[1-9][0-9]*)){2}(?:-[a-z0-9.-]+)?$",
                              "(?<word>[a-z]+)\\k<word>",
                              "(?<=a|bc)[a-z]*",
                              "(?:(a)?(b){0,7}){2,9}",
                              "[\\p{Letter}\\p{Number}]+"};
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        size_t baseline = 0;
        int flags = LRE_FLAG_UNICODE;
        CHECK(!compile(patterns[i], flags, 0, &baseline, false));
        for (size_t fail = 1; fail <= baseline; fail++) {
            CHECK(!compile(patterns[i], flags, fail, &calls, false));
        }
        CHECK(!compile(patterns[i], flags, 0, &calls, false) && calls == baseline);
        total += baseline;
    }
    printf("REGEXP OOM PASS: 7 patterns / %zu allocation ordinals; no surviving allocations\n",
           total);
    return 0;
}
