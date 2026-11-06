#define _POSIX_C_SOURCE 200809L
#include "extension/registry.h"
#include "extension/memory.h"
#include <string.h>

typedef struct {
    gt_package_checked_t *package;
    char id[129], digest[65];
    bool executor;
} registry_entry_t;

struct gt_plugin_registry {
    registry_entry_t entries[GT_PLUGIN_REGISTRY_MAX];
};

static bool digest_valid(const char *digest) {
    if (!digest || strnlen(digest, 65) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool command_valid(const char *command) {
    if (!command || !*command || strnlen(command, 520) >= 520) {
        return false;
    }
    size_t length = 0, tokens = 1;
    for (const char *p = command; *p; p++) {
        if (*p == ' ') {
            if (!length || ++tokens > 8) {
                return false;
            }
            length = 0;
            continue;
        }
        if (!length && (*p < 'a' || *p > 'z')) {
            return false;
        }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') || ++length > 64) {
            return false;
        }
    }
    return length != 0;
}

static bool overlap(const char *a, const char *b) {
    size_t x = strlen(a), y = strlen(b), n = x < y ? x : y;
    return !memcmp(a, b, n) && (x == y || (x < y ? b[n] : a[n]) == ' ');
}

gt_registry_status_t gt__registry_open(gt_plugin_registry_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return GT_REGISTRY_INVALID;
    }
    *out = gt__plugin_memory_calloc(1, sizeof(**out));
    return *out ? GT_REGISTRY_OK : GT_REGISTRY_MEMORY;
}

gt_registry_status_t gt__registry_put(gt_plugin_registry_t *r,
                                      gt_package_checked_t *p,
                                      gt_registry_source_t source,
                                      const char *expected) {
    if (!r || !p || (expected && !digest_valid(expected))) {
        return GT_REGISTRY_INVALID;
    }
    if (source == GT_REGISTRY_SOURCE_UNKNOWN) {
        return GT_REGISTRY_UNKNOWN;
    }
    if (source == GT_REGISTRY_SOURCE_DENIED) {
        return GT_REGISTRY_DENIED;
    }
    if (source != GT_REGISTRY_SOURCE_ALLOWED) {
        return GT_REGISTRY_INVALID;
    }
    gt_package_check_info_t info;
    if (!gt__package_checked_info(p, &info) || !gt__package_checked_command_count(p)) {
        return GT_REGISTRY_INVALID;
    }
    registry_entry_t *entry = NULL, *empty = NULL;
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX; i++) {
        registry_entry_t *e = &r->entries[i];
        if (!e->package) {
            if (!empty) {
                empty = e;
            }
        } else if (e->executor == info.executor && !strcmp(e->id, info.id)) {
            entry = e;
            break;
        }
    }
    if (entry) {
        if (!expected) {
            return GT_REGISTRY_CONFLICT;
        }
        if (strcmp(entry->digest, expected)) {
            return GT_REGISTRY_STALE;
        }
    } else {
        if (expected) {
            return GT_REGISTRY_STALE;
        }
        if (!empty) {
            return GT_REGISTRY_LIMIT;
        }
        entry = empty;
    }
    gt_package_checked_t *hold = NULL;
    if (!gt__package_checked_retain(p, &hold)) {
        return GT_REGISTRY_MEMORY;
    }
    gt_package_checked_t *old = entry->package;
    entry->package = hold;
    memcpy(entry->id, info.id, sizeof(entry->id));
    memcpy(entry->digest, info.package_digest, sizeof(entry->digest));
    entry->executor = info.executor;
    return old && gt__package_checked_destroy(&old) != GT_PACKAGE_OK ? GT_REGISTRY_IO
                                                                     : GT_REGISTRY_OK;
}

gt_registry_status_t
gt__registry_remove(gt_plugin_registry_t *r, const char *id, bool executor, const char *expected) {
    if (!r || !id || !digest_valid(expected)) {
        return GT_REGISTRY_INVALID;
    }
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX; i++) {
        registry_entry_t *e = &r->entries[i];
        if (e->package && e->executor == executor && !strcmp(e->id, id)) {
            if (strcmp(e->digest, expected)) {
                return GT_REGISTRY_STALE;
            }
            gt_package_checked_t *p = e->package;
            memset(e, 0, sizeof(*e));
            return gt__package_checked_destroy(&p) == GT_PACKAGE_OK ? GT_REGISTRY_OK
                                                                    : GT_REGISTRY_IO;
        }
    }
    return GT_REGISTRY_NOT_FOUND;
}

gt_registry_status_t gt__registry_resolve(gt_plugin_registry_t *r,
                                          const char *command,
                                          bool executor,
                                          const char *id,
                                          const char *digest,
                                          gt_package_checked_t **out,
                                          gt_package_command_t *contribution) {
    if (out) {
        *out = NULL;
    }
    if (contribution) {
        memset(contribution, 0, sizeof(*contribution));
    }
    if (!r || !out || !contribution || !command_valid(command) || (!!id != !!digest) ||
        (digest && !digest_valid(digest))) {
        return GT_REGISTRY_INVALID;
    }
    registry_entry_t *selected = NULL, *exact = NULL;
    size_t exact_index = 0, overlaps = 0;
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX; i++) {
        registry_entry_t *e = &r->entries[i];
        if (!e->package || e->executor != executor) {
            continue;
        }
        if (id && !strcmp(id, e->id)) {
            if (strcmp(digest, e->digest)) {
                return GT_REGISTRY_STALE;
            }
            selected = e;
        }
        for (size_t j = 0; j < gt__package_checked_command_count(e->package); j++) {
            gt_package_command_t c;
            if (!gt__package_checked_command(e->package, j, &c)) {
                return GT_REGISTRY_INVALID;
            }
            if (overlap(command, c.path)) {
                overlaps++;
            }
            if (!strcmp(command, c.path) && (!id || !strcmp(id, e->id))) {
                exact = e;
                exact_index = j;
            }
        }
    }
    if (id && !selected) {
        return GT_REGISTRY_STALE;
    }
    if (!exact) {
        return GT_REGISTRY_NOT_FOUND;
    }
    if (!id && overlaps > 1) {
        return GT_REGISTRY_CONFLICT;
    }
    if (!gt__package_checked_command(exact->package, exact_index, contribution)) {
        return GT_REGISTRY_INVALID;
    }
    if (!gt__package_checked_retain(exact->package, out)) {
        memset(contribution, 0, sizeof(*contribution));
        return GT_REGISTRY_MEMORY;
    }
    return GT_REGISTRY_OK;
}

gt_registry_status_t gt__registry_destroy(gt_plugin_registry_t **out) {
    if (!out || !*out) {
        return GT_REGISTRY_INVALID;
    }
    gt_plugin_registry_t *r = *out;
    *out = NULL;
    gt_registry_status_t status = GT_REGISTRY_OK;
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX; i++) {
        if (r->entries[i].package &&
            gt__package_checked_destroy(&r->entries[i].package) != GT_PACKAGE_OK) {
            status = GT_REGISTRY_IO;
        }
    }
    gt__plugin_memory_free(NULL, r, sizeof(*r));
    return status;
}
