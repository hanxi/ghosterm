/* Native view registry on the Lua runtime's serial owner. */
static uint32_t runtime_view_bit(const char *kind) {
    return !strcmp(kind, "table")  ? 1u
           : !strcmp(kind, "diff") ? 2u
           : !strcmp(kind, "task") ? 4u
                                   : 0u;
}

static const char *runtime_view_access(gt_bridge_runtime_t *r, const char *kind) {
    if (!r || !r->views) {
        return "unsupported";
    }
    if (!r->session || atomic_load_explicit(&r->stopped, memory_order_relaxed) ||
        r->info.phase >= GT_RUNTIME_FAILED) {
        return "revoked";
    }
    uint32_t bit = runtime_view_bit(kind);
    if (!bit || !(r->views->kinds & bit)) {
        return "unsupported";
    }
    char capability[24];
    snprintf(capability, sizeof(capability), "ui.%s", kind);
    const char *names[] = {"session.rpc", capability};
    for (size_t i = 0; i < 2; i++) {
        switch (gt_ext_session_access(r->session, &r->rpc.lease, names[i])) {
            case GT_EXT_ACCESS_UNKNOWN:
                return "unknown";
            case GT_EXT_ACCESS_DENIED:
                return "denied";
            case GT_EXT_ACCESS_REVOKED:
                return "revoked";
            case GT_EXT_ACCESS_UNSUPPORTED:
                return "unsupported";
            case GT_EXT_ACCESS_ALLOWED:
                break;
        }
    }
    return "ok";
}

gt_status_t gt__bridge_runtime_views_enable(gt_bridge_runtime_t *r, uint32_t kinds) {
    if (!r || !kinds || (kinds & ~7u)) {
        return GT_ERR_INVALID_ARG;
    }
    if (r->role != GT_EXT_HOST) {
        return GT_ERR_UNSUPPORTED;
    }
    if (r->views || r->info.phase != GT_RUNTIME_CREATED ||
        atomic_load_explicit(&r->stopped, memory_order_relaxed)) {
        return GT_ERR_CANCELLED;
    }
    const char *names[] = {"table", "diff", "task"};
    uint32_t declared = 7u;
    gt_package_check_info_t info;
    if (r->package) {
        declared = 0;
        if (!(r->command[0] ? r->command : NULL) || !gt__package_checked_info(r->package, &info)) {
            return GT_ERR_AUTH;
        }
        for (size_t i = 0; i < gt__package_checked_command_count(r->package); i++) {
            gt_package_command_t command;
            if (gt__package_checked_command(r->package, i, &command) &&
                !strcmp(command.path, (r->command[0] ? r->command : NULL))) {
                for (size_t j = 0; j < command.view_count; j++) {
                    declared |= runtime_view_bit(command.views[j]);
                }
                break;
            }
        }
    }
    if ((kinds & declared) != kinds) {
        return GT_ERR_AUTH;
    }
    if (gt_ext_session_access(r->session, &r->rpc.lease, "session.rpc") != GT_EXT_ACCESS_ALLOWED) {
        return GT_ERR_AUTH;
    }
    for (size_t i = 0; i < 3; i++) {
        if (!(kinds & (1u << i))) {
            continue;
        }
        char capability[24];
        snprintf(capability, sizeof(capability), "ui.%s", names[i]);
        if (gt_ext_session_access(r->session, &r->rpc.lease, capability) != GT_EXT_ACCESS_ALLOWED) {
            return GT_ERR_AUTH;
        }
        if (r->package) {
            bool required = false;
            for (size_t j = 0; j < info.requires_count; j++) {
                required |= !strcmp(info.requires[j], capability);
            }
            if (!required) {
                return GT_ERR_AUTH;
            }
        }
    }
    uint8_t nonce[16];
    gt_status_t status = gt__secure_random(nonce, sizeof(nonce));
    if (status != GT_OK) {
        return status;
    }
    runtime_views_t *views = gt__plugin_memory_alloc(NULL, sizeof(*views));
    if (!views) {
        gt__secure_erase(nonce, sizeof(nonce));
        return GT_ERR_IO;
    }
    memset(views, 0, sizeof(*views));
    views->kinds = kinds;
    views->next_id = views->revision = 1;
    for (size_t i = 0; i < sizeof(nonce); i++) {
        snprintf(views->nonce + i * 2, 3, "%02x", nonce[i]);
    }
    gt__secure_erase(nonce, sizeof(nonce));
    r->views = views;
    return GT_OK;
}

