/* Private child resource envelope. Included only by process-rpc.c; all native
 * ownership, deadlines, grants and receipts remain in runtime-resource.c. */
static const char *const process_resource_statuses[] = {"ok",
                                                        "wait",
                                                        "invalid",
                                                        "denied",
                                                        "unsupported",
                                                        "io",
                                                        "limit",
                                                        "changed",
                                                        "cancelled",
                                                        "busy",
                                                        "unknown",
                                                        "closed",
                                                        "not-found",
                                                        "expired",
                                                        "protocol"};

static bool process_resource_string(gt_process_rpc_t *p, const char *text) {
    if (!process_append(p, "\"")) {
        return false;
    }
    for (const unsigned char *s = (const unsigned char *)text; *s; s++) {
        if (*s == '"' || *s == '\\') {
            if (!process_append(p, "\\%c", *s)) {
                return false;
            }
        } else if (*s < 32) {
            if (!process_append(p, "\\u%04x", *s)) {
                return false;
            }
        } else if (!process_append(p, "%c", *s)) {
            return false;
        }
    }
    return process_append(p, "\"");
}

static bool process_resource_status(gt_process_rpc_t *p, gt_runtime_resource_status_t status) {
    return (unsigned)status <
               sizeof(process_resource_statuses) / sizeof(*process_resource_statuses) &&
           process_status(p, GT_EXT_RPC_OK) &&
           process_append(p, ",\"value\":{\"status\":\"%s\"", process_resource_statuses[status]);
}

static bool process_resource_decimal(const process_json_t *d, uint16_t id, uint64_t *out) {
    const json_node_t *n = &d->nodes[id];
    if (id && n->kind == J_STRING && n->size == 1 && d->strings[n->text] == '0') {
        *out = 0;
        return true;
    }
    return process_counter(d, id, out);
}

/* Canonical padded base64 allows all 4096 UTF-8 path bytes to fit the 8 KiB
 * envelope even when every component contains JSON quotes/backslashes. */
static bool process_resource_path(const process_json_t *d, uint16_t id, char out[4097]) {
    if (!id || d->nodes[id].kind != J_STRING) {
        return false;
    }
    const char *s = (const char *)d->strings + d->nodes[id].text;
    size_t n = d->nodes[id].size, used = 0;
    if (!n || n % 4 || n > 5464) {
        return false;
    }
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 4) {
        unsigned v[4] = {0};
        size_t padding = 0;
        for (size_t j = 0; j < 4; j++) {
            if (s[i + j] == '=') {
                if (i + 4 != n || j < 2) {
                    return false;
                }
                padding++;
            } else {
                const char *at = s[i + j] ? strchr(alphabet, s[i + j]) : NULL;
                if (!at || padding) {
                    return false;
                }
                v[j] = (unsigned)(at - alphabet);
            }
        }
        if (padding > 2 || (padding == 2 && (v[1] & 15)) || (padding == 1 && (v[2] & 3))) {
            return false;
        }
        uint32_t value = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        for (size_t j = 0; j < 3 - padding; j++) {
            unsigned char byte = (unsigned char)(value >> (16 - j * 8));
            if (!byte || used == 4096) {
                return false;
            }
            out[used++] = (char)byte;
        }
    }
    out[used] = 0;
    return used != 0;
}

static bool process_resource_descriptor(gt_process_rpc_t *p,
                                        const gt_ext_resource_descriptor_t *d) {
    return process_append(p,
                          "{\"id\":\"%s\",\"generation\":\"%" PRIu64
                          "\",\"sha256\":\"%s\",\"displayName\":",
                          d->resource_id,
                          d->generation,
                          d->sha256) &&
           process_resource_string(p, d->display_name) && process_append(p, ",\"mediaType\":") &&
           process_resource_string(p, d->mime) &&
           process_append(p,
                          ",\"sizeKnown\":%s,\"bytes\":\"%" PRIu64 "\"}",
                          d->size_known ? "true" : "false",
                          d->size);
}

