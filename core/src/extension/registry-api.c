#include "ghosterm/extension_registry.h"
#include "extension/registry.h"
#include "extension/package-owner.h"
#include "extension/memory.h"
#include <string.h>

struct gt_ext_registry {
    gt_plugin_registry_t *inner;
};

static gt_ext_registry_status_t registry_status(gt_registry_status_t status) {
    switch (status) {
        case GT_REGISTRY_OK:
            return GT_EXT_REGISTRY_OK;
        case GT_REGISTRY_INVALID:
            return GT_EXT_REGISTRY_INVALID;
        case GT_REGISTRY_UNKNOWN:
            return GT_EXT_REGISTRY_UNKNOWN;
        case GT_REGISTRY_DENIED:
            return GT_EXT_REGISTRY_DENIED;
        case GT_REGISTRY_LIMIT:
            return GT_EXT_REGISTRY_LIMIT;
        case GT_REGISTRY_MEMORY:
            return GT_EXT_REGISTRY_MEMORY;
        case GT_REGISTRY_NOT_FOUND:
            return GT_EXT_REGISTRY_NOT_FOUND;
        case GT_REGISTRY_CONFLICT:
            return GT_EXT_REGISTRY_CONFLICT;
        case GT_REGISTRY_STALE:
            return GT_EXT_REGISTRY_STALE;
        case GT_REGISTRY_IO:
            return GT_EXT_REGISTRY_IO;
    }
    return GT_EXT_REGISTRY_INVALID;
}

static void registry_command(const gt_package_command_t *c, gt_ext_package_command_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->path, c->path, sizeof(c->path));
    memcpy(out->title, c->title, sizeof(c->title));
    memcpy(out->args_schema, c->args_schema, sizeof(c->args_schema));
    memcpy(out->views, c->views, sizeof(c->views));
    out->view_count = (uint32_t)c->view_count;
}

bool gt_ext_package_command_count(const gt_ext_package_t *p, uint32_t *out) {
    if (out) {
        *out = 0;
    }
    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED) {
        return false;
    }
    *out = (uint32_t)gt__package_checked_command_count(p->checked);
    return true;
}

bool gt_ext_package_command(const gt_ext_package_t *p,
                            uint32_t index,
                            gt_ext_package_command_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED) {
        return false;
    }
    gt_package_command_t command;
    if (!gt__package_checked_command(p->checked, index, &command)) {
        return false;
    }
    registry_command(&command, out);
    return true;
}

gt_ext_registry_status_t gt_ext_registry_open(gt_ext_registry_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return GT_EXT_REGISTRY_INVALID;
    }
#ifndef GT_HAVE_PLUGIN_JS
    return GT_EXT_REGISTRY_UNSUPPORTED;
#else
    gt_ext_registry_t *r = gt__plugin_memory_calloc(1, sizeof(*r));
    if (!r) {
        return GT_EXT_REGISTRY_MEMORY;
    }
    gt_registry_status_t status = gt__registry_open(&r->inner);
    if (status != GT_REGISTRY_OK) {
        gt__plugin_memory_free(NULL, r, sizeof(*r));
        return registry_status(status);
    }
    *out = r;
    return GT_EXT_REGISTRY_OK;
#endif
}

gt_ext_registry_status_t gt_ext_registry_put(gt_ext_registry_t *r,
                                             const gt_ext_package_t *p,
                                             gt_ext_registry_source_t source,
                                             const char *expected) {
    if (!r || !p || p->info.phase != GT_EXT_PACKAGE_CHECKED) {
        return GT_EXT_REGISTRY_INVALID;
    }
    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        return GT_EXT_REGISTRY_CANCELLED;
    }
    gt_registry_source_t fact;
    switch (source) {
        case GT_EXT_REGISTRY_SOURCE_UNKNOWN:
            fact = GT_REGISTRY_SOURCE_UNKNOWN;
            break;
        case GT_EXT_REGISTRY_SOURCE_DENIED:
            fact = GT_REGISTRY_SOURCE_DENIED;
            break;
        case GT_EXT_REGISTRY_SOURCE_ALLOWED:
            fact = GT_REGISTRY_SOURCE_ALLOWED;
            break;
        default:
            return GT_EXT_REGISTRY_INVALID;
    }
    return registry_status(gt__registry_put(r->inner, p->checked, fact, expected));
}

gt_ext_registry_status_t
gt_ext_registry_remove(gt_ext_registry_t *r, const char *id, bool executor, const char *expected) {
    return r ? registry_status(gt__registry_remove(r->inner, id, executor, expected))
             : GT_EXT_REGISTRY_INVALID;
}

gt_ext_registry_status_t gt_ext_registry_resolve(gt_ext_registry_t *r,
                                                 const char *command,
                                                 bool executor,
                                                 const char *id,
                                                 const char *digest,
                                                 gt_ext_package_t **out,
                                                 gt_ext_package_command_t *contribution) {
    if (out) {
        *out = NULL;
    }
    if (contribution) {
        memset(contribution, 0, sizeof(*contribution));
    }
    if (!r || !out || !contribution) {
        return GT_EXT_REGISTRY_INVALID;
    }
    gt_ext_package_t *p = gt__plugin_memory_calloc(1, sizeof(*p));
    if (!p) {
        return GT_EXT_REGISTRY_MEMORY;
    }
    atomic_init(&p->stopped, false);
    gt_package_command_t c;
    gt_registry_status_t status =
        gt__registry_resolve(r->inner, command, executor, id, digest, &p->checked, &c);
    if (status != GT_REGISTRY_OK) {
        gt__plugin_memory_free(NULL, p, sizeof(*p));
        return registry_status(status);
    }
    size_t count = 0;
    uint64_t bytes = 0;
    if (!gt__package_checked_stats(p->checked, &count, &bytes)) {
        gt_ext_package_destroy(&p);
        return GT_EXT_REGISTRY_IO;
    }
    p->info.phase = GT_EXT_PACKAGE_CHECKED;
    p->info.members = count;
    p->info.bytes = bytes;
    memcpy(p->info.code, "COMPATIBLE", 11);
    registry_command(&c, contribution);
    *out = p;
    return GT_EXT_REGISTRY_OK;
}

gt_ext_registry_status_t gt_ext_registry_destroy(gt_ext_registry_t **out) {
    if (!out || !*out) {
        return GT_EXT_REGISTRY_INVALID;
    }
    gt_ext_registry_t *r = *out;
    *out = NULL;
    gt_registry_status_t status = gt__registry_destroy(&r->inner);
    gt__plugin_memory_free(NULL, r, sizeof(*r));
    return registry_status(status);
}
