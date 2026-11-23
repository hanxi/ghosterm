#define PUBLIC_PACKAGE_MAIN public_package_fixture_main
#include "test_ext_package.c"
#include "extension/registry.h"

typedef struct {
    fixture_t files;
    gt_package_checked_t *package;
    gt_package_check_info_t info;
} registry_fixture_t;

static void open_registry_fixture(registry_fixture_t *fixture,
                                  const char *id,
                                  const char *version,
                                  const char *command,
                                  bool executor,
                                  const char *temporary) {
    fixture_open(&fixture->files, temporary, id, version, command, executor);
    const gt_package_capture_config_t capture = {fixture->files.fd, 32, 64, 65536, 262144};
    gt_package_snapshot_t *snapshot = NULL;
    CHECK(gt__package_capture_open(&capture, NULL, &snapshot) == GT_PACKAGE_OK);
    gt_package_status_t state = GT_PACKAGE_WAIT;
    for (size_t i = 0; i < 50000 && state == GT_PACKAGE_WAIT; i++) {
        state = gt__package_capture_step(snapshot);
    }
    CHECK(state == GT_PACKAGE_OK);
    char facts[192];
    const int size = snprintf(facts,
                              sizeof(facts),
                              "{\"side\":\"%s\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],"
                              "\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}",
                              executor ? "executor" : "client");
    CHECK(size > 0 && (size_t)size < sizeof(facts));
    const gt_package_check_config_t config = {.host_json = facts, .host_size = (size_t)size};
    gt_package_check_report_t report;
    CHECK(gt__package_check(&snapshot, &config, NULL, &fixture->package, &report) ==
              GT_PACKAGE_CHECK_COMPATIBLE &&
          !snapshot);
    CHECK(gt__package_checked_info(fixture->package, &fixture->info));
    CHECK(!strcmp(fixture->info.id, id) &&
          !strcmp(fixture->info.package_digest, fixture->files.digest));
}

