#ifndef GTERM_STDIO_H
#define GTERM_STDIO_H
#include <signal.h>
/* argv begins after `relay --stdio`; stdout is protocol only. */
int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled);
/* Lexical boundary shared with IPC invocation matching. Returns the index of
 * `--` after complete known options; option values may themselves be `--`. */
int gterm_stdio_invocation_index(int argc, char **argv);
#endif
