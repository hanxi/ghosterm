/* Swift↔C 桥接工具（实现见 shim.c）——不属于稳定 ABI，仅供 App 侧绑定使用。 */
#ifndef GHOSTERM_BRIDGE_H
#define GHOSTERM_BRIDGE_H

#include "ghosterm.h"

#ifdef __cplusplus
extern "C" {
#endif

int ghosterm_config_edit_flat(gt_config_t *cfg, const char *alias,
                              const char *const *kws, const char *const *vals,
                              const int *offsets, int n, int reset);

gt_transport_t *ghosterm_open_pty_flat(const char *exe, const char *const *argv,
                                       const char *const *env, const char *cwd,
                                       const char *term, int cmd_kind,
                                       unsigned short cols,
                                       unsigned short rows);

gt_transport_t *ghosterm_open_ssh_shell(gt_ssh_t *ssh, const char *term,
                                        unsigned short cols, unsigned short rows);

#ifdef __cplusplus
}
#endif

#endif /* GHOSTERM_BRIDGE_H */
