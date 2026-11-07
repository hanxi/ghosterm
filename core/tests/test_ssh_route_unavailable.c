#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL route unavailable: %s\n", #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int main(void) {
    gt_ssh_opts_t options = {.host = "route.invalid", .port = 22, .username = "fixture"};
    gt_ssh_t *ssh = gt_ssh_new(&options);
    CHECK(ssh);
    gt_ssh_t *saved = ssh;
    gt_ssh_route_hop_t hop = {.authentication = {.family = 4, .address = {127, 0, 0, 1}}};
    gt_ssh_route_t *route = (void *)1;
    CHECK(gt_ssh_route_open(&ssh, &hop, 1, 0, &route) == GT_ERR_UNSUPPORTED && !route &&
          ssh == saved);
    bool released = false;
    CHECK(gt_ssh_route_dispose(&route, &released) == GT_OK && released);
    gt_ssh_close(ssh);
    puts("PASS unavailable route rejects without consuming SSH");
    return 0;
}
