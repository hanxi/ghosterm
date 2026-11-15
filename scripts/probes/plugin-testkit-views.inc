/* Shared Client view-registration test service. No renderer or UI thread. */
static uint32_t kit_view_kinds;
static bool kit_view_registry;
static char kit_view_closed[17][65];
static size_t kit_view_closed_count;

static int kit_views_parse(int argc, char **argv, int offset) {
    kit_check(offset < argc, "explicit view service kinds");
    char *end = NULL;
    unsigned long bits = strtoul(argv[offset], &end, 10);
    kit_check(end && !*end && (bits & 7u) && bits <= 15, "bounded view service kinds");
    kit_view_kinds = (uint32_t)bits & 7u;
    kit_view_registry = (bits & 8u) != 0;
    return offset + 1;
}

static void kit_view_method(gt_ext_rpc_method_t *method, const char *entry) {
    const char *split = strchr(entry, '=');
    kit_check(split && split != entry && (size_t)(split - entry) < sizeof(method->method),
              "view RPC method entry");
    memcpy(method->method, entry, (size_t)(split - entry));
    kit_check(!strcmp(split + 1, "client") || !strcmp(split + 1, "executor"),
              "view RPC method direction");
    method->receiver = !strcmp(split + 1, "client") ? GT_EXT_HOST : GT_EXT_EXECUTOR;
    method->input_shape = method->output_shape = GT_EXT_JSON_OBJECT;
    strcpy(method->capability, "session.rpc");
}

static void kit_views_capabilities(const gt_ext_package_metadata_t *client,
                                   gt_ext_session_config_t *config) {
    const char *names[] = {"ui.table", "ui.diff", "ui.task", "ui.views"};
    for (size_t i = 0; i < 4; i++) {
        if (i == 3 ? !kit_view_registry : !(kit_view_kinds & (1u << i))) {
            continue;
        }
        bool declared = false;
        for (size_t j = 0; j < client->requires_count; j++) {
            declared |= !strcmp(client->requires[j], names[i]);
        }
        kit_check(declared, "explicit Client UI capability declaration");
        kit_check(config->required_count < GT_EXT_CAPS_MAX &&
                      config->available_count < GT_EXT_CAPS_MAX,
                  "view capability capacity");
        strcpy(config->required[config->required_count++], names[i]);
        strcpy(config->available[config->available_count++], names[i]);
    }
}

static void kit_views_enable(gt_ext_runtime_t *runtime) {
    kit_check(gt_ext_runtime_views_enable(runtime, kit_view_kinds) == GT_OK,
              "explicit Native Client view registration service");
    kit_check(gt_ext_runtime_views_enable(runtime, kit_view_kinds) == GT_ERR_CANCELLED,
              "view service cannot be enabled twice");
}

#ifdef GT_TESTKIT_HELPERS_ONLY
static void kit_views_executor(gt_ext_runtime_t *runtime) {
    kit_check(gt_ext_runtime_views_enable(runtime, kit_view_kinds) == GT_ERR_UNSUPPORTED,
              "Executor cannot acquire Native UI service");
    const char *json = "sentinel";
    size_t size = 1;
    kit_check(gt_ext_runtime_views_snapshot(runtime, &json, &size) == GT_ERR_UNSUPPORTED && !json &&
                  !size,
              "Executor cannot expose UI snapshot");
}

#endif

static void kit_views_observe(gt_ext_runtime_t *runtime) {
    const char *json = NULL;
    size_t size = 0;
    kit_check(gt_ext_runtime_views_snapshot(runtime, &json, &size) == GT_OK && json && size <= 8192,
              "actual Client view snapshot");
    /* Native controls this bounded format. Copy before any mutating API. */
    char snapshot[8193];
    memcpy(snapshot, json, size);
    snapshot[size] = 0;
    const char *first = strstr(snapshot, "\"handle\":\"");
    kit_check(first != NULL, "Client registered at least one expected view");
    first += strlen("\"handle\":\"");
    const char *end = strchr(first, '"');
    char handle[65] = {0};
    kit_check(end && end > first && (size_t)(end - first) < sizeof(handle),
              "bounded Native view handle");
    memcpy(handle, first, (size_t)(end - first));
    kit_check(gt_ext_runtime_view_close(runtime, "00000000000000000000000000000000.1") ==
                  GT_ERR_NOT_FOUND,
              "foreign view handle rejected");
    kit_check(gt_ext_runtime_view_close(runtime, handle) == GT_OK,
              "Native independently closes one view");
    kit_check(kit_view_closed_count < 17, "bounded closed view history");
    strcpy(kit_view_closed[kit_view_closed_count++], handle);
    kit_check(gt_ext_runtime_view_close(runtime, handle) == GT_ERR_NOT_FOUND,
              "closed view handle cannot be reused");
    kit_check(gt_ext_runtime_views_snapshot(runtime, &json, &size) == GT_OK && json && size <= 8192,
              "remaining Native view snapshot");
    printf("{\"case\":\"views\",\"snapshot\":%s,\"afterClose\":%.*s,"
           "\"staleRejected\":true}\n",
           snapshot,
           (int)size,
           json);
}

static void kit_views_revoked(gt_ext_runtime_t *runtime) {
    const char *json = "sentinel";
    size_t size = 1;
    gt_status_t status = gt_ext_runtime_views_snapshot(runtime, &json, &size);
    kit_check(status == GT_ERR_AUTH && !json && !size,
              "stopped views expose no stale registrations");
}
