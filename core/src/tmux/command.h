#ifndef GT_TMUX_COMMAND_H
#define GT_TMUX_COMMAND_H
#include "ghosterm.h"
#define GT_COMMAND_LIMIT (1024u * 1024u)
typedef struct {
  char *out, *err;
  size_t out_len, err_len;
  int exit_code;
  bool completed, started;
} gt_command_result_t;
typedef struct {
  int64_t deadline;
  bool (*cancelled)(void *);
  void *context;
} gt_command_budget_t;
int64_t gt__command_now(void);
bool gt__command_expired(const gt_command_budget_t *);
int gt__command_append(char **, size_t *, const char *, size_t);
void gt__command_free(gt_command_result_t *);
int gt__command_local(const char *, const char *const *,
                      const gt_command_budget_t *, gt_command_result_t *);
int gt__command_ssh(gt_ssh_t *, const char *, const gt_command_budget_t *,
                    gt_command_result_t *);
/* POSIX shell single-quote encoding. No command substitution or interpolation.
 */
char *gt__command_quote(const char *);
#endif
