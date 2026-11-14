/* Owned App Client -> real SSH -> Exec snapshot consumption and release. */
static void app_receive_client(uint64_t *seq, char reply[8193], const char *root) {
    const char *handle;
    for (;;) {
        call(seq, "\"op\":\"resource\",\"action\":\"take\"", reply);
        handle = strstr(reply, "\"handle\":\"");
        if (handle) {
            break;
        }
        CHECK(strstr(reply, "\"status\":\"wait\""));
    }
    unsigned long long id = strtoull(handle + 10, NULL, 10);
    CHECK(id);
    uint16_t v;
    app_json_t *d = app_proc_value(reply, &v);
    uint16_t descriptor = app_member(d, v, "descriptor"), sha = app_member(d, descriptor, "sha256");
    CHECK(app_string(d, app_member(d, descriptor, "displayName"), "upload.bin") &&
          app_string(d, app_member(d, descriptor, "mediaType"), "application/octet-stream") &&
          app_string(d, app_member(d, descriptor, "bytes"), "262144") &&
          d->nodes[app_member(d, descriptor, "sizeKnown")].kind == J_TRUE && sha &&
          d->nodes[sha].kind == J_STRING && d->nodes[sha].size == 64);
    char expected[65];
    memcpy(expected, d->strings + d->nodes[sha].text, 64);
    expected[64] = 0;
    release(NULL, d, sizeof(*d));
    char fields[512];
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"resource\",\"action\":\"open\",\"ref\":\"%llu\",\"limit\":\"262144\","
                   "\"timeoutMs\":10000",
                   id) > 0);
    call(seq, fields, reply);
    app_proc_ok(reply);
    gt_sha256_t hash;
    gt__sha256_init(&hash);
    size_t at = 0;
    for (;;) {
        CHECK(snprintf(fields,
                       sizeof(fields),
                       "\"op\":\"resource\",\"action\":\"read\",\"ref\":\"%llu\",\"at\":\"%zu\","
                       "\"maxBytes\":16384",
                       id,
                       at) > 0);
        call(seq, fields, reply);
        if (strstr(reply, "\"status\":\"wait\"")) {
            continue;
        }
        app_proc_ok(reply);
        const char *hex = strstr(reply, "\"hex\":\"");
        CHECK(hex);
        hex += 7;
        size_t n = strcspn(hex, "\"");
        CHECK(n % 2 == 0 && n <= 6144 && n / 2 <= 262144 - at);
        uint8_t bytes[3072];
        for (size_t i = 0; i < n / 2; i++) {
            char part[3] = {hex[i * 2], hex[i * 2 + 1], 0}, *end;
            unsigned long value = strtoul(part, &end, 16);
            CHECK(end == part + 2 && value == ((at + i) * 7 + 3) % 251);
            bytes[i] = (uint8_t)value;
        }
        CHECK(gt__sha256_update(&hash, bytes, n / 2));
        at += n / 2;
        if (!n) {
            break;
        }
    }
    char digest[65], receipt[128];
    CHECK(at == 262144 && gt__sha256_final(&hash, digest) && !strcmp(expected, digest));
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"op\":\"resource\",\"action\":\"release\",\"ref\":\"%llu\"",
                   id) > 0);
    call(seq, fields, reply);
    app_proc_ok(reply);
    call(seq, fields, reply);
    CHECK(strstr(reply, "\"value\":{\"status\":\"invalid\""));
    CHECK(snprintf(receipt, sizeof(receipt), "%s %zu", digest, at) > 0);
    app_text(root, "client-received", receipt);
}
