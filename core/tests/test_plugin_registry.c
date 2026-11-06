#define PACKAGE_RUNTIME_MAIN package_loader_fixture_main
#include "test_package_runtime.c"
#undef PACKAGE_RUNTIME_MAIN
#include "extension/registry.h"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    size_t fds = ext_fds();
    gt_plugin_memory_info_t before = gt__plugin_memory_info();
    load_fixture_t first, other, replacement;
    load_fixture(&first, "command-boundary", argv[1]);
    load_clear(&first);
    load_fixture(&first, "command-many", argv[1]);
    load_clear(&first);
    load_fixture(&first, "roundtrip", argv[1]);
    load_fixture(&other, "registry-other", argv[1]);
    load_fixture(&replacement, "relative", argv[1]);
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
    load_clear(&first);
    load_clear(&other);
    load_clear(&replacement);
    /* Prefix conflict is token-aware; an ancestor is never an exact command. */
    load_fixture(&first, "roundtrip", argv[1]);
    load_fixture(&other, "registry-prefix", argv[1]);
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
    load_clear(&other);
    load_fixture(&other, "registry-neighbor", argv[1]);
    load_fixture(&replacement, "client", argv[1]);
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
    load_clear(&first);
    load_clear(&other);
    load_clear(&replacement);
    /* Bounded slot table, failed admission leaves existing selections intact. */
    load_fixture_t slots[GT_PLUGIN_REGISTRY_MAX + 1];
    CHECK(gt__registry_open(&r) == GT_REGISTRY_OK);
    for (size_t i = 0; i < GT_PLUGIN_REGISTRY_MAX + 1; i++) {
        char mode[64];
        CHECK(snprintf(mode, sizeof(mode), "registry-slot-%zu", i) > 0);
        load_fixture(&slots[i], mode, argv[1]);
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
        load_clear(&slots[i]);
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
