/* Actual decoder, with a checked-member boundary whose byte after the fixed
 * schema is deliberately non-NUL. No package bytes or upstream code are edited. */
#include "extension/package-owner.h"
#include "extension/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char fixed_schema[] =
    "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false,"
    "\"x-ghosterm-cli\":{\"version\":1,\"fields\":{}}}!";
static size_t live_allocations;
static bool fail_allocation;

static void *boundary_alloc(const gt_allocator_t *allocator, size_t size) {
    (void)allocator;
    if (fail_allocation) {
        return NULL;
    }
    void *bytes = malloc(size);
    if (bytes) {
        memset(bytes, 0xa5, size);
        live_allocations++;
    }
    return bytes;
}

static void boundary_free(const gt_allocator_t *allocator, void *bytes, size_t size) {
    (void)allocator;
    (void)size;
    if (bytes) {
        live_allocations--;
        free(bytes);
    }
}

static size_t boundary_count(const gt_package_checked_t *checked) {
    (void)checked;
    return 1;
}

static bool
boundary_command(const gt_package_checked_t *checked, size_t index, gt_package_command_t *command) {
    (void)checked;
    if (index) {
        return false;
    }
    memset(command, 0, sizeof(*command));
    strcpy(command->path, "fixture");
    strcpy(command->args_schema, "schema.json");
    return true;
}

static bool boundary_member(const gt_package_checked_t *checked,
                            const char *path,
                            gt_package_member_t *member,
                            const uint8_t **bytes,
                            size_t *size) {
    (void)checked;
    if (strcmp(path, "schema.json")) {
        return false;
    }
    memset(member, 0, sizeof(*member));
    *size = sizeof(fixed_schema) - 2;
    member->bytes = *size;
    *bytes = (const uint8_t *)fixed_schema;
    return true;
}

#define gt__plugin_memory_alloc boundary_alloc
#define gt__plugin_memory_free boundary_free
#define gt__package_checked_command_count boundary_count
#define gt__package_checked_command boundary_command
#define gt__package_checked_member boundary_member
#define gt__package_decode_arguments boundary_decode_checked
#define gt_ext_package_decode_command_arguments boundary_decode
#include "../src/extension/package-arguments.c"
#undef gt_ext_package_decode_command_arguments
#undef gt__package_decode_arguments

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            fprintf(stderr, "FAIL arguments:%d %s\n", __LINE__, #value);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2);
    void *sample = boundary_alloc(NULL, 1);
    CHECK(sample);
    boundary_free(NULL, sample, 1);
    /* Borrowed opaque marker, consumed only by the boundary hooks above. The
     * actual shared decoder now validates the non-NULL checked owner first. */
    max_align_t checked_marker;
    gt_ext_package_t package = {.info = {.phase = GT_EXT_PACKAGE_CHECKED},
                                .checked = (gt_package_checked_t *)&checked_marker};
    size_t size = 0;
    if (!strcmp(argv[1], "control")) {
        CHECK(boundary_decode(&package, "missing", NULL, 0, NULL, 0, &size) == GT_ERR_NOT_FOUND);
        CHECK(!live_allocations);
        return 0;
    }
    if (!strcmp(argv[1], "oom")) {
        fail_allocation = true;
        CHECK(boundary_decode(&package, "fixture", NULL, 0, NULL, 0, &size) == GT_ERR_IO);
        CHECK(size == 0 && !live_allocations);
        return 0;
    }
    CHECK(fixed_schema[sizeof(fixed_schema) - 2] == '!');
    CHECK(boundary_decode(&package, "fixture", NULL, 0, NULL, 0, &size) == GT_OK);
    CHECK(size == 2 && !live_allocations);
    char output[3] = {'X', 'Y', 'Z'};
    if (!strcmp(argv[1], "capacity")) {
        CHECK(boundary_decode(&package, "fixture", NULL, 0, output, 1, &size) ==
              GT_ERR_INVALID_ARG);
        CHECK(output[0] == 'X' && size == 2 && !live_allocations);
    } else {
        CHECK(!strcmp(argv[1], "tail"));
        CHECK(boundary_decode(&package, "fixture", NULL, 0, output, 2, &size) == GT_OK);
        CHECK(!memcmp(output, "{}", 2) && output[2] == 'Z' && !live_allocations);
    }
    return 0;
}