static void close_registry_fixture(registry_fixture_t *fixture) {
    CHECK(gt__package_checked_destroy(&fixture->package) == GT_PACKAGE_OK && !fixture->package);
    fixture_close(&fixture->files);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    size_t fds = ext_fds();
    gt_plugin_memory_info_t before = gt__plugin_memory_info();
    registry_fixture_t first, other, replacement;
    open_registry_fixture(&first, fixture_id, "0.1.0", "\"fixture\"", true, argv[1]);
    open_registry_fixture(&other, "org.ghosterm.other", "0.1.0", "\"fixture\"", true, argv[1]);
    open_registry_fixture(&replacement, fixture_id, "0.2.0", "\"fixture\"", true, argv[1]);
    gt_plugin_registry_t *r = NULL;
    CHECK(gt__registry_open(&r) == GT_REGISTRY_OK);
    gt_package_checked_t *selected = NULL;
    gt_package_command_t command;
    CHECK(gt__registry_put(r, first.package, GT_REGISTRY_SOURCE_UNKNOWN, NULL) ==
          GT_REGISTRY_UNKNOWN);
    CHECK(gt__registry_put(r, first.package, GT_REGISTRY_SOURCE_DENIED, NULL) ==
          GT_REGISTRY_DENIED);
    CHECK(gt__registry_resolve(r, "fixture", true, NULL, NULL, &selected, &command) ==
              GT_REGISTRY_NOT_FOUND &&
          !selected);
    CHECK(gt__registry_put(r, first.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) == GT_REGISTRY_OK);
    CHECK(gt__registry_put(r, first.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) ==
          GT_REGISTRY_CONFLICT);
    CHECK(gt__registry_put(r, other.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) == GT_REGISTRY_OK);
    CHECK(gt__registry_resolve(r, "fixture", true, NULL, NULL, &selected, &command) ==
              GT_REGISTRY_CONFLICT &&
          !selected && !command.path[0]);
    CHECK(gt__registry_resolve(
              r, "fixture", true, first.info.id, first.info.package_digest, &selected, &command) ==
              GT_REGISTRY_OK &&
          selected);
    CHECK(gt__registry_put(
              r, replacement.package, GT_REGISTRY_SOURCE_ALLOWED, other.info.package_digest) ==
          GT_REGISTRY_STALE);
    CHECK(gt__registry_put(
              r, replacement.package, GT_REGISTRY_SOURCE_ALLOWED, first.info.package_digest) ==
          GT_REGISTRY_OK);
    gt_package_checked_t *newer = NULL;
    CHECK(gt__registry_resolve(
              r, "fixture", true, first.info.id, first.info.package_digest, &newer, &command) ==
              GT_REGISTRY_STALE &&
          !newer);
    CHECK(gt__registry_resolve(r,
                               "fixture",
                               true,
                               replacement.info.id,
                               replacement.info.package_digest,
                               &newer,
                               &command) == GT_REGISTRY_OK);
    CHECK(gt__package_checked_destroy(&newer) == GT_PACKAGE_OK);
    CHECK(gt__registry_remove(r, first.info.id, true, first.info.package_digest) ==
          GT_REGISTRY_STALE);
    CHECK(gt__registry_remove(r, replacement.info.id, true, replacement.info.package_digest) ==
          GT_REGISTRY_OK);
    CHECK(gt__registry_destroy(&r) == GT_REGISTRY_OK && !r);
    gt_package_check_info_t saved;
    CHECK(gt__package_checked_info(selected, &saved) &&
          !strcmp(saved.package_digest, first.info.package_digest));
    CHECK(gt__package_checked_destroy(&selected) == GT_PACKAGE_OK);
    close_registry_fixture(&first);
    close_registry_fixture(&other);
    close_registry_fixture(&replacement);
    /* Prefix conflict is token-aware; an ancestor is never an exact command. */
    open_registry_fixture(&first, fixture_id, "0.1.0", "\"fixture\"", true, argv[1]);
    open_registry_fixture(
        &other, "org.ghosterm.other", "0.1.0", "\"fixture\",\"child\"", true, argv[1]);
    CHECK(gt__registry_open(&r) == GT_REGISTRY_OK);
    CHECK(gt__registry_put(r, first.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) == GT_REGISTRY_OK);
    CHECK(gt__registry_put(r, other.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) == GT_REGISTRY_OK);
    CHECK(gt__registry_resolve(r, "fixture", true, NULL, NULL, &selected, &command) ==
          GT_REGISTRY_CONFLICT);
    CHECK(gt__registry_resolve(r, "fixture child", true, NULL, NULL, &selected, &command) ==
          GT_REGISTRY_CONFLICT);
    CHECK(
        gt__registry_resolve(r, "fixture child argument", true, NULL, NULL, &selected, &command) ==
        GT_REGISTRY_NOT_FOUND);
    CHECK(gt__registry_resolve(r,
                               "fixture child",
                               true,
                               other.info.id,
                               other.info.package_digest,
                               &selected,
                               &command) == GT_REGISTRY_OK);
    CHECK(!strcmp(command.path, "fixture child") &&
          gt__package_checked_destroy(&selected) == GT_PACKAGE_OK);
    const char *invalid[] = {"",
                             " fixture",
                             "fixture ",
                             "fixture  child",
                             "Fixture",
                             "fixture/child",
                             "fixture\tchild",
                             "a b c d e f g h i"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(gt__registry_resolve(r, invalid[i], true, NULL, NULL, &selected, &command) ==
                  GT_REGISTRY_INVALID &&
              !selected);
    }
    CHECK(gt__registry_destroy(&r) == GT_REGISTRY_OK);
    close_registry_fixture(&other);
    open_registry_fixture(&other, "org.ghosterm.other", "0.1.0", "\"fixturex\"", true, argv[1]);
    open_registry_fixture(&replacement, fixture_id, "0.1.0", "\"fixture\"", false, argv[1]);
    CHECK(gt__registry_open(&r) == GT_REGISTRY_OK);
    CHECK(gt__registry_put(r, first.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) == GT_REGISTRY_OK);
    CHECK(gt__registry_put(r, other.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) == GT_REGISTRY_OK);
    CHECK(gt__registry_put(r, replacement.package, GT_REGISTRY_SOURCE_ALLOWED, NULL) ==
          GT_REGISTRY_OK);
    CHECK(gt__registry_resolve(r, "fixture", true, NULL, NULL, &selected, &command) ==
          GT_REGISTRY_OK);
    CHECK(gt__package_checked_info(selected, &saved) && saved.executor);
    CHECK(gt__package_checked_destroy(&selected) == GT_PACKAGE_OK);
    CHECK(gt__registry_resolve(r, "fixture", false, NULL, NULL, &selected, &command) ==
          GT_REGISTRY_OK);
    CHECK(gt__package_checked_info(selected, &saved) && !saved.executor);
    CHECK(gt__package_checked_destroy(&selected) == GT_PACKAGE_OK);
    CHECK(gt__registry_resolve(r, "fixturex", true, NULL, NULL, &selected, &command) ==
          GT_REGISTRY_OK);
    CHECK(gt__package_checked_destroy(&selected) == GT_PACKAGE_OK);
    CHECK(gt__registry_destroy(&r) == GT_REGISTRY_OK);
    close_registry_fixture(&first);
    close_registry_fixture(&other);
    close_registry_fixture(&replacement);
    /* Bounded slot table, failed admission leaves existing selections intact. */
    registry_fixture_t slots[GT_PLUGIN_REGISTRY_MAX + 1];
    CHECK(gt__registry_open(&r) == GT_REGISTRY_OK);
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX + 1; i++) {
        char id[64];
        CHECK(snprintf(id, sizeof(id), "org.ghosterm.slot-%zu", i) > 0);
        open_registry_fixture(&slots[i], id, "0.1.0", "\"fixture\"", true, argv[1]);
        CHECK(gt__registry_put(r, slots[i].package, GT_REGISTRY_SOURCE_ALLOWED, NULL) ==
              (i < GT_PLUGIN_REGISTRY_MAX ? GT_REGISTRY_OK : GT_REGISTRY_LIMIT));
    }
    CHECK(gt__registry_resolve(r,
                               "fixture",
                               true,
                               slots[0].info.id,
                               slots[0].info.package_digest,
                               &selected,
                               &command) == GT_REGISTRY_OK);
    CHECK(gt__package_checked_destroy(&selected) == GT_PACKAGE_OK);
    CHECK(gt__registry_destroy(&r) == GT_REGISTRY_OK);
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX + 1; i++) {
        close_registry_fixture(&slots[i]);
    }
    gt_plugin_memory_info_t memory = gt__plugin_memory_info();
    size_t reserve = memory.limit - memory.used;
    void *held = gt__plugin_memory_alloc(NULL, reserve);
    CHECK(held);
    CHECK(gt__registry_open(&r) == GT_REGISTRY_MEMORY && !r);
    gt__plugin_memory_free(NULL, held, reserve);
    CHECK(ext_fds() == fds && gt__plugin_memory_info().used == before.used);
    return 0;
}
