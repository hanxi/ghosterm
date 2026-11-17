#ifndef GTERM_IPC_H
#define GTERM_IPC_H
#include <signal.h>
/* Explicit foreground endpoint; no discovery, terminal carrier or daemon. */
int gterm_ipc_listen(const char *, int, char **, volatile sig_atomic_t *);
int gterm_ipc_attach(const char *, const char *, int, char **, volatile sig_atomic_t *);
int gterm_ipc_inspect(const char *, const char *, volatile sig_atomic_t *);
#endif
