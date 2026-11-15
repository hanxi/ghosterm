/* Native receiver and explicitly authorized read-only source for dev tests. */
#include "../../core/src/base/sha256.h"

static struct {
    gt_ext_resource_store_t *host, *executor;
    gt_ext_session_t *peer;
    gt_ext_resource_snapshot_t snapshot;
    gt_ext_lease_t lease;
    int root_fd;
    const char *sha, *name, *mime;
    uint64_t bytes;
    bool received, client_received;
} kit_res = {.root_fd = -1};

static void kit_resource_cleanup(void) {
    if (kit_res.root_fd >= 0) {
        close(kit_res.root_fd);
        kit_res.root_fd = -1;
    }
    if (kit_res.peer) {
        for (unsigned i = 0; i < 1000; i++) {
            if (gt_ext_session_close(kit_res.peer, 0) == GT_EXT_RELEASED) {
                gt_ext_session_destroy(&kit_res.peer);
                break;
            }
        }
    }
    if (kit_res.snapshot.handle && kit_res.host) {
        gt_ext_resource_store_release(kit_res.host, &kit_res.lease, &kit_res.snapshot);
    }
    if (kit_res.host) {
        gt_ext_resource_store_destroy(&kit_res.host);
    }
    if (kit_res.executor) {
        gt_ext_resource_store_destroy(&kit_res.executor);
    }
}

static gt_ext_resource_store_t *kit_store(const char *root, const char *name) {
    char path[4097];
    kit_check(snprintf(path, sizeof(path), "%s/%s", root, name) > 0, "resource store path");
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    kit_check(fd >= 0, "resource private directory");
    gt_ext_resource_store_config_t config = {fd, 1048576, 262144, 4};
    gt_ext_resource_store_t *store = NULL;
    gt_ext_resource_status_t status = gt_ext_resource_store_open(&config, &store);
    close(fd);
    kit_check(status == GT_EXT_RESOURCE_OK && store, "actual resource store");
    return store;
}

static void kit_resource_open(char **fields) {
    kit_res.host = kit_store(fields[1], "host");
    kit_res.executor = kit_store(fields[1], "executor");
    kit_res.root_fd = open(fields[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    kit_check(kit_res.root_fd >= 0, "authorized physical source directory");
    kit_res.sha = fields[2];
    kit_res.bytes = strtoull(fields[3], NULL, 10);
    kit_res.name = fields[4];
    kit_res.mime = fields[5];
}

static void kit_resource_verify(const gt_ext_lease_t *lease) {
    uint8_t bytes[16384];
    size_t read = 0;
    uint64_t at = 0;
    gt_sha256_t hash;
    char actual[65];
    gt__sha256_init(&hash);
    while (at < kit_res.bytes) {
        kit_check(gt_ext_resource_store_read(
                      kit_res.host, lease, &kit_res.snapshot, at, bytes, sizeof(bytes), &read) ==
                          GT_EXT_RESOURCE_OK &&
                      read > 0 && read <= kit_res.bytes - at,
                  "resource bytes from actual snapshot");
        kit_check(gt__sha256_update(&hash, bytes, read), "resource read digest update");
        at += read;
    }
    kit_check(gt__sha256_final(&hash, actual) && !strcmp(actual, kit_res.sha),
              "independent received resource digest");
}

static void kit_resource_take(gt_ext_session_t *peer, const gt_ext_lease_t *lease) {
    kit_res.lease = *lease;
    gt_ext_resource_status_t status = gt_ext_session_take_resource(peer, lease, &kit_res.snapshot);
    if (!*kit_res.sha) {
        kit_check(status == GT_EXT_RESOURCE_WAIT, "no unexpected publication");
        return;
    }
    kit_check(status == GT_EXT_RESOURCE_OK, "native confirmed resource handoff");
    kit_res.received = true;
    gt_ext_resource_descriptor_t *d = &kit_res.snapshot.descriptor;
    kit_check(d->size_known && d->size == kit_res.bytes && !strcmp(d->sha256, kit_res.sha) &&
                  !strcmp(d->display_name, kit_res.name) && !strcmp(d->mime, kit_res.mime) &&
                  strlen(d->resource_id) == 32 && d->generation,
              "exact received resource metadata");
    kit_resource_verify(lease);
    gt_ext_lease_t other = *lease;
    other.epoch ^= 1;
    uint8_t byte;
    size_t read = 999;
    status =
        gt_ext_resource_store_read(kit_res.host, &other, &kit_res.snapshot, 0, &byte, 1, &read);
    kit_check((status == GT_EXT_RESOURCE_DENIED || status == GT_EXT_RESOURCE_INVALID) && !read,
              "foreign resource lease refused without bytes");
    gt_ext_resource_snapshot_t extra;
    kit_check(gt_ext_session_take_resource(peer, lease, &extra) == GT_EXT_RESOURCE_WAIT,
              "exactly one resource published");
}

static void kit_resource_finish(const gt_ext_lease_t *lease) {
    if (kit_res.received) {
        kit_resource_verify(lease); /* Original published pin survives Session close. */
        kit_check(gt_ext_resource_store_destroy(&kit_res.host) == GT_EXT_RESOURCE_WAIT &&
                      kit_res.host,
                  "retained snapshot prevents store destruction");
        kit_check(gt_ext_resource_store_release(kit_res.host, lease, &kit_res.snapshot) ==
                          GT_EXT_RESOURCE_OK &&
                      !kit_res.snapshot.handle,
                  "native snapshot release");
    }
    gt_ext_resource_usage_t h, e;
    kit_check(gt_ext_resource_store_usage(kit_res.host, &h) &&
                  gt_ext_resource_store_usage(kit_res.executor, &e) && !h.reserved_bytes &&
                  !h.resources && !h.scopes && !h.sessions && !e.reserved_bytes && !e.resources &&
                  !e.scopes && !e.sessions,
              "resource reservations references and sessions released");
    kit_check(gt_ext_resource_store_destroy(&kit_res.host) == GT_EXT_RESOURCE_OK && !kit_res.host &&
                  gt_ext_resource_store_destroy(&kit_res.executor) == GT_EXT_RESOURCE_OK &&
                  !kit_res.executor,
              "both stores destroyed");
    printf("{\"case\":\"resource\",\"verified\":true,\"released\":true,\"received\":%s%s}\n",
           kit_res.received || kit_res.client_received ? "true" : "false",
           kit_res.client_received ? ",\"consumer\":\"client\"" : "");
}
