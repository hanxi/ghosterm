/*
 * Cghosterm shim — Swift↔C 之间的小工具，避免在 Swift 里摆弄「指针的指针」
 * 与内含指针的 C struct（易错）。所有语义仍在 core；这里只做搬运。
 */
#include "ghosterm.h"
#include <stdlib.h>
#include <string.h>

int ghosterm_c_binding_touch(void) { return (int)strlen(gt_version()); }

/* 稀疏编辑：kws[i] 为关键字；vals 是「每组以 NULL 结尾」的扁平数组；
 * offsets[i] 为第 i 组在 vals 中的起点。组首为 NULL 即删除。 */
int ghosterm_config_edit_flat(gt_config_t *cfg, const char *alias,
                              const char *const *kws,
                              const char *const *vals, const int *offsets,
                              int n, int reset) {
  gt_config_edit_t *edits = calloc((size_t)(n > 0 ? n : 1), sizeof *edits);
  if (!edits) return GT_ERR_INTERNAL;
  for (int i = 0; i < n; i++) {
    edits[i].keyword = kws[i];
    edits[i].values = (vals && vals[offsets[i]]) ? &vals[offsets[i]] : NULL;
  }
  int rc = gt_config_edit(cfg, alias, edits, (size_t)n, reset != 0);
  free(edits);
  return rc;
}

gt_transport_t *ghosterm_open_pty_flat(const char *exe, const char *const *argv,
                                       const char *const *env, const char *cwd,
                                       const char *term, int cmd_kind,
                                       unsigned short cols,
                                       unsigned short rows) {
  gt_pty_opts_t o;
  memset(&o, 0, sizeof o);
  o.executable = exe;
  o.argv = argv;
  o.env = env;
  o.cwd = cwd;
  o.term = term;
  o.cmd_kind = (gt_pty_cmd_kind_t)cmd_kind;
  o.cols = cols;
  o.rows = rows;
  return gt_transport_open_pty(&o);
}

gt_transport_t *ghosterm_open_ssh_shell(gt_ssh_t *ssh, const char *term,
                                        unsigned short cols, unsigned short rows) {
  gt_shell_opts_t o;
  memset(&o, 0, sizeof o);
  o.term = term;
  o.cols = cols;
  o.rows = rows;
  return gt_transport_open_ssh_shell(ssh, &o);
}
