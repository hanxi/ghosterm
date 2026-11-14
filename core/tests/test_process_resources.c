#define RUNTIME_RESOURCE_MAIN process_resources_fixture_main
#include "test_runtime_resources.c"
#include "extension/process-rpc.h"
#include "extension/memory.h"

typedef struct {
    gt_process_rpc_t *rpc;
    uint64_t sequence;
    char reply[8193];
} resource_pipe_t;

static const char *resource_call(resource_pipe_t *p, const char *fields) {
    char input[8193];
    int n = snprintf(input,
                     sizeof(input),
                     "{\"v\":1,\"id\":\"%llu\",\"op\":\"resource\",%s}",
                     (unsigned long long)++p->sequence,
                     fields);
    CHECK(n > 0 && n <= 8192 &&
          gt__process_rpc_call(p->rpc, input, (size_t)n) == GT_PROCESS_RPC_OK);
    const uint8_t *bytes;
    size_t size;
    CHECK(gt__process_rpc_reply(p->rpc, &bytes, &size) && size <= 8192);
    memcpy(p->reply, bytes, size);
    p->reply[size] = 0;
    CHECK(gt__process_rpc_consume(p->rpc) == GT_PROCESS_RPC_OK);
    return p->reply;
}

static uint64_t resource_handle(const char *reply) {
    const char *s = strstr(reply, "\"handle\":\"");
    CHECK(s);
    return strtoull(s + 10, NULL, 10);
}

static const char *resource_reference(resource_pipe_t *p, const char *action, uint64_t id) {
    char fields[160];
    CHECK(snprintf(fields,
                   sizeof(fields),
                   "\"action\":\"%s\",\"ref\":\"%llu\"",
                   action,
                   (unsigned long long)id) > 0);
    return resource_call(p, fields);
}

