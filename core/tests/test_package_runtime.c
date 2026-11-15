/* Real directory -> fixed metadata -> admitted Session -> actual pinned engine. */
#define main package_metadata_fixture_main
#include "test_package_check.c"
#undef main
#undef CHECK
#define EXT_RPC_MAIN package_rpc_fixture_main
#include "test_ext_rpc.c"
#include "extension/runtime.h"
#include "extension/package-runtime.h"
#include "extension/sdk-identity.h"
#include "base/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char root[4096];
    int fd;
    gt_package_checked_t *package;
    gt_package_check_info_t info;
    memory_t memory;
} load_fixture_t;

typedef struct {
    char path[4097];
    size_t size;
    char digest[65];
} load_member_t;

static void json_text(char *out, size_t cap, const char *text) {
    size_t at = 0;
    CHECK(cap >= 3);
    out[at++] = '"';
    for (; *text; text++) {
        CHECK(at + 3 < cap);
        if (*text == '"' || *text == '\\') {
            out[at++] = '\\';
        }
        out[at++] = *text;
    }
    out[at++] = '"';
    out[at] = 0;
}

static bool process_file_digest(const char *path, char digest[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    unsigned char buffer[32768];
    bool ok = true;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            ok = gt__sha256_update(&hash, buffer, (size_t)count);
            if (!ok) {
                break;
            }
        } else if (!count) {
            break;
        } else if (errno != EINTR) {
            ok = false;
            break;
        }
    }
    close(fd);
    return ok && gt__sha256_final(&hash, digest);
}

static void load_replace(char *text, size_t capacity, const char *old, const char *replacement) {
    char *at = strstr(text, old);
    CHECK(at);
    size_t before = (size_t)(at - text), tail = strlen(at + strlen(old)), n = strlen(replacement);
    CHECK(before + n + tail < capacity);
    memmove(at + n, at + strlen(old), tail + 1);
    memcpy(at, replacement, n);
}

static void
load_write(load_fixture_t *f, load_member_t *m, const char *name, const void *bytes, size_t size) {
    char path[4097];
    CHECK(strlen(name) < sizeof(path));
    strcpy(path, name);
    for (char *at = path; *at; at++) {
        if (*at == '/') {
            *at = 0;
            CHECK(!mkdirat(f->fd, path, 0700) || errno == EEXIST);
            *at = '/';
        }
    }
    CHECK(!put(f->fd, name, bytes, size));
    strcpy(m->path, name);
    m->size = size;
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    CHECK(gt__sha256_update(&hash, bytes, size) && gt__sha256_final(&hash, m->digest));
}

static const char *load_echo =
    "import {object,text,integer} from '@ghosterm/plugin-sdk';"
    "const codec=object({text:text(40),count:integer(0,100)});"
    "export async function activate(ctx){if(ctx.cancellation.cancelled)throw Error('cancelled');"
    "ctx.session.register({name:ctx.side==='executor'?'fixture.echo':'fixture.ask',input:codec,"
    "output:codec},async v=>v);return {dispose(){}};}";

static const char *load_process_roundtrip =
    "export async function activate(ctx){if(!ctx.process)throw Error('process unavailable');"
    "const child=await "
    "ctx.process.spawn({program:'cat',argv:['cat'],cwd:ctx.invocation.workspaceHandle,"
    "envPolicy:'selected-user-environment',cancellation:ctx.cancellation});"
    "if(child.verifiedVersion!=='fixture-1')throw Error('version');"
    "const write=child.writeStdin(new Uint8Array([65,66,67]));"
    "if(write.state!=='data'||write.count!==3)throw Error('write');child.closeStdin();"
    "let output;for(let i=0;i<200000&&!output;i++){const read=child.readStdout(16);"
    "if(read.state==='data')output=read.bytes;else if(read.state==='error')throw Error('read');}"
    "if(!output||output.length!==3||output[0]!==65||output[1]!==66||output[2]!==67)"
    "throw Error('roundtrip');await child.release();"
    "const env=await ctx.process.spawn({program:'env',argv:['env'],"
    "cwd:ctx.invocation.workspaceHandle,envPolicy:'selected-user-environment',"
    "cancellation:ctx.cancellation});env.closeStdin();let envBytes;"
    "for(let i=0;i<200000&&!envBytes;i++){const read=env.readStdout(128);"
    "if(read.state==='data')envBytes=read.bytes;else if(read.state==='error')throw Error('env "
    "read');}"
    "if(!envBytes||String.fromCharCode(...envBytes)!=='GT_FIXTURE_ENV=approved\\n')"
    "throw Error('environment');await env.release();return {dispose(){}};}";
static const char *load_process_kept =
    "export async function activate(ctx){await ctx.process.spawn({program:'cat',argv:['cat'],"
    "cwd:ctx.invocation.workspaceHandle,envPolicy:'selected-user-environment',"
    "cancellation:ctx.cancellation});"
    "return {dispose(){}};}";
static const char *load_process_deadline =
    "let child;export async function activate(ctx){child=await ctx.process.spawn({program:'cat',"
    "argv:['cat'],cwd:ctx.invocation.workspaceHandle,envPolicy:'selected-user-environment',"
    "cancellation:ctx.cancellation});return {dispose(){const p=child.poll();if(p.state!=="
    "'exited'||p.terminationReason!=='deadline')throw Error('deadline not reported')}};}";
static const char *load_process_deadline_live =
    "let child;export async function activate(ctx){child=await ctx.process.spawn({program:'cat',"
    "argv:['cat'],cwd:ctx.invocation.workspaceHandle,envPolicy:'selected-user-environment',"
    "cancellation:ctx.cancellation});return {dispose(){if(child.poll().terminationReason)"
    "throw Error('deadline fired early')}};}";
