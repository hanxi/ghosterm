/* Ordered developer scenarios on the two real Runtime owners. */
static struct {
    size_t count;
    const char *action[16], *target[16], *input[16];
} kit_scenario;

static int kit_scenario_parse(int argc, char **argv, int offset) {
    kit_check(offset < argc, "scenario step count");
    char *end = NULL;
    unsigned long count = strtoul(argv[offset++], &end, 10);
    kit_check(end && !*end && count > 0 && count <= 16 && argc >= offset + (int)count * 3,
              "bounded scenario steps");
    kit_scenario.count = (size_t)count;
    for (size_t i = 0; i < kit_scenario.count; i++) {
        kit_scenario.action[i] = argv[offset++];
        kit_scenario.target[i] = argv[offset++];
        kit_scenario.input[i] = argv[offset++];
    }
    return offset;
}

static void kit_scenario_snapshot(gt_ext_runtime_t *client, const char **json, size_t *size) {
    for (size_t i = 0; i < kit_view_closed_count; i++) {
        kit_check(gt_ext_runtime_view_close(client, kit_view_closed[i]) == GT_ERR_NOT_FOUND,
                  "late close cannot remove a reopened view");
    }
    kit_check(gt_ext_runtime_views_snapshot(client, json, size) == GT_OK && *json && *size <= 8192,
              "actual scenario view snapshot");
}

static void kit_scenario_close(gt_ext_runtime_t *client, const char *id) {
    const char *json = NULL;
    size_t size = 0;
    kit_scenario_snapshot(client, &json, &size);
    char snapshot[8193], identity[96], handle[65] = {0};
    memcpy(snapshot, json, size);
    snapshot[size] = 0;
    int n = snprintf(identity, sizeof(identity), "\"viewId\":\"%s\"", id);
    kit_check(n > 0 && (size_t)n < sizeof(identity), "bounded scenario view identity");
    const char *cursor = snapshot;
    for (size_t i = 0; i < 8 && (cursor = strstr(cursor, "\"handle\":\"")); i++) {
        cursor += strlen("\"handle\":\"");
        const char *end = strchr(cursor, '"');
        kit_check(end && end > cursor && (size_t)(end - cursor) < sizeof(handle),
                  "scenario Native handle format");
        const char *match = strstr(end, identity), *next = strstr(end, "\"handle\":\"");
        if (match && (!next || match < next)) {
            memcpy(handle, cursor, (size_t)(end - cursor));
            break;
        }
        cursor = end;
    }
    kit_check(handle[0] && kit_view_closed_count < 17, "scenario view exists and close is bounded");
    kit_check(gt_ext_runtime_view_close(client, handle) == GT_OK, "close selected scenario view");
    strcpy(kit_view_closed[kit_view_closed_count++], handle);
}

static void kit_scenario_run(gt_ext_runtime_t *client,
                             gt_ext_runtime_t *executor,
                             gt_ext_session_t *host,
                             gt_ext_session_t *peer,
                             const gt_ext_rpc_method_t *methods,
                             size_t method_count) {
    for (size_t i = 0; i < kit_scenario.count; i++) {
        const char *action = kit_scenario.action[i], *target = kit_scenario.target[i];
        char label[24];
        kit_check(snprintf(label, sizeof(label), "step-%zu", i) > 0, "scenario trace label");
        if (!strcmp(action, "request")) {
            gt_ext_session_t *caller = NULL;
            for (size_t j = 0; j < method_count; j++) {
                if (!strcmp(methods[j].method, target)) {
                    caller = methods[j].receiver == GT_EXT_EXECUTOR ? host : peer;
                }
            }
            gt_ext_lease_t lease;
            kit_check(caller && gt_ext_session_lease(caller, &lease),
                      "scenario caller direction and lease");
            pair_request(caller, &lease, client, executor, target, kit_scenario.input[i], label);
        } else {
            bool close = !strcmp(action, "closeView");
            kit_check(close || !strcmp(action, "snapshot"), "known scenario action");
            if (close) {
                kit_scenario_close(client, target);
            }
            const char *json = NULL;
            size_t size = 0;
            kit_scenario_snapshot(client, &json, &size);
            if (close) {
                printf("{\"case\":\"%s\",\"action\":\"closeView\",\"closed\":true,"
                       "\"viewId\":\"%s\",\"afterClose\":%.*s,\"staleRejected\":true}\n",
                       label,
                       target,
                       (int)size,
                       json);
            } else {
                printf("{\"case\":\"%s\",\"action\":\"snapshot\",\"snapshot\":%.*s,"
                       "\"staleRejected\":true}\n",
                       label,
                       (int)size,
                       json);
            }
        }
    }
}
