#define EXT_RPC_MAIN package_rpc_fixture_main
#include "test_ext_rpc.c"
#include "base/sha256.h"
#include "extension/memory.h"

typedef struct {
    char directory[4096];
    char digest[65];
    int fd;
} fixture_t;

static const char fixture_id[] = "org.ghosterm.fixture";
static const char *const paths[] = {"manifest.json", "entry.lua", "input.json"};

static void write_member(int folder, const char *name, const char *bytes, char digest[65]) {
    const size_t size = strlen(bytes);
    int fd = openat(folder, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, bytes, size) == (ssize_t)size && !close(fd));
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    CHECK(gt__sha256_update(&hash, bytes, size) && gt__sha256_final(&hash, digest));
}

static void fixture_open(fixture_t *f,
                         const char *temporary,
                         const char *id,
                         const char *version,
                         const char *command,
                         bool executor) {
    CHECK(snprintf(f->directory, sizeof(f->directory), "%s/public-package-XXXXXX", temporary) > 0 &&
          mkdtemp(f->directory));
    f->fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(f->fd >= 0);
    char manifest[1024];
    int length = snprintf(
        manifest,
        sizeof(manifest),
        "{\"schemaVersion\":2,\"id\":\"%s\",\"pluginVersion\":\"%s\","
        "\"sdkApiRange\":\"^1.0.0\",\"bridgeProtocolRange\":[1],\"publisher\":\"Fixture\","
        "\"license\":\"MIT\","
        "%s,\"commands\":[{\"path\":[%s],\"title\":\"Fixture\",\"argsSchema\":\"input.json\"}]}",
        id,
        version,
        executor ? "\"executor\":{\"entry\":\"entry.lua\",\"requires\":[\"session.rpc\"],\"rpc\":["
                   "\"fixture.echo\"]}"
                 : "\"client\":{\"entry\":\"entry.lua\",\"requires\":[\"session.rpc\"]}",
        command);
    CHECK(length > 0 && (size_t)length < sizeof(manifest));
    const char lua[] = "return {['fixture.echo'] = function(input) return input end}";
    const char schema[] =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"maxLength\":40},\"lines\":{\"type\":\"integer\",\"minimum\":1,"
        "\"maximum\":100},\"wrap\":{\"type\":\"boolean\"}},"
        "\"required\":[\"path\",\"lines\",\"wrap\"],\"additionalProperties\":false,"
        "\"x-ghosterm-cli\":{\"version\":1,\"fields\":{\"path\":{"
        "\"position\":0},\"lines\":{\"option\":\"lines\",\"default\":10},"
        "\"wrap\":{\"option\":\"wrap\",\"default\":false}}}}";
    const char *const payload[] = {manifest, lua, schema};
    char integrity[1024] = "{\"schemaVersion\":1,\"members\":[";
    size_t at = strlen(integrity);
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char digest[65];
        write_member(f->fd, paths[i], payload[i], digest);
        const int size = snprintf(integrity + at,
                                  sizeof(integrity) - at,
                                  "%s{\"path\":\"%s\",\"bytes\":%zu,\"sha256\":\"%s\"}",
                                  i ? "," : "",
                                  paths[i],
                                  strlen(payload[i]),
                                  digest);
        CHECK(size > 0 && (size_t)size < sizeof(integrity) - at);
        at += (size_t)size;
    }
    CHECK(sizeof(integrity) - at > 2);
    memcpy(integrity + at, "]}", 3);
    write_member(f->fd, "integrity.json", integrity, f->digest);
}

static void fixture_close(fixture_t *f) {
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        CHECK(!unlinkat(f->fd, paths[i], 0));
    }
    CHECK(!unlinkat(f->fd, "integrity.json", 0) && !close(f->fd) && !rmdir(f->directory));
}

