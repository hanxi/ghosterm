#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "release.h"
#include "catalog.h"
#include "stdio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef GTERM_HAVE_RELEASE
#include <openssl/evp.h>
#include <quickjs.h>
static const unsigned char policy_code[] = {
#include "gterm-release-policy.inc"
    0};
#endif

static bool stable(const struct stat *a, const struct stat *b) {
    bool common = a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
                  a->st_size == b->st_size && a->st_uid == b->st_uid && a->st_nlink == b->st_nlink;
#ifdef __APPLE__
    return common && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return common && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

unsigned char *gterm_release_file(const char *path, size_t limit, size_t *size) {
    *size = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat before, after, current;
    unsigned char *bytes = NULL;
    if (fd < 0 || fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size < 1 || (uint64_t)before.st_size > limit) {
        goto done;
    }
    size_t n = (size_t)before.st_size, at = 0;
    bytes = malloc(n + 1);
    if (!bytes) {
        goto done;
    }
    while (at <= n) {
        ssize_t got = read(fd, bytes + at, n + 1 - at);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 || (got > 0 && (size_t)got > n - at)) {
            goto failed;
        }
        if (!got) {
            break;
        }
        at += (size_t)got;
    }
    if (at != n || fstat(fd, &after) || lstat(path, &current) || !stable(&before, &after) ||
        !stable(&before, &current)) {
        goto failed;
    }
    bytes[n] = 0;
    *size = n;
    goto done;
failed:
    free(bytes);
    bytes = NULL;
done:
    if (fd >= 0 && close(fd)) {
        free(bytes);
        bytes = NULL;
        *size = 0;
    }
    return bytes;
}

void gterm_release_clear(gterm_release_t *release) {
    free(release->descriptor);
    free(release->signature);
    memset(release, 0, sizeof(*release));
}

bool gterm_release_supported(void) {
#ifdef GTERM_HAVE_RELEASE
    return true;
#else
    return false;
#endif
}

bool gterm_release_hash(const void *bytes, size_t size, char hex[65]) {
#ifdef GTERM_HAVE_RELEASE
    unsigned char raw[32];
    unsigned count = 0;
    if (!EVP_Digest(bytes, size, raw, &count, EVP_sha256(), NULL) || count != 32) {
        return false;
    }
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        hex[i * 2] = alphabet[raw[i] >> 4];
        hex[i * 2 + 1] = alphabet[raw[i] & 15];
    }
    hex[64] = 0;
    return true;
#else
    (void)bytes;
    (void)size;
    (void)hex;
    return false;
#endif
}

#ifdef GTERM_HAVE_RELEASE
typedef struct {
    uint64_t end;
    volatile sig_atomic_t *cancelled;
} budget_t;

static uint64_t clock_ms(void) {
    struct timespec clock;
    return clock_gettime(CLOCK_MONOTONIC, &clock)
               ? UINT64_MAX
               : (uint64_t)clock.tv_sec * 1000 + (uint64_t)clock.tv_nsec / 1000000;
}

static int interrupt(JSContext *runtime, void *opaque) {
    (void)runtime;
    budget_t *budget = opaque;
    return *budget->cancelled || clock_ms() >= budget->end;
}

static bool unhex(const char *hex, unsigned char raw[32]) {
    if (!gterm_catalog_digest(hex)) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        unsigned a = (unsigned)(hex[i * 2] <= '9' ? hex[i * 2] - '0' : hex[i * 2] - 'a' + 10);
        unsigned b =
            (unsigned)(hex[i * 2 + 1] <= '9' ? hex[i * 2 + 1] - '0' : hex[i * 2 + 1] - 'a' + 10);
        raw[i] = (unsigned char)(a * 16 + b);
    }
    return true;
}

static JSValue key_hash(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const char *hex = argc == 1 ? JS_ToCString(context, argv[0]) : NULL;
    unsigned char raw[32];
    char digest[65];
    bool ok = hex && unhex(hex, raw) && gterm_release_hash(raw, sizeof(raw), digest);
    JS_FreeCString(context, hex);
    return ok ? JS_NewString(context, digest) : JS_ThrowInternalError(context, "RELEASE_CRYPTO");
}

static bool
field(JSContext *context, JSValueConst object, const char *name, char *out, size_t capacity) {
    JSValue value = JS_GetPropertyStr(context, object, name);
    size_t size = 0;
    const char *text = JS_IsString(value) ? JS_ToCStringLen(context, &size, value) : NULL;
    bool ok = text && size && size < capacity && !memchr(text, 0, size);
    if (ok) {
        memcpy(out, text, size + 1);
    }
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);
    return ok;
}
#endif

