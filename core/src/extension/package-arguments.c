#include "ghosterm/extension_package.h"
#include "extension/package-owner.h"
#include "extension/package-runtime.h"
#include "extension/memory.h"
#include <string.h>

#if defined(GT_HAVE_PLUGIN_LUA)
#include "extension/package-arguments-lua.inc"
#endif

gt_status_t gt__package_decode_arguments(const gt_package_checked_t *p,
                                         const char *command,
                                         const char *const *argv,
                                         size_t argc,
                                         char *output,
                                         size_t capacity,
                                         size_t *size) {
    if (size) {
        *size = 0;
    }
    if (!p || !command || !size || (!output && capacity) || !strnlen(command, 520) ||
        strnlen(command, 520) >= 520) {
        return GT_ERR_INVALID_ARG;
    }
#if defined(GT_HAVE_PLUGIN_LUA)
    return decode_checked(p, command, argv, argc, output, capacity, size);
#else
    (void)argv;
    (void)argc;
    return GT_ERR_UNSUPPORTED;
#endif
}

gt_status_t gt_ext_package_decode_command_arguments(const gt_ext_package_t *p,
                                                    const char *command,
                                                    const char *const *argv,
                                                    size_t argc,
                                                    char *output,
                                                    size_t capacity,
                                                    size_t *size) {
    if (size) {
        *size = 0;
    }
    if (!p || !command || !size || (!output && capacity) ||
        p->info.phase != GT_EXT_PACKAGE_CHECKED) {
        return GT_ERR_INVALID_ARG;
    }
#if !defined(GT_HAVE_PLUGIN_LUA)
    (void)argv;
    (void)argc;
    return GT_ERR_UNSUPPORTED;
#else
    size_t n = strnlen(command, 520);
    if (!n || n >= 520) {
        return GT_ERR_INVALID_ARG;
    }
    return gt__package_decode_arguments(p->checked, command, argv, argc, output, capacity, size);
#endif
}