static runtime_view_t *runtime_view_find(gt_bridge_runtime_t *r, const char *handle) {
    if (!r || !r->views || !handle || strnlen(handle, 65) > 64) {
        return NULL;
    }
    for (size_t i = 0; i < 8; i++) {
        if (r->views->entries[i].handle[0] && !strcmp(r->views->entries[i].handle, handle)) {
            return &r->views->entries[i];
        }
    }
    return NULL;
}

gt_status_t gt__bridge_runtime_view_close(gt_bridge_runtime_t *r, const char *handle) {
    if (!r || !handle) {
        return GT_ERR_INVALID_ARG;
    }
    if (!r->views) {
        return GT_ERR_UNSUPPORTED;
    }
    runtime_view_t *view = runtime_view_find(r, handle);
    if (!view) {
        return GT_ERR_NOT_FOUND;
    }
    if (strcmp(runtime_view_access(r, view->kind), "ok")) {
        return GT_ERR_AUTH;
    }
    if (r->views->revision == UINT64_MAX) {
        return GT_ERR_IO;
    }
    gt__secure_erase(view, sizeof(*view));
    r->views->revision++;
    return GT_OK;
}

gt_status_t
gt__bridge_runtime_views_snapshot(gt_bridge_runtime_t *r, const char **json, size_t *size) {
    if (json) {
        *json = NULL;
    }
    if (size) {
        *size = 0;
    }
    if (!r || !json || !size) {
        return GT_ERR_INVALID_ARG;
    }
    if (!r->views) {
        return GT_ERR_UNSUPPORTED;
    }
    for (size_t i = 0; i < 3; i++) {
        const char *kind = i == 0 ? "table" : i == 1 ? "diff" : "task";
        if ((r->views->kinds & (1u << i)) && strcmp(runtime_view_access(r, kind), "ok")) {
            return GT_ERR_AUTH;
        }
    }
    runtime_views_t *v = r->views;
    size_t at = (size_t)snprintf(v->json,
                                 sizeof(v->json),
                                 "{\"instanceId\":\"%s\",\"epoch\":\"%" PRIu64
                                 "\",\"revision\":\"%" PRIu64 "\",\"views\":[",
                                 r->rpc.lease.instance,
                                 r->rpc.lease.epoch,
                                 v->revision);
    bool comma = false;
    for (size_t i = 0; i < 8; i++) {
        runtime_view_t *entry = &v->entries[i];
        if (!entry->handle[0]) {
            continue;
        }
        int n = snprintf(v->json + at,
                         sizeof(v->json) - at,
                         "%s{\"handle\":\"%s\",\"viewId\":\"%s\",\"kind\":\"%s\",\"methods\":{"
                         "\"describe\":\"%s\",\"read\":\"%s\"%s%s%s}}",
                         comma ? "," : "",
                         entry->handle,
                         entry->id,
                         entry->kind,
                         entry->describe,
                         entry->read,
                         entry->cancel[0] ? ",\"cancel\":\"" : "",
                         entry->cancel,
                         entry->cancel[0] ? "\"" : "");
        if (n < 0 || (size_t)n >= sizeof(v->json) - at) {
            return GT_ERR_INTERNAL;
        }
        at += (size_t)n;
        comma = true;
    }
    if (at + 3 > sizeof(v->json)) {
        return GT_ERR_INTERNAL;
    }
    memcpy(v->json + at, "]}", 3);
    *json = v->json;
    *size = at + 2;
    return GT_OK;
}

static bool runtime_view_method(gt_bridge_runtime_t *r, const char *name) {
    size_t count = 0;
    gt_bridge_role_t role;
    const gt_bridge_rpc_method_t *methods =
        gt__bridge_session_methods(r->session->inner, &count, &role);
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(methods[i].method, name) && methods[i].receiver == GT_BRIDGE_EXECUTOR &&
            !strcmp(methods[i].capability, "session.rpc")) {
            return true;
        }
    }
    return false;
}