static bool process_resource_dispatch(gt_process_rpc_t *p) {
    static const char *const basic[] = {"v", "id", "op", "action"};
    static const char *const reference[] = {"v", "id", "op", "action", "ref"};
    static const char *const publish[] = {
        "v", "id", "op", "action", "root", "path64", "mime", "limit", "timeoutMs"};
    static const char *const open[] = {"v", "id", "op", "action", "ref", "limit", "timeoutMs"};
    static const char *const read[] = {"v", "id", "op", "action", "ref", "at", "maxBytes"};
    process_json_t *d = p->json;
    char action[24], root[65], path[4097], mime[129];
    uint64_t ref = 0, limit = 0, timeout = 0, at = 0, maximum = 0;
    if (!process_text(d, process_member(d, "action"), action, sizeof(action))) {
        return false;
    }
    bool roots = !strcmp(action, "workspaces"), taking = !strcmp(action, "take"),
         publishing = !strcmp(action, "publish"), opening = !strcmp(action, "open"),
         reading = !strcmp(action, "read"), observing = !strcmp(action, "publication"),
         cancel = !strcmp(action, "cancel"), forget = !strcmp(action, "forget"),
         release = !strcmp(action, "release");
    if (roots || taking) {
        if (!process_keys(d, basic, 4)) {
            return false;
        }
    } else if (publishing) {
        if (!process_keys(d, publish, 9) ||
            !process_text(d, process_member(d, "root"), root, sizeof(root)) ||
            !process_resource_path(d, process_member(d, "path64"), path) ||
            !process_text(d, process_member(d, "mime"), mime, sizeof(mime)) ||
            !process_resource_decimal(d, process_member(d, "limit"), &limit) ||
            !node_uint(d, process_member(d, "timeoutMs"), &timeout) || !timeout ||
            timeout > 3600000) {
            return false;
        }
    } else if (opening || reading || observing || cancel || forget || release) {
        if (!process_counter(d, process_member(d, "ref"), &ref)) {
            return false;
        }
        if (opening) {
            if (!process_keys(d, open, 7) ||
                !process_resource_decimal(d, process_member(d, "limit"), &limit) ||
                !node_uint(d, process_member(d, "timeoutMs"), &timeout) || !timeout ||
                timeout > 3600000) {
                return false;
            }
        } else if (reading) {
            if (!process_keys(d, read, 7) ||
                !process_resource_decimal(d, process_member(d, "at"), &at) ||
                !node_uint(d, process_member(d, "maxBytes"), &maximum) || !maximum ||
                maximum > 16384) {
                return false;
            }
        } else if (!process_keys(d, reference, 5)) {
            return false;
        }
    } else {
        return false;
    }
    if (!p->resources) {
        return process_resource_status(p, GT_RUNTIME_RESOURCE_UNSUPPORTED) &&
               (!reading || process_append(p, ",\"hex\":\"\"")) && process_append(p, "}");
    }
    if (roots) {
        if (!process_resource_status(p, GT_RUNTIME_RESOURCE_OK) ||
            !process_append(p, ",\"roots\":[")) {
            return false;
        }
        for (size_t i = 0; i < gt__runtime_resources_roots(p->resources); i++) {
            if ((i && !process_append(p, ",")) ||
                !process_resource_string(p, gt__runtime_resources_root(p->resources, i))) {
                return false;
            }
        }
        return process_append(p, "]}");
    }
    process_reference_t *slot =
        process_find(release || opening || reading ? p->snapshots : p->publications, ref);
    gt_runtime_resource_status_t status = GT_RUNTIME_RESOURCE_INVALID;
    gt_ext_resource_descriptor_t descriptor = {0};
    if (publishing || taking) {
        slot = process_empty(taking ? p->snapshots : p->publications);
        if (!slot || p->reference == UINT64_MAX) {
            status = GT_RUNTIME_RESOURCE_LIMIT;
        } else {
            uint64_t native = 0;
            status = taking
                         ? gt__runtime_resources_take(p->resources, &native, &descriptor)
                         : gt__runtime_resources_publish(
                               p->resources, root, path, mime, limit, (uint32_t)timeout, &native);
            if (status == GT_RUNTIME_RESOURCE_OK) {
                *slot = (process_reference_t){++p->reference, native};
            }
        }
        if (!process_resource_status(p, status)) {
            return false;
        }
        if (status == GT_RUNTIME_RESOURCE_OK &&
            (!process_append(p, ",\"handle\":\"%" PRIu64 "\"", slot->local) ||
             (taking && (!process_append(p, ",\"descriptor\":") ||
                         !process_resource_descriptor(p, &descriptor))))) {
            return false;
        }
    } else if (observing && slot) {
        gt_runtime_publication_t publication;
        status = gt__runtime_resources_publication(p->resources, slot->native, &publication);
        if (!process_resource_status(p, status)) {
            return false;
        }
        if (status == GT_RUNTIME_RESOURCE_OK) {
            static const char *const phases[] = {"hashing",
                                                 "ready",
                                                 "sending",
                                                 "receipt",
                                                 "cancelling",
                                                 "succeeded",
                                                 "failed",
                                                 "stopped",
                                                 "uncertain"};
            static const char *const states[] = {
                "queued", "sending", "pending", "succeeded", "failed", "cancelled", "unknown"};
            const gt_ext_source_info_t *s = &publication.source;
            if ((unsigned)s->phase >= 9 || (unsigned)s->error >= 13 ||
                (unsigned)s->transfer_state >= 7) {
                return false;
            }
            descriptor = s->descriptor;
            memcpy(descriptor.resource_id, s->resource_id, 33);
            descriptor.generation = s->generation;
            if (!process_append(p,
                                ",\"publication\":{\"phase\":\"%s\",\"error\":\"%s\",\"expired\":%"
                                "s,\"clockError\":%s,\"hasTransfer\":%s,\"confirmed\":%s,"
                                "\"transferState\":\"%s\",\"readBytes\":\"%" PRIu64
                                "\",\"accepted\":\"%" PRIu64 "\",\"descriptor\":",
                                phases[s->phase],
                                process_resource_statuses[s->error],
                                publication.expired ? "true" : "false",
                                publication.clock_error ? "true" : "false",
                                s->has_transfer ? "true" : "false",
                                s->confirmed ? "true" : "false",
                                states[s->transfer_state],
                                s->read_bytes,
                                s->accepted) ||
                !process_resource_descriptor(p, &descriptor) || !process_append(p, "}")) {
                return false;
            }
        }
    } else if (reading && slot) {
        uint8_t bytes[3072];
        size_t count = 0, cap = maximum > sizeof(bytes) ? sizeof(bytes) : (size_t)maximum;
        status = gt__runtime_resources_read(p->resources, slot->native, at, bytes, cap, &count);
        if (!process_resource_status(p, status) || !process_append(p, ",\"hex\":\"")) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (!process_append(p, "%02x", bytes[i])) {
                return false;
            }
        }
        gt__secure_erase(bytes, sizeof(bytes));
        if (!process_append(p, "\"")) {
            return false;
        }
    } else {
        if (slot) {
            if (opening) {
                status = gt__runtime_resources_begin_read(
                    p->resources, slot->native, limit, (uint32_t)timeout);
            } else if (cancel) {
                status = gt__runtime_resources_cancel(p->resources, slot->native);
            } else if (forget) {
                status = gt__runtime_resources_forget(p->resources, slot->native);
            } else if (release) {
                status = gt__runtime_resources_release(p->resources, slot->native);
            }
            if (status == GT_RUNTIME_RESOURCE_OK && (forget || release)) {
                gt__secure_erase(slot, sizeof(*slot));
            }
        }
        if (!process_resource_status(p, status)) {
            return false;
        }
        if (reading && !process_append(p, ",\"hex\":\"\"")) {
            return false;
        }
    }
    return process_append(p, "}");
}
