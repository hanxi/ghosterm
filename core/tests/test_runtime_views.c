/* Actual public Host Runtime, local view service and embedded Lua. */
#define main views_session_fixture_main
#include "test_ext_session.c"
#undef main
#include "base/sha256.h"
#include "extension/memory.h"

static void view_module(gt_ext_runtime_module_t *m, const char *name, const char *text) {
    m->name = name;
    m->source = text;
    m->size = strlen(text);
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    CHECK(gt__sha256_update(&hash, text, m->size) && gt__sha256_final(&hash, m->sha256));
}

static size_t view_count(gt_ext_runtime_t *runtime, char handle[65]) {
    const char *json = NULL;
    size_t size = 0;
    CHECK(gt_ext_runtime_views_snapshot(runtime, &json, &size) == GT_OK && json && size > 0 &&
          size <= 8192 && strlen(json) == size);
    CHECK(strstr(json, "\"instanceId\":\"") && strstr(json, "\"epoch\":\"") &&
          strstr(json, "\"revision\":\""));
    size_t count = 0;
    for (const char *p = json; (p = strstr(p, "\"handle\":\"")); count++) {
        p += 10;
        const char *end = strchr(p, '"');
        CHECK(end && end > p && end - p < 65);
        if (!count && handle) {
            memcpy(handle, p, (size_t)(end - p));
            handle[end - p] = 0;
        }
    }
    return count;
}

static const char *view_plugin =
    "local ctx = ...; local v = assert(ctx.views);"
    "local function open() return assert(v.open('table','table-a','table.describe','table.page')) "
    "end;"
    "local first = open();"
    "assert(v.open('diff','diff-b','diff.describe','diff.page'));"
    "assert(v.open('task','task-c','task.describe','task.read','task.cancel'));"
    "return {['fixture.control'] = function(input) "
    "if input.action == 'state' then return {text = v.isOpen(first) and 'open' or 'closed'} end;"
    "assert(input.action == 'reopen'); first = open(); return {text = first} end}";

