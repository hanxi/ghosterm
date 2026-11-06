#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include "stdio.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* No terminal mode changes, network, installation, or business execution here.
 * SIGINT/TERM only set a flag: Core cancellation is not async-signal-safe. */
static volatile sig_atomic_t interrupted;
static void interrupt_check(int number) { interrupted = number; }
static int usage(void) {
    fputs("usage: gterm doctor\n"
          "       gterm plugin resolve --host-json FILE [--select ID DIGEST] -- COMMAND DIRECTORY DIGEST [DIRECTORY DIGEST]...\n"
          "       gterm plugin check [--host-json FILE] -- DIRECTORY\n"
          "       gterm relay --stdio --trust-development-digest SHA256 --client-digest SHA256 [--method NAME]... -- DIRECTORY COMMAND [ARG...]\n"
          "Host JSON is caller-supplied compatibility evidence, not a grant.\n", stderr);
    return 2;
}
/* JSON escapes all non-ASCII scalars, including terminal/bidi controls. Inputs
 * below are already validated UTF-8 by Core; ASCII paths never get interpolated. */
static void json_string(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    putchar('"');
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0x80) {
            unsigned n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            c &= n == 1 ? 0x1f : n == 2 ? 0x0f : 0x07;
            for (unsigned i = 0; i < n; i++) c = (c << 6) | (*p++ & 0x3f);
        }
        if (c == '"' || c == '\\') { putchar('\\'); putchar((int)c); }
        else if (c < 32 || c >= 127) {
            if (c <= 0xffff) printf("\\u%04x", (unsigned)c);
            else { c -= 0x10000; printf("\\u%04x\\u%04x", (unsigned)(0xd800 + (c >> 10)), (unsigned)(0xdc00 + (c & 1023))); }
        } else putchar((int)c);
    }
    putchar('"');
}
static int failure(const char *kind, const char *code, int exit_code) {
    fputs("{\"result\":", stdout); json_string(kind);
    fputs(",\"code\":", stdout); json_string(code);
    puts(",\"authorizationGranted\":false,\"publisherAuthenticated\":false}");
    return exit_code;
}
/* Refuse FIFOs/devices before reading; O_NONBLOCK avoids a FIFO open hang.
 * A bounded buffer + EOF probe rejects growth and embedded NUL. */
