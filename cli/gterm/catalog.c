#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include "catalog.h"
#include "ipc.h"
#include "stdio.h"
#include "release.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int gterm_catalog_report(int status, const char *code, const gterm_catalog_t *catalog) {
    if (!status) {
        return 0;
    }
    const char *kind = status == 2     ? "invalid"
                       : status == 3   ? "unknown"
                       : status == 4   ? "unsupported"
                       : status == 6   ? "denied"
                       : status == 8   ? "conflict"
                       : status == 9   ? "stale"
                       : status == 10  ? "not-found"
                       : status == 11  ? "busy"
                       : status == 130 ? "cancelled"
                                       : "io";
    fputs("{\"result\":", stdout);
    gterm_json_string(kind);
    fputs(",\"code\":", stdout);
    gterm_json_string(code);
    if (catalog && catalog->installed) {
        fputs(",\"installed\":true", stdout);
    }
    printf(",\"committed\":%s,\"durabilityConfirmed\":false,\"authorizationGranted\":false,"
           "\"publisherAuthenticated\":false}\n",
           catalog && catalog->committed ? "true" : "false");
    return status;
}

bool gterm_catalog_revision(const char *s, uint64_t *out) {
    if (!s || !*s || (s[0] == '0' && s[1])) {
        return false;
    }
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10) {
            return false;
        }
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n;
    return true;
}

static char *read_host(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat s;
    bool ok =
        fd >= 0 && !fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_size > 0 && s.st_size <= 65536;
    char *text = ok ? malloc((size_t)s.st_size + 2) : NULL;
    if (!text) {
        ok = false;
    }
    size_t at = 0, size = ok ? (size_t)s.st_size : 0;
    while (ok && at <= size) {
        ssize_t n = read(fd, text + at, size + 1 - at);
        if (n > 0) {
            at += (size_t)n;
            if (at > size) {
                ok = false;
            }
        } else if (!n) {
            break;
        } else if (errno != EINTR) {
            ok = false;
        }
    }
    if (fd >= 0 && close(fd)) {
        ok = false;
    }
    if (ok) {
        ok = at == size && !memchr(text, 0, size);
    }
    if (!ok) {
        free(text);
        return NULL;
    }
    text[size] = 0;
    return text;
}