static void view_call(gt_ext_session_t *peer,
                      const gt_ext_lease_t *lease,
                      gt_ext_runtime_t *runtime,
                      const char *action,
                      const char *expected) {
    char input[128];
    int n = snprintf(input, sizeof(input), "{\"action\":\"%s\"}", action);
    CHECK(n > 0 && (size_t)n < sizeof(input));
    uint64_t handle = 0;
    CHECK(gt_ext_session_request(peer, lease, "fixture.control", input, (size_t)n, 1000, &handle) ==
          GT_EXT_RPC_OK);
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(peer, 0);
        gt_ext_runtime_step(runtime);
        gt_ext_rpc_result_t result;
        CHECK(gt_ext_session_request_result(peer, lease, handle, &result) == GT_EXT_RPC_OK);
        if (result.confirmed) {
            CHECK(result.state == GT_EXT_RPC_SUCCEEDED && strstr(result.outcome, expected));
            CHECK(gt_ext_session_forget_request(peer, lease, handle) == GT_EXT_RPC_OK);
            return;
        }
    }
    CHECK(false);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *mode = argv[1];
    size_t fds = ext_fds();
    gt_plugin_memory_info_t before = gt__plugin_memory_info();
    ext_pair_t pair = {0};
    int sockets[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    gt_ext_channel_t *h = ext_channel(sockets[0]), *e = ext_channel(sockets[1]);
    gt_ext_session_config_t config = ext_config();
    config.features = GT_EXT_FEATURE_REQUESTS;
    const char *capabilities[] = {"session.rpc", "ui.table", "ui.diff", "ui.task"};
    config.available_count = 4;
    config.required_count = !strcmp(mode, "ungranted") ? 1 : 4;
    for (size_t i = 0; i < 4; i++) {
        strcpy(config.required[i], capabilities[i]);
        strcpy(config.available[i], capabilities[i]);
    }
    const char *methods[] = {"table.describe",
                             "table.page",
                             "diff.describe",
                             "diff.page",
                             "task.describe",
                             "task.read",
                             "task.cancel",
                             "fixture.control"};
    gt_ext_rpc_method_t rpc[8] = {0};
    for (size_t i = 0; i < 8; i++) {
        strcpy(rpc[i].method, methods[i]);
        strcpy(rpc[i].capability, "session.rpc");
        rpc[i].receiver =
            i == 7 || (!strcmp(mode, "direction") && i == 0) ? GT_EXT_HOST : GT_EXT_EXECUTOR;
        rpc[i].input_shape = rpc[i].output_shape = GT_EXT_JSON_OBJECT;
    }
    CHECK(gt_ext_session_open_rpc(&h, GT_EXT_HOST, &config, rpc, 8, &pair.h) == GT_OK);
    CHECK(gt_ext_session_open_rpc(&e, GT_EXT_EXECUTOR, &config, rpc, 8, &pair.e) == GT_OK);
    ext_allow(&pair);
    gt_ext_lease_t peer_lease;
    CHECK(gt_ext_session_lease(pair.e, &peer_lease));
    char script[1024];
    const char *plugin = view_plugin;
    if (!strcmp(mode, "disabled") || !strcmp(mode, "executor") || !strcmp(mode, "ungranted")) {
        plugin = "local ctx = ...; assert(ctx.views == nil); return {}";
    } else if (!strcmp(mode, "direction") || !strcmp(mode, "kind") || !strcmp(mode, "method")) {
        CHECK(snprintf(script,
                       sizeof(script),
                       "local ctx = ...; assert(ctx.views.open('%s','a','%s','table.page') == nil);"
                       "return {}",
                       !strcmp(mode, "kind") ? "diff" : "table",
                       !strcmp(mode, "method") ? "missing.describe" : "table.describe") > 0);
        plugin = script;
    } else if (!strcmp(mode, "limit")) {
        plugin = "local ctx = ...; local v = ctx.views;"
                 "for i=1,8 do assert(v.open('table','v'..i,'table.describe','table.page')) end;"
                 "assert(v.open('table','extra','table.describe','table.page') == nil); return {}";
    } else if (!strcmp(mode, "duplicate")) {
        plugin =
            "local ctx = ...; local v = ctx.views;"
            "local function open() return v.open('table','a','table.describe','table.page') end;"
            "assert(open()); assert(open() == nil); return {}";
    }
    gt_ext_runtime_module_t module;
    view_module(&module, "plugin:/entry.lua", plugin);
    bool executor = !strcmp(mode, "executor");
    gt_ext_runtime_config_t settings = {.modules = &module,
                                        .module_count = 1,
                                        .entry = module.name,
                                        .role = executor ? GT_EXT_EXECUTOR : GT_EXT_HOST};
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open(executor ? &pair.e : &pair.h, &settings, &runtime) == GT_OK &&
          runtime);
    const char *json = (void *)1;
    size_t size = 123;
    CHECK(gt_ext_runtime_views_snapshot(runtime, &json, &size) == GT_ERR_UNSUPPORTED && !json &&
          !size);
    if (strcmp(mode, "disabled")) {
        gt_status_t expected = executor                     ? GT_ERR_UNSUPPORTED
                               : !strcmp(mode, "ungranted") ? GT_ERR_AUTH
                                                            : GT_OK;
        CHECK(gt_ext_runtime_views_enable(runtime, !strcmp(mode, "kind") ? 1 : 7) == expected);
    }
    gt_ext_session_t *peer = executor ? pair.h : pair.e;
    gt_ext_runtime_info_t info;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(peer, 0);
        gt_ext_runtime_step(runtime);
        CHECK(gt_ext_runtime_info(runtime, &info));
        if (info.phase == GT_EXT_RUNTIME_FAILED) {
            fprintf(stderr, "views activation error=%u\n", info.error);
        }
        CHECK(info.phase != GT_EXT_RUNTIME_FAILED);
        if (info.phase == GT_EXT_RUNTIME_ACTIVE) {
            break;
        }
    }
    CHECK(info.phase == GT_EXT_RUNTIME_ACTIVE);
    if (!executor && strcmp(mode, "disabled") && strcmp(mode, "ungranted")) {
        CHECK(gt_ext_runtime_views_enable(runtime, 1) == GT_ERR_CANCELLED);
        char old[65] = {0};
        size_t count = view_count(runtime, old);
        if (!strcmp(mode, "direction") || !strcmp(mode, "kind") || !strcmp(mode, "method")) {
            CHECK(count == 0);
        } else if (!strcmp(mode, "limit")) {
            CHECK(count == 8);
        } else if (!strcmp(mode, "duplicate")) {
            CHECK(count == 1);
        } else {
            CHECK(count == 3);
            if (!strcmp(mode, "native-close") || !strcmp(mode, "reopen")) {
                CHECK(gt_ext_runtime_view_close(runtime, old) == GT_OK &&
                      view_count(runtime, NULL) == 2);
                view_call(peer, &peer_lease, runtime, "state", "closed");
                if (!strcmp(mode, "reopen")) {
                    view_call(peer, &peer_lease, runtime, "reopen", "success");
                    CHECK(view_count(runtime, NULL) == 3 &&
                          gt_ext_runtime_view_close(runtime, old) == GT_ERR_NOT_FOUND);
                }
            }
            CHECK(gt_ext_runtime_view_close(runtime, "foreign.1") == GT_ERR_NOT_FOUND);
        }
        gt_ext_runtime_stop(runtime);
        json = (void *)1;
        size = 123;
        CHECK(gt_ext_runtime_views_snapshot(runtime, &json, &size) == GT_ERR_AUTH && !json &&
              !size);
    }
    for (unsigned i = 0; i < 2000; i++) {
        if (gt_ext_runtime_close(runtime) == GT_EXT_RELEASED) {
            break;
        }
    }
    CHECK(gt_ext_runtime_info(runtime, &info) && info.phase == GT_EXT_RUNTIME_CLOSED &&
          info.engine_released && !info.dispose_error && info.cleanup_error == GT_OK);
    CHECK(gt_ext_runtime_destroy(&runtime) == GT_OK && !runtime);
    ext_drop(executor ? &pair.h : &pair.e);
    CHECK(ext_fds() == fds && gt__plugin_memory_info().used == before.used);
    printf("PASS views_%s\n", mode);
    return 0;
}
