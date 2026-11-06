#ifndef GTERM_STDIO_H
#define GTERM_STDIO_H
#include <signal.h>
/* argv begins after `relay --stdio`; stdout is protocol only. */
int gterm_stdio(int argc, char **argv, volatile sig_atomic_t *cancelled);
#endif