static char *read_host(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *text = NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 65536) goto done;
    text = malloc(65538);
    if (!text) goto done;
    size_t used = 0;
    while (used < 65537 && !interrupted) {
        ssize_t n = read(fd, text + used, 65537 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { free(text); text = NULL; goto done; }
        if (!n) break;
        used += (size_t)n;
    }
    if (interrupted || used > 65536 || memchr(text, 0, used)) { free(text); text = NULL; }
    else text[used] = 0;
done:
    if (close(fd)) { free(text); text = NULL; }
    return text;
}
static int package_check(const char *directory, const char *host_path) {
    if (gt_ext_runtime_support() != GT_OK) return failure("unsupported", "PLUGIN_JS_DISABLED", 4);
    char *host = host_path ? read_host(host_path) : NULL;
    if (host_path && !host) return failure(interrupted ? "cancelled" : "invalid", "HOST_INPUT", interrupted ? 130 : 2);
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { free(host); return failure("io", "PACKAGE_DIRECTORY", 5); }
    gt_ext_package_config_t capture = {fd, 1024, 4096, 16777216, 67108864};
    gt_ext_package_t *package = NULL;
    gt_status_t status = gt_ext_package_open(&capture, &package);
    int closed = close(fd);
    int result;
    if (status != GT_OK || closed) { result = failure("io", "PACKAGE_OPEN", 5); goto done; }
    gt_ext_package_phase_t phase;
    do {
        if (interrupted) gt_ext_package_stop(package);
        phase = gt_ext_package_step(package);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    if (phase == GT_EXT_PACKAGE_CAPTURED) {
        if (interrupted) gt_ext_package_stop(package);
        gt_ext_package_check_config_t check = {.host_json = host};
        phase = gt_ext_package_check(package, &check);
    }
    gt_ext_package_info_t info;
    if (!gt_ext_package_info(package, &info)) { result = failure("io", "PACKAGE_INFO", 5); goto done; }
    if (interrupted) { result = failure("cancelled", "INTERRUPTED", 130); goto done; }
    if (phase != GT_EXT_PACKAGE_CHECKED) {
        static const char *const errors[] = {"none", "invalid", "unknown", "unsupported", "denied", "limit", "memory", "cancelled", "io", "internal", "changed"};
        const char *kind = (unsigned)info.error < sizeof(errors) / sizeof(errors[0]) ? errors[info.error] : "internal";
        result = failure(kind, info.code, info.error == GT_EXT_PACKAGE_UNKNOWN ? 3 : info.error == GT_EXT_PACKAGE_UNSUPPORTED ? 4 : 5);
        goto done;
    }
    gt_ext_package_metadata_t metadata;
    uint32_t count;
    if (!gt_ext_package_metadata(package, &metadata) || !gt_ext_package_command_count(package, &count)) {
        result = failure("io", "PACKAGE_METADATA", 5); goto done;
    }
    /* Copy all observations before output, so failure never emits partial success. */
    gt_ext_package_command_t *commands = calloc(count ? count : 1, sizeof(*commands));
    if (!commands) { result = failure("memory", "COMMANDS", 5); goto done; }
    for (uint32_t i = 0; i < count; i++) if (!gt_ext_package_command(package, i, &commands[i])) {
        free(commands); result = failure("io", "PACKAGE_COMMAND", 5); goto done;
    }
    if (gt_ext_package_destroy(&package) != GT_OK) {
        free(commands); result = failure("io", "PACKAGE_CLOSE", 5); goto done;
    }
    fputs("{\"result\":\"compatible\",\"hostEvidence\":\"caller-supplied\",\"id\":", stdout); json_string(metadata.id);
    fputs(",\"version\":", stdout); json_string(metadata.version);
    fputs(",\"publisher\":", stdout); json_string(metadata.publisher);
    fputs(",\"packageDigest\":", stdout); json_string(metadata.package_digest);
    fputs(",\"authorizationGranted\":false,\"publisherAuthenticated\":false,\"commands\":[", stdout);
    for (uint32_t i = 0; i < count; i++) {
        if (i) putchar(',');
        fputs("{\"path\":", stdout); json_string(commands[i].path);
        fputs(",\"title\":", stdout); json_string(commands[i].title);
        fputs(",\"argsSchema\":", stdout); json_string((const char *)commands[i].args_schema);
        putchar('}');
    }
    puts("]}"); free(commands); result = 0;
done:
    free(host);
    if (package && gt_ext_package_destroy(&package) != GT_OK) {
        fputs("gterm: package reference cleanup failed\n", stderr); result = 5;
    }
    return result;
}
/* Explicit development inputs only: each directory has its own expected digest.
 * Resolving a command neither activates code nor grants Session capabilities. */
static int package_resolve(int argc, char **argv) {
    const char *host_path = NULL, *selected_id = NULL, *selected_digest = NULL;
    int at = 0;
    while (at < argc && strcmp(argv[at], "--")) {
        if (!strcmp(argv[at], "--host-json") && !host_path && at + 1 < argc) {
            host_path = argv[at + 1]; at += 2;
        } else if (!strcmp(argv[at], "--select") && !selected_id && at + 2 < argc) {
            selected_id = argv[at + 1]; selected_digest = argv[at + 2]; at += 3;
        } else return usage();
    }
    if (!host_path || at >= argc || argc - at < 4 || (argc - at - 2) % 2 ||
        (unsigned)(argc - at - 2) / 2 > GT_EXT_REGISTRY_PACKAGES_MAX) return usage();
    const char *command = argv[at + 1]; at += 2;
    if (gt_ext_runtime_support() != GT_OK) return failure("unsupported", "PLUGIN_JS_DISABLED", 4);
    char *host = read_host(host_path);
    if (!host) return failure(interrupted ? "cancelled" : "invalid", "HOST_INPUT", interrupted ? 130 : 2);
    gt_ext_registry_t *registry = NULL; gt_ext_package_t *package = NULL;
    gt_ext_registry_status_t registered = gt_ext_registry_open(&registry);
    const char *kind = "io", *code = "REGISTRY_OPEN"; int result = 5;
    gt_ext_package_metadata_t metadata; gt_ext_package_command_t contribution;
    if (registered != GT_EXT_REGISTRY_OK) goto done;
    for (; at < argc; at += 2) {
        if (interrupted) goto cancelled;
        const char *expected = argv[at + 1];
        bool valid = strlen(expected) == 64;
        for (size_t i = 0; valid && i < 64; i++) valid = (expected[i] >= '0' && expected[i] <= '9') || (expected[i] >= 'a' && expected[i] <= 'f');
        if (!valid) { kind = "invalid"; code = "DEVELOPMENT_DIGEST"; result = 2; goto done; }
        int fd = open(argv[at], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { code = "PACKAGE_DIRECTORY"; goto done; }
        gt_ext_package_config_t capture = {fd, 1024, 4096, 16777216, 67108864};
        gt_status_t status = gt_ext_package_open(&capture, &package); int closed = close(fd);
        if (status != GT_OK || closed) { code = "PACKAGE_OPEN"; goto done; }
        gt_ext_package_phase_t phase;
        do { if (interrupted) gt_ext_package_stop(package); phase = gt_ext_package_step(package); } while (phase == GT_EXT_PACKAGE_CAPTURING);
        if (interrupted) goto cancelled;
        gt_ext_package_check_config_t check = {.host_json = host};
        if (phase != GT_EXT_PACKAGE_CAPTURED || gt_ext_package_check(package, &check) != GT_EXT_PACKAGE_CHECKED) {
            gt_ext_package_info_t info; code = "PACKAGE_CHECK";
            if (gt_ext_package_info(package, &info)) {
                static const char *const errors[] = {"none", "invalid", "unknown", "unsupported", "denied", "limit", "memory", "cancelled", "io", "internal", "changed"};
                kind = (unsigned)info.error < sizeof(errors) / sizeof(errors[0]) ? errors[info.error] : "internal";
                result = info.error == GT_EXT_PACKAGE_UNKNOWN ? 3 : info.error == GT_EXT_PACKAGE_UNSUPPORTED ? 4 : info.error == GT_EXT_PACKAGE_DENIED ? 6 : info.error == GT_EXT_PACKAGE_CANCELLED ? 130 : 5;
            }
            goto done;
        }
        if (!gt_ext_package_metadata(package, &metadata)) { code = "PACKAGE_METADATA"; goto done; }
        if (strcmp(metadata.package_digest, expected)) { kind = "denied"; code = "DEVELOPMENT_DIGEST_CHANGED"; result = 6; goto done; }
        registered = gt_ext_registry_put(registry, package, GT_EXT_REGISTRY_SOURCE_ALLOWED, NULL);
        if (registered != GT_EXT_REGISTRY_OK) { code = "REGISTRY_ADD"; goto registry_error; }
        if (gt_ext_package_destroy(&package) != GT_OK) { code = "PACKAGE_CLOSE"; goto done; }
    }
    if (interrupted) goto cancelled;
    registered = gt_ext_registry_resolve(registry, command, true, selected_id, selected_digest, &package, &contribution);
    if (registered != GT_EXT_REGISTRY_OK) { code = "COMMAND_RESOLVE"; goto registry_error; }
    if (!gt_ext_package_metadata(package, &metadata)) { code = "PACKAGE_METADATA"; goto done; }
    result = 0; goto done;
registry_error:
    switch (registered) {
        case GT_EXT_REGISTRY_CONFLICT: kind = "conflict"; result = 8; break;
        case GT_EXT_REGISTRY_STALE: kind = "stale"; result = 9; break;
        case GT_EXT_REGISTRY_NOT_FOUND: kind = "not-found"; result = 10; break;
        case GT_EXT_REGISTRY_UNKNOWN: kind = "unknown"; result = 3; break;
        case GT_EXT_REGISTRY_DENIED: kind = "denied"; result = 6; break;
        case GT_EXT_REGISTRY_INVALID: kind = "invalid"; result = 2; break;
        case GT_EXT_REGISTRY_UNSUPPORTED: kind = "unsupported"; result = 4; break;
        default: break;
    }
    goto done;
cancelled:
    kind = "cancelled"; code = "INTERRUPTED"; result = 130;
done:
    free(host);
    if (package && gt_ext_package_destroy(&package) != GT_OK) { kind = "io"; code = "PACKAGE_CLOSE"; result = 5; }
    if (registry && gt_ext_registry_destroy(&registry) != GT_EXT_REGISTRY_OK) { kind = "io"; code = "REGISTRY_CLOSE"; result = 5; }
    if (interrupted && !result) { kind = "cancelled"; code = "INTERRUPTED"; result = 130; }
    if (result) return failure(kind, code, result);
    fputs("{\"result\":\"resolved\",\"hostEvidence\":\"caller-supplied\",\"id\":", stdout); json_string(metadata.id);
    fputs(",\"version\":", stdout); json_string(metadata.version);
    fputs(",\"packageDigest\":", stdout); json_string(metadata.package_digest);
    fputs(",\"command\":", stdout); json_string(contribution.path);
    fputs(",\"title\":", stdout); json_string(contribution.title);
    fputs(",\"argsSchema\":", stdout); json_string((const char *)contribution.args_schema);
    puts(",\"authorizationGranted\":false,\"publisherAuthenticated\":false}");
    return 0;
}

int main(int argc, char **argv) {
    struct sigaction action = {0};
    action.sa_handler = interrupt_check; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 5;
    int result;
    if (argc >= 3 && !strcmp(argv[1], "relay") && !strcmp(argv[2], "--stdio")) return gterm_stdio(argc - 3, argv + 3, &interrupted);
    if (argc == 2 && !strcmp(argv[1], "doctor")) {
        bool available = gt_ext_runtime_support() == GT_OK;
        printf("{\"portableJs\":\"%s\",\"commandActivation\":\"%s\",\"localIpc\":\"unsupported\"}\n", available ? "available" : "unsupported", available ? "explicit-stdio-only" : "unsupported");
        result = available ? 0 : 4;
    } else if (argc >= 3 && !strcmp(argv[1], "plugin") && !strcmp(argv[2], "resolve")) {
        result = package_resolve(argc - 3, argv + 3);
    } else if (argc >= 3 && !strcmp(argv[1], "plugin") && !strcmp(argv[2], "check")) {
        if (argc == 5 && !strcmp(argv[3], "--")) result = package_check(argv[4], NULL);
        else if (argc == 7 && !strcmp(argv[3], "--host-json") && !strcmp(argv[5], "--")) result = package_check(argv[6], argv[4]);
        else result = usage();
    } else if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--help"))) result = usage();
    else result = failure("unsupported", "COMMAND_ACTIVATION_UNAVAILABLE", 4);
    if (fflush(stdout) || ferror(stdout)) return 5;
    return result;
}
