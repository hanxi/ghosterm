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
    } else if (argc >= 3 && !strcmp(argv[1], "plugin") && !strcmp(argv[2], "check")) {
        if (argc == 5 && !strcmp(argv[3], "--")) result = package_check(argv[4], NULL);
        else if (argc == 7 && !strcmp(argv[3], "--host-json") && !strcmp(argv[5], "--")) result = package_check(argv[6], argv[4]);
        else result = usage();
    } else if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--help"))) result = usage();
    else result = failure("unsupported", "COMMAND_ACTIVATION_UNAVAILABLE", 4);
    if (fflush(stdout) || ferror(stdout)) return 5;
    return result;
}
