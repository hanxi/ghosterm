#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "catalog.h"
#include "release.h"
#include "stdio.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An observation is not a grant or a policy migration. Only a confirmed denial
 * under the selected current policy is eligible for explicit deactivation. */
typedef struct {
    const char *state;
    int detail;
    bool selected, denied, binding_current;
    char descriptor[65], signature[65];
} observation_t;

#define FINGERPRINT_BYTES ((GTERM_CATALOG_ENTRIES * (GTERM_CATALOG_ARGUMENTS + 16u) + 16u) * 64u)

typedef struct {
    char *bytes;
    size_t used;
} fingerprint_t;

static bool add(fingerprint_t *f, const char *value) {
    char hash[65];
    if (f->used > FINGERPRINT_BYTES - 64 || !value ||
        !gterm_release_hash(value, strlen(value), hash)) {
        return false;
    }
    memcpy(f->bytes + f->used, hash, 64);
    f->used += 64;
    return true;
}

static bool number(fingerprint_t *f, uint64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "%" PRIu64, value);
    return add(f, text);
}

static int snapshot(const char *path, char digest[65]) {
    size_t size;
    unsigned char *bytes = gterm_release_file(path, 1048576, &size);
    bool ok = bytes && gterm_release_hash(bytes, size, digest);
    free(bytes);
    return ok ? 0 : 5;
}

/* Parse option arities: a value that happens to spell --release-evidence is
 * never treated as an option. Catalog bytes are declarations, not authority. */
static int evidence(const gterm_catalog_entry_t *e,
                    const char **root,
                    const char **policy,
                    const char **pinned) {
    *root = *policy = *pinned = NULL;
    for (uint32_t i = 0; i < e->profile_count;) {
        int arity = gterm_stdio_option_arity(e->profile[i]);
        if (arity < 0 || (uint32_t)arity >= e->profile_count - i) {
            return 2;
        }
        if (!strcmp(e->profile[i], "--release-evidence")) {
            if (*root) {
                return 2;
            }
            *root = e->profile[i + 1];
            *policy = e->profile[i + 2];
            *pinned = e->profile[i + 3];
            if (!gterm_catalog_path(*root) || !gterm_catalog_path(*policy) ||
                !gterm_catalog_digest(*pinned)) {
                return 2;
            }
        }
        i += (uint32_t)arity + 1;
    }
    return 0;
}

static int observe(const gterm_catalog_entry_t *e,
                   const char *policy,
                   const char *policy_digest,
                   observation_t *out,
                   volatile sig_atomic_t *cancelled) {
    const char *root, *record_policy, *pinned;
    out->state = "invalid-profile";
    out->detail = evidence(e, &root, &record_policy, &pinned);
    if (out->detail) {
        return 0;
    }
    if (!root) {
        out->state = "development";
        return 0;
    }
    if (strcmp(policy, record_policy)) {
        out->state = "other-policy";
        return 0;
    }
    out->selected = true;
    out->binding_current = !strcmp(pinned, policy_digest);
    size_t n = strlen(root);
    char *descriptor = malloc(n + 14), *signature = malloc(n + 14);
    if (!descriptor || !signature) {
        free(descriptor);
        free(signature);
        return 5;
    }
    snprintf(descriptor, n + 14, "%s/release.json", root);
    snprintf(signature, n + 14, "%s/release.sig", root);
    out->state = "evidence-unavailable";
    out->detail = snapshot(descriptor, out->descriptor);
    if (!out->detail) {
        out->detail = snapshot(signature, out->signature);
    }
    if (!out->detail) {
        gterm_release_t proof;
        out->detail = gterm_release_authenticate_snapshot(descriptor,
                                                          signature,
                                                          policy,
                                                          policy_digest,
                                                          out->descriptor,
                                                          out->signature,
                                                          &proof,
                                                          cancelled);
        if (!out->detail) {
            if (strcmp(proof.id, e->id) || strcmp(proof.version, e->version) ||
                strcmp(proof.digest, e->digest)) {
                out->state = "record-mismatch";
                out->detail = 9;
            } else {
                gt_ext_package_t *package = NULL;
                gt_ext_package_metadata_t metadata;
                out->detail = gterm_catalog_capture(e, &package, &metadata, false, cancelled);
                if (!out->detail && strcmp(proof.publisher, metadata.publisher)) {
                    out->detail = 6;
                }
                if (package && gt_ext_package_destroy(&package) != GT_OK) {
                    out->detail = 5;
                }
                out->state = out->detail ? "package-unavailable" : "allowed";
            }
        } else if (out->detail == 6) {
            out->state = "denied";
            out->denied = true;
        } else {
            out->state = out->detail == 2 ? "invalid-evidence" : "authentication-unavailable";
        }
        gterm_release_clear(&proof);
        /* Bind exactly the evidence observed, including denial. A changed or
         * unreadable second snapshot aborts instead of confirming old bytes. */
        char current[65];
        if (snapshot(descriptor, current) || strcmp(current, out->descriptor) ||
            snapshot(signature, current) || strcmp(current, out->signature)) {
            free(descriptor);
            free(signature);
            return 9;
        }
        if (out->detail == 9 && strcmp(out->state, "record-mismatch")) {
            free(descriptor);
            free(signature);
            return 9;
        }
    }
    free(descriptor);
    free(signature);
    return *cancelled ? 130 : 0;
}

