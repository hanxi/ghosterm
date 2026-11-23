#include "extension/runtime.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "disabled")) {
        return 1;
    }
    gt_bridge_runtime_t *runtime = NULL;
    if (gt__bridge_runtime_open_package(NULL, NULL, NULL, false, &runtime) != GT_ERR_UNSUPPORTED ||
        runtime) {
        fputs("FAIL disabled package runtime admission\n", stderr);
        return 1;
    }
    return 0;
}
