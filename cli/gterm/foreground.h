#ifndef GTERM_FOREGROUND_H
#define GTERM_FOREGROUND_H
#include <stdbool.h>
#include <stddef.h>
/* Private, bounded declaration. No endpoint, paths, credentials or grants. */
#define GTERM_FOREGROUND_MAX 24576u
bool gterm_foreground_encode(int, char **, unsigned char *, size_t *);
/* Returns a malloc-owned ASCII JSON object, after strict wire validation. */
char *gterm_foreground_json(unsigned char *, size_t, const char *, size_t *);
#endif
