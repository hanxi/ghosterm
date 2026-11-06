#define _POSIX_C_SOURCE 200809L
#include "extension/package-runtime.h"
#include "extension/sdk-identity.h"
#include <string.h>

bool gt__runtime_source_utf8(const void *bytes, size_t size) {
    if (!bytes && size) {
        return false;
    }
    const uint8_t *p = bytes;
    size_t at = 0;
    while (at < size) {
        uint8_t b = p[at++];
        uint32_t cp, minimum;
        unsigned tails;
        if (b < 0x80) {
            if (!b) {
                return false;
            }
            continue;
        }
        if (b >= 0xc2 && b <= 0xdf) {
            cp = b & 31;
            minimum = 0x80;
            tails = 1;
        } else if (b >= 0xe0 && b <= 0xef) {
            cp = b & 15;
            minimum = 0x800;
            tails = 2;
        } else if (b >= 0xf0 && b <= 0xf4) {
            cp = b & 7;
            minimum = 0x10000;
            tails = 3;
        } else {
            return false;
        }
        if (tails > size - at) {
            return false;
        }
        while (tails--) {
            b = p[at++];
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool gt__runtime_module_name(const char *name) {
    if (!name) {
        return false;
    }
    size_t n = strnlen(name, GT_RUNTIME_MODULE_NAME_MAX + 1);
    if (n > GT_RUNTIME_MODULE_NAME_MAX) {
        return false;
    }
    size_t prefix = !strncmp(name, "plugin:/", 8) ? 8 : !strncmp(name, "sdk:/", 5) ? 5 : 0;
    return prefix && n > prefix && gt__package_path_valid(name + prefix, n - prefix);
}

gt_status_t gt__runtime_package_entry(gt_package_checked_t *package,
                                      gt_bridge_session_t *session,
                                      gt_bridge_role_t role,
                                      const gt_bridge_lease_t *lease,
                                      bool associated,
                                      char *entry,
                                      size_t cap) {
    if (entry && cap) {
        entry[0] = 0;
    }
    gt_package_check_info_t info;
    if (!entry || !cap || !gt__package_checked_info(package, &info) || !session || !lease ||
        (role != GT_BRIDGE_HOST && role != GT_BRIDGE_EXECUTOR)) {
        return GT_ERR_INVALID_ARG;
    }
    if (strcmp(info.runtime, "portable-js") ||
        strcmp(info.sdk_version, GT_PLUGIN_SDK_API_VERSION)) {
        return GT_ERR_UNSUPPORTED;
    }
    bool protocol = false;
    for (size_t i = 0; i < info.protocol_count; i++) {
        if (info.protocols[i] == GT_PLUGIN_SDK_PROTOCOL) {
            protocol = true;
        }
    }
    if (!protocol) {
        return GT_ERR_UNSUPPORTED;
    }
    if (info.executor != (role == GT_BRIDGE_EXECUTOR) ||
        !gt__bridge_session_package_matches(
            session, role, info.id, info.version, info.package_digest, GT_PLUGIN_SDK_API_VERSION)) {
        return GT_ERR_PROTOCOL;
    }
    if (!gt__bridge_session_scope(session, lease) || (info.association_required && !associated)) {
        return GT_ERR_AUTH;
    }
    for (size_t i = 0; i < info.requires_count; i++) {
        const char *name = info.requires[i];
        if (strcmp(name, "session.rpc") && strcmp(name, "file.readScoped") &&
            strcmp(name, "resource.receive")) {
            return GT_ERR_UNSUPPORTED;
        }
        if (gt__bridge_session_access(session, lease, name) != GT_BRIDGE_ACCESS_ALLOWED) {
            return GT_ERR_AUTH;
        }
    }
    size_t n = strlen(info.entry);
    if (n + 8 >= cap) {
        return GT_ERR_INVALID_ARG;
    }
    memcpy(entry, "plugin:/", 8);
    memcpy(entry + 8, info.entry, n + 1);
    if (!gt__runtime_module_name(entry)) {
        entry[0] = 0;
        return GT_ERR_INVALID_ARG;
    }
    return GT_OK;
}
#ifdef GT_HAVE_PLUGIN_JS
typedef struct {
    const char *name;
    size_t size;
    const char *sha256;
    const char *const *chunks;
    size_t count;
} runtime_sdk_module_t;

#include "extension/sdk-code.inc"
#endif
gt_status_t gt__runtime_package_source(const gt_package_checked_t *package,
                                       const char *name,
                                       gt_runtime_source_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!out || !package || !gt__runtime_module_name(name)) {
        return GT_ERR_INVALID_ARG;
    }
#ifndef GT_HAVE_PLUGIN_JS
    return GT_ERR_UNSUPPORTED;
#else
    if (!strncmp(name, "sdk:/", 5)) {
        for (size_t i = 0; i < sizeof(sdk_modules) / sizeof(sdk_modules[0]); i++) {
            if (!strcmp(name, sdk_modules[i].name)) {
                out->size = sdk_modules[i].size;
                memcpy(out->sha256, sdk_modules[i].sha256, 65);
                out->chunks = sdk_modules[i].chunks;
                out->chunk_count = sdk_modules[i].count;
                return GT_OK;
            }
        }
        return GT_ERR_NOT_FOUND;
    }
    const char *path = name + 8;
    size_t n = strlen(path);
    if (!((n >= 3 && !strcmp(path + n - 3, ".js")) || (n >= 4 && !strcmp(path + n - 4, ".mjs")))) {
        return GT_ERR_UNSUPPORTED;
    }
    gt_package_member_t member;
    if (!gt__package_checked_member(package, path, &member, &out->bytes, &out->size)) {
        return GT_ERR_NOT_FOUND;
    }
    memcpy(out->sha256, member.sha256, 65);
    return GT_OK;
#endif
}
