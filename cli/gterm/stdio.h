#ifndef GTERM_STDIO_H
#define GTERM_STDIO_H
#include <signal.h>
/* argv begins after `relay --stdio`; stdout is protocol only. */
int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled);
/* Explicit native-scoped instance group over one physical stdio connection. */
int gterm_multiplex(int argc, char **argv, volatile sig_atomic_t *cancelled);
/* Lexical boundary shared with IPC invocation matching. Returns the index of
 * `--` after complete known options; option values may themselves be `--`. */
int gterm_stdio_invocation_index(int argc, char **argv);
/* Lexical/basic runtime preflight only; no package IO, activation or grants. */
int gterm_stdio_validate(int argc, char **argv);
int gterm_stdio_option_arity(const char *);
#endif
