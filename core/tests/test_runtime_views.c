/* Actual public Host Runtime, local view service and pinned Portable SDK. */
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

static char *view_read(const char *root, const char *name) {
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/%s", root, name) > 0);
    FILE *file = fopen(path, "rb");
    CHECK(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    CHECK(size > 0 && size < 131072 && !fseek(file, 0, SEEK_SET));
    char *text = calloc(1, (size_t)size + 1);
    CHECK(text && fread(text, 1, (size_t)size, file) == (size_t)size && !fclose(file));
    return text;
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
    "import {object,text} from '@ghosterm/plugin-sdk';"
    "export function activate(ctx){const v=ctx.views;if(!v)throw Error('missing service');"
    "const "
    "spec={kind:'table',viewId:'table-a',methods:{describe:'table.describe',read:'table.page'}};"
    "let "
    "first=v.open(spec);v.open({kind:'diff',viewId:'diff-b',methods:{describe:'diff.describe',read:"
    "'diff.page'}});"
    "v.open({kind:'task',viewId:'task-c',methods:{describe:'task.describe',read:'task.read',cancel:"
    "'task.cancel'}});"
    "ctx.session.register({name:'fixture.control',input:object({action:text(32)}),output:object({"
    "text:text(128)})},(input,request)=>{request.check();"
    "if(input.action==='state')return {text:first.state};"
    "if(input.action==='reopen'){first=v.open(spec);return {text:first.handle}}"
    "if(input.action==='close'){first.close();first.close();return {text:first.state}}"
    "if(input.action==='dispose'){v.dispose();return {text:String(v.retained)}}"
    "throw Error('action')});return {dispose(){}}}";

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
    CHECK(argc == 3);
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
    const char *names[] = {"index.js",
                           "codec.js",
                           "session.js",
                           "wire-contract.js",
                           "table.js",
                           "diff.js",
                           "task.js",
                           "views.js"};
    char *sources[8], ids[8][64];
    gt_ext_runtime_module_t modules[9] = {0};
    for (size_t i = 0; i < 8; i++) {
        sources[i] = view_read(argv[2], names[i]);
        CHECK(snprintf(ids[i], sizeof(ids[i]), "sdk:/%s", names[i]) > 0);
        view_module(&modules[i], ids[i], sources[i]);
    }
    char script[8192];
    char raw_sdk[4096];
    const char *plugin = view_plugin;
    bool raw = !strcmp(mode, "native-limit") || !strcmp(mode, "native-duplicate") ||
               !strcmp(mode, "native-method");
    if (!strcmp(mode, "disabled") || !strcmp(mode, "executor") || !strcmp(mode, "ungranted")) {
        plugin = "export function activate(ctx){if(ctx.views!==undefined)throw Error('unexpected "
                 "UI');return {dispose(){}}}";
    } else if (!strcmp(mode, "direction") || !strcmp(mode, "kind")) {
        CHECK(snprintf(script,
                       sizeof(script),
                       "export function activate(ctx){let "
                       "rejected=false;try{ctx.views.open({kind:'%s',viewId:'a',methods:{describe:'"
                       "table.describe',read:'table.page'}})}catch(e){rejected=e.code==='VIEW_%s'}"
                       "if(!rejected)throw Error('missing rejection');return {dispose(){}}}",
                       !strcmp(mode, "kind") ? "diff" : "table",
                       !strcmp(mode, "kind") ? "UNSUPPORTED" : "INVALID") > 0);
        plugin = script;
    } else if (!strcmp(mode, "limit") || !strcmp(mode, "duplicate")) {
        CHECK(snprintf(script,
                       sizeof(script),
                       "export function activate(ctx){const v=ctx.views;for(let "
                       "i=0;i<8;i++)v.open({kind:'table',viewId:'v'+i,methods:{describe:'table."
                       "describe',read:'table.page'}});let "
                       "rejected=false;try{v.open({kind:'table',viewId:'%s',methods:{describe:'"
                       "table.describe',read:'table.page'}})}catch(e){rejected=e.code==='VIEW_%s'}"
                       "if(!rejected)throw Error('missing rejection');return {dispose(){}}}",
                       !strcmp(mode, "duplicate") ? "v0" : "extra",
                       !strcmp(mode, "duplicate") ? "DUPLICATE" : "LIMIT") > 0);
        plugin = script;
    } else if (raw) {
        /* Trusted SDK adapter fault injection tests the Native boundary itself. */
        const char *code =
            !strcmp(mode, "native-duplicate")
                ? "export class "
                  "ViewRegistry{constructor(p){if(p.viewOpen('table','a','table.describe','table."
                  "page',undefined).status!=='ok'||p.viewOpen('table','a','table.describe','table."
                  "page',undefined).status!=='invalid')throw Error('native duplicate')}dispose(){}}"
            : !strcmp(mode, "native-method")
                ? "export class "
                  "ViewRegistry{constructor(p){if(p.viewOpen('table','a','missing.describe','table."
                  "page',undefined).status!=='invalid')throw Error('native method')}dispose(){}}"
                : "export class ViewRegistry{constructor(p){for(let "
                  "i=0;i<8;i++)if(p.viewOpen('table','v'+i,'table.describe','table.page',undefined)"
                  ".status!=='ok')throw "
                  "Error('capacity');if(p.viewOpen('table','extra','table.describe','table.page',"
                  "undefined).status!=='busy')throw Error('native limit')}dispose(){}}";
        CHECK(snprintf(
                  raw_sdk, sizeof(raw_sdk), "export const VIEWS_MAX=8,ViewSpecCodec={};%s", code) >
              0);
        view_module(&modules[7], ids[7], raw_sdk);
        plugin = "export function activate(ctx){if(!ctx.views)throw Error('service');return "
                 "{dispose(){}}}";
    }
    view_module(&modules[8], "plugin:/entry.js", plugin);
    bool executor = !strcmp(mode, "executor");
    gt_ext_runtime_config_t settings = {.modules = modules,
                                        .module_count = 9,
                                        .entry = "plugin:/entry.js",
                                        .role = executor ? GT_EXT_EXECUTOR : GT_EXT_HOST};
    gt_ext_runtime_t *runtime = NULL;
    CHECK(gt_ext_runtime_open(executor ? &pair.e : &pair.h, &settings, &runtime) == GT_OK &&
          runtime);
    for (size_t i = 0; i < 8; i++) {
        free(sources[i]);
    }
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
        if (!strcmp(mode, "direction") || !strcmp(mode, "kind") || !strcmp(mode, "native-method")) {
            CHECK(count == 0);
        } else if (!strcmp(mode, "duplicate") || !strcmp(mode, "limit") ||
                   !strcmp(mode, "native-limit")) {
            CHECK(count == 8);
        } else if (!strcmp(mode, "native-duplicate")) {
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
            } else if (!strcmp(mode, "dispose")) {
                view_call(peer, &peer_lease, runtime, "dispose", "0");
                CHECK(view_count(runtime, NULL) == 0);
            } else if (!strcmp(mode, "js-close")) {
                view_call(peer, &peer_lease, runtime, "close", "closed");
                CHECK(view_count(runtime, NULL) == 2);
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
