/* Strict release and public trust-policy validation. Included by release-auth.c. */
typedef gt_package_json_t document_t;
typedef gt_package_json_node_t node_t;

static uint16_t member(const document_t *doc, uint16_t object, const char *key) {
    node_t node;
    if (!gt__package_json_node(doc, object, &node) || node.kind != GT_PACKAGE_JSON_OBJECT) {
        return 0;
    }
    const size_t size = strlen(key);
    for (uint16_t i = node.child; gt__package_json_node(doc, i, &node); i = node.next) {
        if (node.key_size == size && !memcmp(node.key, key, size)) {
            return i;
        }
    }
    return 0;
}

static bool fields(const document_t *doc, uint16_t id, const char *const *keys, size_t count) {
    node_t node;
    if (!gt__package_json_node(doc, id, &node) || node.kind != GT_PACKAGE_JSON_OBJECT ||
        node.count != count) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!member(doc, id, keys[i])) {
            return false;
        }
    }
    return true;
}

static bool copy_text(const document_t *doc, uint16_t id, char *out, size_t capacity) {
    node_t node;
    if (!gt__package_json_node(doc, id, &node) || node.kind != GT_PACKAGE_JSON_STRING ||
        !node.text_size || node.text_size >= capacity || memchr(node.text, 0, node.text_size)) {
        return false;
    }
    memcpy(out, node.text, node.text_size);
    out[node.text_size] = 0;
    return true;
}

static bool token(const document_t *doc, uint16_t id, const char *expected) {
    char value[32];
    return copy_text(doc, id, value, sizeof(value)) && !strcmp(value, expected);
}

static bool channel_name(const char *value) {
    const char *alnum = "abcdefghijklmnopqrstuvwxyz0123456789";
    return *value && strchr(alnum, *value) &&
           value[strspn(value, "abcdefghijklmnopqrstuvwxyz0123456789.-")] == 0;
}

static bool plugin_id(const char *value) {
    const char *alnum = "abcdefghijklmnopqrstuvwxyz0123456789";
    if (*value < 'a' || *value > 'z') {
        return false;
    }
    value += strspn(value, alnum);
    if (!*value) {
        return false;
    }
    do {
        if ((*value != '.' && *value != '-') || !value[1] || !strchr(alnum, value[1])) {
            return false;
        }
        value += 1 + strspn(value + 1, alnum);
    } while (*value);
    return true;
}

static bool display_text(const char *value) {
    size_t points = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 32 || *p == 127) {
            return false;
        }
        points += (*p & 0xc0) != 0x80;
    }
    if (!points || points > 128) {
        return false;
    }
#ifdef __APPLE__
    CFStringRef original = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    if (!original) {
        return false;
    }
    CFMutableStringRef normalized = CFStringCreateMutableCopy(NULL, 0, original);
    bool valid = false;
    if (normalized) {
        CFStringNormalize(normalized, kCFStringNormalizationFormC);
        char utf8[513];
        valid = CFStringGetCString(normalized, utf8, sizeof(utf8), kCFStringEncodingUTF8) &&
                !strcmp(value, utf8);
        CFRelease(normalized);
    }
    CFRelease(original);
    return valid;
#else
    UChar utf16[513];
    int32_t length;
    UErrorCode error = U_ZERO_ERROR;
    u_strFromUTF8(utf16, 513, &length, value, -1, &error);
    const UNormalizer2 *nfc = unorm2_getNFCInstance(&error);
    return U_SUCCESS(error) && unorm2_isNormalized(nfc, utf16, length, &error) && U_SUCCESS(error);
#endif
}

static bool decode_key(const document_t *doc, uint16_t id, unsigned char key[32]) {
    char text[45];
    unsigned char bytes[33], canonical[45];
    if (!copy_text(doc, id, text, sizeof(text)) || strlen(text) != 44 || text[43] != '=' ||
        EVP_DecodeBlock(bytes, (const unsigned char *)text, 44) != 33 ||
        EVP_EncodeBlock(canonical, bytes, 32) != 44 || memcmp(text, canonical, 44)) {
        return false;
    }
    memcpy(key, bytes, 32);
    return true;
}

static bool unique_strings(const document_t *doc,
                           node_t array,
                           bool (*validate)(const char *),
                           const char *wanted,
                           bool *found) {
    node_t node;
    for (uint16_t i = array.child; gt__package_json_node(doc, i, &node); i = node.next) {
        char text[129];
        if (!copy_text(doc, i, text, sizeof(text)) || !validate(text)) {
            return false;
        }
        *found |= wanted && !strcmp(text, wanted);
        node_t previous;
        for (uint16_t j = array.child; j != i && gt__package_json_node(doc, j, &previous);
             j = previous.next) {
            if (previous.text_size == node.text_size &&
                !memcmp(previous.text, node.text, node.text_size)) {
                return false;
            }
        }
    }
    return true;
}

