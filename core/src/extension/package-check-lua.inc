#include "extension/package-runtime.h"
#include "extension/wire.h"

static void *checked_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void checked_release(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static bool snapshot_file(const gt_package_snapshot_t *snapshot,
                          const char *path,
                          gt_package_member_t *member,
                          const uint8_t **bytes,
                          size_t *size) {
    size_t index;
    return gt__package_snapshot_find(snapshot, path, &index) == GT_PACKAGE_OK &&
           gt__package_snapshot_member(snapshot, index, member) &&
           gt__package_snapshot_view(snapshot, index, bytes, size);
}

static bool copy_string(const gt_bridge_wire_t *doc, uint16_t node, char *out, size_t capacity) {
    const uint8_t *text;
    size_t size;
    if (!gt__bridge_wire_string(doc, node, &text, &size) || !size || size >= capacity ||
        memchr(text, 0, size)) {
        return false;
    }
    memcpy(out, text, size);
    out[size] = 0;
    return true;
}

static bool equals(const gt_bridge_wire_t *doc, uint16_t node, const char *expected) {
    const uint8_t *text;
    size_t size;
    return gt__bridge_wire_string(doc, node, &text, &size) && size == strlen(expected) &&
           !memcmp(text, expected, size);
}

static bool contains(const gt_bridge_wire_t *doc, uint16_t array, const char *text) {
    const size_t count = gt__bridge_wire_count(doc, array);
    for (size_t i = 0; i < count; ++i) {
        if (equals(doc, gt__bridge_wire_at(doc, array, i), text)) {
            return true;
        }
    }
    return false;
}

static bool array_node(const gt_bridge_wire_t *doc, uint16_t id) {
    gt_bridge_json_node_t node;
    return gt__bridge_json_node(doc, id, &node) && node.kind == GT_BRIDGE_JSON_ARRAY;
}

static bool
integer_field(const gt_bridge_wire_t *doc, uint16_t node, const char *field, uint64_t expected) {
    uint64_t value;
    return gt__bridge_wire_uint(doc, gt__bridge_wire_member(doc, node, field), &value) &&
           value == expected;
}

static bool retired_member(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (!strcmp(path, "plugin.json") || !strcasecmp(name, "package.json") ||
        !strcasecmp(name, "package-lock.json")) {
        return true;
    }
    const char *suffix = strrchr(name, '.');
    if (!suffix) {
        return false;
    }
    const char *const retired[] = {
        ".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".mts", ".cts", ".luac"};
    for (size_t i = 0; i < sizeof(retired) / sizeof(*retired); ++i) {
        if (!strcasecmp(suffix, retired[i])) {
            return true;
        }
    }
    return false;
}

static bool integrity_matches(gt_bridge_wire_t *doc,
                              const gt_package_snapshot_t *snapshot,
                              char digest[65],
                              const atomic_bool *stop) {
    gt_package_member_t integrity;
    const uint8_t *bytes;
    size_t size, count;
    uint64_t total;
    if (!snapshot_file(snapshot, "integrity.json", &integrity, &bytes, &size) ||
        !gt__package_snapshot_count(snapshot, &count, &total) ||
        gt__bridge_json_parse(doc, bytes, size) != GT_OK ||
        !integer_field(doc, 1, "schemaVersion", 1)) {
        return false;
    }
    const char *const fields[] = {"schemaVersion", "members"};
    const uint16_t array = gt__bridge_wire_member(doc, 1, "members");
    if (!gt__bridge_wire_object_fields(doc, 1, fields, 2) || !array_node(doc, array) ||
        gt__bridge_wire_count(doc, array) + 1 != count) {
        return false;
    }
    bool seen[GT_PACKAGE_MEMBERS_MAX] = {0};
    for (size_t i = 0; i + 1 < count; ++i) {
        if (stop && atomic_load(stop)) {
            return false;
        }
        const uint16_t record = gt__bridge_wire_at(doc, array, i);
        const char *const member_fields[] = {"path", "bytes", "sha256"};
        char path[GT_PACKAGE_PATH_BYTES_MAX + 1];
        size_t index;
        gt_package_member_t member;
        if (!gt__bridge_wire_object_fields(doc, record, member_fields, 3) ||
            !copy_string(doc, gt__bridge_wire_member(doc, record, "path"), path, sizeof(path)) ||
            !strcmp(path, "integrity.json") || retired_member(path) ||
            gt__package_snapshot_find(snapshot, path, &index) != GT_PACKAGE_OK || seen[index] ||
            !gt__package_snapshot_member(snapshot, index, &member) ||
            !integer_field(doc, record, "bytes", member.bytes) ||
            !equals(doc, gt__bridge_wire_member(doc, record, "sha256"), member.sha256)) {
            return false;
        }
        seen[index] = true;
    }
    memcpy(digest, integrity.sha256, 65);
    return true;
}

static bool commands(gt_bridge_wire_t *doc,
                     const gt_package_snapshot_t *snapshot,
                     uint16_t array,
                     gt_package_command_t *out,
                     size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint16_t command = gt__bridge_wire_at(doc, array, i);
        const uint16_t path = gt__bridge_wire_member(doc, command, "path");
        const size_t segments = gt__bridge_wire_count(doc, path);
        if (!segments || segments > 8 ||
            !copy_string(doc,
                         gt__bridge_wire_member(doc, command, "title"),
                         out[i].title,
                         sizeof(out[i].title))) {
            return false;
        }
        size_t at = 0;
        for (size_t j = 0; j < segments; ++j) {
            char segment[65];
            if (!copy_string(doc, gt__bridge_wire_at(doc, path, j), segment, sizeof(segment)) ||
                segment[0] < 'a' || segment[0] > 'z' ||
                strspn(segment, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(segment)) {
                return false;
            }
            if (j) {
                out[i].path[at++] = ' ';
            }
            const size_t length = strlen(segment);
            memcpy(out[i].path + at, segment, length);
            at += length;
        }
        for (size_t j = 0; j < i; ++j) {
            if (!strcmp(out[j].path, out[i].path)) {
                return false;
            }
        }
        const uint16_t schema = gt__bridge_wire_member(doc, command, "argsSchema");
        size_t member;
        if (schema &&
            (!copy_string(doc, schema, out[i].args_schema, sizeof(out[i].args_schema)) ||
             gt__package_snapshot_find(snapshot, out[i].args_schema, &member) != GT_PACKAGE_OK)) {
            return false;
        }
        const uint16_t views = gt__bridge_wire_member(doc, command, "views");
        out[i].view_count = gt__bridge_wire_count(doc, views);
        if ((views && !array_node(doc, views)) || out[i].view_count > 8) {
            return false;
        }
        for (size_t j = 0; j < out[i].view_count; ++j) {
            if (!copy_string(doc,
                             gt__bridge_wire_at(doc, views, j),
                             out[i].views[j],
                             sizeof(out[i].views[j]))) {
                return false;
            }
        }
        const uint16_t toggle = gt__bridge_wire_member(doc, command, "togglePanel");
        if (toggle) {
            gt_bridge_json_node_t value;
            if (!gt__bridge_json_node(doc, toggle, &value) ||
                (value.kind != GT_BRIDGE_JSON_TRUE && value.kind != GT_BRIDGE_JSON_FALSE)) {
                return false;
            }
            out[i].toggle_panel = value.kind == GT_BRIDGE_JSON_TRUE;
            if (out[i].toggle_panel && !out[i].view_count) {
                return false;
            }
        }
    }
    return true;
}

gt_package_check_status_t gt__package_check(gt_package_snapshot_t **snapshot,
                                            const gt_package_check_config_t *config,
                                            const gt_allocator_t *allocator,
                                            gt_package_checked_t **out,
                                            gt_package_check_report_t *report) {
    if (out) {
        *out = NULL;
    }
    if (!snapshot || !*snapshot || !config || !out) {
        return check_report(report, GT_PACKAGE_CHECK_INVALID, "ARGUMENT");
    }
    if (!config->host_json || !config->host_size) {
        return check_report(report, GT_PACKAGE_CHECK_UNKNOWN, "HOST_REQUIRED");
    }
    size_t index;
    if (gt__package_snapshot_find(*snapshot, "manifest.json", &index) == GT_PACKAGE_NOT_FOUND &&
        gt__package_snapshot_find(*snapshot, "plugin.json", &index) == GT_PACKAGE_OK) {
        return check_report(report, GT_PACKAGE_CHECK_UNSUPPORTED, "LEGACY_PACKAGE_REINSTALL");
    }
    gt_bridge_wire_t *doc = NULL;
    if (gt__bridge_wire_new(allocator, &doc) != GT_OK) {
        return check_report(report, GT_PACKAGE_CHECK_MEMORY, "MEMORY");
    }
    gt_package_check_status_t status = GT_PACKAGE_CHECK_INVALID;
    const char *code = "INTEGRITY";
    gt_package_checked_t *checked = NULL;
    gt_package_check_info_t info = {0};
    if (!integrity_matches(doc, *snapshot, info.package_digest, config->stop)) {
        goto done;
    }
    code = "HOST";
    if (gt__bridge_json_parse(doc, config->host_json, config->host_size) != GT_OK) {
        goto done;
    }
    const uint16_t side = gt__bridge_wire_member(doc, 1, "side");
    info.executor = equals(doc, side, "executor");
    if ((!info.executor && !equals(doc, side, "client")) ||
        !equals(doc, gt__bridge_wire_member(doc, 1, "sdkVersion"), "1.0.0")) {
        goto done;
    }
    if (!contains(doc, gt__bridge_wire_member(doc, 1, "runtimes"), "lua")) {
        status = GT_PACKAGE_CHECK_UNSUPPORTED;
        goto done;
    }

    gt_bridge_wire_t *host = NULL;
    if (gt__bridge_wire_new(allocator, &host) != GT_OK) {
        status = GT_PACKAGE_CHECK_MEMORY;
        code = "MEMORY";
        goto done;
    }
    (void)gt__bridge_json_parse(host, config->host_json, config->host_size);
    const uint16_t caps = gt__bridge_wire_member(host, 1, "capabilities");
    const uint16_t protocols = gt__bridge_wire_member(host, 1, "protocols");
    uint64_t protocol;
    if (gt__bridge_wire_count(host, protocols) != 1 ||
        !gt__bridge_wire_uint(host, gt__bridge_wire_at(host, protocols, 0), &protocol) ||
        protocol != 1 || !array_node(host, caps)) {
        gt__bridge_wire_free(host);
        goto done;
    }
    gt_package_member_t manifest;
    const uint8_t *bytes;
    size_t size;
    code = "MANIFEST";
    if (!snapshot_file(*snapshot, "manifest.json", &manifest, &bytes, &size) ||
        gt__bridge_json_parse(doc, bytes, size) != GT_OK ||
        !integer_field(doc, 1, "schemaVersion", 2) ||
        !equals(doc, gt__bridge_wire_member(doc, 1, "sdkApiRange"), "^1.0.0") ||
        !copy_string(doc, gt__bridge_wire_member(doc, 1, "id"), info.id, sizeof(info.id)) ||
        !copy_string(doc,
                     gt__bridge_wire_member(doc, 1, "pluginVersion"),
                     info.version,
                     sizeof(info.version)) ||
        !copy_string(doc,
                     gt__bridge_wire_member(doc, 1, "publisher"),
                     info.publisher,
                     sizeof(info.publisher)) ||
        !copy_string(
            doc, gt__bridge_wire_member(doc, 1, "license"), info.license, sizeof(info.license))) {
        gt__bridge_wire_free(host);
        goto done;
    }
    const uint16_t manifest_protocols = gt__bridge_wire_member(doc, 1, "bridgeProtocolRange");
    const uint16_t selected = gt__bridge_wire_member(doc, 1, info.executor ? "executor" : "client");
    const uint16_t requirements = gt__bridge_wire_member(doc, selected, "requires");
    const uint16_t programs = gt__bridge_wire_member(doc, selected, "programs");
    const uint16_t rpc = gt__bridge_wire_member(doc, selected, "rpc");
    const char *const role_fields[] = {"entry", "requires", programs ? "programs" : "rpc", "rpc"};
    const size_t role_field_count = 2 + (programs != 0) + (rpc != 0);
    info.requires_count = gt__bridge_wire_count(doc, requirements);
    if (gt__bridge_wire_count(doc, manifest_protocols) != 1 ||
        !gt__bridge_wire_uint(doc, gt__bridge_wire_at(doc, manifest_protocols, 0), &protocol) ||
        protocol != 1 ||
        !gt__bridge_wire_object_fields(doc, selected, role_fields, role_field_count) ||
        !array_node(doc, requirements) || info.requires_count > 16 ||
        !copy_string(
            doc, gt__bridge_wire_member(doc, selected, "entry"), info.entry, sizeof(info.entry))) {
        gt__bridge_wire_free(host);
        goto done;
    }
    for (size_t i = 0; i < info.requires_count; ++i) {
        if (!copy_string(doc,
                         gt__bridge_wire_at(doc, requirements, i),
                         info.requires[i],
                         sizeof(info.requires[i])) ||
            !contains(host, caps, info.requires[i])) {
            gt__bridge_wire_free(host);
            status = GT_PACKAGE_CHECK_UNSUPPORTED;
            code = "CAPABILITY";
            goto done;
        }
    }
    gt__bridge_wire_free(host);
    if (rpc) {
        info.rpc_handler_count = gt__bridge_wire_count(doc, rpc);
        if (!array_node(doc, rpc) || !info.rpc_handler_count || info.rpc_handler_count > 16 ||
            !contains(doc, requirements, "session.rpc")) {
            goto done;
        }
        for (size_t i = 0; i < info.rpc_handler_count; ++i) {
            char *method = info.rpc_handlers[i];
            if (!copy_string(
                    doc, gt__bridge_wire_at(doc, rpc, i), method, sizeof(info.rpc_handlers[i])) ||
                method[0] < 'a' || method[0] > 'z' || !strchr(method, '.') ||
                strspn(method,
                       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                    strlen(method)) {
                goto done;
            }
            for (size_t j = 0; j < i; ++j) {
                if (!strcmp(method, info.rpc_handlers[j])) {
                    goto done;
                }
            }
        }
    }
    const bool process = contains(doc, requirements, "process.spawn");
    const bool pipes = contains(doc, requirements, "process.pipes");
    if (process != pipes || process != (programs != 0) || (process && !info.executor)) {
        goto done;
    }
    if (process) {
        info.process_program_count = gt__bridge_wire_count(doc, programs);
        if (!info.process_program_count || info.process_program_count > 16) {
            goto done;
        }
        for (size_t i = 0; i < info.process_program_count; ++i) {
            char *program = info.process_programs[i];
            if (!copy_string(doc,
                             gt__bridge_wire_at(doc, programs, i),
                             program,
                             sizeof(info.process_programs[i])) ||
                program[0] < 'a' || program[0] > 'z' ||
                strspn(program, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(program)) {
                goto done;
            }
            for (size_t j = 0; j < i; ++j) {
                if (!strcmp(program, info.process_programs[j])) {
                    goto done;
                }
            }
        }
    }

    gt_package_member_t entry;
    const size_t length = strlen(info.entry);
    if (length < 5 || strcmp(info.entry + length - 4, ".lua") ||
        !snapshot_file(*snapshot, info.entry, &entry, &bytes, &size) || !size ||
        size > 128 * 1024 || bytes[0] == 0x1b || !gt__runtime_source_utf8(bytes, size)) {
        goto done;
    }
    const uint16_t command_array = gt__bridge_wire_member(doc, 1, "commands");
    const size_t count = gt__bridge_wire_count(doc, command_array);
    if (!count || count > 32) {
        goto done;
    }
    const size_t allocation = sizeof(*checked) + count * sizeof(*checked->commands);
    checked = gt__plugin_memory_alloc(allocator, allocation);
    if (!checked) {
        status = GT_PACKAGE_CHECK_MEMORY;
        code = "MEMORY";
        goto done;
    }
    memset(checked, 0, allocation);
    checked->allocation_size = allocation;
    checked->allocator =
        allocator ? *allocator : (gt_allocator_t){NULL, checked_allocate, checked_release};
    if (!commands(doc, *snapshot, command_array, checked->commands, count)) {
        goto done;
    }
    /* Until registry comparison is moved to the native validator, require an
     * explicit command association whenever a registry was supplied. */
    info.association_required = config->registry_size != 0;
    info.protocol_count = 1;
    info.protocols[0] = 1;
    strcpy(info.runtime, "lua");
    strcpy(info.sdk_version, "1.0.0");
    memcpy(info.manifest_digest, manifest.sha256, 65);
    checked->info = info;
    checked->command_count = count;
    atomic_init(&checked->references, 1);
    status = GT_PACKAGE_CHECK_COMPATIBLE;
    code = "OK";

done:
    gt__bridge_wire_free(doc);
    if (config->stop && atomic_load(config->stop)) {
        status = GT_PACKAGE_CHECK_CANCELLED;
        code = "CANCELLED";
    }
    if (status == GT_PACKAGE_CHECK_COMPATIBLE) {
        checked->snapshot = *snapshot;
        *snapshot = NULL;
        *out = checked;
    } else if (checked) {
        gt__plugin_memory_free(allocator, checked, checked->allocation_size);
    }
    return check_report(report, status, code);
}