static int review(const gterm_catalog_t *c,
                  const char *policy,
                  const char *policy_digest,
                  observation_t *items,
                  char digest[65],
                  unsigned *denials,
                  volatile sig_atomic_t *cancelled) {
    fingerprint_t f = {.bytes = malloc(FINGERPRINT_BYTES)};
    if (!f.bytes) {
        return 5;
    }
    bool ok = add(&f, "ghosterm-cli-policy-review-v1") && add(&f, c->path) &&
              number(&f, (uint64_t)c->root.st_dev) && number(&f, (uint64_t)c->root.st_ino) &&
              number(&f, c->revision) && number(&f, c->count) && add(&f, policy) &&
              add(&f, policy_digest);
    int result = 0;
    *denials = 0;
    for (uint32_t i = 0; ok && i < c->count; i++) {
        const gterm_catalog_entry_t *e = &c->entries[i];
        result = observe(e, policy, policy_digest, &items[i], cancelled);
        if (result) {
            break;
        }
        *denials += items[i].denied;
        ok = add(&f, e->id) && add(&f, e->version) && add(&f, e->digest) && add(&f, e->directory) &&
             add(&f, e->host) && number(&f, e->profile_count);
        for (uint32_t j = 0; ok && j < e->profile_count; j++) {
            ok = add(&f, e->profile[j]);
        }
        ok = ok && add(&f, items[i].state) && number(&f, (unsigned)items[i].detail) &&
             add(&f, items[i].descriptor) && add(&f, items[i].signature);
    }
    if (!result && (!ok || !gterm_release_hash(f.bytes, f.used, digest))) {
        result = 5;
    }
    free(f.bytes);
    char current[65];
    if (!result && (snapshot(policy, current) || strcmp(current, policy_digest))) {
        result = 9;
    }
    return *cancelled ? 130 : result;
}

static void print_review(const gterm_catalog_t *c,
                         const char *policy_digest,
                         const char *digest,
                         const observation_t *items,
                         unsigned denials) {
    printf("{\"result\":\"reviewed\",\"revision\":\"%" PRIu64 "\",\"policyDigest\":", c->revision);
    gterm_json_string(policy_digest);
    fputs(",\"reviewDigest\":", stdout);
    gterm_json_string(digest);
    printf(",\"deniedCount\":%u,\"authorizationGranted\":false,\"publisherAuthenticated\":false,"
           "\"entries\":[",
           denials);
    for (uint32_t i = 0; i < c->count; i++) {
        const gterm_catalog_entry_t *e = &c->entries[i];
        const observation_t *o = &items[i];
        if (i) {
            putchar(',');
        }
        fputs("{\"id\":", stdout);
        gterm_json_string(e->id);
        fputs(",\"version\":", stdout);
        gterm_json_string(e->version);
        fputs(",\"digest\":", stdout);
        gterm_json_string(e->digest);
        fputs(",\"state\":", stdout);
        gterm_json_string(o->state);
        printf(",\"detailCode\":%d,\"eligibleToDisable\":%s,\"policyBindingCurrent\":%s}",
               o->detail,
               o->denied ? "true" : "false",
               !o->selected         ? "null"
               : o->binding_current ? "true"
                                    : "false");
    }
    puts("]}");
}