static const char *load_process_output =
    "export async function activate(ctx){const child=await ctx.process.spawn({program:'cat',"
    "argv:['cat'],cwd:ctx.invocation.workspaceHandle,envPolicy:'selected-user-environment',"
    "cancellation:ctx.cancellation});child.writeStdin(new Uint8Array([65,66]));child.closeStdin();"
    "return {dispose(){let limited=false;for(let i=0;i<8&&!limited;i++){const "
    "read=child.readStdout(2);"
    "if(read.state==='error')limited=read.error.code==='RESOURCE_LIMIT';}"
    "if(!limited)throw Error('output quota not reported')}};}";

static bool load_process_mode(const char *mode) {
    return !strcmp(mode, "process") || !strcmp(mode, "process-close") ||
           !strcmp(mode, "process-identity") || !strcmp(mode, "process-digest") ||
           !strcmp(mode, "process-deadline") || !strcmp(mode, "process-deadline-live") ||
           !strcmp(mode, "process-output");
}

static void load_fixture(load_fixture_t *f, const char *mode, const char *temp) {
    memset(f, 0, sizeof(*f));
    f->fd = -1;
    f->memory.wiped = true;
    CHECK(snprintf(f->root, sizeof(f->root), "%s/package-runtime-XXXXXX", temp) > 0 &&
          mkdtemp(f->root));
    f->fd = open(f->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(f->fd >= 0);
    bool task = !strncmp(mode, "client-task", 11) || !strcmp(mode, "executor-task");
    bool diff = !strncmp(mode, "client-diff", 11) || !strcmp(mode, "executor-diff");
    bool table =
        !strncmp(mode, "client-table", 12) || !strcmp(mode, "executor-table") || diff || task;
    bool client = !strcmp(mode, "client") || !strcmp(mode, "role") ||
                  !strncmp(mode, "client-table", 12) || !strncmp(mode, "client-diff", 11) ||
                  !strncmp(mode, "client-task", 11) || !strcmp(mode, "client-service");
    char entry[4097] = "executor/index.js";
    if (client) {
        strcpy(entry, "client/index.js");
    }
    if (!strcmp(mode, "unicode")) {
        strcpy(entry, "executor/中文-'quoted\".js");
    }
    if (!strcmp(mode, "long-path")) {
        strcpy(entry, "executor/");
        for (unsigned i = 0; i < 6; i++) {
            strcat(entry, "long-module-component-012345678901234567890123456789/");
        }
        strcat(entry, "index.js");
    }
    bool process = load_process_mode(mode);
    const char *script = !strcmp(mode, "process")                 ? load_process_roundtrip
                         : !strcmp(mode, "process-close")         ? load_process_kept
                         : !strcmp(mode, "process-identity")      ? load_process_kept
                         : !strcmp(mode, "process-deadline")      ? load_process_deadline
                         : !strcmp(mode, "process-deadline-live") ? load_process_deadline_live
                         : !strcmp(mode, "process-output")        ? load_process_output
                                                                  : load_echo;
    if (!strcmp(mode, "relative")) {
        script = "import {value} from '../shared/value.js';export function "
                 "activate(){if(value!==42)throw Error('relative');return {dispose(){}}}";
    }
    if (!strcmp(mode, "dynamic")) {
        script = "export async function activate(){const m=await "
                 "import('../shared/中文.js');if(m.value!==42)throw Error('dynamic');return "
                 "{dispose(){}}}";
    }
    if (!strcmp(mode, "cycle")) {
        script = "import {value} from '../shared/a.js';export function "
                 "activate(){if(value()!==42)throw Error('cycle');return {dispose(){}}}";
    }
    if (!strcmp(mode, "missing")) {
        script = "import '../shared/missing.js';export function activate(){return {dispose(){}}}";
    }
    if (!strcmp(mode, "escape")) {
        script = "import '../../escape.js';export function activate(){return {dispose(){}}}";
    }
    if (!strcmp(mode, "native")) {
        script = "import 'host:/native';export function activate(){return {dispose(){}}}";
    }
    if (!strcmp(mode, "node")) {
        script = "import 'node:fs';export function activate(){return {dispose(){}}}";
    }
    if (!strcmp(mode, "network")) {
        script = "import 'https://example.invalid/code.js';export function activate(){return "
                 "{dispose(){}}}";
    }
    if (!strcmp(mode, "binary")) {
        script = "import '../README';export function activate(){return {dispose(){}}}";
    }
    if (!strcmp(mode, "module-count") || !strcmp(mode, "module-boundary") ||
        !strcmp(mode, "module-boundary-overflow") || !strcmp(mode, "source-total")) {
        static char loop[4096];
        unsigned n = !strcmp(mode, "module-count")               ? 65
                     : !strcmp(mode, "module-boundary")          ? 56
                     : !strcmp(mode, "module-boundary-overflow") ? 57
                                                                 : 8;
        size_t used = 0;
        for (unsigned i = 0; i < n; i++) {
            int count = snprintf(loop + used, sizeof(loop) - used, "import '../shared/m%u.js';", i);
            CHECK(count > 0 && (size_t)count < sizeof(loop) - used);
            used += (size_t)count;
        }
        CHECK(used + 60 < sizeof(loop));
        strcpy(loop + used, "export function activate(){return {dispose(){}}}");
        script = loop;
    }
    if (!strcmp(mode, "utf8")) {
        script = "/* invalid \xc0\x80 */export function activate(){return {dispose(){}}}";
    }
    bool nonportable = !strcmp(mode, "nonportable"), old_sdk = !strcmp(mode, "sdk-version"),
         service = !strcmp(mode, "service") || !strcmp(mode, "client-service");
    char entry_json[8200], manifest_json[16384], side[12288];
    json_text(entry_json, sizeof(entry_json), entry);
    if (client) {
        CHECK(snprintf(side,
                       sizeof(side),
                       "\"client\":{\"runtime\":\"portable-js\",\"entry\":%s,\"requires\":[%s]}",
                       entry_json,
                       task      ? "\"session.rpc\",\"ui.task\""
                       : diff    ? "\"session.rpc\",\"ui.diff\""
                       : table   ? "\"session.rpc\",\"ui.table\""
                       : service ? "\"session.rpc\",\"ui.chat\""
                                 : "\"session.rpc\"") > 0);
    }
    if (!client) {
        CHECK(snprintf(side,
                       sizeof(side),
                       "\"executor\":{\"variants\":[{\"id\":\"portable\",\"runtime\":\"%s\","
                       "\"entry\":%s,\"requires\":[%s]%s}]}",
                       nonportable ? "node" : "portable-js",
                       entry_json,
                       process   ? "\"session.rpc\",\"process.spawn\",\"process.pipes\""
                       : task    ? "\"session.rpc\",\"ui.task\""
                       : diff    ? "\"session.rpc\",\"ui.diff\""
                       : table   ? "\"session.rpc\",\"ui.table\""
                       : service ? "\"session.rpc\",\"ui.chat\""
                                 : "\"session.rpc\"",
                       nonportable ? ",\"runtimeVersionRange\":\">=20.0.0\"" : "") > 0);
    }
    CHECK(snprintf(manifest_json,
                   sizeof(manifest_json),
                   "{\"schemaVersion\":1,\"id\":\"org.ghosterm.fixture\",\"pluginVersion\":\"0.1."
                   "0\",\"sdkApiRange\":\">=%s "
                   "<2.0.0\",\"bridgeProtocolRange\":[1],\"publisher\":\"fixture\",\"license\":"
                   "\"MIT\",\"commands\":[{\"path\":[\"fixture\"],\"title\":\"Fixture\","
                   "\"argsSchema\":\"schemas/input.json\"}],%s,\"permissions\":[]}",
                   old_sdk ? "0.1.0" : "1.0.0",
                   side) > 0);
    if (process) {
        load_replace(manifest_json,
                     sizeof(manifest_json),
                     "\"permissions\":[]",
                     "\"permissions\":[{\"side\":\"executor\",\"capability\":"
                     "\"process.spawn\",\"purpose\":\"runtime fixture\",\"scope\":"
                     "\"instance\",\"programs\":[\"cat\",\"env\"]}]");
    }
    if (!strcmp(mode, "registry-other") || !strcmp(mode, "registry-prefix") ||
        !strcmp(mode, "registry-neighbor")) {
        load_replace(
            manifest_json, sizeof(manifest_json), "org.ghosterm.fixture", "org.ghosterm.other");
    }
    if (!strncmp(mode, "registry-slot-", 14)) {
        char id[64];
        CHECK(snprintf(id, sizeof(id), "org.ghosterm.slot%s", mode + 14) > 0);
        load_replace(manifest_json, sizeof(manifest_json), "org.ghosterm.fixture", id);
    }
    if (!strcmp(mode, "registry-prefix")) {
        load_replace(
            manifest_json, sizeof(manifest_json), "[\"fixture\"]", "[\"fixture\",\"child\"]");
    }
    if (!strcmp(mode, "registry-neighbor")) {
        load_replace(manifest_json, sizeof(manifest_json), "[\"fixture\"]", "[\"fixturex\"]");
    }

    if (!strcmp(mode, "command-boundary")) {
        char tokens[600] = "[", title[1025] = {0}, replacement[1300];
        size_t used = 1;
        for (size_t i = 0; i < 8; i++) {
            if (i) {
                tokens[used++] = ',';
            }
            tokens[used++] = '"';
            memset(tokens + used, 'a' + (int)i, 64);
            used += 64;
            tokens[used++] = '"';
        }
        tokens[used++] = ']';
        tokens[used] = 0;
        load_replace(manifest_json, sizeof(manifest_json), "[\"fixture\"]", tokens);
        for (size_t i = 0; i < 256; i++) {
            memcpy(title + i * 4, "😀", 4);
        }
        CHECK(snprintf(replacement,
                       sizeof(replacement),
                       "\"title\":\"%s\",\"views\":[\"chat\",\"approval\",\"diff\",\"table\","
                       "\"task\",\"form\",\"resource\",\"web-content\"]",
                       title) > 0);
        load_replace(manifest_json, sizeof(manifest_json), "\"title\":\"Fixture\"", replacement);
    }
    if (!strcmp(mode, "command-many")) {
        char commands[12000] = "";
        size_t used = 0;
        for (size_t i = 0; i < 64; i++) {
            int n = snprintf(commands + used,
                             sizeof(commands) - used,
                             "%s{\"path\":[\"cmd%zu\"],\"title\":\"Command "
                             "%zu\",\"argsSchema\":\"schemas/input.json\"}",
                             i ? "," : "",
                             i,
                             i);
            CHECK(n > 0 && (size_t)n < sizeof(commands) - used);
            used += (size_t)n;
        }
        load_replace(
            manifest_json,
            sizeof(manifest_json),
            "{\"path\":[\"fixture\"],\"title\":\"Fixture\",\"argsSchema\":\"schemas/input.json\"}",
            commands);
    }
    load_member_t *members = calloc(128, sizeof(*members));
    CHECK(members);
    size_t count = 0;
    load_write(f, &members[count++], "plugin.json", manifest_json, strlen(manifest_json));
    load_write(f, &members[count++], "README", "fixture", 7);
    load_write(f, &members[count++], "LICENSES/MIT.txt", "fixture license", 15);
    const char *argument_schema = "{}";
    if (!strcmp(mode, "cli-arguments")) {
        argument_schema =
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
            "\"maxLength\":40},\"lines\":{\"type\":\"integer\",\"minimum\":1,"
            "\"maximum\":100},\"wrap\":{\"type\":\"boolean\"}},"
            "\"required\":[\"path\",\"lines\",\"wrap\"],\"additionalProperties\":false,"
            "\"x-ghosterm-cli\":{\"version\":1,\"fields\":{\"path\":{"
            "\"position\":0},\"lines\":{\"option\":\"lines\",\"default\":10},"
            "\"wrap\":{\"option\":\"wrap\",\"default\":false}}}}";
    }
    load_write(
        f, &members[count++], "schemas/input.json", argument_schema, strlen(argument_schema));
    char *large = NULL;
    size_t source_size = strlen(script);
    if (!strcmp(mode, "source-limit")) {
        source_size = 131073;
        large = malloc(source_size);
        CHECK(large);
        memset(large, ' ', source_size);
        memcpy(large, load_echo, strlen(load_echo));
        script = large;
    }
    if (!strcmp(mode, "nul")) {
        source_size = 80;
        large = calloc(1, source_size);
        CHECK(large);
        memcpy(large, "export function activate(){}", 28);
        script = large;
    }
    load_write(f, &members[count++], entry, script, source_size);
    free(large);
    load_write(f, &members[count++], "shared/value.js", "export const value=42;", 22);
    load_write(f, &members[count++], "shared/中文.js", "export const value=42;", 22);
    const char *a = "import {base} from './b.js';export function value(){return base()+1;}";
    const char *b =
        "import {value} from './a.js';export function base(){return 41;}export {value};";
    load_write(f, &members[count++], "shared/a.js", a, strlen(a));
    load_write(f, &members[count++], "shared/b.js", b, strlen(b));
    const char *fake = "throw Error('package must not replace the host SDK');";
    load_write(f, &members[count++], "sdk/index.js", fake, strlen(fake));
    if (!strcmp(mode, "unused")) {
        large = malloc(131073);
        CHECK(large);
        memset(large, 'x', 131073);
        load_write(f, &members[count++], "shared/unused.js", large, 131073);
        free(large);
    }
    unsigned extra = !strcmp(mode, "module-count")               ? 65
                     : !strcmp(mode, "module-boundary")          ? 56
                     : !strcmp(mode, "module-boundary-overflow") ? 57
                     : !strcmp(mode, "source-total")             ? 8
                                                                 : 0;
    for (unsigned i = 0; i < extra; i++) {
        char name[64];
        CHECK(snprintf(name, sizeof(name), "shared/m%u.js", i) > 0);
        size_t size = !strcmp(mode, "source-total") ? 131072 : 22;
        large = malloc(size);
        CHECK(large);
        memset(large, ' ', size);
        memcpy(large, "export const value=42;", 22);
        load_write(f, &members[count++], name, large, size);
        free(large);
    }
    char *integrity = calloc(1, 131072);
    CHECK(integrity);
    size_t used = 0;
    used += (size_t)snprintf(integrity, 131072, "{\"schemaVersion\":1,\"members\":[");
    for (size_t i = 0; i < count; i++) {
        char name[8200];
        json_text(name, sizeof(name), members[i].path);
        int n = snprintf(integrity + used,
                         131072 - used,
                         "%s{\"path\":%s,\"bytes\":%zu,\"sha256\":\"%s\"}",
                         i ? "," : "",
                         name,
                         members[i].size,
                         members[i].digest);
        CHECK(n > 0 && (size_t)n < 131072 - used);
        used += (size_t)n;
    }
    CHECK(used + 3 < 131072);
    memcpy(integrity + used, "]}", 3);
    CHECK(!put(f->fd, "integrity.json", integrity, used + 2));
    free(integrity);
    free(members);
    gt_package_capture_config_t capture = {f->fd,
                                           GT_PACKAGE_MEMBERS_MAX,
                                           GT_PACKAGE_ENTRIES_MAX,
                                           GT_PACKAGE_MEMBER_BYTES_MAX,
                                           GT_PACKAGE_TOTAL_BYTES_MAX};
    gt_package_snapshot_t *snapshot = NULL;
    CHECK(gt__package_capture_open(&capture, NULL, &snapshot) == GT_PACKAGE_OK);
    gt_package_status_t status = GT_PACKAGE_WAIT;
    for (unsigned i = 0; i < 50000 && status == GT_PACKAGE_WAIT; i++) {
        status = gt__package_capture_step(snapshot);
    }
    CHECK(status == GT_PACKAGE_OK);
    char facts[1024], registry[20000];
    CHECK(snprintf(facts,
                   sizeof(facts),
                   "{\"side\":\"%s\",\"sdkVersion\":\"%s\",\"protocols\":[1],\"runtimes\":[\"%s\"],"
                   "\"capabilities\":[%s],\"programs\":{\"node\":{"
                   "\"state\":\"available\",\"version\":\"22.0.0\"},\"cat\":{"
                   "\"state\":\"available\",\"version\":\"fixture-1\"},\"env\":{"
                   "\"state\":\"available\",\"version\":\"fixture-1\"}}}",
                   client ? "client" : "executor",
                   old_sdk ? "0.1.1" : GT_PLUGIN_SDK_API_VERSION,
                   nonportable ? "node" : "portable-js",
                   process ? "\"session.rpc\",\"process.spawn\",\"process.pipes\""
                   : task  ? "\"session.rpc\",\"ui.task\""
                   : diff  ? "\"session.rpc\",\"ui.diff\""
                   : table ? "\"session.rpc\",\"ui.table\""
                           : "\"session.rpc\",\"ui.chat\"") > 0);
    bool conflict = !strcmp(mode, "association") || !strcmp(mode, "associated");
    CHECK(snprintf(registry, sizeof(registry), "[%s]", manifest_json) > 0);
    clock_tester_t clock = {.mode = "compatible"};
    gt_allocator_t allocator = {&f->memory, alloc, release};
    gt_package_check_config_t config = {.host_json = facts,
                                        .host_size = strlen(facts),
                                        .clock_context = &clock,
                                        .clock = clock_test,
                                        .registry_json = conflict ? registry : NULL,
                                        .registry_size = conflict ? strlen(registry) : 0};
    gt_package_check_report_t report;
    gt_package_check_status_t result =
        gt__package_check(&snapshot, &config, &allocator, &f->package, &report);
    if (result != GT_PACKAGE_CHECK_COMPATIBLE) {
        fprintf(stderr, "metadata %s: %u %s\n", mode, result, report.code);
    }
    CHECK(result == GT_PACKAGE_CHECK_COMPATIBLE && !snapshot && f->package &&
          gt__package_checked_info(f->package, &f->info));
    CHECK(!f->info.authorization_granted && !f->info.publisher_authenticated &&
          f->memory.live == 1);
    gt_package_command_t command;
    size_t command_count = !strcmp(mode, "command-many") ? 64 : 1;
    CHECK(gt__package_checked_command_count(f->package) == command_count &&
          gt__package_checked_command(f->package, 0, &command));
    if (!strcmp(mode, "command-boundary")) {
        CHECK(strlen(command.path) == 519 && strlen(command.title) == 1024 &&
              command.view_count == 8 && !strcmp(command.views[7], "web-content"));
        for (size_t i = 0; i < 8; i++) {
            for (size_t j = 0; j < 64; j++) {
                CHECK(command.path[i * 65 + j] == 'a' + (int)i);
            }
        }
        for (size_t i = 0; i < 256; i++) {
            CHECK(!memcmp(command.title + i * 4, "😀", 4));
        }
    } else if (!strcmp(mode, "command-many")) {
        for (size_t i = 0; i < 64; i++) {
            char path[64], title[64];
            snprintf(path, sizeof(path), "cmd%zu", i);
            snprintf(title, sizeof(title), "Command %zu", i);
            CHECK(gt__package_checked_command(f->package, i, &command));
            CHECK(!strcmp(command.path, path) && !strcmp(command.title, title));
        }
    } else {
        CHECK(!strcmp(command.path,
                      !strcmp(mode, "registry-prefix")     ? "fixture child"
                      : !strcmp(mode, "registry-neighbor") ? "fixturex"
                                                           : "fixture") &&
              !strcmp(command.title, "Fixture") && command.view_count == 0);
    }
    CHECK(!strcmp(command.args_schema, "schemas/input.json"));
    memset(&command, 0x55, sizeof(command));
    CHECK(!gt__package_checked_command(f->package, command_count, &command) && !command.path[0] &&
          !command.title[0]);
}

static void load_clear(load_fixture_t *f) {
    if (f->package) {
        CHECK(gt__package_checked_destroy(&f->package) == GT_PACKAGE_OK);
    }
    CHECK(!f->memory.live && f->memory.wiped);
    CHECK(!clear(f->fd) && !close(f->fd) && !rmdir(f->root));
}

static void load_pair(rpc_pair_t *p, const load_fixture_t *f, const char *mode) {
    memset(p, 0, sizeof(*p));
    int fd[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    gt_ext_channel_t *h = ext_channel(fd[0]), *e = ext_channel(fd[1]);
    gt_ext_session_config_t c = ext_config();
    c.features = GT_EXT_FEATURE_REQUESTS;
    strcpy(c.plugin_id, f->info.id);
    strcpy(c.plugin_version, f->info.version);
    strcpy(c.client_digest, f->info.package_digest);
    strcpy(c.executor_digest, f->info.package_digest);
    strcpy(c.sdk_version, GT_PLUGIN_SDK_API_VERSION);
    c.required_count = !strcmp(mode, "ungranted") ? 0 : 1;
    c.available_count = 1;
    strcpy(c.required[0], "session.rpc");
    strcpy(c.available[0], "session.rpc");
    if (load_process_mode(mode)) {
        c.required_count = c.available_count = 3;
        strcpy(c.required[1], "process.spawn");
        strcpy(c.required[2], "process.pipes");
        strcpy(c.available[1], "process.spawn");
        strcpy(c.available[2], "process.pipes");
    }
    if (!strcmp(mode, "id")) {
        strcpy(c.plugin_id, "org.ghosterm.other");
    }
    if (!strcmp(mode, "version")) {
        strcpy(c.plugin_version, "0.2.0");
    }
    if (!strcmp(mode, "digest")) {
        memset(c.executor_digest, 'a', 64);
    }
    if (!strcmp(mode, "service") || !strcmp(mode, "client-service")) {
        c.required_count = c.available_count = 2;
        strcpy(c.required[1], "ui.chat");
        strcpy(c.available[1], "ui.chat");
    }
    if (!strncmp(mode, "client-table", 12) || !strcmp(mode, "executor-table") ||
        !strncmp(mode, "client-diff", 11) || !strcmp(mode, "executor-diff") ||
        !strncmp(mode, "client-task", 11) || !strcmp(mode, "executor-task")) {
        bool task = !strncmp(mode, "client-task", 11) || !strcmp(mode, "executor-task");
        bool diff = !strncmp(mode, "client-diff", 11) || !strcmp(mode, "executor-diff");
        c.required_count =
            (!strcmp(mode, "client-table-ungranted") || !strcmp(mode, "client-diff-ungranted") ||
             !strcmp(mode, "client-task-ungranted"))
                ? 1
                : 2;
        c.available_count = 2;
        strcpy(c.required[1], task ? "ui.task" : diff ? "ui.diff" : "ui.table");
        strcpy(c.available[1], task ? "ui.task" : diff ? "ui.diff" : "ui.table");
    }
    gt_ext_rpc_method_t methods[4];
    rpc_methods(methods);
    for (size_t i = 0; i < 4; i++) {
        strcpy(methods[i].capability, "session.rpc");
    }
    p->pair.h = rpc_adopt(&h, GT_EXT_HOST, &c, methods, 4);
    p->pair.e = rpc_adopt(&e, GT_EXT_EXECUTOR, &c, methods, 4);
    if (!strcmp(mode, "inactive")) {
        return;
    }
    ext_allow(&p->pair);
    CHECK(gt_ext_session_lease(p->pair.h, &p->h) && gt_ext_session_lease(p->pair.e, &p->e));
    if (!strcmp(mode, "revoked")) {
        gt_ext_session_revoke(p->pair.e);
    }
}

static gt_runtime_info_t load_info(gt_bridge_runtime_t *r) {
    gt_runtime_info_t out;
    CHECK(gt__bridge_runtime_info(r, &out));
    return out;
}

static bool load_dispose_must_succeed;

static void load_drop(gt_bridge_runtime_t **r) {
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_close_t state = gt__bridge_runtime_close(*r);
        CHECK(state == GT_EXT_DRAINING || state == GT_EXT_RELEASED);
        if (load_dispose_must_succeed) {
            CHECK(!load_info(*r).dispose_error);
        }
        if (state == GT_EXT_RELEASED) {
            CHECK(load_info(*r).engine_released && gt__bridge_runtime_destroy(r) == GT_OK && !*r);
            load_dispose_must_succeed = false;
            return;
        }
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(false);
}

static void load_run(gt_bridge_runtime_t *r, gt_ext_session_t *peer) {
    for (unsigned i = 0; i < 20000 && load_info(r).phase < GT_RUNTIME_ACTIVE; i++) {
        gt_ext_session_step(peer, 0);
        gt__bridge_runtime_step(r);
    }
}

static void load_echo_request(gt_bridge_runtime_t *r, rpc_pair_t *p, bool client) {
    uint64_t h = rpc_start(p, client, "{\"text\":\"固定字节\",\"count\":17}", 1000);
    gt_ext_rpc_result_t result = {0};
    gt_ext_session_t *peer = client ? p->pair.e : p->pair.h;
    gt_ext_lease_t *lease = client ? &p->e : &p->h;
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(peer, 0);
        CHECK(gt__bridge_runtime_step(r) == GT_RUNTIME_ACTIVE);
        result = rpc_result(peer, lease, h);
        if (result.confirmed) {
            break;
        }
    }
    CHECK(result.confirmed && result.state == GT_EXT_RPC_SUCCEEDED &&
          strstr(result.outcome, "固定字节"));
    CHECK(gt_ext_session_forget_request(peer, lease, h) == GT_EXT_RPC_OK);
}

static void *load_thread(void *arg) {
    gt_package_checked_t *package = arg;
    for (unsigned i = 0; i < 100; i++) {
        gt_package_checked_t *hold = NULL;
        CHECK(gt__package_checked_retain(package, &hold));
        gt_package_check_info_t info;
        CHECK(gt__package_checked_info(hold, &info) && !strcmp(info.id, "org.ghosterm.fixture"));
        CHECK(gt__package_checked_destroy(&hold) == GT_PACKAGE_OK && !hold);
    }
    CHECK(gt__package_checked_destroy(&package) == GT_PACKAGE_OK);
    return NULL;
}
#ifndef PACKAGE_RUNTIME_MAIN
#define PACKAGE_RUNTIME_MAIN main
#endif
int PACKAGE_RUNTIME_MAIN(int argc, char **argv) {
    CHECK(argc >= 2);
    const char *mode = argv[1];
    gt_bridge_runtime_t *r = NULL;
    if (!strcmp(mode, "disabled")) {
        CHECK(gt__bridge_runtime_open_package(NULL, NULL, NULL, false, &r) == GT_ERR_UNSUPPORTED &&
              !r);
        return 0;
    }
    CHECK(argc == 3);
    size_t fds = ext_fds();
    gt_plugin_memory_info_t before = gt__plugin_memory_info();
    load_fixture_t f;
    load_fixture(&f, mode, argv[2]);
    if (!strcmp(mode, "retained") || !strcmp(mode, "thread")) {
        if (!strcmp(mode, "retained")) {
            gt_package_checked_t *hold = NULL;
            CHECK(gt__package_checked_retain(f.package, &hold) && hold == f.package);
            CHECK(gt__package_checked_destroy(&f.package) == GT_PACKAGE_OK && !f.package &&
                  f.memory.live == 1);
            CHECK(gt__package_checked_info(hold, &f.info));
            CHECK(gt__package_checked_destroy(&hold) == GT_PACKAGE_OK);
        } else {
            pthread_t threads[4];
            gt_package_checked_t *holds[4];
            for (unsigned i = 0; i < 4; i++) {
                CHECK(gt__package_checked_retain(f.package, &holds[i]));
            }
            CHECK(gt__package_checked_destroy(&f.package) == GT_PACKAGE_OK);
            for (unsigned i = 0; i < 4; i++) {
                CHECK(!pthread_create(&threads[i], NULL, load_thread, holds[i]));
            }
            for (unsigned i = 0; i < 4; i++) {
                CHECK(!pthread_join(threads[i], NULL));
            }
        }
    } else {
        rpc_pair_t p;
        load_pair(&p, &f, mode);
        bool client = !strcmp(mode, "client") || !strncmp(mode, "client-table", 12) ||
                      !strncmp(mode, "client-diff", 11) || !strncmp(mode, "client-task", 11) ||
                      !strcmp(mode, "client-service");
        gt_ext_session_t **owned = client ? &p.pair.h : &p.pair.e;
        gt_ext_session_t *peer = client ? p.pair.e : p.pair.h;
        gt_runtime_config_t config = {.role = client ? GT_EXT_HOST : GT_EXT_EXECUTOR,
                                      .cpu_ms = 1000,
                                      .activation_ms = 10000,
                                      .jobs_per_step = 128};
        gt_ext_process_program_mapping_t process_programs[] = {
            {"cat", "/bin/cat", "fixture-1", NULL, NULL, 0},
            {"env", "/usr/bin/env", "fixture-1", NULL, NULL, 0}};
        char identity_cat_path[4097];
        const char *cat_path = process_programs[0].executable;
        if (!strcmp(mode, "process-identity")) {
            CHECK(
                snprintf(identity_cat_path, sizeof(identity_cat_path), "%s/verified-cat", f.root) >
                    0 &&
                symlink("/bin/cat", identity_cat_path) == 0);
            cat_path = identity_cat_path;
            process_programs[0].executable = cat_path;
        }
        char process_digests[2][65];
        CHECK(process_file_digest(process_programs[0].executable, process_digests[0]) &&
              process_file_digest(process_programs[1].executable, process_digests[1]));
        process_programs[0].verified_sha256 = process_digests[0];
        process_programs[1].verified_sha256 = process_digests[1];
        if (!strcmp(mode, "process-digest")) {
            process_programs[0].verified_sha256 =
                "0000000000000000000000000000000000000000000000000000000000000000";
        }
        const gt_ext_process_environment_t process_environment = {"GT_FIXTURE_ENV", "approved"};
        const gt_runtime_root_t process_root = {"workspace", f.fd, 16777216};
        const gt_ext_invocation_t process_invocation = {"fixture", NULL, 0};
        if (load_process_mode(mode)) {
            config.process_programs = process_programs;
            config.process_program_count = 2;
            config.process_environment = &process_environment;
            config.process_environment_count = 1;
            config.roots = &process_root;
            config.root_count = 1;
            config.invocation = &process_invocation;
            config.process_lifetime_ms = !strcmp(mode, "process-deadline")        ? 10
                                         : !strcmp(mode, "process-deadline-live") ? 10000
                                                                                  : 600000;
            config.process_output_bytes = !strcmp(mode, "process-output") ? 1 : 1048576;
        }
        gt_status_t expected = GT_OK;
        if (!strcmp(mode, "id") || !strcmp(mode, "version") || !strcmp(mode, "digest") ||
            !strcmp(mode, "role")) {
            expected = GT_ERR_PROTOCOL;
        }
        if (!strcmp(mode, "sdk-version") || !strcmp(mode, "nonportable") ||
            !strcmp(mode, "service") || !strcmp(mode, "client-service") ||
            !strcmp(mode, "executor-table") || !strcmp(mode, "executor-diff") ||
            !strcmp(mode, "executor-task")) {
            expected = GT_ERR_UNSUPPORTED;
        }
        if (!strcmp(mode, "association") || !strcmp(mode, "ungranted") ||
            !strcmp(mode, "client-table-ungranted") || !strcmp(mode, "client-diff-ungranted") ||
            !strcmp(mode, "client-task-ungranted")) {
            expected = GT_ERR_AUTH;
        }
        if (!strcmp(mode, "inactive") || !strcmp(mode, "revoked") || !strcmp(mode, "invalid")) {
            expected = GT_ERR_INVALID_ARG;
        }
        if (!strcmp(mode, "process-digest")) {
            expected = GT_ERR_AUTH;
        }
        if (!strcmp(mode, "invalid")) {
            config.entry = "plugin:/override.js";
        }
        if (!strcmp(mode, "frozen")) {
            CHECK(!clear(f.fd));
        }
        if (!strcmp(mode, "allocation")) {
            gt_plugin_memory_info_t m = gt__plugin_memory_info();
            size_t n = m.limit - m.used;
            void *reservation = gt__plugin_memory_alloc(NULL, n);
            CHECK(reservation);
            CHECK(gt__bridge_runtime_open_package(owned, f.package, &config, false, &r) ==
                      GT_ERR_IO &&
                  !r && *owned && f.memory.live == 1);
            gt__plugin_memory_free(NULL, reservation, n);
        }
        gt_ext_session_t *original = *owned;
        gt_status_t result = gt__bridge_runtime_open_package(
            owned, f.package, &config, !strcmp(mode, "associated"), &r);
        if (result != expected && strcmp(mode, "process-digest")) {
            fprintf(stderr, "open %s: actual=%u expected=%u\n", mode, result, expected);
        }
        CHECK(result == expected);
        if (expected != GT_OK) {
            CHECK(!r && *owned == original && f.package && f.memory.live == 1);
            rpc_drop(&p);
        } else {
            CHECK(r && !*owned && load_info(r).phase == GT_RUNTIME_CREATED &&
                  load_info(r).module_bytes == 0);
            rpc_pair_t second = {0};
            gt_bridge_runtime_t *other = NULL;
            if (!strcmp(mode, "share")) {
                load_pair(&second, &f, "share");
                CHECK(gt__bridge_runtime_open_package(
                          &second.pair.e, f.package, &config, false, &other) == GT_OK &&
                      other && !second.pair.e && f.memory.live == 1);
            }
            CHECK(gt__package_checked_destroy(&f.package) == GT_PACKAGE_OK && !f.package &&
                  f.memory.live == 1);
            void *reservation = NULL;
            size_t reserved = 0;
            if (!strcmp(mode, "source-memory")) {
                gt_plugin_memory_info_t m = gt__plugin_memory_info();
                reserved = m.limit - m.used - 65536;
                reservation = gt__plugin_memory_alloc(NULL, reserved);
                CHECK(reservation);
            }
            if (!strcmp(mode, "close-created")) {
                CHECK(gt__bridge_runtime_destroy(&r) == GT_ERR_INVALID_ARG && r);
                CHECK(load_info(r).module_bytes == 0);
                load_drop(&r);
                ext_drop(&p.pair.h);
                load_clear(&f);
                CHECK(ext_fds() == fds && gt__plugin_memory_info().used == before.used);
                return 0;
            }
            if (!strcmp(mode, "stop-created")) {
                gt__bridge_runtime_stop(r);
                CHECK(gt__bridge_runtime_step(r) == GT_RUNTIME_STOPPED);
                CHECK(load_info(r).error == GT_RUNTIME_CANCELLED && load_info(r).module_bytes == 0);
                load_drop(&r);
                ext_drop(&p.pair.h);
                load_clear(&f);
                CHECK(ext_fds() == fds && gt__plugin_memory_info().used == before.used);
                return 0;
            }
            if (!strcmp(mode, "process-identity")) {
                CHECK(!unlink(identity_cat_path) &&
                      symlink("/usr/bin/env", identity_cat_path) == 0);
            }
            size_t before_process_run = ext_fds();
            load_run(r, peer);
            gt__plugin_memory_free(NULL, reservation, reserved);
            bool memory = !strcmp(mode, "source-limit") || !strcmp(mode, "source-total") ||
                          !strcmp(mode, "module-count") ||
                          !strcmp(mode, "module-boundary-overflow") ||
                          !strcmp(mode, "source-memory");
            bool script = !strcmp(mode, "missing") || !strcmp(mode, "escape") ||
                          !strcmp(mode, "process-identity") || !strcmp(mode, "native") ||
                          !strcmp(mode, "node") || !strcmp(mode, "network") ||
                          !strcmp(mode, "binary") || !strcmp(mode, "nul") || !strcmp(mode, "utf8");
            CHECK(load_info(r).phase == (memory || script ? GT_RUNTIME_FAILED : GT_RUNTIME_ACTIVE));
            if (!strcmp(mode, "process-close")) {
                CHECK(load_info(r).phase == GT_RUNTIME_ACTIVE &&
                      ext_fds() >= before_process_run + 3);
            }
            if (!strcmp(mode, "process-deadline") || !strcmp(mode, "process-deadline-live") ||
                !strcmp(mode, "process-output")) {
                struct timespec delay = {0, 2000000};
                unsigned waits = !strcmp(mode, "process-deadline") ? 40 : 10;
                for (unsigned i = 0; i < waits; i++) {
                    if (!strcmp(mode, "process-deadline")) {
                        (void)gt__bridge_runtime_step(r);
                    }
                    (void)nanosleep(&delay, NULL);
                }
                load_dispose_must_succeed = true;
            }
            if (memory) {
                CHECK(load_info(r).error == GT_RUNTIME_MEMORY);
            }
            if (script) {
                CHECK(load_info(r).error == GT_RUNTIME_SCRIPT);
            }
            bool echo = !strcmp(mode, "roundtrip") || !strcmp(mode, "client") ||
                        !strcmp(mode, "client-table") || !strcmp(mode, "client-diff") ||
                        !strcmp(mode, "client-task") || !strcmp(mode, "unicode") ||
                        !strcmp(mode, "long-path") || !strcmp(mode, "frozen") ||
                        !strcmp(mode, "unused") || !strcmp(mode, "share") ||
                        !strcmp(mode, "associated") || !strcmp(mode, "allocation");
            if (echo) {
                load_echo_request(r, &p, client);
            }
            load_drop(&r);
            if (client) {
                ext_drop(&p.pair.e);
            } else {
                ext_drop(&p.pair.h);
            }
            if (other) {
                CHECK(f.memory.live == 1);
                load_run(other, second.pair.h);
                CHECK(load_info(other).phase == GT_RUNTIME_ACTIVE);
                load_echo_request(other, &second, false);
                load_drop(&other);
                ext_drop(&second.pair.h);
            }
        }
    }
    load_clear(&f);
    CHECK(ext_fds() == fds && gt__plugin_memory_info().used == before.used);
    printf("PASS package-runtime %s\n", mode);
    return 0;
}
