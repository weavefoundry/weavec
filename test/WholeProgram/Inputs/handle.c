/* An allocator and two releasers split from their callers, for the RFC 0007
 * whole-program tests: `log_open` hands out a `FILE` (family `fclose`),
 * `log_close` closes one, and `xfree` is a wrapper around `free`. */
#include <stdio.h>
#include <stdlib.h>
#include "handle.h"

FILE *log_open(const char *path) { return fopen(path, "a"); }

void log_close(FILE *f) {
  if (f)
    fclose(f);
}

void xfree(void *p) { free(p); }

int log_peek(FILE *f) { return f != NULL; }
