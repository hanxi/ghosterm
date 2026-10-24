/* Private OSC 133 owner state. Decoded events only; not a public ABI/wire. */
#ifndef GT_COMMAND_STATUS_H
#define GT_COMMAND_STATUS_H
#include "ghosterm/types.h"
typedef struct {
  bool running,has_exit_code,has_duration,has_start_time;
  int32_t exit_code;
  uint64_t duration_ns,start_ns;
  char command[4097],error[4097];
} gt_command_status_t;
typedef struct {
  unsigned kind; /* 1 prompt, 2 input, 3 output start, 4 command end */
  const char *command,*error;
  bool has_exit_code,has_time;
  int32_t exit_code;
  uint64_t monotonic_ns;
} gt_command_event_t;
/* Zero-initialized state = unknown/empty, not successful command completion.
 * Single non-UI owner; caller serializes apply/copy, may publish a value copy
 * to another thread. No pointers remain in state. Raw text is NOT UI-cleaned.
 * Only relevant fields read: start requires command, end requires error, each
 * NUL terminated <=4096 bytes. Invalid input preserves the entire prior value.
 * kind 0 invalid; other unknown kinds ignored as engine's contract requires.
 * prompt/input do not erase last command. Start replaces prior command/result,
 * sets running even if clock unavailable. End stops running, copies optional
 * exit/error, computes duration only with a valid start and nondecreasing clock.
 * End without a start never invents elapsed time or successful exit code. */
gt_status_t gt__command_status_apply(gt_command_status_t *,const gt_command_event_t *);
#endif
