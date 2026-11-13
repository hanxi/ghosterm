/* Real engine/runtime. A controlled native wait during an interrupt callback
 * distinguishes thread CPU consumption from wall-clock activation lifetime. */
#define _POSIX_C_SOURCE 200809L
#include "extension/runtime.h"
#include "base/sha256.h"
#include <quickjs.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool wall_mode, wait_mode, injected;
static uint64_t expired_step;
static JSInterruptHandler *original_interrupt;
static void clock_fixture_handler(JSRuntime *, JSInterruptHandler *, void *);
static gt_status_t
clock_fixture_open(gt_ext_session_t **, const gt_runtime_config_t *, gt_bridge_runtime_t **);
static gt_runtime_phase_t clock_fixture_step(gt_bridge_runtime_t *);
#define JS_SetInterruptHandler clock_fixture_handler
#define gt__bridge_runtime_open clock_fixture_open
#define gt__bridge_runtime_step clock_fixture_step
#include "../src/extension/runtime.c"
#undef JS_SetInterruptHandler
#undef gt__bridge_runtime_open
#undef gt__bridge_runtime_step

static int clock_fixture_interrupt(JSContext *context, void *opaque) {
    gt_bridge_runtime_t *runtime = opaque;
    if (wait_mode && !injected && runtime->deadline && !atomic_load(&runtime->stopped)) {
        injected = true;
        struct timespec pause = {.tv_nsec = 700000000};
        while (nanosleep(&pause, &pause) && errno == EINTR) {
        }
    }
    return original_interrupt(context, opaque);
}

static void clock_fixture_handler(JSRuntime *engine, JSInterruptHandler *callback, void *opaque) {
    original_interrupt = callback;
    JS_SetInterruptHandler(engine, clock_fixture_interrupt, opaque);
}

gt_status_t gt__bridge_runtime_open(gt_ext_session_t **session,
                                    const gt_runtime_config_t *config,
                                    gt_bridge_runtime_t **out) {
    gt_runtime_config_t copy = *config;
    if (wall_mode) {
        copy.activation_ms = 200;
    }
    gt_status_t status = clock_fixture_open(session, &copy, out);
    return status;
}

gt_runtime_phase_t gt__bridge_runtime_step(gt_bridge_runtime_t *runtime) {
    gt_runtime_phase_t phase = clock_fixture_step(runtime);
    if (runtime && runtime->info.error == GT_RUNTIME_EXPIRED) {
        expired_step = runtime->info.steps;
    }
    return phase;
}

#define GT_RUNTIME_MAIN clock_fixture_original_main
#include "test_runtime.c"

int main(int argc, char **argv) {
    CHECK(argc == 3);
    size_t before = ext_fds();
    wall_mode = !strcmp(argv[1], "wall");
    wait_mode = wall_mode || !strcmp(argv[1], "wait");
    const char *mode = wall_mode                  ? "activation-timeout"
                       : !strcmp(argv[1], "wait") ? "roundtrip"
                                                  : argv[1];
    runtime_case(mode, argv[2]);
    CHECK(!wait_mode || injected);
    CHECK(!wall_mode || (expired_step > 0 && expired_step <= 2));
    CHECK(ext_fds() == before && !gt__plugin_memory_info().used);
    puts("PASS runtime_cpu_clock");
    return 0;
}