static int authenticate(const char *descriptor_path,
                        const char *signature_path,
                        const char *policy_path,
                        const char *expected_policy,
                        const char *expected_descriptor,
                        const char *expected_signature,
                        gterm_release_t *out,
                        volatile sig_atomic_t *cancelled) {
    memset(out, 0, sizeof(*out));
#ifndef GTERM_HAVE_RELEASE
    (void)descriptor_path;
    (void)signature_path;
    (void)policy_path;
    (void)expected_policy;
    (void)expected_descriptor;
    (void)expected_signature;
    (void)cancelled;
    return 4;
#else
    size_t size = 0, signature_size = 0;
    unsigned char *policy = gterm_release_file(policy_path, 131072, &size);
    out->descriptor = gterm_release_file(descriptor_path, 65536, &out->descriptor_size);
    out->signature = gterm_release_file(signature_path, 64, &signature_size);
    int result = 5;
    if (!policy || !out->descriptor || !out->signature) {
        goto done;
    }
    result = 2;
    if (signature_size != 64 || memchr(policy, 0, size) ||
        memchr(out->descriptor, 0, out->descriptor_size) || !gterm_catalog_utf8(policy, size) ||
        !gterm_catalog_utf8(out->descriptor, out->descriptor_size)) {
        goto done;
    }
    result = 5;
    if (!gterm_release_hash(policy, size, out->policy_digest) ||
        !gterm_release_hash(out->descriptor, out->descriptor_size, out->descriptor_digest)) {
        goto done;
    }
    char signature_digest[65];
    if ((expected_signature &&
         !gterm_release_hash(out->signature, signature_size, signature_digest))) {
        goto done;
    }
    if ((expected_policy && strcmp(expected_policy, out->policy_digest)) ||
        (expected_descriptor && strcmp(expected_descriptor, out->descriptor_digest)) ||
        (expected_signature && strcmp(expected_signature, signature_digest))) {
        result = 9;
        goto done;
    }
    uint64_t now = clock_ms();
    if (now == UINT64_MAX) {
        goto done;
    }
    budget_t budget = {now + 250, cancelled};
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) {
        goto done;
    }
    JS_SetMemoryLimit(runtime, 8388608);
    JS_SetMaxStackSize(runtime, 262144);
    JS_SetInterruptHandler(runtime, interrupt, &budget);
    JSContext *context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        goto done;
    }
    JSValue global = JS_GetGlobalObject(context);
    JS_SetPropertyStr(
        context, global, "__release_sha256", JS_NewCFunction(context, key_hash, "sha256", 1));
    JS_FreeValue(context, global);
    JSValue function = JS_Eval(context,
                               (const char *)policy_code,
                               sizeof(policy_code) - 1,
                               "gterm:release-policy",
                               JS_EVAL_TYPE_GLOBAL);
    JSValue args[] = {JS_NewStringLen(context, (const char *)out->descriptor, out->descriptor_size),
                      JS_NewStringLen(context, (const char *)policy, size)};
    JSValue checked = JS_IsException(function) || JS_IsException(args[0]) || JS_IsException(args[1])
                          ? JS_EXCEPTION
                          : JS_Call(context, function, JS_UNDEFINED, 2, args);
    unsigned char public_key[32];
    char key[65];
    if (JS_IsException(checked)) {
        JSValue exception = JS_GetException(context);
        JSValue detail = JS_GetPropertyStr(context, exception, "message");
        const char *message = JS_IsString(detail) ? JS_ToCString(context, detail) : NULL;
        result = 5;
        static const char *invalid[] = {"RELEASE_JSON",
                                        "RELEASE_UTF8",
                                        "RELEASE_DUPLICATE",
                                        "RELEASE_LIMIT",
                                        "RELEASE_NUMBER",
                                        "RELEASE_POLICY",
                                        "RELEASE_DESCRIPTOR"};
        for (size_t i = 0; message && i < sizeof(invalid) / sizeof(*invalid); i++) {
            if (!strcmp(message, invalid[i])) {
                result = 2;
            }
        }
        if (message &&
            (!strcmp(message, "RELEASE_SCOPE") || !strcmp(message, "RELEASE_UNKNOWN_KEY") ||
             !strcmp(message, "RELEASE_REVOKED"))) {
            result = 6;
        }
        JS_FreeCString(context, message);
        JS_FreeValue(context, detail);
        JS_FreeValue(context, exception);
    } else {
        JSValue count = JS_GetPropertyStr(context, checked, "artifact_bytes");
        int64_t artifact_bytes = 0;
        bool ok = !JS_ToInt64(context, &artifact_bytes, count) && artifact_bytes >= 22 &&
                  artifact_bytes <= 75497472 &&
                  field(context, checked, "id", out->id, sizeof(out->id)) &&
                  field(context, checked, "version", out->version, sizeof(out->version)) &&
                  field(context, checked, "publisher", out->publisher, sizeof(out->publisher)) &&
                  field(context, checked, "digest", out->digest, sizeof(out->digest)) &&
                  field(context, checked, "key", key, sizeof(key)) &&
                  field(context,
                        checked,
                        "artifact_digest",
                        out->artifact_digest,
                        sizeof(out->artifact_digest)) &&
                  unhex(key, public_key);
        JS_FreeValue(context, count);
        out->artifact_bytes = (uint64_t)artifact_bytes;
        gt_status_t status = ok ? gt_ext_release_verify(public_key,
                                                        32,
                                                        out->signature,
                                                        signature_size,
                                                        out->descriptor,
                                                        out->descriptor_size)
                                : GT_ERR_INVALID_ARG;
        result = status == GT_OK                ? 0
                 : status == GT_ERR_AUTH        ? 6
                 : status == GT_ERR_UNSUPPORTED ? 4
                 : status == GT_ERR_INVALID_ARG ? 2
                                                : 5;
    }
    JS_FreeValue(context, checked);
    JS_FreeValue(context, args[0]);
    JS_FreeValue(context, args[1]);
    JS_FreeValue(context, function);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
