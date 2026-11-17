/* Native bookkeeping fault fixture. Only its injected update callback fails;
 * no live app/surface or user configuration is touched by this probe. */
#include "../../apple/GhostermTerminal/Sources/CGhostty/shim.c"

static int update_calls;
static bool restoration_succeeds;

static bool controlled_update(ghostty_app_t app, ghostty_config_t config) {
    (void)app;
    (void)config;
    update_calls++;
    return update_calls == 2 && restoration_succeeds;
}

int main(void) {
    gt_ghostty_app_t app = {.app = (void *)(uintptr_t)1, .config = (void *)(uintptr_t)2};
    gt_ghostty_app_t other = {.app = (void *)(uintptr_t)4, .config = (void *)(uintptr_t)5};
    gt_ghostty_prepared_config_t candidate = {.owner = &app, .config = (void *)(uintptr_t)3};
    int invalid = apply_config_with(&other, &candidate, controlled_update);
    bool owner_guard = invalid == -1 && update_calls == 0;
    restoration_succeeds = true;
    int restored = apply_config_with(&app, &candidate, controlled_update);
    bool restored_recorded = restored == 1 && update_calls == 2 &&
                             app.config == (void *)(uintptr_t)2 &&
                             candidate.config == (void *)(uintptr_t)3;
    update_calls = 0;
    restoration_succeeds = false;
    int unknown = apply_config_with(&app, &candidate, controlled_update);
    bool unknown_recorded =
        unknown == 2 && update_calls == 2 && strstr(gt_ghostty_last_error(), "未确认") != NULL &&
        app.config == (void *)(uintptr_t)2 && candidate.config == (void *)(uintptr_t)3;
    if (!owner_guard || !restored_recorded || !unknown_recorded) {
        fprintf(stderr,
                "FAIL native config apply bookkeeping: owner=%d restored=%d unknown=%d\n",
                owner_guard,
                restored_recorded,
                unknown_recorded);
        return 1;
    }
    puts("PASS native config apply bookkeeping: invalid owner, checked restoration, unknown "
         "restoration");
    return 0;
}