static void fixture_pair(rpc_pair_t *pair, const fixture_t *fixture, const char *mode) {
    int sockets[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    gt_ext_channel_t *host = ext_channel(sockets[0]), *executor = ext_channel(sockets[1]);
    gt_ext_session_config_t config = ext_config();
    config.features = GT_EXT_FEATURE_REQUESTS;
    strcpy(config.plugin_id, !strcmp(mode, "wrong-id") ? "org.ghosterm.other" : fixture_id);
    strcpy(config.plugin_version, !strcmp(mode, "wrong-version") ? "0.2.0" : "0.1.0");
    strcpy(config.client_digest, fixture->digest);
    strcpy(config.executor_digest, fixture->digest);
    if (!strcmp(mode, "mismatch")) {
        config.executor_digest[0] = config.executor_digest[0] == 'a' ? 'b' : 'a';
    }
    strcpy(config.sdk_version, "1.0.0");
    config.required_count = config.available_count = 1;
    strcpy(config.required[0], "session.rpc");
    strcpy(config.available[0], "session.rpc");
    const gt_ext_rpc_method_t method = {.method = "fixture.echo",
                                        .capability = "session.rpc",
                                        .receiver = GT_EXT_EXECUTOR,
                                        .input_shape = GT_EXT_JSON_OBJECT,
                                        .output_shape = GT_EXT_JSON_OBJECT};
    pair->pair.h = rpc_adopt(&host, GT_EXT_HOST, &config, &method, 1);
    pair->pair.e = rpc_adopt(&executor, GT_EXT_EXECUTOR, &config, &method, 1);
    ext_allow(&pair->pair);
    CHECK(gt_ext_session_lease(pair->pair.h, &pair->h));
}

#ifndef PUBLIC_PACKAGE_MAIN
#define PUBLIC_PACKAGE_MAIN main
#endif

int PUBLIC_PACKAGE_MAIN(int argc, char **argv) {
    CHECK(argc >= 2);
    const char *mode = argv[1];
    gt_ext_package_t *package = NULL;
    if (!strcmp(mode, "disabled")) {
        gt_ext_registry_t *registry = NULL;
        CHECK(gt_ext_registry_open(&registry) == GT_EXT_REGISTRY_UNSUPPORTED && !registry);
        CHECK(gt_ext_package_open(NULL, &package) == GT_ERR_UNSUPPORTED && !package);
        gt_ext_runtime_t *r = NULL;
        CHECK(gt_ext_runtime_open_package(NULL, NULL, NULL, false, &r) == GT_ERR_UNSUPPORTED && !r);
        return 0;
    }
    CHECK(argc == 3);
    size_t fds = ext_fds();
    gt_plugin_memory_info_t before = gt__plugin_memory_info();
    fixture_t f;
    fixture_open(&f, argv[2], fixture_id, "0.1.0", "\"fixture\"", true);
    gt_ext_package_config_t capture = {f.fd, 1024, 4096, 16777216, 67108864};
    CHECK(gt_ext_package_open(&capture, &package) == GT_OK && package);
    gt_ext_package_metadata_t metadata;
    memset(&metadata, 0x55, sizeof(metadata));
    CHECK(!gt_ext_package_metadata(package, &metadata) && !metadata.id[0]);
    if (!strcmp(mode, "capture-stop")) {
        gt_ext_package_stop(package);
    }
    gt_ext_package_phase_t phase = GT_EXT_PACKAGE_CAPTURING;
    for (unsigned i = 0; i < 50000 && phase == GT_EXT_PACKAGE_CAPTURING; i++) {
        phase = gt_ext_package_step(package);
    }
    gt_ext_package_info_t info;
    CHECK(gt_ext_package_info(package, &info));
    if (!strcmp(mode, "capture-stop")) {
        CHECK(phase == GT_EXT_PACKAGE_FAILED && info.error == GT_EXT_PACKAGE_CANCELLED);
        goto done;
    }
    CHECK(phase == GT_EXT_PACKAGE_CAPTURED && info.members > 0 && info.bytes > 0);
    if (!strcmp(mode, "check-stop")) {
        gt_ext_package_stop(package);
    }
    const char *host = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                       "\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}";
    const bool association = !strcmp(mode, "association");
    gt_ext_package_check_config_t config = {.host_json = !strcmp(mode, "unknown") ? NULL : host,
                                            .registry_json = association ? "[]" : NULL};
    phase = gt_ext_package_check(package, &config);
    CHECK(gt_ext_package_info(package, &info));
    if (!strcmp(mode, "unknown")) {
        CHECK(phase == GT_EXT_PACKAGE_FAILED && info.error == GT_EXT_PACKAGE_UNKNOWN &&
              info.code[0]);
        goto done;
    }
    if (!strcmp(mode, "check-stop")) {
        CHECK(phase == GT_EXT_PACKAGE_FAILED && info.error == GT_EXT_PACKAGE_CANCELLED);
        goto done;
    }
    CHECK(phase == GT_EXT_PACKAGE_CHECKED && gt_ext_package_metadata(package, &metadata));
    CHECK(!strcmp(metadata.id, fixture_id) && !strcmp(metadata.package_digest, f.digest) &&
          metadata.requires_count == 1 && !metadata.authorization_granted &&
          !metadata.publisher_authenticated);
    if (!strcmp(mode, "cli-arguments")) {
        const char *arguments[] = {"report.txt", "--lines=20", "--wrap"};
        size_t size = 0;
        CHECK(gt_ext_package_decode_command_arguments(
                  package, "fixture", arguments, 3, NULL, 0, &size) == GT_OK &&
              size > 0);
        char decoded[256] = {0};
        CHECK(size < sizeof(decoded));
        CHECK(gt_ext_package_decode_command_arguments(
                  package, "fixture", arguments, 3, decoded, sizeof(decoded), &size) == GT_OK);
        CHECK(strstr(decoded, "\"path\":\"report.txt\"") && strstr(decoded, "\"lines\":20") &&
              strstr(decoded, "\"wrap\":true"));
        const char *invalid[] = {"report.txt", "--lines=01"};
        CHECK(gt_ext_package_decode_command_arguments(
                  package, "fixture", invalid, 2, decoded, sizeof(decoded), &size) ==
              GT_ERR_INVALID_ARG);
        goto done;
    }
    if (!strcmp(mode, "registry")) {
        uint32_t count = 0;
        gt_ext_package_command_t command;
        CHECK(gt_ext_package_command_count(package, &count) && count == 1);
        CHECK(gt_ext_package_command(package, 0, &command) && !strcmp(command.path, "fixture") &&
              !strcmp((char *)command.args_schema, "input.json"));
        memset(&command, 0x55, sizeof(command));
        CHECK(!gt_ext_package_command(package, 1, &command) && !command.path[0]);
        gt_ext_registry_t *registry = NULL;
        CHECK(gt_ext_registry_open(&registry) == GT_EXT_REGISTRY_OK);
        CHECK(gt_ext_registry_put(registry, package, GT_EXT_REGISTRY_SOURCE_UNKNOWN, NULL) ==
              GT_EXT_REGISTRY_UNKNOWN);
        CHECK(gt_ext_registry_put(registry, package, GT_EXT_REGISTRY_SOURCE_DENIED, NULL) ==
              GT_EXT_REGISTRY_DENIED);
        CHECK(gt_ext_registry_put(registry, package, GT_EXT_REGISTRY_SOURCE_ALLOWED, NULL) ==
              GT_EXT_REGISTRY_OK);
        gt_ext_package_stop(package);
        CHECK(gt_ext_registry_put(registry, package, GT_EXT_REGISTRY_SOURCE_ALLOWED, NULL) ==
              GT_EXT_REGISTRY_CANCELLED);
        gt_ext_package_t *selected = NULL;
        CHECK(gt_ext_registry_resolve(registry,
                                      "fixture",
                                      true,
                                      metadata.id,
                                      metadata.package_digest,
                                      &selected,
                                      &command) == GT_EXT_REGISTRY_OK);
        gt_ext_package_info_t selected_info;
        CHECK(gt_ext_package_info(selected, &selected_info) &&
              selected_info.members == info.members && selected_info.bytes == info.bytes);
        CHECK(gt_ext_registry_remove(registry, metadata.id, true, metadata.package_digest) ==
              GT_EXT_REGISTRY_OK);
        CHECK(gt_ext_registry_destroy(&registry) == GT_EXT_REGISTRY_OK && !registry);
        CHECK(gt_ext_package_destroy(&package) == GT_OK);
        package = selected;
    }
    rpc_pair_t pair = {0};
    fixture_pair(&pair, &f, mode);
    gt_ext_session_t *original = pair.pair.e;
    gt_ext_runtime_config_t runtime = {
        .role = GT_EXT_EXECUTOR, .cpu_ms = 1000, .activation_ms = 10000};
    gt_ext_runtime_t *r = NULL;
    if (!strcmp(mode, "process-config")) {
        const gt_ext_process_program_mapping_t program = {
            "codex",
            "/usr/bin/codex",
            "0.166.0",
            "0000000000000000000000000000000000000000000000000000000000000000",
            NULL,
            0};
        runtime.process_programs = &program;
        runtime.process_program_count = 1;
        CHECK(gt_ext_runtime_open_package(&pair.pair.e, package, &runtime, false, &r) ==
                  GT_ERR_UNSUPPORTED &&
              !r && pair.pair.e == original);
        rpc_drop(&pair);
        goto done;
    }
    if (!strcmp(mode, "override")) {
        runtime.entry = "plugin:/override.lua";
    }
    if (!strcmp(mode, "load-stop")) {
        gt_ext_package_stop(package);
    }
    const char *const arguments[] = {"report.txt"};
    gt_ext_invocation_t invocation = {.command = "fixture", .argv = arguments, .argc = 1};
    runtime.invocation = &invocation;
    if (!strcmp(mode, "registry")) {
        invocation.command = "missing";
        CHECK(gt_ext_runtime_open_package(&pair.pair.e, package, &runtime, false, &r) ==
                  GT_ERR_NOT_FOUND &&
              !r && pair.pair.e == original);
        invocation.command = "fixture";
    }
    if (association) {
        CHECK(gt_ext_runtime_open_package(&pair.pair.e, package, &runtime, false, &r) ==
                  GT_ERR_AUTH &&
              !r && pair.pair.e == original);
    }
    gt_status_t status =
        gt_ext_runtime_open_package(&pair.pair.e, package, &runtime, association, &r);
    if (!strcmp(mode, "override") || !strcmp(mode, "load-stop") || !strcmp(mode, "mismatch") ||
        !strcmp(mode, "wrong-id") || !strcmp(mode, "wrong-version")) {
        CHECK(status == (!strcmp(mode, "override")    ? GT_ERR_INVALID_ARG
                         : !strcmp(mode, "load-stop") ? GT_ERR_CANCELLED
                                                      : GT_ERR_PROTOCOL) &&
              !r && pair.pair.e == original);
        rpc_drop(&pair);
        goto done;
    }
    if (status != GT_OK) {
        fprintf(stderr, "opening Lua package: status=%d\n", status);
    }
    CHECK(status == GT_OK && r && !pair.pair.e);
    CHECK(gt_ext_package_destroy(&package) == GT_OK && !package);
    gt_ext_runtime_info_t ri;
    for (unsigned i = 0; i < 20000; i++) {
        gt_ext_session_step(pair.pair.h, 0);
        gt_ext_runtime_step(r);
        CHECK(gt_ext_runtime_info(r, &ri));
        if (ri.phase >= GT_EXT_RUNTIME_ACTIVE) {
            break;
        }
    }
    CHECK(ri.phase == GT_EXT_RUNTIME_ACTIVE);
    uint64_t request = rpc_start(&pair, false, "{\"text\":\"public\",\"count\":17}", 1000);
    gt_ext_rpc_result_t result = {0};
    for (unsigned i = 0; i < 2000; i++) {
        gt_ext_session_step(pair.pair.h, 0);
        CHECK(gt_ext_runtime_step(r) == GT_EXT_RUNTIME_ACTIVE);
        result = rpc_result(pair.pair.h, &pair.h, request);
        if (result.confirmed) {
            break;
        }
    }
    CHECK(result.confirmed && result.state == GT_EXT_RPC_SUCCEEDED &&
          strstr(result.outcome, "public"));
    CHECK(gt_ext_session_forget_request(pair.pair.h, &pair.h, request) == GT_EXT_RPC_OK);
    gt_ext_close_t closed = GT_EXT_DRAINING;
    for (unsigned i = 0; i < 200 && closed == GT_EXT_DRAINING; i++) {
        closed = gt_ext_runtime_close(r);
    }
    CHECK(closed == GT_EXT_RELEASED && gt_ext_runtime_destroy(&r) == GT_OK && !r);
    ext_drop(&pair.pair.h);
done:
    if (package) {
        CHECK(gt_ext_package_destroy(&package) == GT_OK && !package);
    }
    fixture_close(&f);
    CHECK(ext_fds() == fds && gt__plugin_memory_info().used == before.used);
    return 0;
}
