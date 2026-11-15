/* Included after the runtime allocator and JS value helpers. Same serial owner. */
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
        switch (gt_ext_session_access(r->session, &r->lease, names[i])) {
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
    if (r->started || r->views || r->info.phase != GT_RUNTIME_CREATED ||
        atomic_load_explicit(&r->stopped, memory_order_relaxed)) {
        return GT_ERR_CANCELLED;
    }
    const char *names[] = {"table", "diff", "task"};
    uint32_t declared = 7u;
    gt_package_check_info_t info;
    if (r->package) {
        declared = 0;
        if (!r->invocation || !gt__package_checked_info(r->package, &info)) {
            return GT_ERR_AUTH;
        }
        for (size_t i = 0; i < gt__package_checked_command_count(r->package); i++) {
            gt_package_command_t command;
            if (gt__package_checked_command(r->package, i, &command) &&
                !strcmp(command.path, r->invocation)) {
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
    if (gt_ext_session_access(r->session, &r->lease, "session.rpc") != GT_EXT_ACCESS_ALLOWED) {
        return GT_ERR_AUTH;
    }
    for (size_t i = 0; i < 3; i++) {
        if (!(kinds & (1u << i))) {
            continue;
        }
        char capability[24];
        snprintf(capability, sizeof(capability), "ui.%s", names[i]);
        if (gt_ext_session_access(r->session, &r->lease, capability) != GT_EXT_ACCESS_ALLOWED) {
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
    runtime_views_t *views = runtime_malloc(r, sizeof(*views));
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
                                 r->lease.instance,
                                 r->lease.epoch,
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

static bool
runtime_view_string(JSContext *c, JSValueConst value, char *out, size_t cap, bool method) {
    if (!JS_IsString(value)) {
        return false;
    }
    size_t n;
    const char *text = JS_ToCStringLen(c, &n, value);
    if (!text) {
        return false;
    }
    bool valid = n > 0 && n < cap;
    for (size_t i = 0; valid && i < n; i++) {
        char b = text[i];
        bool lower = b >= 'a' && b <= 'z', upper = b >= 'A' && b <= 'Z',
             digit = b >= '0' && b <= '9';
        valid = lower || ((!method || i > 0) && (upper || digit)) ||
                (i > 0 && (b == '.' || b == '_' || b == '-'));
    }
    if (valid) {
        memcpy(out, text, n + 1);
    }
    JS_FreeCString(c, text);
    return valid;
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

static const char *const runtime_view_exports[] = {"viewOpen", "viewState", "viewClose"};

static JSValue
runtime_view_native(JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int op) {
    (void)self;
    gt_bridge_runtime_t *r = JS_GetContextOpaque(c);
    const char *status = "invalid";
    char handle[65] = {0};
    if (!r || !r->views) {
        status = "unsupported";
        goto done;
    }
    if (op == 0) {
        runtime_view_t candidate = {0};
        if (argc != 5 ||
            !runtime_view_string(c, argv[0], candidate.kind, sizeof(candidate.kind), false) ||
            !runtime_view_string(c, argv[1], candidate.id, sizeof(candidate.id), false) ||
            !runtime_view_string(
                c, argv[2], candidate.describe, sizeof(candidate.describe), true) ||
            !runtime_view_string(c, argv[3], candidate.read, sizeof(candidate.read), true)) {
            goto done;
        }
        bool task = !strcmp(candidate.kind, "task");
        if (task
                ? !runtime_view_string(c, argv[4], candidate.cancel, sizeof(candidate.cancel), true)
                : !JS_IsUndefined(argv[4])) {
            goto done;
        }
        status = runtime_view_access(r, candidate.kind);
        if (strcmp(status, "ok")) {
            goto done;
        }
        status = "invalid";
        if (!runtime_view_method(r, candidate.describe) ||
            !runtime_view_method(r, candidate.read) ||
            !strcmp(candidate.describe, candidate.read) ||
            (task && (!runtime_view_method(r, candidate.cancel) ||
                      !strcmp(candidate.describe, candidate.cancel) ||
                      !strcmp(candidate.read, candidate.cancel)))) {
            goto done;
        }
        runtime_view_t *slot = NULL;
        for (size_t i = 0; i < 8; i++) {
            runtime_view_t *entry = &r->views->entries[i];
            if (entry->handle[0] && !strcmp(entry->id, candidate.id)) {
                goto done;
            }
            if (!entry->handle[0] && !slot) {
                slot = entry;
            }
        }
        status = "busy";
        if (!slot || r->views->next_id == UINT64_MAX || r->views->revision == UINT64_MAX) {
            goto done;
        }
        snprintf(candidate.handle,
                 sizeof(candidate.handle),
                 "%s.%" PRIu64,
                 r->views->nonce,
                 r->views->next_id++);
        *slot = candidate;
        r->views->revision++;
        strcpy(handle, candidate.handle);
        status = "ok";
    } else {
        char id[65];
        if (argc != 1 || !runtime_view_string(c, argv[0], id, sizeof(id), false)) {
            goto done;
        }
        runtime_view_t *entry = runtime_view_find(r, id);
        if (!entry) {
            status = "closed";
            goto done;
        }
        status = runtime_view_access(r, entry->kind);
        if (op == 2 && !strcmp(status, "ok")) {
            if (gt__bridge_runtime_view_close(r, id) != GT_OK) {
                status = "invalid";
            }
        }
    }
done:
    if (op != 0) {
        return JS_NewString(c, status);
    }
    JSValue result = JS_NewObjectProto(c, JS_NULL);
    if (JS_IsException(result)) {
        return result;
    }
    if (runtime_set(c, result, "status", JS_NewString(c, status)) < 0 ||
        (handle[0] && runtime_set(c, result, "handle", JS_NewString(c, handle)) < 0)) {
        JS_FreeValue(c, result);
        return JS_EXCEPTION;
    }
    return result;
}