int gterm_catalog_review_policy(const char *operation,
                                int argc,
                                char **argv,
                                volatile sig_atomic_t *cancelled) {
    const char *path = NULL, *policy = NULL, *revision = NULL, *expected_policy = NULL,
               *expected_review = NULL;
    bool apply = !strcmp(operation, "disable-denied");
    for (int i = 0; i < argc; i += 2) {
        if (i + 1 == argc) {
            return gterm_catalog_report(2, "POLICY_OPTIONS", NULL);
        }
        const char **field = !strcmp(argv[i], "--catalog")                ? &path
                             : !strcmp(argv[i], "--policy")               ? &policy
                             : !strcmp(argv[i], "--expect-revision")      ? &revision
                             : !strcmp(argv[i], "--expect-policy-digest") ? &expected_policy
                             : !strcmp(argv[i], "--review-digest")        ? &expected_review
                                                                          : NULL;
        if (!field || *field) {
            return gterm_catalog_report(2, "POLICY_OPTIONS", NULL);
        }
        *field = argv[i + 1];
    }
    if (!path) {
        path = getenv("GTERM_CATALOG");
    }
    uint64_t expected_revision = 0;
    if (!gterm_catalog_path(path) || !gterm_catalog_path(policy) ||
        (apply ? (!gterm_catalog_revision(revision, &expected_revision) ||
                  !gterm_catalog_digest(expected_policy) || !gterm_catalog_digest(expected_review))
               : (revision || expected_policy || expected_review))) {
        return gterm_catalog_report(2, "POLICY_OPTIONS", NULL);
    }
    if (!gterm_release_supported()) {
        return gterm_catalog_report(4, "RELEASE_VERIFY_UNAVAILABLE", NULL);
    }
    gterm_catalog_t c;
    int status = gterm_catalog_open(&c, path, apply);
    const char *code = "CATALOG_OPEN";
    char policy_digest[65], digest[65];
    observation_t items[GTERM_CATALOG_ENTRIES] = {0};
    unsigned denials = 0;
    if (status) {
        goto done;
    }
    if (apply && c.revision != expected_revision) {
        status = 9;
        code = "CATALOG_REVISION";
        goto done;
    }
    status = snapshot(policy, policy_digest);
    code = "POLICY_SNAPSHOT";
    if (status) {
        goto done;
    }
    if (apply && strcmp(policy_digest, expected_policy)) {
        status = 9;
        goto done;
    }
    status = review(&c, policy, policy_digest, items, digest, &denials, cancelled);
    code = "POLICY_REVIEW";
    if (status) {
        goto done;
    }
    if (apply && strcmp(digest, expected_review)) {
        status = 9;
        goto done;
    }
    if (!apply) {
        if (!gterm_catalog_release(&c)) {
            status = 5;
            code = "CATALOG_CLOSE";
            goto done;
        }
        print_review(&c, policy_digest, digest, items, denials);
        goto done;
    }
    /* One catalog transaction, preserving every non-denied record verbatim.
     * Existing foreground instances keep their copied invocation configuration. */
    if (denials) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < c.count; i++) {
            if (items[i].denied) {
                gterm_catalog_entry_clear(&c.entries[i]);
            } else {
                if (kept != i) {
                    c.entries[kept] = c.entries[i];
                    memset(&c.entries[i], 0, sizeof(c.entries[i]));
                }
                kept++;
            }
        }
        c.count = kept;
        char current[65];
        if (snapshot(policy, current) || strcmp(current, policy_digest)) {
            status = 9;
            goto done;
        }
        status = gterm_catalog_save(&c, expected_revision, cancelled);
        code = "POLICY_COMMIT";
        if (status) {
            goto done;
        }
    }
    if (!gterm_catalog_release(&c)) {
        status = 5;
        code = "CATALOG_CLOSE";
        goto done;
    }
    printf("{\"result\":\"%s\",\"revision\":\"%" PRIu64
           "\",\"disabledCount\":%u,\"committed\":%s,\"durabilityConfirmed\":%s,"
           "\"authorizationGranted\":false,\"publisherAuthenticated\":false}\n",
           denials ? "disabled" : "unchanged",
           c.revision,
           denials,
           c.committed ? "true" : "false",
           c.committed ? "true" : "false");
done:
    if (!gterm_catalog_close(&c)) {
        status = 5;
        code = "CATALOG_CLOSE";
    }
    return gterm_catalog_report(status, code, &c);
}