static bool catalog_command_paths(gt_ext_package_t *package) {
    uint32_t count;
    if (!gt_ext_package_command_count(package, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        gt_ext_package_command_t command;
        if (!gt_ext_package_command(package, i, &command)) {
            return false;
        }
        size_t first = strcspn(command.path, " ");
        static const char *const builtins[] = {"doctor", "plugin", "relay"};
        for (size_t j = 0; j < sizeof(builtins) / sizeof(builtins[0]); ++j) {
            if (first == strlen(builtins[j]) && !strncmp(command.path, builtins[j], first)) {
                return false;
            }
        }
        for (uint32_t j = 0; j < i; ++j) {
            gt_ext_package_command_t other;
            if (!gt_ext_package_command(package, j, &other)) {
                return false;
            }
            size_t a = strlen(command.path), b = strlen(other.path);
            if ((a < b && other.path[a] == ' ' && !strncmp(command.path, other.path, a)) ||
                (b < a && command.path[b] == ' ' && !strncmp(command.path, other.path, b))) {
                return false;
            }
        }
    }
    return true;
}

int gterm_catalog_capture(const gterm_catalog_entry_t *entry,
                          gt_ext_package_t **out,
                          gt_ext_package_metadata_t *metadata,
                          bool authenticate,
                          volatile sig_atomic_t *cancelled) {
    int fd = open(entry->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return 5;
    }
    gt_ext_package_config_t config = {fd, 1024, 4096, 16777216, 67108864};
    gt_status_t opened = gt_ext_package_open(&config, out);
    int closed = close(fd);
    if (opened != GT_OK || closed) {
        return 5;
    }
    gt_ext_package_phase_t phase;
    do {
        if (*cancelled) {
            gt_ext_package_stop(*out);
        }
        phase = gt_ext_package_step(*out);
    } while (phase == GT_EXT_PACKAGE_CAPTURING);
    if (*cancelled) {
        return 130;
    }
    gt_ext_package_check_config_t check = {.host_json = entry->host};
    if (phase != GT_EXT_PACKAGE_CAPTURED ||
        gt_ext_package_check(*out, &check) != GT_EXT_PACKAGE_CHECKED) {
        gt_ext_package_info_t info;
        if (gt_ext_package_info(*out, &info)) {
            if (info.error == GT_EXT_PACKAGE_UNKNOWN) {
                return 3;
            }
            if (info.error == GT_EXT_PACKAGE_UNSUPPORTED) {
                return 4;
            }
            if (info.error == GT_EXT_PACKAGE_DENIED) {
                return 6;
            }
        }
        return 5;
    }
    if (!gt_ext_package_metadata(*out, metadata) || !metadata->executor) {
        return 2;
    }
    if (strcmp(metadata->package_digest, entry->digest)) {
        return 6;
    }
    if (!catalog_command_paths(*out)) {
        return 5;
    }
    if (entry->id &&
        (strcmp(metadata->id, entry->id) || strcmp(metadata->version, entry->version))) {
        return 9;
    }
    return *cancelled ? 130
           : !authenticate
               ? 0
               : gterm_release_profile(
                     (int)entry->profile_count, entry->profile, entry->digest, metadata, cancelled);
}

static char **
invocation(const gterm_catalog_entry_t *e, const char *command, int argc, char **argv, int *count) {
    if (argc < 0 || argc > 32 || e->profile_count > GTERM_CATALOG_ARGUMENTS) {
        return NULL;
    }
    *count = (int)e->profile_count + 5 + argc;
    char **args = calloc((size_t)*count + 1, sizeof(char *));
    if (!args) {
        return NULL;
    }
    args[0] = "--trust-development-digest";
    args[1] = e->digest;
    for (uint32_t i = 0; i < e->profile_count; i++) {
        args[2 + i] = e->profile[i];
    }
    for (uint32_t i = 0; i < e->profile_count;) {
        int fields = gterm_stdio_option_arity(e->profile[i]);
        if (fields < 0 || (uint32_t)fields >= e->profile_count - i) {
            free(args);
            return NULL;
        }
        if (!strcmp(e->profile[i], "--root") && !strcmp(e->profile[i + 2], "@cwd")) {
            /* listen restores the original cwd by fd before stdio opens the
             * root. A renamed cwd is never replaced by a later path lookup. */
            args[4 + i] = ".";
        }
        i += (uint32_t)fields + 1;
    }
    int at = 2 + (int)e->profile_count;
    args[at++] = "--";
    args[at++] = e->directory;
    args[at++] = (char *)command;
    for (int i = 0; i < argc; i++) {
        args[at++] = argv[i];
    }
    return args;
}

static bool fixed_paths(const gterm_catalog_entry_t *e) {
    for (uint32_t i = 0; i < e->profile_count;) {
        const char *name = e->profile[i];
        int n = gterm_stdio_option_arity(name);
        if (n < 0 || (uint32_t)n >= e->profile_count - i ||
            !strcmp(name, "--trust-development-digest")) {
            return false;
        }
        const char *path = NULL;
        if (!strcmp(name, "--root") || !strcmp(name, "--process-program")) {
            path = e->profile[i + 2];
            if (!strcmp(name, "--root") && !strcmp(path, "@cwd")) {
                path = NULL;
            }
        } else if (!strcmp(name, "--release-evidence")) {
            if (!gterm_catalog_path(e->profile[i + 2]) ||
                !gterm_catalog_digest(e->profile[i + 3])) {
                return false;
            }
            path = e->profile[i + 1];
        }
        if (path && !gterm_catalog_path(path)) {
            return false;
        }
        i += (uint32_t)n + 1;
    }
    return true;
}

static int profile_check(const gterm_catalog_entry_t *e,
                         const char *command,
                         const gt_ext_package_metadata_t *metadata) {
    if (!fixed_paths(e)) {
        return 2;
    }
    int count = 0;
    char **args = invocation(e, command, 0, NULL, &count);
    if (!args) {
        return 5;
    }
    int result = gterm_stdio_invocation_index(count, args) == (int)e->profile_count + 2
                     ? gterm_stdio_validate(count, args)
                     : 2;
    free(args);
    if (!result && strcmp(metadata->runtime, GTERM_NATIVE_RUNTIME)) {
        return 4;
    }
    return result;
}

static void list(const gterm_catalog_t *c) {
    printf("{\"result\":\"catalog\",\"revision\":\"%" PRIu64 "\",\"entries\":[", c->revision);
    for (uint32_t i = 0; i < c->count; i++) {
        const gterm_catalog_entry_t *e = &c->entries[i];
        if (i) {
            putchar(',');
        }
        fputs("{\"id\":", stdout);
        gterm_json_string(e->id);
        fputs(",\"version\":", stdout);
        gterm_json_string(e->version);
        fputs(",\"packageDigest\":", stdout);
        gterm_json_string(e->digest);
        bool signed_record = false;
        for (uint32_t j = 0; j < e->profile_count;) {
            int fields = gterm_stdio_option_arity(e->profile[j]);
            if (fields < 0 || (uint32_t)fields >= e->profile_count - j) {
                break;
            }
            if (!strcmp(e->profile[j], "--release-evidence")) {
                signed_record = true;
            }
            j += (uint32_t)fields + 1;
        }
        printf(",\"source\":\"%s\",\"publisherAuthenticated\":false}",
               signed_record ? "signed-release-record" : "explicit-development-digest");
    }
    puts("],\"authorizationGranted\":false,\"publisherAuthenticated\":false}");
}

int gterm_catalog_manage(const char *operation,
                         int argc,
                         char **argv,
                         volatile sig_atomic_t *cancelled) {
    const char *path = NULL, *host = NULL, *trusted = NULL, *expected_digest = NULL, *id = NULL,
               *expected_text = NULL;
    const char *policy = NULL, *store = NULL, *artifact = NULL, *descriptor = NULL,
               *signature = NULL;
    bool installing = !strcmp(operation, "install"),
         signed_registration = !strcmp(operation, "register-signed"),
         signed_source = installing || signed_registration;
    int at = 0;
    for (; at < argc && strcmp(argv[at], "--"); at += 2) {
        if (at + 1 >= argc) {
            return gterm_catalog_report(2, "CATALOG_OPTIONS", NULL);
        }
        const char **value = !strcmp(argv[at], "--catalog")                    ? &path
                             : !strcmp(argv[at], "--expect-revision")          ? &expected_text
                             : !strcmp(argv[at], "--host-json")                ? &host
                             : !strcmp(argv[at], "--trust-development-digest") ? &trusted
                             : !strcmp(argv[at], "--expect-digest")            ? &expected_digest
                             : !strcmp(argv[at], "--id")                       ? &id
                             : !strcmp(argv[at], "--policy")                   ? &policy
                             : !strcmp(argv[at], "--store")                    ? &store
                             : !strcmp(argv[at], "--artifact")                 ? &artifact
                             : !strcmp(argv[at], "--descriptor")               ? &descriptor
                             : !strcmp(argv[at], "--signature")                ? &signature
                                                                               : NULL;
        if (!value || *value) {
            return gterm_catalog_report(2, "CATALOG_OPTIONS", NULL);
        }
        *value = argv[at + 1];
    }
    if (!path) {
        path = getenv("GTERM_CATALOG");
    }
    bool listing = !strcmp(operation, "list"), removing = !strcmp(operation, "unregister");
    uint64_t expected = 0;
    if (!gterm_catalog_path(path) ||
        (listing ? (at != argc || expected_text || host || trusted || expected_digest || id ||
                    policy || store || artifact || descriptor || signature)
                 : !gterm_catalog_revision(expected_text, &expected))) {
        return gterm_catalog_report(2, "CATALOG_OPTIONS", NULL);
    }
    if (!listing &&
        (removing ? (at != argc || host || trusted || policy || store || artifact || descriptor ||
                     signature || !id || !gterm_catalog_digest(expected_digest))
                  : (at >= argc || strcmp(argv[at], "--") || id || !host ||
                     (signed_source ? (trusted || !gterm_catalog_path(policy))
                                    : (!gterm_catalog_digest(trusted) || policy || store ||
                                       artifact || descriptor || signature)) ||
                     (installing ? (!gterm_catalog_path(store) || !strcmp(store, path) ||
                                    !artifact || !descriptor || !signature)
                                 : (at + 1 >= argc || !gterm_catalog_path(argv[at + 1]) || store ||
                                    artifact || descriptor || signature)) ||
                     (expected_digest && !gterm_catalog_digest(expected_digest)) ||
                     argc - at - (installing ? 1 : 2) + (signed_source ? 4 : 0) >
                         (int)GTERM_CATALOG_ARGUMENTS))) {
        return gterm_catalog_report(2, "CATALOG_OPTIONS", NULL);
    }
    if (gt_ext_runtime_support() != GT_OK) {
        return gterm_catalog_report(4, "PLUGIN_RUNTIME_DISABLED", NULL);
    }
    if (signed_source && !gterm_release_supported()) {
        return gterm_catalog_report(4, "RELEASE_VERIFY_UNAVAILABLE", NULL);
    }
    gterm_catalog_t c;
    int status = gterm_catalog_open(&c, path, !listing);
    const char *code = "CATALOG_OPEN";
    gt_ext_package_t *package = NULL;
    gterm_catalog_entry_t fresh = {0};
    gt_ext_package_metadata_t metadata = {0};
    if (status) {
        goto done;
    }
    if (*cancelled) {
        status = 130;
        code = "CATALOG_CANCELLED";
        goto done;
    }
    if (listing) {
        goto done;
    }
    if (c.revision != expected || expected == UINT64_MAX) {
        status = 9;
        code = "CATALOG_REVISION";
        goto done;
    }
    if (!removing) {
        fresh.host = read_host(host);
        char *release_directory = NULL;
        char policy_digest[65] = {0};
        if (signed_source) {
            if (!fresh.host) {
                status = 5;
                code = "CATALOG_INPUT";
                goto done;
            }
            if (installing) {
                status = gterm_release_install(store,
                                               artifact,
                                               descriptor,
                                               signature,
                                               policy,
                                               fresh.host,
                                               &fresh.directory,
                                               policy_digest,
                                               &c.installed,
                                               cancelled);
                code = "RELEASE_INSTALL";
                if (status) {
                    goto done;
                }
                release_directory = strdup(fresh.directory);
                if (release_directory) {
                    release_directory[strlen(release_directory) - 8] = 0;
                }
            } else {
                release_directory = strdup(argv[at + 1]);
                fresh.directory = malloc(strlen(argv[at + 1]) + 9);
                if (fresh.directory) {
                    sprintf(fresh.directory, "%s/payload", argv[at + 1]);
                }
            }
            if (!release_directory || !fresh.directory) {
                free(release_directory);
                status = 5;
                goto done;
            }
            size_t n = strlen(release_directory);
            char *release_json = malloc(n + 14), *release_sig = malloc(n + 14);
            if (!release_json || !release_sig) {
                free(release_json);
                free(release_sig);
                free(release_directory);
                status = 5;
                goto done;
            }
            snprintf(release_json, n + 14, "%s/release.json", release_directory);
            snprintf(release_sig, n + 14, "%s/release.sig", release_directory);
            gterm_release_t proof;
            status = gterm_release_authenticate(release_json,
                                                release_sig,
                                                policy,
                                                installing ? policy_digest : NULL,
                                                &proof,
                                                cancelled);
            free(release_json);
            free(release_sig);
            code = "RELEASE_AUTHENTICATION";
            if (status) {
                free(release_directory);
                goto done;
            }
            fresh.digest = strdup(proof.digest);
            memcpy(policy_digest, proof.policy_digest, 65);
            gterm_release_clear(&proof);
            fresh.profile = calloc((size_t)(argc - at + 4), sizeof(char *));
            if (fresh.profile) {
                const char *evidence[] = {
                    "--release-evidence", release_directory, policy, policy_digest};
                for (unsigned i = 0; i < 4; i++) {
                    char *copy = strdup(evidence[i]);
                    if (!copy) {
                        status = 5;
                        break;
                    }
                    fresh.profile[fresh.profile_count++] = copy;
                }
            }
            free(release_directory);
            if (status) {
                goto done;
            }
        } else {
            fresh.directory = strdup(argv[at + 1]);
            fresh.digest = strdup(trusted);
            fresh.profile = calloc((size_t)(argc - at - 2) + 1, sizeof(char *));
        }
        if (!fresh.directory || !fresh.digest || !fresh.host || !fresh.profile) {
            status = 5;
            code = "CATALOG_INPUT";
            goto done;
        }
        size_t bytes = 0;
        for (uint32_t i = 0; i < fresh.profile_count; i++) {
            bytes += strlen(fresh.profile[i]) + 1;
        }
        for (int i = at + (installing ? 1 : 2); i < argc; i++) {
            size_t n = strlen(argv[i]);
            bytes += n + 1;
            if (n > 4096 || bytes > 32768) {
                status = 2;
                code = "CATALOG_PROFILE";
                goto done;
            }
            char *copy = strdup(argv[i]);
            if (!copy) {
                status = 5;
                goto done;
            }
            fresh.profile[fresh.profile_count++] = copy;
        }
        status = gterm_catalog_capture(&fresh, &package, &metadata, true, cancelled);
        code = "CATALOG_PACKAGE";
        if (status) {
            goto done;
        }
        gt_ext_package_command_t command;
        if (!gt_ext_package_command(package, 0, &command)) {
            status = 2;
            goto done;
        }
        status = profile_check(&fresh, command.path, &metadata);
        code = "CATALOG_PROFILE";
        if (status) {
            goto done;
        }
        fresh.id = strdup(metadata.id);
        fresh.version = strdup(metadata.version);
        if (!fresh.id || !fresh.version) {
            status = 5;
            goto done;
        }
        id = fresh.id;
    }
    uint32_t index = c.count;
    for (uint32_t i = 0; i < c.count; i++) {
        if (!strcmp(c.entries[i].id, id)) {
            index = i;
            break;
        }
    }
    if (index == c.count) {
        if (expected_digest) {
            status = 9;
            code = "CATALOG_PACKAGE_SELECTION";
            goto done;
        }
        if (c.count == GTERM_CATALOG_ENTRIES) {
            status = 5;
            code = "CATALOG_LIMIT";
            goto done;
        }
    } else {
        if (!expected_digest || strcmp(c.entries[index].digest, expected_digest)) {
            status = expected_digest ? 9 : 8;
            code = "CATALOG_PACKAGE_SELECTION";
            goto done;
        }
        gterm_catalog_entry_clear(&c.entries[index]);
    }
    if (removing) {
        for (uint32_t i = index + 1; i < c.count; i++) {
            c.entries[i - 1] = c.entries[i];
        }
        c.count--;
        memset(&c.entries[c.count], 0, sizeof(c.entries[c.count]));
    } else {
        c.entries[index] = fresh;
        memset(&fresh, 0, sizeof(fresh));
        if (index == c.count) {
            c.count++;
        }
    }
    status = *cancelled ? 130 : gterm_catalog_save(&c, expected, cancelled);
    code = "CATALOG_COMMIT";
done:
    gterm_catalog_entry_clear(&fresh);
    if (package && gt_ext_package_destroy(&package) != GT_OK) {
        status = 5;
        code = "PACKAGE_CLOSE";
    }
    if (!gterm_catalog_release(&c)) {
        status = 5;
        code = "CATALOG_CLOSE";
    }
    if (!status) {
        if (listing) {
            list(&c);
        } else {
            printf("{\"result\":\"%s\",\"revision\":\"%" PRIu64
                   "\",\"committed\":true,\"durabilityConfirmed\":true,\"authorizationGranted\":"
                   "false,\"publisherAuthenticated\":%s,\"installed\":%s}\n",
                   removing     ? "unregistered"
                   : installing ? "installed-and-registered"
                                : "registered",
                   c.revision,
                   signed_source ? "true" : "false",
                   c.installed ? "true" : "false");
        }
    }
    gterm_catalog_close(&c);
    return gterm_catalog_report(status, code, &c);
}

static int registry_status(gt_ext_registry_status_t status) {
    switch (status) {
        case GT_EXT_REGISTRY_OK:
            return 0;
        case GT_EXT_REGISTRY_CONFLICT:
            return 8;
        case GT_EXT_REGISTRY_STALE:
            return 9;
        case GT_EXT_REGISTRY_NOT_FOUND:
            return 10;
        case GT_EXT_REGISTRY_UNKNOWN:
            return 3;
        case GT_EXT_REGISTRY_DENIED:
            return 6;
        case GT_EXT_REGISTRY_INVALID:
            return 2;
        case GT_EXT_REGISTRY_UNSUPPORTED:
            return 4;
        case GT_EXT_REGISTRY_CANCELLED:
            return 130;
        default:
            return 5;
    }
}

static int matched_tokens(const char *command, int argc, char **argv) {
    const char *p = command;
    int tokens = 0;
    while (*p) {
        const char *space = strchr(p, ' ');
        size_t n = space ? (size_t)(space - p) : strlen(p);
        if (tokens == argc || strlen(argv[tokens]) != n || memcmp(p, argv[tokens], n)) {
            return 0;
        }
        tokens++;
        if (!space) {
            break;
        }
        p = space + 1;
    }
    return tokens;
}

int gterm_catalog_dispatch(int argc, char **argv, volatile sig_atomic_t *cancelled) {
    const char *path = getenv("GTERM_CATALOG"), *endpoint = getenv("GTERM_ENDPOINT"), *id = NULL,
               *digest = NULL, *canonical = NULL;
    bool supplied_path = false, supplied_endpoint = false;
    int at = 0;
    for (; at < argc;) {
        if (!strcmp(argv[at], "--catalog") && !supplied_path && at + 1 < argc) {
            path = argv[at + 1];
            supplied_path = true;
            at += 2;
        } else if (!strcmp(argv[at], "--endpoint") && !supplied_endpoint && at + 1 < argc) {
            endpoint = argv[at + 1];
            supplied_endpoint = true;
            at += 2;
        } else if (!strcmp(argv[at], "--select") && !id && at + 2 < argc) {
            id = argv[at + 1];
            digest = argv[at + 2];
            at += 3;
        } else if (!strcmp(argv[at], "--command") && !canonical && at + 1 < argc) {
            canonical = argv[at + 1];
            at += 2;
        } else {
            break;
        }
    }
    if (!path || !*path) {
        return gterm_catalog_report(4, "COMMAND_ACTIVATION_UNAVAILABLE", NULL);
    }
    if (at < argc && !strcmp(argv[at], "--")) {
        at++;
    }
    if (at == argc || !gterm_catalog_path(path) ||
        (id && (!*id || !gterm_catalog_digest(digest)))) {
        return gterm_catalog_report(2, "COMMAND_OPTIONS", NULL);
    }
    if (gt_ext_runtime_support() != GT_OK) {
        return gterm_catalog_report(4, "PLUGIN_RUNTIME_DISABLED", NULL);
    }
    gterm_catalog_t c;
    int status = gterm_catalog_open(&c, path, false);
    const char *code = "CATALOG_OPEN";
    gt_ext_registry_t *registry = NULL;
    gt_ext_package_t *package = NULL, *selected = NULL;
    gt_ext_package_metadata_t metadata = {0};
    gt_ext_package_command_t contribution = {0};
    char chosen[520] = {0};
    int consumed = 0;
    uint32_t index = 0;
    unsigned matches = 0;
    char **call = NULL;
    int call_count = 0, version_reference = -1;
    bool explicit_id = false;
    if (status) {
        goto done;
    }
    status = registry_status(gt_ext_registry_open(&registry));
    if (status) {
        goto done;
    }
    for (uint32_t i = 0; i < c.count; i++) {
        gterm_catalog_entry_t *e = &c.entries[i];
        status = gterm_catalog_capture(e, &package, &metadata, true, cancelled);
        code = "CATALOG_PACKAGE";
        if (status) {
            goto done;
        }
        status = profile_check(e, "catalog-probe", &metadata);
        code = "CATALOG_PROFILE";
        if (status) {
            goto done;
        }
        status = registry_status(
            gt_ext_registry_put(registry, package, GT_EXT_REGISTRY_SOURCE_ALLOWED, NULL));
        if (status) {
            goto done;
        }
        bool eligible = !id || !strcmp(id, e->id);
        if (id && !strcmp(id, e->id)) {
            explicit_id = true;
            if (strcmp(digest, e->digest)) {
                status = 9;
                code = "COMMAND_SELECTION";
                goto done;
            }
        }
        uint32_t count;
        if (!gt_ext_package_command_count(package, &count)) {
            status = 5;
            goto done;
        }
        for (uint32_t j = 0; eligible && j < count; j++) {
            if (!gt_ext_package_command(package, j, &contribution)) {
                status = 5;
                goto done;
            }
            if (canonical && strcmp(canonical, contribution.path)) {
                continue;
            }
            int n = matched_tokens(contribution.path, argc - at, argv + at);
            if (n) {
                matches++;
                consumed = n;
                index = i;
                memcpy(chosen, contribution.path, sizeof(chosen));
            }
        }
        if (gt_ext_package_destroy(&package) != GT_OK) {
            status = 5;
            goto done;
        }
    }
    code = "COMMAND_RESOLVE";
    if (id && !explicit_id) {
        status = 9;
        goto done;
    }
    if (matches != 1) {
        status = matches ? 8 : 10;
        goto done;
    }
    status = registry_status(
        gt_ext_registry_resolve(registry, chosen, true, id, digest, &selected, &contribution));
    if (status) {
        goto done;
    }
    size_t size = 0;
    if (gt_ext_package_decode_command_arguments(selected,
                                                chosen,
                                                (const char *const *)(argv + at + consumed),
                                                (size_t)(argc - at - consumed),
                                                NULL,
                                                0,
                                                &size) != GT_OK) {
        status = 2;
        code = "COMMAND_ARGUMENTS";
        goto done;
    }
    call = invocation(
        &c.entries[index], chosen, argc - at - consumed, argv + at + consumed, &call_count);
    if (!call) {
        status = 2;
        code = "COMMAND_ARGUMENTS";
        goto done;
    }
    status = gterm_stdio_validate(call_count, call);
    if (status) {
        code = "CATALOG_PROFILE";
        goto done;
    }
    if (!gterm_catalog_path(endpoint)) {
        status = 4;
        code = "COMMAND_ENDPOINT_REQUIRED";
        goto done;
    }
    status = gterm_release_pin_profile(call_count, call, &version_reference);
    if (status) {
        code = "VERSION_REFERENCE";
        goto done;
    }
    /* Selection is linearized under the shared catalog lock. Its fixed bytes
     * remain valid for this invocation; later edits affect future commands. */
    if (gt_ext_package_destroy(&selected) != GT_OK ||
        gt_ext_registry_destroy(&registry) != GT_EXT_REGISTRY_OK || !gterm_catalog_release(&c)) {
        status = 5;
        code = "CATALOG_CLOSE";
        goto done;
    }
    if (*cancelled) {
        status = 130;
        code = "COMMAND_CANCELLED";
        goto done;
    }
    status = gterm_ipc_listen(endpoint, call_count, call, cancelled);
    if (version_reference >= 0 && close(version_reference)) {
        status = 5;
    }
    version_reference = -1;
    /* IPC replaced stdout with the protocol socket. Never print JSON there. */
    free(call);
    call = NULL;
    if (!gterm_catalog_close(&c)) {
        status = 5;
    }
    return status;
done:
    if (version_reference >= 0 && close(version_reference)) {
        status = 5;
    }
    free(call);
    if (package && gt_ext_package_destroy(&package) != GT_OK) {
        status = 5;
    }
    if (selected && gt_ext_package_destroy(&selected) != GT_OK) {
        status = 5;
    }
    if (registry && gt_ext_registry_destroy(&registry) != GT_EXT_REGISTRY_OK) {
        status = 5;
    }
    if (!gterm_catalog_close(&c)) {
        status = 5;
    }
    return gterm_catalog_report(status, code, &c);
}
