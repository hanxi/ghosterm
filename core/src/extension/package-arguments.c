#include "ghosterm/extension_package.h"
#include "extension/package-owner.h"
#include "extension/package-runtime.h"
#include "extension/memory.h"
#include <string.h>

#ifdef GT_HAVE_PLUGIN_JS
#include <quickjs.h>
#include "extension/package-cli-decoder.h"

static gt_status_t decode_checked(const gt_package_checked_t *checked,
                                  const char *command_path,
                                  const char *const *argv,
                                  size_t argc,
                                  char *output,
                                  size_t capacity,
                                  size_t *size) {
    gt_package_command_t command;
    bool found = false;
    for (size_t i = 0; i < gt__package_checked_command_count(checked); i++) {
        if (gt__package_checked_command(checked, i, &command) &&
            !strcmp(command.path, command_path)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return GT_ERR_NOT_FOUND;
    }
    if (argc > 32 || (argc && !argv)) {
        return GT_ERR_INVALID_ARG;
    }
    size_t total = 0;
    for (size_t i = 0; i < argc; i++) {
        if (!argv[i]) {
            return GT_ERR_INVALID_ARG;
        }
        size_t n = strnlen(argv[i], 4097);
        if (n > 4096 || n + 1 > 16384 - total || !gt__runtime_source_utf8(argv[i], n)) {
            return GT_ERR_INVALID_ARG;
        }
        total += n + 1;
    }
    if (!command.args_schema[0]) {
        if (argc) {
            return GT_ERR_INVALID_ARG;
        }
        *size = 0;
        return output || !capacity ? GT_OK : GT_ERR_INVALID_ARG;
    }
    gt_package_member_t member;
    const uint8_t *schema_bytes = NULL;
    size_t schema_size = 0;
    if (!gt__package_checked_member(
            checked, command.args_schema, &member, &schema_bytes, &schema_size) ||
        member.bytes != schema_size || !schema_size || schema_size > 65536) {
        return GT_ERR_NOT_FOUND;
    }
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) {
        return GT_ERR_IO;
    }
    JS_SetMemoryLimit(runtime, 4 * 1024 * 1024);
    JS_SetMaxStackSize(runtime, 512 * 1024);
    JSContext *context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        return GT_ERR_IO;
    }
    /* Checked members are exact byte buffers. QuickJS requires a readable NUL
     * at buf[len], so never borrow the next snapshot byte as a terminator. */
    char *schema_json = gt__plugin_memory_alloc(NULL, schema_size + 1);
    if (!schema_json) {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        return GT_ERR_IO;
    }
    memcpy(schema_json, schema_bytes, schema_size);
    schema_json[schema_size] = 0;
    JSValue schema = JS_ParseJSON(context, schema_json, schema_size, "checked-argument-schema");
    gt__plugin_memory_free(NULL, schema_json, schema_size + 1);
    JSValue arguments = JS_NewArray(context);
    bool ok = !JS_IsException(schema) && !JS_IsException(arguments);
    for (size_t i = 0; i < argc && ok; i++) {
        size_t n = strlen(argv[i]);
        JSValue value = JS_NewStringLen(context, argv[i], n);
        if (JS_IsException(value) ||
            JS_SetPropertyUint32(context, arguments, (uint32_t)i, value) < 0) {
            ok = false;
        }
    }
    JSValue decoder = JS_UNDEFINED, decoded = JS_UNDEFINED, json = JS_UNDEFINED;
    if (ok) {
        decoder = JS_Eval(context,
                          gt__package_cli_decoder,
                          sizeof(gt__package_cli_decoder) - 1,
                          "host:/cli-arguments.js",
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
        ok = !JS_IsException(decoder);
    }
    if (ok) {
        JSValue values[2] = {JS_DupValue(context, schema), JS_DupValue(context, arguments)};
        decoded = JS_Call(context, decoder, JS_UNDEFINED, 2, values);
        JS_FreeValue(context, values[0]);
        JS_FreeValue(context, values[1]);
        ok = !JS_IsException(decoded);
    }
    if (ok && !JS_IsUndefined(decoded)) {
        json = JS_JSONStringify(context, decoded, JS_UNDEFINED, JS_UNDEFINED);
        ok = !JS_IsException(json) && !JS_IsUndefined(json);
    }
    if (ok && JS_IsUndefined(decoded)) {
        *size = 0;
        if (!output && capacity) {
            ok = false;
        }
    } else if (ok) {
        size_t json_size = 0;
        const char *text = JS_ToCStringLen(context, &json_size, json);
        if (!text) {
            ok = false;
        } else {
            *size = json_size;
            if (output) {
                if (capacity < json_size) {
                    ok = false;
                } else {
                    memcpy(output, text, json_size);
                }
            } else if (capacity) {
                ok = false;
            }
            JS_FreeCString(context, text);
        }
    }
    JS_FreeValue(context, json);
    JS_FreeValue(context, decoded);
    JS_FreeValue(context, decoder);
    JS_FreeValue(context, arguments);
    JS_FreeValue(context, schema);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return ok ? GT_OK : GT_ERR_INVALID_ARG;
}
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
#ifdef GT_HAVE_PLUGIN_JS
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
#ifndef GT_HAVE_PLUGIN_JS
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