static bool resource_ok(const char *reply) {
    return strstr(reply, "\"value\":{\"status\":\"ok\"") != NULL;
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    res_root = argv[2];
    const char *mode = argv[1];
    size_t fds = ext_fds();
    pub_source_fixture_t f = {.root = -1};
    pub_pair(&f.pair, !strcmp(mode, "ungranted") ? "ungranted" : "roundtrip");
    size_t size = !strcmp(mode, "empty") ? 0 : 262144;
    pub_file(&f, size, "file.bin");
    gt_runtime_resources_t *e =
        resource_owner(f.pair.rpc.pair.e, f.pair.e.s, &f.pair.rpc.e, f.root);
    gt_runtime_resources_t *h =
        resource_owner(f.pair.rpc.pair.h, f.pair.h.s, &f.pair.rpc.h, f.root);
    resource_pipe_t p = {0};
    if (!strcmp(mode, "foreign") || !strcmp(mode, "foreign-store") ||
        !strcmp(mode, "foreign-lease") || !strcmp(mode, "used-owner")) {
        gt_runtime_resources_t *borrowed = h, *extra = NULL;
        if (!strcmp(mode, "foreign-store")) {
            extra = resource_owner(f.pair.rpc.pair.e, f.pair.h.s, &f.pair.rpc.e, f.root);
        }
        if (!strcmp(mode, "foreign-lease")) {
            gt_ext_lease_t invalid = f.pair.rpc.e;
            invalid.token[0] = invalid.token[0] == 'a' ? 'b' : 'a';
            extra = resource_owner(f.pair.rpc.pair.e, f.pair.e.s, &invalid, f.root);
        }
        if (!strcmp(mode, "used-owner")) {
            uint64_t prior = publish(e);
            CHECK(gt__runtime_resources_cancel(e, prior) == GT_RUNTIME_RESOURCE_OK);
            gt__runtime_resources_step(e);
            CHECK(gt__runtime_resources_forget(e, prior) == GT_RUNTIME_RESOURCE_OK);
            borrowed = e;
        }
        if (extra) {
            borrowed = extra;
        }
        CHECK(gt__process_rpc_open_resources(f.pair.rpc.pair.e, borrowed, NULL, &p.rpc) ==
                  GT_ERR_INVALID_ARG &&
              !p.rpc);
        if (extra) {
            CHECK(gt__runtime_resources_destroy(&extra) == GT_RUNTIME_RESOURCE_OK);
        }
    } else {
        CHECK(gt__process_rpc_open_resources(
                  f.pair.rpc.pair.e, !strcmp(mode, "unsupported") ? NULL : e, NULL, &p.rpc) ==
              GT_OK);
        if (!strcmp(mode, "unsupported")) {
            CHECK(strstr(resource_call(&p, "\"action\":\"workspaces\""),
                         "\"status\":\"unsupported\""));
        } else if (!strcmp(mode, "bad-fields") || !strcmp(mode, "bad-base64")) {
            const char *input = !strcmp(mode, "bad-fields")
                                    ? "{\"v\":1,\"id\":\"1\",\"op\":\"resource\",\"action\":"
                                      "\"take\",\"extra\":true}"
                                    : "{\"v\":1,\"id\":\"1\",\"op\":\"resource\",\"action\":"
                                      "\"publish\",\"root\":\"workspace\",\"path64\":\"Zh==\","
                                      "\"mime\":\"text/plain\",\"limit\":\"1\",\"timeoutMs\":1000}";
            CHECK(gt__process_rpc_call(p.rpc, input, strlen(input)) == GT_PROCESS_RPC_PROTOCOL);
            CHECK(gt_ext_session_access(f.pair.rpc.pair.e, &f.pair.rpc.e, "file.readScoped") ==
                  GT_EXT_ACCESS_REVOKED);
        } else {
            CHECK(strstr(resource_call(&p, "\"action\":\"workspaces\""),
                         "\"roots\":[\"workspace\"]"));
            const char *reply =
                resource_call(&p,
                              !strcmp(mode, "denied")
                                  ? "\"action\":\"publish\",\"root\":\"outside\",\"path64\":"
                                    "\"ZmlsZS5iaW4=\",\"mime\":\"application/"
                                    "octet-stream\",\"limit\":\"262144\",\"timeoutMs\":10000"
                                  : "\"action\":\"publish\",\"root\":\"workspace\",\"path64\":"
                                    "\"ZmlsZS5iaW4=\",\"mime\":\"application/"
                                    "octet-stream\",\"limit\":\"262144\",\"timeoutMs\":10000");
            if (!strcmp(mode, "denied")) {
                CHECK(strstr(reply, "\"status\":\"denied\""));
            } else if (!strcmp(mode, "ungranted")) {
                CHECK(strstr(reply, "\"status\":\"unsupported\""));
            } else {
                CHECK(resource_ok(reply));
                uint64_t sent = resource_handle(reply);
                if (!strcmp(mode, "reuse")) {
                    for (size_t i = 0; i < 80; i++) {
                        CHECK(resource_ok(resource_reference(&p, "cancel", sent)));
                        gt__runtime_resources_step(e);
                        CHECK(resource_ok(resource_reference(&p, "forget", sent)));
                        if (i != 79) {
                            reply = resource_call(
                                &p,
                                "\"action\":\"publish\",\"root\":\"workspace\",\"path64\":"
                                "\"ZmlsZS5iaW4=\",\"mime\":\"application/"
                                "octet-stream\",\"limit\":\"262144\",\"timeoutMs\":10000");
                            CHECK(resource_ok(reply));
                            uint64_t next = resource_handle(reply);
                            CHECK(next > sent);
                            sent = next;
                        }
                    }
                } else if (!strcmp(mode, "unknown")) {
                    for (size_t i = 0; i < 10000; i++) {
                        resource_cycle(&f, e, h);
                        if (strstr(resource_reference(&p, "publication", sent),
                                   "\"hasTransfer\":true")) {
                            break;
                        }
                    }
                    CHECK(strstr(p.reply, "\"hasTransfer\":true"));
                    for (size_t i = 0; i < 5; i++) {
                        resource_cycle(&f, e, h);
                    }
                    gt_ext_session_stop(f.pair.rpc.pair.h);
                    gt_ext_session_step(f.pair.rpc.pair.h, 0);
                    CHECK(gt_ext_session_close(f.pair.rpc.pair.h, 0) == GT_EXT_RELEASED);
                    for (size_t i = 0; i < 10000; i++) {
                        resource_cycle(&f, e, h);
                        reply = resource_reference(&p, "publication", sent);
                        if (strstr(reply, "\"phase\":\"uncertain\"")) {
                            break;
                        }
                    }
                    if (!strstr(reply, "\"phase\":\"uncertain\"")) {
                        fprintf(stderr, "publication observation: %s\n", reply);
                    }
                    CHECK(strstr(reply, "\"phase\":\"uncertain\"") &&
                          strstr(reply, "\"error\":\"unknown\"") &&
                          strstr(reply, "\"transferState\":\"unknown\"") &&
                          strstr(reply, "\"confirmed\":false"));
                    CHECK(gt__runtime_resources_destroy(&e) == GT_RUNTIME_RESOURCE_WAIT && e);
                } else if (!strcmp(mode, "cancel")) {
                    CHECK(resource_ok(resource_reference(&p, "cancel", sent)));
                    gt__runtime_resources_step(e);
                    CHECK(strstr(resource_reference(&p, "publication", sent),
                                 "\"phase\":\"stopped\""));
                    CHECK(resource_ok(resource_reference(&p, "forget", sent)));
                } else {
                    for (size_t i = 0; i < 10000; i++) {
                        resource_cycle(&f, e, h);
                        reply = resource_reference(&p, "publication", sent);
                        if (strstr(reply, "\"phase\":\"succeeded\"")) {
                            break;
                        }
                    }
                    CHECK(strstr(reply, "\"confirmed\":true") &&
                          strstr(reply, "\"transferState\":\"succeeded\""));
                    CHECK(resource_ok(resource_reference(&p, "forget", sent)));
                    CHECK(strstr(resource_reference(&p, "publication", sent),
                                 "\"status\":\"invalid\""));
                    gt_ext_resource_descriptor_t descriptor;
                    uint64_t received;
                    CHECK(gt__runtime_resources_take(h, &received, &descriptor) ==
                              GT_RUNTIME_RESOURCE_OK &&
                          descriptor.size == size);
                    CHECK(gt__runtime_resources_release(h, received) == GT_RUNTIME_RESOURCE_OK);
                    uint64_t host_sent = publish(h);
                    for (size_t i = 0; i < 10000; i++) {
                        resource_cycle(&f, h, e);
                        reply = resource_call(&p, "\"action\":\"take\"");
                        if (resource_ok(reply)) {
                            break;
                        }
                    }
                    CHECK(resource_ok(reply));
                    uint64_t ref = resource_handle(reply);
                    CHECK(ref > sent);
                    CHECK(
                        strstr(resource_reference(&p, "release", sent), "\"status\":\"invalid\""));
                    char fields[256];
                    CHECK(snprintf(fields,
                                   sizeof(fields),
                                   "\"action\":\"open\",\"ref\":\"%llu\",\"limit\":\"262144\","
                                   "\"timeoutMs\":10000",
                                   (unsigned long long)ref) > 0);
                    CHECK(resource_ok(resource_call(&p, fields)));
                    if (!strcmp(mode, "revoked")) {
                        gt_ext_session_revoke(f.pair.rpc.pair.e);
                    }
                    size_t at = 0;
                    for (size_t i = 0; i < 10000; i++) {
                        resource_cycle(&f, h, e);
                        CHECK(snprintf(fields,
                                       sizeof(fields),
                                       "\"action\":\"read\",\"ref\":\"%llu\",\"at\":\"%zu\","
                                       "\"maxBytes\":16384",
                                       (unsigned long long)ref,
                                       at) > 0);
                        reply = resource_call(&p, fields);
                        if (!strcmp(mode, "revoked")) {
                            CHECK(strstr(reply, "\"status\":\"closed\"") &&
                                  strstr(reply, "\"hex\":\"\""));
                            break;
                        }
                        if (strstr(reply, "\"status\":\"wait\"")) {
                            continue;
                        }
                        CHECK(resource_ok(reply));
                        const char *hex = strstr(reply, "\"hex\":\"");
                        CHECK(hex);
                        hex += 7;
                        size_t n = strcspn(hex, "\"");
                        CHECK(n % 2 == 0 && n <= 6144);
                        for (size_t j = 0; j < n; j += 2) {
                            char part[3] = {hex[j], hex[j + 1], 0};
                            CHECK(strtoul(part, NULL, 16) == (at + j / 2) % 251);
                        }
                        at += n / 2;
                        if (!n) {
                            break;
                        }
                    }
                    if (strcmp(mode, "revoked")) {
                        CHECK(at == size);
                    }
                    CHECK(resource_ok(resource_reference(&p, "release", ref)));
                    CHECK(strstr(resource_reference(&p, "release", ref), "\"status\":\"invalid\""));
                    for (size_t i = 0; i < 10000 && publication(h, host_sent).source.phase !=
                                                        GT_EXT_SOURCE_SUCCEEDED;
                         i++) {
                        resource_cycle(&f, h, e);
                    }
                }
            }
        }
        CHECK(gt__process_rpc_destroy(&p.rpc) == GT_OK && !p.rpc);
    }
    gt_ext_session_stop(f.pair.rpc.pair.h);
    gt_ext_session_stop(f.pair.rpc.pair.e);
    gt_ext_session_step(f.pair.rpc.pair.h, 0);
    gt_ext_session_step(f.pair.rpc.pair.e, 0);
    (void)gt_ext_session_close(f.pair.rpc.pair.h, 0);
    (void)gt_ext_session_close(f.pair.rpc.pair.e, 0);
    CHECK(gt__runtime_resources_destroy(&e) == GT_RUNTIME_RESOURCE_OK &&
          gt__runtime_resources_destroy(&h) == GT_RUNTIME_RESOURCE_OK);
    pub_fixture_drop(&f);
    CHECK(ext_fds() == fds && !gt__plugin_memory_info().used);
    return 0;
}