static bool read_policy(const document_t *doc,
                        char channel[65],
                        const gterm_release_t *release,
                        const char *wanted,
                        unsigned char selected[32],
                        bool *authorized) {
    const char *const policy_fields[] = {"schemaVersion", "channel", "keys", "revokedKeyIds"};
    uint64_t version;
    node_t keys, revoked;
    if (!fields(doc, 1, policy_fields, 4) ||
        !gt__package_json_uint(doc, member(doc, 1, "schemaVersion"), &version) || version != 1 ||
        !copy_text(doc, member(doc, 1, "channel"), channel, 65) || !channel_name(channel) ||
        !gt__package_json_node(doc, member(doc, 1, "keys"), &keys) ||
        keys.kind != GT_PACKAGE_JSON_ARRAY || !keys.count || keys.count > 64 ||
        !gt__package_json_node(doc, member(doc, 1, "revokedKeyIds"), &revoked) ||
        revoked.kind != GT_PACKAGE_JSON_ARRAY || revoked.count > 256) {
        return false;
    }
    bool denied = false;
    if (!unique_strings(doc, revoked, gterm_catalog_digest, wanted, &denied)) {
        return false;
    }
    char seen[64][65];
    size_t seen_count = 0;
    node_t item;
    for (uint16_t i = keys.child; gt__package_json_node(doc, i, &item); i = item.next) {
        const char *const key_fields[] = {"id", "algorithm", "publicKey", "publisher", "pluginIds"};
        char id[65], publisher[513], digest[65];
        unsigned char raw[32];
        node_t plugins;
        bool matches = false;
        if (!fields(doc, i, key_fields, 5) ||
            !copy_text(doc, member(doc, i, "id"), id, sizeof(id)) ||
            !token(doc, member(doc, i, "algorithm"), "ed25519") ||
            !decode_key(doc, member(doc, i, "publicKey"), raw) ||
            !gterm_release_hash(raw, sizeof(raw), digest) || strcmp(id, digest) ||
            !copy_text(doc, member(doc, i, "publisher"), publisher, sizeof(publisher)) ||
            !display_text(publisher) ||
            !gt__package_json_node(doc, member(doc, i, "pluginIds"), &plugins) ||
            plugins.kind != GT_PACKAGE_JSON_ARRAY || !plugins.count || plugins.count > 64 ||
            !unique_strings(doc, plugins, plugin_id, release ? release->id : NULL, &matches)) {
            return false;
        }
        for (size_t j = 0; j < seen_count; j++) {
            if (!strcmp(seen[j], id)) {
                return false;
            }
        }
        memcpy(seen[seen_count++], id, sizeof(id));
        if (wanted && !strcmp(wanted, id) && !denied && matches &&
            !strcmp(publisher, release->publisher)) {
            memcpy(selected, raw, 32);
            *authorized = true;
        }
    }
    return true;
}

static bool
read_descriptor(const document_t *doc, gterm_release_t *out, char channel[65], char key[65]) {
    const char *const release_fields[] = {"schemaVersion",
                                          "channel",
                                          "keyId",
                                          "pluginId",
                                          "pluginVersion",
                                          "publisher",
                                          "packageSHA256",
                                          "artifact"};
    const char *const artifact_fields[] = {"format", "bytes", "sha256"};
    const uint16_t artifact = member(doc, 1, "artifact");
    uint64_t version;
    return fields(doc, 1, release_fields, 8) &&
           gt__package_json_uint(doc, member(doc, 1, "schemaVersion"), &version) && version == 1 &&
           copy_text(doc, member(doc, 1, "channel"), channel, 65) && channel_name(channel) &&
           copy_text(doc, member(doc, 1, "keyId"), key, 65) && gterm_catalog_digest(key) &&
           copy_text(doc, member(doc, 1, "pluginId"), out->id, sizeof(out->id)) &&
           plugin_id(out->id) &&
           copy_text(doc, member(doc, 1, "pluginVersion"), out->version, sizeof(out->version)) &&
           display_text(out->version) &&
           copy_text(doc, member(doc, 1, "publisher"), out->publisher, sizeof(out->publisher)) &&
           display_text(out->publisher) &&
           copy_text(doc, member(doc, 1, "packageSHA256"), out->digest, sizeof(out->digest)) &&
           gterm_catalog_digest(out->digest) && fields(doc, artifact, artifact_fields, 3) &&
           token(doc, member(doc, artifact, "format"), "zip-v1") &&
           gt__package_json_uint(doc, member(doc, artifact, "bytes"), &out->artifact_bytes) &&
           out->artifact_bytes >= 22 && out->artifact_bytes <= 75497472 &&
           copy_text(doc,
                     member(doc, artifact, "sha256"),
                     out->artifact_digest,
                     sizeof(out->artifact_digest)) &&
           gterm_catalog_digest(out->artifact_digest);
}

static int evaluate_policy(const unsigned char *policy,
                           size_t size,
                           gterm_release_t *out,
                           size_t signature_size,
                           volatile sig_atomic_t *cancelled) {
    document_t *doc = NULL;
    if (!gt__package_json_open(NULL, &doc)) {
        return 5;
    }
    char channel[65], expected_channel[65], key[65];
    unsigned char selected[32];
    bool authorized = false;
    int result = 2;
    gt_package_json_error_t error = GT_PACKAGE_JSON_OK;
    if (out) {
        error = gt__package_json_parse(doc, out->descriptor, out->descriptor_size);
        if (error != GT_PACKAGE_JSON_OK || !read_descriptor(doc, out, expected_channel, key)) {
            goto done;
        }
    }
    error = gt__package_json_parse(doc, policy, size);
    if (error != GT_PACKAGE_JSON_OK ||
        !read_policy(doc, channel, out, out ? key : NULL, selected, &authorized)) {
        goto done;
    }
    result = 0;
    if (out) {
        if (!authorized || strcmp(channel, expected_channel)) {
            result = 6;
        } else {
            const gt_status_t status = gt_ext_release_verify(selected,
                                                             sizeof(selected),
                                                             out->signature,
                                                             signature_size,
                                                             out->descriptor,
                                                             out->descriptor_size);
            result = status == GT_OK                ? 0
                     : status == GT_ERR_AUTH        ? 6
                     : status == GT_ERR_UNSUPPORTED ? 4
                     : status == GT_ERR_INVALID_ARG ? 2
                                                    : 5;
        }
    }
done:
    gt__package_json_destroy(&doc);
    return *cancelled ? 130 : error == GT_PACKAGE_JSON_MEMORY ? 5 : result;
}