done:
    free(policy);
    if (*cancelled) {
        result = 130;
    }
    if (result) {
        gterm_release_clear(out);
    }
    return result;
#endif
}

int gterm_release_authenticate(const char *descriptor,
                               const char *signature,
                               const char *policy,
                               const char *expected_policy,
                               gterm_release_t *out,
                               volatile sig_atomic_t *cancelled) {
    return authenticate(descriptor, signature, policy, expected_policy, NULL, NULL, out, cancelled);
}

int gterm_release_authenticate_snapshot(const char *descriptor,
                                        const char *signature,
                                        const char *policy,
                                        const char *expected_policy,
                                        const char *expected_descriptor,
                                        const char *expected_signature,
                                        gterm_release_t *out,
                                        volatile sig_atomic_t *cancelled) {
    return authenticate(descriptor,
                        signature,
                        policy,
                        expected_policy,
                        expected_descriptor,
                        expected_signature,
                        out,
                        cancelled);
}

int gterm_release_installed(const char *directory,
                            const char *policy,
                            const char *expected_policy,
                            const char *digest,
                            const gt_ext_package_metadata_t *metadata,
                            volatile sig_atomic_t *cancelled) {
    size_t n = strlen(directory);
    char *descriptor = malloc(n + 14), *signature = malloc(n + 14);
    if (!descriptor || !signature) {
        free(descriptor);
        free(signature);
        return 5;
    }
    snprintf(descriptor, n + 14, "%s/release.json", directory);
    snprintf(signature, n + 14, "%s/release.sig", directory);
    gterm_release_t checked;
    int result = gterm_release_authenticate(
        descriptor, signature, policy, expected_policy, &checked, cancelled);
    if (!result && (strcmp(checked.digest, digest) ||
                    (metadata && (strcmp(checked.id, metadata->id) ||
                                  strcmp(checked.version, metadata->version) ||
                                  strcmp(checked.publisher, metadata->publisher) ||
                                  strcmp(checked.digest, metadata->package_digest))))) {
        result = 6;
    }
    gterm_release_clear(&checked);
    free(descriptor);
    free(signature);
    return result;
}

int gterm_release_profile(int argc,
                          char **argv,
                          const char *digest,
                          const gt_ext_package_metadata_t *metadata,
                          volatile sig_atomic_t *cancelled) {
    const char *directory = NULL, *policy = NULL, *snapshot = NULL;
    for (int at = 0; at < argc && strcmp(argv[at], "--");) {
        int fields = gterm_stdio_option_arity(argv[at]);
        if (fields < 0 || fields >= argc - at) {
            return 2;
        }
        if (!strcmp(argv[at], "--release-evidence")) {
            if (directory) {
                return 2;
            }
            directory = argv[at + 1];
            policy = argv[at + 2];
            snapshot = argv[at + 3];
        }
        at += fields + 1;
    }
    if (!directory) {
        return 0;
    }
    if (!gterm_catalog_path(directory) || !gterm_catalog_path(policy) ||
        !gterm_catalog_digest(snapshot)) {
        return 2;
    }
    return gterm_release_installed(directory, policy, snapshot, digest, metadata, cancelled);
}
